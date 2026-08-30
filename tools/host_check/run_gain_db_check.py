# -*- coding: utf-8 -*-
"""Prove src/app/dsp/gain_db.h and its generated table are what the dB says.

Run from the repo root:
    python tools/host_check/run_gain_db_check.py

WHAT THIS ESTABLISHES, IN THREE SEPARATE CLAIMS
-----------------------------------------------
The feature is "a gain in dB", and there are three independent ways for that to be
false.  Each gets its own check, because a single end-to-end comparison that passes
tells you nothing about which of the three you are relying on.

  1. THE TABLE IS 10^(dB/20).  The committed header is regenerated in memory from
     tools/gen_gain_db_tables.py and required to be byte-identical.  Without this
     the arithmetic could be flawless and the gain still wrong -- and a stale
     committed header is the most likely way for that to happen, since the header
     is the only artefact the compiler sees.

  2. THE FIRMWARE'S ARITHMETIC IS THE 64-BIT ARITHMETIC.  gain_db_dump.c checks the
     two 16x16 multiplies against `(int64)x * m >> 16 << SHIFT` with saturation --
     exhaustively either side of both clamp boundaries, and over a stride that
     visits every possible low word, because "the low half was treated as signed"
     is a bug that only shows up in specific low words.  The boundary sweep also
     carries the fixed-shift form's new claim: that clamping on t's HIGH WORD
     against a 16-bit immediate is the SAME test as clamping the full product.

  3. 0.0 dB IS THE IDENTITY, for all 2^32 inputs, exhaustively -- apart from the low
     GAIN_DB_SHIFT bits, which are pad and not audio (see gain_db.h).  The firmware
     now skips the stage outright at unity, so this proves the path a `*ti 0.0`
     takes rather than the one the shipped configuration takes; a typed 0.0 must not
     be a different signal from a value that was never typed.

This script owns claim 1 and re-derives claim 2's checksums from the model; the C
program owns claims 2 and 3 and says so on stdout.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DSP = os.path.join(ROOT, "src", "app", "dsp")
HEADER = os.path.join(DSP, "gain_db_tables.h")

sys.path.insert(0, os.path.join(ROOT, "tools"))


def fail(msg):
    print("FAIL: %s" % msg)
    sys.exit(1)


# ---------------------------------------------------------------------------
# 1. The committed header IS the generator's output.
# ---------------------------------------------------------------------------
gen = __import__("gen_gain_db_tables")

tmp = os.path.join(HERE, "gain_db_tables.regen.h")
gen.main(tmp)

with open(HEADER, "rb") as f:
    committed = f.read()
with open(tmp, "rb") as f:
    regenerated = f.read()
os.remove(tmp)

# Compared with EOL normalised: .gitattributes checks the working tree out as CRLF
# while the generator writes LF, so a raw byte compare would fail on a clean tree
# and say nothing about the numbers.
if committed.replace(b"\r\n", b"\n") != regenerated.replace(b"\r\n", b"\n"):
    fail("%s is not what tools/gen_gain_db_tables.py produces -- re-run the "
         "generator and commit the result" % os.path.relpath(HEADER, ROOT))
print("table: %s matches the generator" % os.path.relpath(HEADER, ROOT))

HALF_MIN = gen.HALF_MIN
HALF_MAX = gen.HALF_MAX
ENTRIES = HALF_MAX - HALF_MIN + 1

# The realisation error, restated here so a bad grid is visible rather than implied.
#
# The gate is 0.02 dB because the fixed-shift form DELIBERATELY spent resolution to buy
# speed: one mantissa LSB is 0.0148 dB at the worst point of the grid (-24.0 dB, smallest
# mantissa) where the octave form was at 0.002.  That trade was the point, so the gate
# records the new budget rather than the old one -- it is still 34x finer than the 0.5 dB
# grid the knob actually offers, and a regression past 0.02 would mean SHIFT moved or the
# grid widened, which is a decision and not an accident.
worst = max(abs(gen.realised_db(gen.decompose(h * 0.5)) - h * 0.5)
            for h in range(HALF_MIN, HALF_MAX + 1))
print("grid: %+.1f..%+.1f dB in 0.5 dB steps, %d entries, shift %d, worst error %.6f dB"
      % (HALF_MIN * 0.5, HALF_MAX * 0.5, ENTRIES, gen.SHIFT, worst))
if worst > 0.02:
    fail("worst realisation error %.6f dB exceeds the 0.02 dB the design budgets" % worst)


# ---------------------------------------------------------------------------
# 2. Build and run the C harness.
# ---------------------------------------------------------------------------
def find_host_cc():
    """Any host gcc; this is plain integer C99 with no chip dependency.  Same
    candidate list as run_host_check.py -- this box has no gcc on PATH but does
    carry the one MSYS2 ships."""
    env = os.environ.get("HOSTCC")
    cands = ([env] if env else []) + [
        "gcc",
        r"C:\msys64\ucrt64\bin\gcc.exe",
        r"C:\msys64\mingw64\bin\gcc.exe",
        r"C:\msys64\usr\bin\gcc.exe",
    ]
    for c in cands:
        try:
            e = dict(os.environ)
            if os.path.sep in c:
                e["PATH"] = (os.path.dirname(os.path.abspath(c)) + os.pathsep
                             + e.get("PATH", ""))
            if subprocess.run([c, "--version"], capture_output=True, env=e).returncode == 0:
                return c
        except OSError:
            continue
    fail("no host C compiler found; set HOSTCC to one")


CC = find_host_cc()
print("host cc: %s" % CC)
ENV = dict(os.environ)
if os.path.sep in CC:
    ENV["PATH"] = os.path.dirname(os.path.abspath(CC)) + os.pathsep + ENV.get("PATH", "")

exe = os.path.join(HERE, "gain_db_dump.exe" if os.name == "nt" else "gain_db_dump")
cmd = [CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-I", DSP,
       os.path.join(HERE, "gain_db_dump.c"), "-o", exe]
print("$ " + " ".join(cmd))
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV)
if r.returncode != 0:
    fail("host compile failed:\n" + r.stdout + r.stderr)
if r.stderr.strip():
    print(r.stderr)
    fail("host compile produced warnings (the firmware build must be clean too)")

print("running the harness (the 2^32 identity sweep takes a few seconds)...")
r = subprocess.run([exe], capture_output=True, text=True, env=ENV)
if r.returncode != 0:
    fail("harness reported a defect:\n" + r.stdout + r.stderr)

lines = [l.strip() for l in r.stdout.splitlines() if l.strip()]
rows = [l.split() for l in lines if l[0].isdigit()]
tail = [l for l in lines if not l[0].isdigit()]

if ("IDENTITY ok (all 2^32 inputs, low %d bits dropped)" % gen.SHIFT) not in tail:
    fail("the 0.0 dB identity sweep did not pass:\n" + "\n".join(tail))
if "SELFCHECK ok" not in tail:
    fail("the 64-bit self-check did not pass:\n" + "\n".join(tail))
if len(rows) != ENTRIES:
    fail("harness printed %d entries, expected %d" % (len(rows), ENTRIES))
print("firmware arithmetic == 64-bit reference; 0.0 dB identity holds for all 2^32 inputs")


# ---------------------------------------------------------------------------
# 3. Re-derive the C harness's per-entry checksum from the model.
#
# The sample list is defined by a formula on both sides rather than sent over a
# pipe -- 6.3 M numbers would be the slow way to say STRIDE = 65521.
# ---------------------------------------------------------------------------
STRIDE = 65521
SEED = 0x01234567
NSTRIDE = 65536
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >= (1 << 31) else v


def model_scale(x, mant):
    """The 64-bit definition, in python.  floor division, which is what an
    arithmetic >> 16 is for negative values -- python's // agrees by default and
    that agreement is the reason this is written with // and not int()."""
    t = (x * mant) >> 16
    y = t << gen.SHIFT
    if y > INT32_MAX:
        return INT32_MAX, True
    if y < INT32_MIN:
        return INT32_MIN, True
    return y, False


def fnv(acc, y):
    v = y & 0xFFFFFFFF
    for _ in range(4):
        acc = ((acc ^ (v & 0xFF)) * 16777619) & 0xFFFFFFFF
        v >>= 8
    return acc


bad = 0
for row in rows:
    idx, half, mant, csum, nsat = (int(v) for v in row)

    exp_m = gen.decompose(half * 0.5)
    if mant != exp_m:
        print("ENTRY %+.1f dB: header says mant=%d, model says %d"
              % (half * 0.5, mant, exp_m))
        bad += 1
        continue

    acc = 2166136261
    nsat_model = 0
    for k in range(NSTRIDE):
        x = s32(SEED + k * STRIDE)
        y, sat = model_scale(x, mant)
        if sat:
            nsat_model += 1
        acc = fnv(acc, y)

    if acc != csum:
        print("ENTRY %+.1f dB: checksum C=%u model=%u" % (half * 0.5, csum, acc))
        bad += 1
    if (nsat_model & 0xFFFF) != nsat:
        print("ENTRY %+.1f dB: saturated C=%u model=%u (uint16, wraps)"
              % (half * 0.5, nsat, nsat_model & 0xFFFF))
        bad += 1

if bad:
    fail("%d entries disagree with the model" % bad)

print("all %d entries: C output == python model, bit for bit" % ENTRIES)
print("PASS")
