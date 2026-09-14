# -*- coding: utf-8 -*-
"""The Type_LB L3 voice, in the fixed-point model -- and where MAX_SPAN_HZ is decided.

WHAT THIS IS NOT: a second model.  L3's tone part is the same equation the CK engine
already computes, so the arithmetic lives in
exactly one place -- tools/avas_type_ty_fixed_model.py -- and this file drives it with
the Type_LB's coefficient source instead of the Type_TY one.  Every integer below
is that module's; if the two could disagree, neither would say anything about the
firmware.

    python tools/avas_type_lb_fixed_model.py [seconds] [span ...]

WHAT IT DECIDES, and against what
---------------------------------
The design left MAX_SPAN_HZ open (section 9.4) to be settled "by the model's
interpolation error, in phase 2".  That is the RIGHT criterion but not the only
quantity the span moves, and three of the others turned out to matter more:

  1. INTERPOLATION ERROR.  A wider cluster puts a larger baseband offset on its
     lines, so the complex envelope Z_k(t) it has to reconstruct moves faster
     between rebuilds -- 136.5 Hz of offset at span 200 against 60.7 at span 100.
     Measured here directly (envelope error, dB below the envelope's own rms), not
     inferred from the audio, because that is the quantity the span controls.
  2. AMPLITUDE RESOLUTION.  A_SCALE = 32767 / max_k(cluster amp sum), so the WIDEST
     cluster sets the Q15 scale for every line in the table.  Fewer, fatter clusters
     cost every line amplitude bits: the weakest line is 77 counts at span 100 and
     43 at span 200.
  3. OUTPUT HEADROOM.  The same worst-cluster sum is the denominator of the output
     gain, and the numerator is the peak the whole 264-line sum reaches (0.6311).
     At span 100 the two are close enough that the gain stays below unity, as
     Type_TY's does; at 150 and above it exceeds unity and the engine needs the
     OUT_SHIFT + clamp path (see the model's comment on _OUT_GAIN_LIN).
  4. LOAD.  Each extra cluster is one more full-rate carrier: 11.88 us per block,
     MEASURED on the Type_TY voice (design section 5's calibration).

So the span is priced on all four here, and the load column is what makes 100 a
real candidate rather than obviously-too-expensive.

The reference is the OFFLINE L3 TONE, i.e. the float64 sum of the same 264 lines --
not `out_lines_L3.wav`, which also carries the noise bank phase 4 has not built yet.
Comparing against the wav would fold two unfinished things into one number.
"""
import importlib
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
PARAMS = os.path.join(HERE, "L3_params_type_lb.txt")

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
SPANS = [float(a) for a in sys.argv[2:]] or [100.0, 150.0, 200.0, 250.0]

# The knobs the SHIPPED image is built with, read out of the generated Type_TY header
# rather than restated -- they are engine-wide (the sine table and the envelope
# scheme are shared by both voices), so the Type_LB inherits them and this study
# has to evaluate the combination that will actually run.  Same argument, and the
# same mechanism, as tools/host_check/run_host_check.py.
import re                                                        # noqa: E402

HDR = open(os.path.join(ROOT, "src", "app", "dsp", "avas_type_ty_ck_tables.h"),
           encoding="utf-8").read()


def hdr_int(name):
    return int(re.search(r"#define\s+%s\s+\((-?\d+)" % name, HDR).group(1))


SHIPPED = {
    "DEC": str(hdr_int("AVAS_TYPE_TY_CK_DEC")),
    "TABBITS": str(hdr_int("AVAS_TYPE_TY_CK_TABBITS")),
    "FRACBITS": str(hdr_int("AVAS_TYPE_TY_CK_FRACBITS")),
    "TERMSHIFT": str(hdr_int("AVAS_TYPE_TY_CK_TERMSHIFT")),
    "CARRIER": "table",
    # The firmware's own conversion, not the model's exact bound: what ships is a
    # 12-iteration integer CORDIC, and measuring the scheme with hypot/arctan2 would
    # report an accuracy the engine does not have.
    "ENVINTERP": "polar" if hdr_int("AVAS_TYPE_TY_CK_ENVINTERP") == 1 else "rect",
    "POLAR_CONV": "cordic",
    "CORDIC_N": str(hdr_int("AVAS_TYPE_TY_CK_CORDIC_N")),
    "ENVFRAC": str(hdr_int("AVAS_TYPE_TY_CK_ENVFRAC")),
    "ENVROUND": str(hdr_int("AVAS_TYPE_TY_CK_ENVROUND")),
    # ACC_MODE 2 on the target; mode 1 is its bit-exact host definition, so acc40 is
    # what the shipped arithmetic is (host_check proves model == mode 1, hardware
    # proves mode 1 == mode 2).
    "ACC": "acc40",
}

# MEASURED per-block costs, from the Type_TY voice on this board (design section 5,
# calibrated on section 24 of the Type_TY doc): one full-rate carrier, one cluster's
# CORDIC + rebuild, one line's baseband oscillator, and the gate + call overhead.
US_PER_CARRIER, US_PER_CLUSTER, US_PER_LINE, US_FIXED = 11.88, 1.92, 0.573, 27.0
BLOCK_US = 667.0

sys.path.insert(0, HERE)
os.environ["MAX_SPAN_HZ"] = str(SPANS[0])
sys.argv = [sys.argv[0], PARAMS, str(SECONDS)]
import avas_type_ty_fixed_model as M                                # noqa: E402

N = int(round(SECONDS * 48000))


def configure(span, phbits, source=PARAMS, **over):
    """Reload the shared model with one coefficient source and one knob set.

    importlib.reload() is how the Type_TY generator switches phase widths too: the
    module's tables are built at import from the environment, so a reload IS the
    model run again with different parameters -- never a second implementation.

    `phbits` is one width for both accumulators, or (carrier, baseband) -- they are
    independent knobs in the generated header, and for this voice that turns out to
    matter (section 4 below).
    """
    car, bb = phbits if isinstance(phbits, tuple) else (phbits, phbits)
    env = dict(SHIPPED)
    env.update({k: str(v) for k, v in over.items()})
    env["MAX_SPAN_HZ"] = str(span)
    env["PHBITS_CAR"] = str(car)
    env["PHBITS_BB"] = str(bb)
    os.environ.update(env)
    M.TRACE_ENV = True
    sys.argv = [sys.argv[0], source, str(SECONDS)]
    importlib.reload(M)
    M.TRACE_ENV = True
    return M


def measure(span, phbits, source=PARAMS, **over):
    m = configure(span, phbits, source=source, **over)
    phcar = phbits[0] if isinstance(phbits, tuple) else phbits
    y = m.run(N)
    idl = m.ideal(N)
    d = y - idl
    db_below = 20.0 * np.log10(idl.std() / max(d.std(), 1e-30))

    # The line-free floor, on the same band definition verify_c_l1_model.py uses, so
    # the figure is comparable with every other number in these two documents.
    bl = m.band_levels(idl, y)
    emp = [e for e in bl if e[2] <= -80.0]
    occ = [e for e in bl if e[2] > -80.0]
    floor = max((e[3] for e in emp), default=float("nan"))
    band_err = max((abs(e[3] - e[2]) for e in occ), default=float("nan"))

    # THE SPAN'S OWN QUANTITY: how well the interpolated envelope tracks the true
    # Z_k(t) between rebuilds, as a ratio so it is comparable across spans (A_SCALE
    # differs between them, so an absolute error would not be).
    zi, zq = m.true_envelope(N)
    ei, eq = m.LAST_ENV_I, m.LAST_ENV_Q
    num = float(np.sqrt(np.mean((ei - zi) ** 2 + (eq - zq) ** 2)))
    den = float(np.sqrt(np.mean(zi ** 2 + zq ** 2)))
    env_db = 20.0 * np.log10(den / max(num, 1e-30))

    fc = m.FC
    real = m.CAR_STEP / 2.0 ** phcar * m.FS
    cents = float(np.max(1200.0 * np.abs(np.log2(real / fc))))
    load = (US_FIXED + US_PER_CARRIER * m.K + US_PER_CLUSTER * m.K
            + US_PER_LINE * m.NLINE)
    return {
        "span": span, "phbits": phbits, "K": m.K, "lines": m.NLINE, "src": source,
        "ampsum": float(m.CLUS_AMPSUM.max()), "a_scale": m.A_SCALE,
        "amp_min": int(m.AMP_Q15.min()), "amp_max": int(m.AMP_Q15.max()),
        "max_off": float(np.abs(m.FRQ - np.repeat(m.FC, np.diff(
            np.append(m.FIRST, m.NLINE)))).max()),
        "gain": m.OUT_GAIN_Q15, "shift": m.OUT_SHIFT, "gain_lin": m._OUT_GAIN_LIN,
        "clip": m.CLIP_COUNT, "db": db_below, "env_db": env_db, "floor": floor,
        "band_err": band_err, "cents": cents, "peak": float(np.abs(y).max()),
        "load": load, "pct": 100.0 * load / BLOCK_US,
    }


def head(row):
    print("  span %4.0f Hz: %d clusters / %d lines, worst cluster sum %.4f, "
          "max |f-fc| %5.1f Hz" % (row["span"], row["K"], row["lines"],
                                   row["ampsum"], row["max_off"]))


def show(row, label):
    print("    %-22s env %5.2f dB | audio %5.2f dB below | floor %6.1f dBFS | "
          "band err %.2f dB | peak %.4f" % (label, row["env_db"], row["db"],
                                            row["floor"], row["band_err"], row["peak"]))


print("Type_LB L3, fixed-point, %s" % os.path.relpath(PARAMS, ROOT))
print("  reference = the offline L3 TONE (float64 sum of the same lines); the noise "
      "bank is phase 4")
print("  shipped knobs, read out of src/app/dsp/avas_type_ty_ck_tables.h: "
      "ENVINTERP=%s ENVFRAC=%s ENVROUND=%s ACC=%s TABBITS=%s DEC=%s CORDIC_N=%s"
      % (SHIPPED["ENVINTERP"], SHIPPED["ENVFRAC"], SHIPPED["ENVROUND"],
         SHIPPED["ACC"], SHIPPED["TABBITS"], SHIPPED["DEC"], SHIPPED["CORDIC_N"]))
print("  %.3f s per configuration (%d samples)\n" % (SECONDS, N))

print("1. MAX_SPAN_HZ, at the shipped 16-bit phase")
rows = {}
for span in SPANS:
    r16 = measure(span, 16)
    rows[span] = r16
    head(r16)
    show(r16, "16/16 phase")
    print("      A_SCALE %8.1f -> amp_q15 %4d..%d | out gain %.4f = %d >> %d "
          "(shift %d)%s | carrier err <= %.2f cents | load %.0f us = %.1f %% "
          "of the block"
          % (r16["a_scale"], r16["amp_min"], r16["amp_max"], r16["gain_lin"],
             r16["gain"], 15 - r16["shift"], r16["shift"],
             "  CLAMPED %d samples" % r16["clip"] if r16["shift"] else "",
             r16["cents"], r16["load"], r16["pct"]))

print("\n2. Phase width, per span.  THIS IS WHERE THE SPAN IS ACTUALLY DECIDED: at")
print("   32/32 the frequency error is ~0, so what the metric measures is the")
print("   INTERPOLATION error and nothing else.  At 16 bits it measures the phase")
print("   resolution instead -- see section 4 for the calibration that proves that")
print("   reading, on the shipped voice that the ear has already passed.")
rows32 = {}
for span in SPANS:
    r32 = measure(span, 32)
    rows32[span] = r32
    show(r32, "span %4.0f, 32/32 phase" % span)
    print("      vs 16/16: audio %+.2f dB, envelope %+.2f dB, carrier err <= %.2f "
          "cents (16-bit: %.2f)"
          % (r32["db"] - rows[span]["db"], r32["env_db"] - rows[span]["env_db"],
             r32["cents"], rows[span]["cents"]))

print("\n3. TABBITS 9 -> 8, which is phase 3's ROM lever (-1 280 B) and is free of")
print("   the Type_TY voice's objection in a Type_TY-excluded image (design section 5)")
for span in SPANS:
    r8 = measure(span, 16, TABBITS=8)
    show(r8, "span %4.0f, TABBITS=8" % span)
    print("      vs TABBITS=9: audio %+.2f dB, floor %+.1f dB"
          % (r8["db"] - rows[span]["db"], r8["floor"] - rows[span]["floor"]))

print("\n4. What a 16-bit phase does to the METRIC, calibrated on the voice that has")
print("   already been accepted BY EAR at that width (Type_TY L1, its own AK table,")
print("   its own 11 clusters -- the span argument does not apply to it):")
AK = os.path.abspath(os.path.join(ROOT, "tools", "host_check", "ref",
                                  "avas_synth_type_ty_tables.h"))
if os.path.isfile(AK):
    for w, tag in ((32, "32/32"), (16, "16/16 -- SHIPPED")):
        t = measure(0.0, w, source=AK)
        show(t, "Type_TY L1, %s" % tag)
        print("      carrier err <= %.2f cents" % t["cents"])
    print("   So the shipped Type_TY voice scores single digits on this metric and was")
    print("   nonetheless called perfect through this board's own speaker.  The number")
    print("   is a DEFECT watchdog, not the acceptance gate (Type_TY doc section 16).")
else:
    print("   SKIPPED: reference fixture missing at %s" % AK)

print("\n   And the graded middle for this voice, which the two widths being")
print("   INDEPENDENT knobs makes available -- wide carriers, narrow baseband:")
for span in SPANS:
    rm = measure(span, (32, 16))
    show(rm, "span %4.0f, car 32 / bb 16" % span)
    print("      +%d B flash and +%d B RAM over 16/16 (car_step and the carrier phase"
          " state), ~%.0f us/block for the wider add; removes the cents error entirely"
          % (2 * rm["K"], 2 * rm["K"], 0.0177 * rm["K"] * 32))

print("\nSummary, one line per span, shipped knobs:")
print("  span  K  A_SCALE  amp_min  gain(shift)  env dB(32)  audio dB(32)  "
      "audio dB(16)  floor(32)  load %")
for span in SPANS:
    r, r32 = rows[span], rows32[span]
    print("  %4.0f %2d %8.1f %8d  %.4f(%d)%s %9.2f %13.2f %13.2f %10.1f %7.1f"
          % (span, r["K"], r["a_scale"], r["amp_min"], r["gain_lin"], r["shift"],
             "*" if r["shift"] else " ", r32["env_db"], r32["db"], r["db"],
             r32["floor"], r["pct"]))
print("  (* needs the OUT_SHIFT clamp: the output gain exceeds unity)")
print("  The 32-bit columns are the interpolation error -- i.e. the span's own")
print("  quantity.  The 16-bit column is what the shipped image measures, and its")
print("  calibration is section 4.")
