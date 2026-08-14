# -*- coding: utf-8 -*-
"""Prove src/app/dsp/avas_noise_bank_ck.c computes the same integers as the model.

Run from the repo root:
    python tools/host_check/run_noise_check.py

WHY THIS EXISTS.  The tone half of the Type_LB voice can be audited by ear at a
pinch -- a wrong line frequency is a wrong note.  The noise half cannot: a shift off by
one, a sign lost on an arithmetic shift, or a state that wraps once a minute all produce
something that still sounds like broadband noise.  So the noise bank's correctness is
established the only way it can be, by requiring bit-identical output against the numpy
model that the design was measured on.

WHAT IS AND IS NOT COVERED.  Covered: the xorshift source, the one-pole source tilt, all
twelve SVFs, and the counts-to-A_SCALE level -- the reference is emitted in the units
avas_noise_bank_ck_sample() returns, so a wrong gain fails here too.  Not covered: the
gusts, which draw from a PRNG once per band per control block and cannot be made to
agree across two languages' draw orders.  Their specification is a modulation standard
deviation, and tools/gen_avas_type_lb_ck_noise.py measures the realised one (1.56 dB
against the model's 1.5 dB) -- a different claim, checked in a different place, rather
than an untested gap.

There is also one class of defect NEITHER can catch, so it is called out here: `int` is
16 bits on the dsPIC and 32 on any host, so an intermediate that wraps on the target
will not wrap under this check.  Those casts are load-bearing and commented as such in
avas_noise_bank_ck.c.
"""
import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
APP = os.path.join(ROOT, "src", "app", "dsp")
REF = os.path.join(HERE, "type_lb_noise_ref.txt")


def fail(msg):
    print("FAIL: %s" % msg)
    sys.exit(1)


def find_host_cc():
    """Any host gcc will do -- the bank is pure integer C99.  This box has no gcc on
    PATH but does carry the one MSYS2 ships, so look there too."""
    env = os.environ.get("HOSTCC")
    for c in ([env] if env else []) + [
            "gcc",
            r"C:\msys64\ucrt64\bin\gcc.exe",
            r"C:\msys64\mingw64\bin\gcc.exe",
            r"C:\msys64\usr\bin\gcc.exe"]:
        try:
            e = dict(os.environ)
            if os.path.sep in c:
                e["PATH"] = os.path.dirname(os.path.abspath(c)) + os.pathsep + e.get("PATH", "")
            if subprocess.run([c, "--version"], capture_output=True, env=e).returncode == 0:
                return c
        except OSError:
            continue
    fail("no host C compiler found; set HOSTCC to one (any gcc)")


if not os.path.isfile(REF):
    fail("%s is missing -- run tools/gen_avas_type_lb_ck_noise.py, which emits it from the "
         "same fit the header comes from" % os.path.relpath(REF, ROOT))

CC = find_host_cc()
print("host cc: %s" % CC)
ENV = dict(os.environ)
if os.path.sep in CC:
    ENV["PATH"] = os.path.dirname(os.path.abspath(CC)) + os.pathsep + ENV.get("PATH", "")

exe = os.path.join(HERE, "avas_noise_bank_dump.exe" if os.name == "nt"
                   else "avas_noise_bank_dump")
cmd = [CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-I", APP,
       os.path.join(HERE, "avas_noise_bank_dump.c"), "-o", exe]
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV)
if r.returncode != 0:
    fail("host compile failed:\n%s\n%s" % (r.stdout, r.stderr))

ref = [int(v) for v in io.open(REF, encoding="utf-8").read().split()]
r = subprocess.run([exe, str(len(ref))], capture_output=True, text=True, env=ENV)
if r.returncode != 0:
    fail("the host dump exited %d:\n%s" % (r.returncode, r.stderr))
got = [int(v) for v in r.stdout.split()]

if len(got) != len(ref):
    fail("the C produced %d samples, the model %d" % (len(got), len(ref)))
bad = [i for i in range(len(ref)) if got[i] != ref[i]]
if bad:
    i = bad[0]
    fail("%d of %d samples differ; first at %d: C %d vs model %d\n"
         "      (a first difference at sample 0 is a coefficient or seed mismatch; one\n"
         "       that appears later is arithmetic -- a shift, a sign, or a truncation)"
         % (len(bad), len(ref), i, got[i], ref[i]))

peak = max(abs(v) for v in ref)
print("noise bank: %d samples bit-identical to the model (peak %d in A_SCALE units, "
      "gusts off)" % (len(ref), peak))
print("PASS")
