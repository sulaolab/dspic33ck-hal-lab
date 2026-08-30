# -*- coding: utf-8 -*-
"""Dump the Type_LB L3 line list, and the cluster decomposition it implies.

L3 is defined in the analysis tree's a16_lines.py as `tone + noise(mod_db=1.5)`, where
`tone` is EVERY detected line -- 264 of them -- not the top 64 that
`tools/L8_params_type_lb.txt` holds.  So there is no file in that repo carrying
L3's tone parameters, and this script produces one.

It does NOT re-derive anything: it calls the same `flat_pitch_window` and
`pick_lines` that a16_lines.py stages 1-2 call, on the same cached mono/f0 data, so
the 264 lines here are the 264 lines that produced `out_lines_L3.wav`.

    python tools/gen_avas_type_lb_lines.py [out_path] [max_span_hz ...]

Output is `# FRQ_Hz  AMP  PHA_rad`, frequency ascending -- ascending because the
firmware's clusters must be contiguous runs of table entries.

THE ONE SCRIPT HERE THAT DOES NOT RUN STANDALONE.  It needs the AVAS analysis tree
(`AVAS_dev_claude`) beside this repo, with its type_lb cache populated
(`python a0_profile.py --source type_lb` then `a2`, or `run_all.py`).  That tree is
not published, so this runs only where it exists -- which costs nothing to build
with, because its output is committed: the generated headers under src/app/dsp are
what the firmware compiles.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
DEV = os.path.abspath(os.path.join(ROOT, "..", "AVAS_dev_claude"))

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "L3_params_type_lb.txt")
SPANS = [float(a) for a in sys.argv[2:]] or [100.0, 150.0, 200.0, 250.0]

if not os.path.isdir(DEV):
    sys.exit("AVAS_dev_claude not found at %s -- it is the source of the analysis, "
             "not vendored here." % DEV)

sys.path.insert(0, DEV)
os.environ.setdefault("AVAS_SOURCE", "type_lb")
os.chdir(DEV)                      # avaspath resolves relative to the dev folder

from avaspath import CACHE, PROF   # noqa: E402
import avaslib as L                # noqa: E402

mono = np.load(os.path.join(CACHE, "mono.npy"))
fs = int(np.load(os.path.join(CACHE, "fs.npy"))[0])
grid = np.load(os.path.join(CACHE, "f0_grid.npy"))
div = int(PROF.get("grid_div", 1))

# a16_lines.py stage 1: the flat-pitch window.  The line model is only meaningful
# where the pitch is constant -- a partial that wanders smears into a crowd of
# stationary lines, which is the failure a16_lines.py documents at its SEG_CAP.
t0, t1, f0seg = L.flat_pitch_window(grid[:, 0], grid[:, 1], tol=0.010)
if t1 <= t0:
    t0, t1 = PROF["t_start"], min(PROF["t_start"] + 1.0, PROF["t_end"])
    f0seg = PROF["f0_hz"]

x = mono[int(t0 * fs):int(t1 * fs)].copy()
dur = len(x) / fs

# a16_lines.py stage 2.  fmin follows the source: the Type_LB's strongest
# partial is at 54.9 Hz and a fixed 60 Hz floor discarded it outright.
fmin = max(15.0, 0.5 * (f0seg / div))
fmax = min(12000.0, 0.45 * fs)
lines = L.pick_lines(x, fs, dur, thr_db=6.0, keep_frac=0.995, fmin=fmin, fmax=fmax)

r = np.array([[f, a, p] for f, a, p in lines])
r = r[np.argsort(r[:, 0])]

# THE PEAK OF THE FULL SUM, over 60 s of running time and not over the analysed
# segment -- the AK generator's own convention, chunked the
# same way, because it is what the firmware's output level is normalised against
# (AVAS_TY_L1_PEAK_ABS on the Type_TY side, and its comment says why: quasi-periodic
# beating keeps pushing the peak up, so a synth normalised on the segment peak
# clips after the first two seconds).
#
# It is measured HERE rather than in the fixed-point model because it depends only
# on the line list -- the model would recompute half a minute of numpy on every
# host-check run to arrive at the same number.  The model reads it back out of the
# `peak_abs=` field below, which is why that field is not decoration.
pk_long, pk_seg = 0.0, 0.0
for s0 in range(0, 60 * fs, fs):
    tt = np.arange(s0, s0 + fs) / float(fs)
    yy = (r[:, 1:2] * np.cos(2.0 * np.pi * r[:, 0:1] * tt + r[:, 2:3])).sum(0)
    pk_long = max(pk_long, float(np.abs(yy).max()))
    if s0 == 0:
        pk_seg = pk_long
        rms = float(np.sqrt((yy ** 2).mean()))

# ---------------------------------------------------------------------------
# The NOISE half of L3, measured the way a16_lines.py stage 4 measures it.
#
# L3 is `tone + noise(mod_db=1.5)`, so a line list alone does not define it.  The
# levels come from the RESIDUAL (x - tone) with every modelled line's neighbourhood
# masked out and interpolated across -- not from a plain band rms, and that is not a
# refinement: a band edge landing on a strong cluster puts that cluster's
# reconstruction error into the band's "noise" level, which a16's own comment
# measured at +13 dB in one band with the tone itself correct.
#
# NG[b] comes out as an ABSOLUTE rms in the reference sound's own amplitude units, i.e. the
# same units as AMP above, which is what makes tone + noise a sum rather than a mix
# with a free parameter.
# ---------------------------------------------------------------------------
NBN = 18
SPACING = f0seg / div
NLO = max(20.0, 0.5 * SPACING)
NHI = min(16000.0, 0.45 * fs)
NE = np.geomspace(NLO, NHI, NBN + 1)

tt = np.arange(len(x)) / float(fs)
tone_seg = (r[:, 1:2] * np.cos(2.0 * np.pi * r[:, 0:1] * tt + r[:, 2:3])).sum(0)
resid = x - tone_seg
fwl, floorl = L.line_masked_floor(resid, fs, r[:, 0], 0.0, dur)
NG = np.zeros(NBN)
for b in range(NBN):
    mb = (fwl >= NE[b]) & (fwl < NE[b + 1])
    NG[b] = float(np.sqrt(np.sum(floorl[mb] ** 2))) if mb.any() else 0.0
NOISE_RMS = float(np.sqrt(np.sum(NG ** 2)))
TONE_RMS = float(np.sqrt(np.mean(tone_seg ** 2)))
RESID_PCT = 100.0 * float(np.mean(resid ** 2) / np.mean(x ** 2))

os.chdir(ROOT)
NOUT = os.path.join(HERE, "L3_noise_type_lb.txt")
with open(NOUT, "w", encoding="utf-8", newline="\n") as fh:
    fh.write("# Type_LB L3 noise part: sum_b NG[b] * 10^(g_b(t)/20) * noise_b(t)\n")
    fh.write("# GENERATED by tools/gen_avas_type_lb_lines.py -- DO NOT EDIT\n")
    fh.write("# a16_lines.py stage 4: level = line-masked floor of (x - tone), so a\n"
             "#   reconstruction error at a strong line does not become a noise level.\n")
    fh.write("# %d geometric bands, %.4f-%.1f Hz, ratio %.4f\n"
             % (NBN, NLO, NHI, (NHI / NLO) ** (1.0 / NBN)))
    fh.write("# gust_sd_db=1.5  gust_rate_hz=1.2  UNIFORM across bands -- that is what\n"
             "#   makes this L3 and not L8 (no per-band onset ramp, no measured depth).\n")
    fh.write("# NG is an ABSOLUTE rms in the same units as the line AMPs.\n")
    fh.write("# noise_rms=%.9e  tone_rms=%.9e  noise/tone=%.2f dB  residual=%.1f%% of energy\n"
             % (NOISE_RMS, TONE_RMS, 20.0 * np.log10(NOISE_RMS / TONE_RMS), RESID_PCT))
    fh.write("# lo_Hz  hi_Hz  NG_rms\n")
    for b in range(NBN):
        fh.write("%.6f %.6f %.9e\n" % (NE[b], NE[b + 1], NG[b]))

with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
    fh.write("# Type_LB L3 tone part: sum_j AMP[j]*cos(2*pi*FRQ[j]*t + PHA[j])\n")
    fh.write("# GENERATED by tools/gen_avas_type_lb_lines.py -- DO NOT EDIT\n")
    fh.write("# source=type_lb  fs=%d  n=%d  dur=%.6f s  t0=%.3f t1=%.3f\n"
             % (fs, len(x), dur, t0, t1))
    fh.write("# f0=%.4f Hz  spacing=%.4f Hz  search %.1f-%.0f Hz\n"
             % (f0seg, f0seg / div, fmin, fmax))
    fh.write("# %d oscillators (ALL detected -- L3 is not truncated; L4/L5/L8 take"
             " the top 64)\n" % len(r))
    fh.write("# peak_abs=%.6f  peak of the full sum over 60 s of running time"
             " (AK's convention; %.6f over the first second, rms %.6f).\n"
             "#   READ BY tools/avas_type_ty_fixed_model.py -- it is what the output"
             " level is normalised against.\n" % (pk_long, pk_seg, rms))
    fh.write("# FRQ_Hz  AMP  PHA_rad, frequency ascending\n")
    for f, a, p in r:
        fh.write("%.10f %.12e %.10f\n" % (f, a, p))

print("wrote %s : %d lines, %.2f-%.2f Hz, sum AMP %.4f"
      % (os.path.relpath(OUT, ROOT), len(r), r[0, 0], r[-1, 0], r[:, 1].sum()))

# The clustering the firmware will use, so the table's shape is visible here and
# not only inside the generator.  Greedy contiguous runs within max_span_hz; the
# carrier is the AMPLITUDE-WEIGHTED centroid, which puts the smallest baseband
# offset on the strongest lines (measured to matter, per gen_c_l1_table.py).
print("  cluster decomposition (carriers run at fs; the rest at fs/DEC):")
for span in SPANS:
    cl = []
    first = 0
    for i in range(1, len(r) + 1):
        if (i < len(r)) and ((r[i, 0] - r[first, 0]) <= span):
            continue
        seg = r[first:i]
        cl.append((first, i - first,
                   float((seg[:, 1] * seg[:, 0]).sum() / seg[:, 1].sum())))
        first = i
    maxoff = max(float(np.abs(r[f:f + c, 0] - fc).max()) for f, c, fc in cl)
    ampsum = max(float(r[f:f + c, 1].sum()) for f, c, fc in cl)
    print("    max span %4.0f Hz -> %2d clusters, sizes %s"
          % (span, len(cl), [c for _, c, _ in cl]))
    print("                        largest baseband offset %6.1f Hz,"
          " worst cluster sum AMP %.4f" % (maxoff, ampsum))
