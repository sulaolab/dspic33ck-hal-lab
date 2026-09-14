# -*- coding: utf-8 -*-
"""Section 19 item 4, tested rather than inherited: rounding at the SHIPPED width.

Section 19 ended with an order of work whose item 4 was the only one conditional on
nothing:

    ENVROUND on its own, at the shipped 32-bit width -- 22 instructions per block
    for a lower offset and a lower error rms than the engine has today.

That sentence was an INFERENCE, not a measurement.  What section 19 measured was
`ENVFRAC=0 + round` (offset 0.56, err rms 10.21) against `ENVFRAC=16 + floor` (1.97,
11.13) and concluded that the add was doing the work.  But those two rows differ in
BOTH knobs, and the study never ran (16, round) at all -- VARIANTS in
avas_type_ty_env16_study.py stops at (0, 1).  The model's own comment on the parameter
says why that omission mattered:

    Untouched at ENVFRAC=16, where the fraction bits already make the floor's
    residue 2**-16 of an LSB.

So this study asks the question the document skipped, and separates the two FLOORS
that the word "rounding" was covering:

  * the SLOPE floor -- `(target - now) >> DECSHIFT`, which ENVROUND fixes.  Its LSB
    is 2**-ENVFRAC of a Q15 count, so at the shipped width it is 1/65536 of an LSB.
  * the READ-OUT floor -- `(int16_t)(ei >> 16)` in the carrier loop, which nothing
    in this document has ever touched.  Its LSB is a whole Q15 count, it applies on
    every sample to both components, and C's `>>` biases it DOWNWARDS every time.

Only the second one can explain a 1.97 LSB coherent offset at the shipped width, and
a coherent offset is the expensive kind: constant on the envelope means a spurious
LINE at the cluster frequency once the carrier multiplies it.

Both of those are cheap to fix, which is why they were worth measuring: the target
floor is `sac.r` instead of `sac` in the shipped ACC_MODE 2 -- the same instruction --
and the read-out floor needs no instruction in the loop at all, because floor(x + 1/2)
is round(x), so the half-LSB can be carried permanently in the accumulator's fraction
bits (added once at reset and once per rebuild to the target, where it cancels out of
the slope difference exactly).  Those are the ENVOUTROUND and ENVTGTROUND columns.

What the measurement then says -- section 20 -- is that removing both of them is worth
+0.05 dB, so none of it was built.  This file is kept as the harness: it changes ONE
floor at a time, which is what section 19 did not do.

TERMINOLOGY, since the default has moved underneath this file: "the shipped width"
throughout the text above means ENVFRAC=16, which is what shipped when the study was
written.  Section 25 made ENVFRAC=0 (V3) the default, and the recipe above does NOT
carry across that flip -- at ENVFRAC=0 the read-out shift is `>> 0`, so that floor does
not exist, and section 20 measured that adding the target rounding on top of V3's
already-over-correcting slope rounding makes V3 WORSE (offset 0.56 -> 0.76).  The
variants below are unchanged and still say so; only the word "shipped" moved.

Run from the repo root:
    python tools/avas_type_ty_envround_study.py [seconds]
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
AK_TABLE = os.path.join(ROOT, "tools", "host_check", "ref",
                        "avas_synth_type_ty_tables.h")
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 2.0

# Same arithmetic and same phase width as the V3 study, for the same reason: the
# dB-below metric only measures arithmetic at 32/32, and the envelope's floors change
# no frequency, so this is the width at which the question is answerable at all.
os.environ.setdefault("ACC", "acc40")
sys.argv = [sys.argv[0], AK_TABLE, str(SECONDS)]
sys.path.insert(0, os.path.join(ROOT, "tools"))
import avas_type_ty_fixed_model as M      # noqa: E402

N, FS, K, DEC = M.N, M.FS, M.K, M.DEC

# (ENVFRAC, ENVROUND, ENVOUTROUND, ENVTGTROUND).  Row 1 is the wide state -- what the
# board ran through section 23, and what every figure in sections 14-19 was taken with;
# the last row is what ships since section 25.  Rows 2-4 take the three
# floors ONE AT A TIME, which is the whole method here: section 19 changed two at once
# and attributed the result to the wrong one.  Row 5 is the pair that is free in the
# shipped ACC_MODE 2, row 6 adds the one that is provably inert.  The last three carry
# V3 along so section 19's table stays comparable -- at ENVFRAC=0 there are no
# fraction bits, so the read-out shift is a no-op and ENVOUTROUND cannot apply.
VARIANTS = ((16, 0, 0, 0), (16, 1, 0, 0), (16, 0, 1, 0), (16, 0, 0, 1),
            (16, 0, 1, 1), (16, 1, 1, 1),
            (0, 0, 0, 0), (0, 1, 0, 0), (0, 1, 0, 1))

IDX = np.arange(0, N, DEC)


def vlabel(w, r, o, t):
    return "%d%s%s%s" % (w, "+sl" if r else "", "+rd" if o else "",
                         "+tg" if t else "")


def db(x):
    return 20.0 * np.log10(max(float(x), 1e-30))


idl = M.ideal(N)
zi, zq = M.true_envelope(N)
Zt = zi + 1j * zq
rms_k = np.sqrt((np.abs(Zt) ** 2).mean(0))


def measure(envfrac, envround, envoutround, envtgtround, envi):
    M.ENVFRAC = envfrac
    M.ENVROUND = envround
    M.ENVOUTROUND = envoutround
    M.ENVTGTROUND = envtgtround
    M.ENVI = envi
    M.TRACE_ENV = True
    M.ACC_PEAK = 0
    y = M.run(N)
    Ze = M.LAST_ENV_I + 1j * M.LAST_ENV_Q
    d = y - idl
    return dict(y=y, err=np.abs(Ze - Zt),
                # The COHERENT part per cluster: constant on the envelope, hence a
                # line on the output.  This is the figure section 19's table calls
                # "max coherent offset".
                off=np.abs((Ze - Zt).mean(0)),
                # Signed and on the I component alone, because a read-out floor is a
                # one-sided bias and |mean| hides its sign.  Predicted -0.5 LSB.
                bias=(M.LAST_ENV_I - Zt.real).mean(0),
                lvl=np.sqrt((np.abs(Ze) ** 2).mean(0)),
                metric=db(idl.std()) - db(d.std()))


def cols(fmt, vals):
    return " ".join(fmt % v for v in vals)


print("ROUNDING AT THE WIDE (ENVFRAC=16) WIDTH -- %.2f s @ %.0f Hz, DEC=%d, ACC=%s, "
      "phase %d/%d"
      % (SECONDS, FS, DEC, M.ACC, M.PHBITS_CAR, M.PHBITS_BB))
print("labels: 16/0 = ENVFRAC, bare = all three floors floored;  +sl = slope"
      " rounded (ENVROUND),\n        +rd = read-out rounded (ENVOUTROUND),"
      " +tg = target rounded (ENVTGTROUND, i.e. sac.r)")

results = {}
for envi in ("rect", "polar"):
    print("\n" + "=" * 78)
    print("ENVINTERP=%s" % envi)
    print("=" * 78)
    rows = [(v, measure(v[0], v[1], v[2], v[3], envi)) for v in VARIANTS]
    results[envi] = dict(rows)

    print("\n1. THE HEADLINE FIGURES  (the three section 19 tabulated)")
    print("   %-9s %9s %9s %9s %9s | %9s"
          % ("variant", "err rms", "err max", "off max", "I bias", "dB below"))
    for v, m in rows:
        print("   %-9s %9.2f %9.1f %9.2f %9.3f | %9.1f"
              % (vlabel(*v), m["err"].std(), m["err"].max(), m["off"].max(),
                 m["bias"].max() if envi == "polar" else m["bias"].mean(),
                 m["metric"]))

    print("\n2. COHERENT OFFSET PER CLUSTER  (Q15 LSB; the part that becomes a line)")
    print("   %-9s %s" % ("variant", cols("%6s", ["k%d" % k for k in range(K)])))
    for v, m in rows:
        print("   %-9s %s" % (vlabel(*v), cols("%6.2f", [m["off"][k]
                                                        for k in range(K)])))

    print("\n3. LEVEL vs the exact Z  (dB; nothing here should move)")
    print("   %-9s %s" % ("variant", cols("%6s", ["k%d" % k for k in range(K)])))
    for v, m in rows:
        print("   %-9s %s" % (vlabel(*v), cols("%+6.2f", [db(m["lvl"][k]) - db(rms_k[k])
                                                         for k in range(K)])))

    print("\n4. IN THE DOCUMENT'S OWN CURRENCY")
    for v, m in rows:
        M.report("%-9s %s" % (vlabel(*v), envi), m["y"], idl)

# ---------------------------------------------------------------------------
# The verdict, computed.  Two claims are on trial and they are separate: that item 4
# as written does something (it should not), and that the read-out floor is the
# mechanism (it should be).
# ---------------------------------------------------------------------------
print("\n" + "=" * 78)
print("VERDICT (computed)")
for envi in ("rect", "polar"):
    r = results[envi]
    base = r[(16, 0, 0, 0)]
    print("\n  %s:" % envi)
    for label, key in (("ships (all floored)", (16, 0, 0, 0)),
                       ("+ slope   (item 4) ", (16, 1, 0, 0)),
                       ("+ read-out         ", (16, 0, 1, 0)),
                       ("+ target           ", (16, 0, 0, 1)),
                       ("+ read-out+target  ", (16, 0, 1, 1)),
                       ("+ all three        ", (16, 1, 1, 1))):
        m = r[key]
        print("    %s off %6.2f  err rms %6.2f  I bias %+6.3f  metric %5.1f dB"
              " (%+.2f)"
              % (label, m["off"].max(), m["err"].std(), m["bias"].mean(),
                 m["metric"], m["metric"] - base["metric"]))
    print("    item 4 as written (slope only) changes the OUTPUT SAMPLES: %s"
          % ("NO -- bit-identical to what ships"
             if np.array_equal(base["y"], r[(16, 1, 0, 0)]["y"]) else "yes"))
    v3 = r[(0, 1, 0, 0)]
    print("    for the record, V3 + slope round is still worth %+.1f dB against V3"
          " floored (%.1f -> %.1f)"
          % (v3["metric"] - r[(0, 0, 0, 0)]["metric"],
             r[(0, 0, 0, 0)]["metric"], v3["metric"]))
