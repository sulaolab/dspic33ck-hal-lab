# -*- coding: utf-8 -*-
"""Prove src/app/dsp/avas_synth_line_ck.c computes the same integers as the numpy model.

Run from the repo root:
    python tools/host_check/run_host_check.py [seconds]

What this establishes, and why it is the check that matters
-----------------------------------------------------------
tools/avas_type_ty_fixed_model.py is where the fixed-point design was measured
against the offline reference (48.1 dB below signal, -79.0 dBFS in the line-free
bands).  Those numbers describe the FIRMWARE only if the firmware computes the
same integers -- and the failure modes of a fixed-point rewrite (a headroom shift
off by one, an interpolation fraction truncated too far, a sign lost on an
arithmetic shift) all produce audio that still sounds like an engine.  A listening
test cannot separate them; an exact comparison can.

So: compile avas_synth_line_ck.c for the host, render the same samples, require
bit-identical output.  Any difference is a defect in one of the two, and the
diff points straight at the first sample where they part company.

The gate is checked separately in closed form: the model has none, so the audio
comparison uses avas_line_ck_render_sample(), and the gate's one-pole -- which is
where the int16-numerator trick could plausibly be wrong -- is verified against
the exact exponential it is meant to be.
"""
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
APP = os.path.join(ROOT, "src", "app", "dsp")
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 0.5
AK_TABLE = os.path.join(ROOT, "tools", "host_check", "ref",
                        "avas_synth_type_ty_tables.h")

# Which carrier-accumulation mode of avas_synth_line_ck.c to prove.
#
#   0  the legacy arithmetic: int32 accumulator, TERMSHIFT per term.  NO LONGER what
#      the board ships -- mode 2 is the target default since section 14 -- but still
#      the DEFAULT HERE, on purpose: mode 0 is kept as the path for a core that
#      cannot reach the DSP accumulator, and an argument-free run of this script is
#      what keeps that path proved instead of merely present.
#   1  the 40-bit semantics written in portable C.  This is the DEFINITION of
#      correctness for ACC_MODE 2 (the mac/msc path), which cannot be compiled
#      for the host at all -- so the verification chain is:
#         model  ==  mode 1        proved here, bit for bit
#         mode 1 ==  mode 2        proved on HARDWARE, via the `sink=` checksum
#                                  ?ta already prints
#      That is what lets the DSP accumulator into the hot path without loosening
#      any quality claim.  Mode 2 is NOT bit-equal to mode 0 (it stops truncating
#      each term), which is why the model needs telling which one to model.
ACC_MODE = int(os.environ.get("ACC_MODE", sys.argv[2] if len(sys.argv) > 2 else 0))
if ACC_MODE not in (0, 1):
    fail_early = "ACC_MODE must be 0 or 1 here; 2 is the target-only mac/msc path"
    print("FAIL: " + fail_early)
    sys.exit(1)
MODEL_ACC = "acc40" if ACC_MODE == 1 else "int32"


def fail(msg):
    print("FAIL: %s" % msg)
    sys.exit(1)


# The generated header is read HERE, before anything is built, because it is where
# the shipped build-time choices live -- and so the defaults below are the shipped
# ones BY CONSTRUCTION rather than by a comment claiming they match.  Restating them
# is exactly how an argument-free run would quietly stop proving what ships.
APP_HDR = os.path.join(APP, "avas_type_ty_ck_tables.h")
hdr = open(APP_HDR, encoding="utf-8").read()


def hdr_int(name, text=None):
    m = re.search(r"#define\s+%s\s+\((-?\d+)" % name, hdr if text is None else text)
    if not m:
        fail("cannot read %s out of the generated header" % name)
    return int(m.group(1))


# ---------------------------------------------------------------------------
# WHICH VOICE is being proved: the Type_TY L1 set (default, so an argument-free run
# keeps certifying what the board ships) or the Type_LB L3 set.
#
# One engine, two coefficient sets -- so this is not a second check, it is the same
# check with a different table, which is exactly the claim that needs proving.  The
# two differ in three inputs and nothing else:
#
#   * the C is compiled with -DAVAS_CK_VOICE_TYPE_LB=1, which swaps the descriptor and
#     the table header (and drops the Type_TY arrays; design section 3.4);
#   * the model is given tools/L3_params_type_lb.txt instead of the AK header,
#     plus the MAX_SPAN_HZ the generated set was built at -- because for this voice
#     the clustering is the model's, not an upstream table's;
#   * the level macros are read out of the Type_LB header.
#
# Everything else -- every knob, the sine table, the CORDIC constants -- is shared,
# and is still read out of avas_type_ty_ck_tables.h below.
# ---------------------------------------------------------------------------
VOICE = os.environ.get("VOICE", "type_ty")
if VOICE not in ("type_ty", "type_lb"):
    print("FAIL: VOICE must be type_ty or type_lb")
    sys.exit(1)
VOICE_TYPE_LB = 1 if VOICE == "type_lb" else 0

# BOTH RESIDENT (phase 7), which is a THIRD thing to prove and not a packaging option.
# In that image the carrier loop takes its bound from the descriptor instead of a
# literal, so it is the one piece of arithmetic the single-voice runs never execute --
# and "the bound changed but nothing else did" is exactly the kind of claim that is
# cheap to assert and cheap to check.  VOICE still picks which voice is rendered; this
# only changes how the loop learns how many clusters that voice has, so a run with it
# set must produce the SAME samples as the run without it.
VOICE_BOTH = 1 if os.environ.get("VOICE_BOTH", "0") not in ("0", "", "no") else 0

if VOICE_TYPE_LB:
    vhdr = open(os.path.join(APP, "avas_type_lb_ck_tables.h"), encoding="utf-8").read()
    MODEL_TABLE = os.path.join(ROOT, "tools", "L3_params_type_lb.txt")
    GAIN_MACRO = "AVAS_TYPE_LB_CK_NORM_GAIN_Q15"
    SHIFT_MACRO = "AVAS_TYPE_LB_CK_NORM_SHIFT"
    # The span the shipped set was generated at.  Passing it to the model is what
    # makes the two agree on which 264 lines belong to which carrier; a different
    # span is a different (also valid) table, and the comparison would then be
    # measuring that instead of the C.
    os.environ["MAX_SPAN_HZ"] = str(hdr_int("AVAS_TYPE_LB_CK_MAX_SPAN_HZ", vhdr))
    LINES = hdr_int("AVAS_TYPE_LB_CK_LINES", vhdr)
    CLUSTERS = hdr_int("AVAS_TYPE_LB_CK_CLUSTERS", vhdr)
else:
    vhdr = hdr
    MODEL_TABLE = AK_TABLE
    GAIN_MACRO = "AVAS_TYPE_TY_CK_NORM_GAIN_Q15"
    SHIFT_MACRO = "AVAS_TYPE_TY_CK_NORM_SHIFT"
    LINES = hdr_int("AVAS_TYPE_TY_CK_LINES")
    CLUSTERS = hdr_int("AVAS_TYPE_TY_CK_CLUSTERS")


# Phase accumulator widths, 32 or 16, passed to BOTH sides -- the C build and the
# model -- from one place here.  Default = whatever the generated header defaults to
# (16/16 since the hardware gate closed: 54.9 % of the block, miss = 0, heard on the
# board).  Pass PHBITS_CAR=32 PHBITS_BB=32 to prove the wide width, which is still a
# supported build.
#
# This is not a second parameter of the same kind as ACC_MODE: a narrower phase
# changes the line FREQUENCIES, so the output is a different (still correct) sound
# rather than a more or less accurate version of the same one.  What this check
# proves is unchanged either way -- that the C computes what the model computed --
# and that is exactly what makes the width safe to choose: the quality question was
# settled by listening (section 16), and this settles that the firmware realises the
# thing that was listened to.
PHBITS_CAR = int(os.environ.get("PHBITS_CAR", hdr_int("AVAS_TYPE_TY_CK_PHBITS_CAR")))
PHBITS_BB = int(os.environ.get("PHBITS_BB", hdr_int("AVAS_TYPE_TY_CK_PHBITS_BB")))
for _n, _v in (("PHBITS_CAR", PHBITS_CAR), ("PHBITS_BB", PHBITS_BB)):
    if _v not in (16, 32):
        print("FAIL: %s must be 32 or 16 (the header carries those two step tables)" % _n)
        sys.exit(1)

# Which coordinates the envelope is interpolated in: 0 rect, 1 polar, again defaulted
# from the header so this proves the shipped scheme.  Polar is the SAME kind of
# parameter as the phase width -- it changes what the engine sounds like, not how
# faithfully it realises the model -- with one addition worth stating: polar's
# (I,Q)->(A,phi) is a CORDIC, so the model must be told to use the integer CORDIC
# rather than its exact hypot/arctan2 reference, or this comparison would be
# measuring the difference between the two conversions instead of proving the C.
ENVINTERP = int(os.environ.get("ENVINTERP", hdr_int("AVAS_TYPE_TY_CK_ENVINTERP")))
if ENVINTERP not in (0, 1):
    print("FAIL: ENVINTERP must be 0 (rect) or 1 (polar)")
    sys.exit(1)

# The envelope state's width and whether its slope is rounded -- V3, and the one knob
# that comes with it.  Defaulted from the header like everything else here, so an
# argument-free run keeps certifying the shipped width rather than a favourite one.
#
# These two are NOT of the same kind as the phase width or the interpolation scheme.
# A narrow envelope state does not change the sound the engine is aiming at; it changes
# how exactly it hits it, and the way it misses is a BIAS (doc sections 19 and 20).  So
# this check earns its keep twice over here: the wrap of the narrow state is where a
# 16-bit target and a 32-bit host most easily disagree, and the model reproduces that
# wrap deliberately (_env()) rather than working in wide integers and hoping.
_HDR_ENVFRAC = hdr_int("AVAS_TYPE_TY_CK_ENVFRAC")
ENVFRAC = int(os.environ.get("ENVFRAC", _HDR_ENVFRAC))
if ENVFRAC not in (0, 16):
    print("FAIL: ENVFRAC must be 0 (V3, the shipped int16 state) or 16 (the wide 32-bit state)")
    sys.exit(1)
# ENVROUND follows the width HERE TOO, by the same rule the generated header states,
# because `ENVFRAC=0 run_host_check.py` would otherwise prove the floored variant --
# which is a real build, and a rejected one.  The header's own value is used whenever
# the width is the header's, so the argument-free run still reads out of the header.
ENVROUND = int(os.environ.get(
    "ENVROUND",
    hdr_int("AVAS_TYPE_TY_CK_ENVROUND") if ENVFRAC == _HDR_ENVFRAC
    else (1 if ENVFRAC == 0 else 0)))
if ENVROUND not in (0, 1):
    print("FAIL: ENVROUND must be 0 (floor) or 1 (round)")
    sys.exit(1)


# ---------------------------------------------------------------------------
# 1. The audio path, bit for bit.
# ---------------------------------------------------------------------------
def find_host_cc():
    """Any host gcc will do -- the engine is pure integer C99.  This box has no
    gcc on PATH but does carry the one MSYS2 ships, so look there too rather than
    make the check depend on a PATH the next person may not have."""
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
                e["PATH"] = os.path.dirname(os.path.abspath(c)) + os.pathsep + e.get("PATH", "")
            if subprocess.run([c, "--version"], capture_output=True, env=e).returncode == 0:
                return c
        except OSError:
            continue
    fail("no host C compiler found; set HOSTCC to one (any gcc; the engine is "
         "plain integer C99 with no chip dependency)")


CC = find_host_cc()
print("host cc: %s" % CC)

# A Cygwin/MSYS gcc needs its own bin directory on PATH -- both to run its cc1
# and for the executable it produces.  Prepend it rather than requiring the
# caller's environment to already be right.
ENV = dict(os.environ)
if os.path.sep in CC:
    ENV["PATH"] = os.path.dirname(os.path.abspath(CC)) + os.pathsep + ENV.get("PATH", "")

exe = os.path.join(HERE, "avas_type_ty_ck_dump.exe" if os.name == "nt" else "avas_type_ty_ck_dump")
cmd = [CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-I", APP,
       "-DAVAS_CK_VOICE_TYPE_LB=%d" % VOICE_TYPE_LB,
       "-DAVAS_CK_VOICE_BOTH=%d" % VOICE_BOTH,
       "-DAVAS_TYPE_TY_CK_ACC_MODE=%d" % ACC_MODE,
       "-DAVAS_TYPE_TY_CK_PHBITS_CAR=%d" % PHBITS_CAR,
       "-DAVAS_TYPE_TY_CK_PHBITS_BB=%d" % PHBITS_BB,
       "-DAVAS_TYPE_TY_CK_ENVINTERP=%d" % ENVINTERP,
       "-DAVAS_TYPE_TY_CK_ENVFRAC=%d" % ENVFRAC,
       "-DAVAS_TYPE_TY_CK_ENVROUND=%d" % ENVROUND,
       # The noise half is compiled OUT here, and that is what makes this check still
       # mean what it says.  The model this compares against is the TONE model, so a
       # Type_LB rendered with its noise bank running would differ from it on every
       # sample and the comparison would be measuring the wrong thing.  The bank has its
       # own bit-exact check (run_noise_check.py) against its own model; keeping the two
       # separate is what lets each one fail for exactly one reason.
       "-DAVAS_CK_VOICE_TYPE_LB_NOISE=0",
       os.path.join(HERE, "avas_type_ty_ck_dump.c"), os.path.join(APP, "avas_synth_line_ck.c"),
       "-o", exe]
print("$ " + " ".join(cmd))
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV)
if r.returncode != 0:
    fail("host compile failed:\n" + r.stdout + r.stderr)
if r.stderr.strip():
    print("compiler warnings (treated as a defect -- the firmware build must be clean too):")
    print(r.stderr)
    fail("host compile produced warnings")

n = int(round(SECONDS * 48000))
r = subprocess.run([exe, str(n)], capture_output=True, text=True, env=ENV)
if r.returncode != 0:
    fail("host run failed:\n" + r.stderr)
c_out = [int(x) for x in r.stdout.split()]
if len(c_out) != n:
    fail("host produced %d samples, expected %d" % (len(c_out), n))

# Import the model with the same design parameters the generator pinned, read out
# of the generated header rather than restated -- so the two cannot drift.
os.environ["DEC"] = str(hdr_int("AVAS_TYPE_TY_CK_DEC"))
os.environ["TABBITS"] = str(hdr_int("AVAS_TYPE_TY_CK_TABBITS"))
os.environ["FRACBITS"] = str(hdr_int("AVAS_TYPE_TY_CK_FRACBITS"))
os.environ["TERMSHIFT"] = str(hdr_int("AVAS_TYPE_TY_CK_TERMSHIFT"))
os.environ["CARRIER"] = "table"
os.environ["ACC"] = MODEL_ACC
os.environ["PHBITS_CAR"] = str(PHBITS_CAR)
os.environ["PHBITS_BB"] = str(PHBITS_BB)
os.environ["ENVINTERP"] = "polar" if ENVINTERP == 1 else "rect"
os.environ["ENVFRAC"] = str(ENVFRAC)
os.environ["ENVROUND"] = str(ENVROUND)
os.environ["POLAR_CONV"] = "cordic"
os.environ["CORDIC_N"] = str(hdr_int("AVAS_TYPE_TY_CK_CORDIC_N"))
sys.argv = [sys.argv[0], MODEL_TABLE, str(SECONDS)]
sys.path.insert(0, os.path.join(ROOT, "tools"))
import avas_type_ty_fixed_model as M      # noqa: E402

if M.OUT_GAIN_Q15 != hdr_int(GAIN_MACRO, vhdr):
    fail("model gain %d != header gain %d" % (M.OUT_GAIN_Q15,
                                              hdr_int(GAIN_MACRO, vhdr)))
if M.OUT_SHIFT != hdr_int(SHIFT_MACRO, vhdr):
    fail("model output shift %d != header %d -- regenerate the table"
         % (M.OUT_SHIFT, hdr_int(SHIFT_MACRO, vhdr)))
# The table's SHAPE, checked before the samples are compared: a stale generated
# header would otherwise show up as a bit difference somewhere in the middle, which
# is far harder to read than this line.
if (M.NLINE, M.K) != (LINES, CLUSTERS):
    fail("model has %d lines / %d clusters, the %s header says %d / %d -- regenerate"
         % (M.NLINE, M.K, VOICE, LINES, CLUSTERS))
if ENVINTERP == 1:
    # The CORDIC's two constants, checked rather than assumed to have come from the
    # same place.  A mismatch here is a stale generated header, and it would show up
    # as a bit difference thousands of samples in -- much harder to read than this.
    if M.CORDIC_KINV_Q15 != hdr_int("AVAS_TYPE_TY_CK_CORDIC_KINV_Q15"):
        fail("model CORDIC 1/K %d != header %d -- regenerate the table header"
             % (M.CORDIC_KINV_Q15, hdr_int("AVAS_TYPE_TY_CK_CORDIC_KINV_Q15")))
    _atan = [int(v) for v in re.findall(
        r"(\d+)u", hdr[hdr.index("s_ck_cordic_atan"):].split("#if")[1].split("#else")[0])]
    if PHBITS_CAR == 16 and _atan != [int(v) for v in M.CORDIC_ATAN]:
        fail("model CORDIC atan table != header's 16-bit table: %s vs %s"
             % (list(M.CORDIC_ATAN), _atan))

M.run(n)
py_out = [int(v) for v in M.LAST_Q15]

bad = [i for i in range(n) if py_out[i] != c_out[i]]
print("\naudio: %d samples compared (%.3f s @ 48 kHz), ACC_MODE=%d vs model ACC=%s"
      % (n, SECONDS, ACC_MODE, MODEL_ACC))
print("  voice: %s -- %d lines in %d clusters%s, gain %d >> %d"
      % ("Type_LB L3" if VOICE_TYPE_LB else "Type_TY L1", M.NLINE, M.K,
         " (MAX_SPAN_HZ=%.0f, clustered by the model)" % M.MAX_SPAN_HZ
         if VOICE_TYPE_LB else " (clusters from the AK table)",
         M.OUT_GAIN_Q15, 15 - M.OUT_SHIFT))
print("  phase: car %d bits, bb %d bits -- realised error <= %.6f Hz carrier, "
      "%.6f Hz baseband" % (PHBITS_CAR, PHBITS_BB, M.CAR_F_ERR.max(), M.BB_F_ERR.max()))
print("  envelope interpolated in %s%s"
      % ("POLAR (A, phi)" if ENVINTERP == 1 else "RECT (I, Q)",
         ", %d-iteration CORDIC, 1/K = %d" % (M.CORDIC_N, M.CORDIC_KINV_Q15)
         if ENVINTERP == 1 else ""))
print("  envelope state: %d-bit (ENVFRAC=%d), slope %s%s"
      % (16 + ENVFRAC, ENVFRAC, "ROUNDED" if ENVROUND else "floored",
         "  <-- V3" if ENVFRAC == 0 else ""))
if bad:
    i = bad[0]
    lo = max(0, i - 3)
    print("  first difference at sample %d of %d (%d differ, %.4f %%)"
          % (i, n, len(bad), 100.0 * len(bad) / n))
    for j in range(lo, min(n, i + 4)):
        print("    [%6d] model %8d   C %8d%s"
              % (j, py_out[j], c_out[j], "   <-- here" if j == i else ""))
    fail("the C and the model disagree, so the measured accuracy figures do not "
         "describe the firmware")
print("  BIT-EXACT against tools/avas_type_ty_fixed_model.py")
if VOICE_TYPE_LB:
    # No dB figure is restated for this voice, and not only because the width moves
    # it: the accuracy of the L3 SPAN was measured at 32/32 in
    # tools/avas_type_lb_fixed_model.py (design section 12), and what this check
    # establishes is the other half -- that the C realises the model it was measured
    # from.  Quoting a number here would be quoting it from the wrong place.
    print("  -> the C realises the L3 coefficient set the model measured; the "
          "accuracy figures for it are tools/avas_type_lb_fixed_model.py's, and the "
          "acceptance gate is the listen in phase 5")
elif (PHBITS_CAR, PHBITS_BB) == (32, 32) and ENVINTERP == 0:
    print("  -> the model's 48.1 dB / -79.0 dBFS describe THIS code")
else:
    # Deliberately not restating a dB figure here.  A narrower phase MOVES the
    # lines, so the headline metric is measured against a reference the firmware is
    # no longer trying to reproduce sample for sample; the model is where that
    # number is computed, and section 16 is where the ear -- not the number --
    # accepted this width.
    print("  -> the C realises the model AT THIS PHASE WIDTH; the accuracy figure "
          "for it is whatever the model prints for these PHBITS, and it is not the "
          "acceptance gate")

# ---------------------------------------------------------------------------
# 2. The gate, in closed form.
# ---------------------------------------------------------------------------
# The firmware's one-pole is  gate += ((err >> 16) * NUM) >> (SHIFT - 16),
# an int16 multiply standing in for  gate += alpha * err  with
# alpha = NUM / 2**SHIFT.  Two things can be wrong: the numerator, and the
# truncation stalling the ramp before it arrives.  Both are checked here rather
# than inferred, because a gate that never closes leaves the engine costing 46 %
# of the block budget forever and nothing in the audio says so.
SHIFT = hdr_int("AVAS_TYPE_TY_CK_GATE_ALPHA_SHIFT")
ATT = hdr_int("AVAS_TYPE_TY_CK_GATE_ATTACK_NUM")
REL = hdr_int("AVAS_TYPE_TY_CK_GATE_RELEASE_NUM")
EPS = hdr_int("AVAS_TYPE_TY_CK_GATE_EPS_Q31")
ONE = 0x7FFFFFFF
FS = hdr_int("AVAS_TYPE_TY_CK_TABLE_FS_HZ")

print("\ngate: alpha numerators attack=%d release=%d, shift=%d" % (ATT, REL, SHIFT))
for name, num, want_tau in (("attack", ATT, 4.000), ("release", REL, 0.500)):
    if not (0 < num <= 32767):
        fail("%s numerator %d does not fit int16 -- the multiply would widen" % (name, num))
    tau = 1.0 / (FS * (num / float(1 << SHIFT)))
    err = abs(tau - want_tau) / want_tau
    print("  %-8s tau %.4f s (want %.3f s, %+.3f %%)" % (name, tau, want_tau, 100.0 * err))
    if err > 0.01:
        fail("%s time constant is %.2f %% off" % (name, 100.0 * err))


def ramp(num, start, target, limit):
    """The firmware's integer update, iterated. Returns (samples, stalled)."""
    g = start
    for i in range(limit):
        e = target - g
        d = ((e >> 16) * num) >> (SHIFT - 16)
        if d == 0:
            return i, True                     # truncated to zero: never arrives
        g += d
        if (target > start and g >= limit_hi) or (target < start and g <= EPS):
            return i + 1, False
    return limit, False


limit_hi = int(ONE * 0.9)      # "arrived" for the attack: within 1 dB of unity
att_n, att_stall = ramp(ATT, 0, ONE, 40 * FS)
rel_n, rel_stall = ramp(REL, ONE, 0, 40 * FS)
print("  attack  reaches -1 dB in %.3f s%s" % (att_n / float(FS), "  STALLED" if att_stall else ""))
print("  release reaches -50 dB in %.3f s%s" % (rel_n / float(FS), "  STALLED" if rel_stall else ""))
if att_stall:
    fail("the attack ramp truncates to zero before arriving")
if rel_stall:
    fail("the release ramp truncates to zero before reaching GATE_EPS -- the engine "
         "would keep costing its full load forever")
# The AK engine's release is 5.8 tau ~ 2.9 s to -50 dB; a large deviation means
# the numerator or the shift is wrong even though tau looked right.
if not (2.0 < rel_n / float(FS) < 4.0):
    fail("release to -50 dB took %.3f s; AK measures ~2.9 s" % (rel_n / float(FS)))

print("\nPASS")
