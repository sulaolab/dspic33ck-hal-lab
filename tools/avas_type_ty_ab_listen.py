# -*- coding: utf-8 -*-
"""A BLIND rect-vs-polar listening test, and a worst-case excerpt to go with it.

Section 16 closed the polar question on a model metric and then had the metric
overruled by a 4 s listen.  That listen has two holes, and this closes both without
touching hardware:

  (a) it was not blind, and it was polar alone rather than against rect.  So the
      files here are named X and Y, the assignment is drawn from the OS entropy
      source, and it is written to ANSWER.txt -- read after judging, not before.
      This script never prints which is which.
  (b) 4 s is short for the failure mode.  polar's envelope error peaks at 11x
      rect's, but the deep-null samples that cause it are 0.187 % of a run, so a
      short excerpt can miss every one of them.  Fixed twice over: 60 s by default,
      AND a separate "worst" file that concatenates the moments where the two
      outputs differ most, so the rare event is put in front of the ear on purpose
      instead of being waited for.

The selector for those moments is `|y_polar - y_rect|` itself, smoothed -- not a
null-detector.  It needs no envelope algebra, and it directly answers the question
being asked: at the instants where these two engines disagree most, can the
disagreement be heard?  Each excerpt is faded 5 ms at both ends, because an edge
click would be an artefact of the test rather than of polar.

Usage (the two renders are independent, so run them at the same time):
    python tools/avas_type_ty_ab_listen.py render rect  60
    python tools/avas_type_ty_ab_listen.py render polar 60
    python tools/avas_type_ty_ab_listen.py assemble     60
"""
import os
import random
import struct
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
AK_TABLE = os.path.join(ROOT, "tools", "host_check", "ref",
                        "avas_synth_type_ty_tables.h")
FS = 48000
MODE = sys.argv[1]
# render takes a variant before the duration, assemble does not, so the positions
# differ and are resolved once here rather than re-parsed per branch (which is how
# the first version managed to fail on float("rect") before reaching either).
if MODE == "render":
    VARIANT = sys.argv[2]
    ARG_S, ARG_DIR = 3, 4
else:
    VARIANT = None
    ARG_S, ARG_DIR = 2, 3
SECONDS = float(sys.argv[ARG_S]) if len(sys.argv) > ARG_S else 60.0
OUTDIR = os.path.abspath(sys.argv[ARG_DIR] if len(sys.argv) > ARG_DIR
                         else os.path.join(ROOT, "..", "_listen_avas"))
N = int(round(SECONDS * FS))


def raw_path(variant):
    return os.path.join(OUTDIR, "_raw_%s_%ds.npy" % (variant, int(SECONDS)))


def write_wav(path, y):
    d = np.clip(np.round(y * 32767.0), -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(struct.pack("<%dh" % len(d), *d.tolist()))
    print("  %-26s %6.2f s  peak %.3f" % (os.path.basename(path), len(d) / float(FS),
                                          float(np.abs(y).max())))


os.makedirs(OUTDIR, exist_ok=True)

# ---------------------------------------------------------------------------
if MODE == "render":
    variant = VARIANT
    assert variant in ("rect", "polar"), "render takes rect or polar"
    os.environ.setdefault("ACC", "acc40")       # the arithmetic the board ships
    sys.argv = [sys.argv[0], AK_TABLE, str(SECONDS)]
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import avas_type_ty_fixed_model as M          # noqa: E402
    M.ENVI = variant
    M.TRACE_ENV = False
    print("rendering %s, %.0f s (%d samples)" % (variant, SECONDS, N))
    y = M.run(N)
    np.save(raw_path(variant), y.astype(np.float64))
    print("  -> %s   peak %.3f  rms %.4f"
          % (os.path.basename(raw_path(variant)), np.abs(y).max(), y.std()))
    sys.exit(0)

if MODE != "assemble":
    print("usage: render <rect|polar> <seconds> | assemble <seconds>")
    sys.exit(2)

# ---------------------------------------------------------------------------
# assemble: blind the two renders, cut the worst-disagreement excerpts, and put the
# revealing files (which are named, and whose loudness gives the answer away) in a
# subfolder so they cannot be stumbled into before the A/B is done.
# ---------------------------------------------------------------------------
y = {v: np.load(raw_path(v)) for v in ("rect", "polar")}
n = min(len(y["rect"]), len(y["polar"]))
y = {v: a[:n] for v, a in y.items()}
print("assembling from %d samples (%.1f s)" % (n, n / float(FS)))

# The worst-disagreement excerpts.
d = np.abs(y["polar"] - y["rect"])
w = 64
sm = np.convolve(d, np.ones(w) / w, mode="same")
HALF = int(0.20 * FS)                  # +-200 ms around each moment
GUARD = int(0.30 * FS)                 # events at least this far apart
NEV = 20
cand = sm.copy()
picks = []
for _ in range(NEV):
    i = int(np.argmax(cand))
    if cand[i] <= 0:
        break
    picks.append(i)
    cand[max(0, i - GUARD):i + GUARD] = 0.0
picks.sort()
print("  %d worst-disagreement moments, smoothed |polar-rect| peak %.5f"
      "  (whole-run rms %.5f)" % (len(picks), sm.max(), d.std()))

fade = np.ones(2 * HALF)
nf = int(0.005 * FS)
fade[:nf] = np.linspace(0.0, 1.0, nf)
fade[-nf:] = np.linspace(1.0, 0.0, nf)


def excerpts(a):
    out = []
    for i in picks:
        lo = max(0, min(i - HALF, n - 2 * HALF))
        seg = a[lo:lo + 2 * HALF].copy()
        out.append(seg * fade)
    return np.concatenate(out) if out else np.zeros(0)


# Blind assignment, from the OS entropy source, and deliberately not printed.
label = {}
first = random.SystemRandom().choice(("rect", "polar"))
label[first] = "X"
label["polar" if first == "rect" else "rect"] = "Y"

for v in ("rect", "polar"):
    write_wav(os.path.join(OUTDIR, "ab_%s_full.wav" % label[v]), y[v])
    write_wav(os.path.join(OUTDIR, "ab_%s_worst.wav" % label[v]), excerpts(y[v]))

with open(os.path.join(OUTDIR, "ANSWER.txt"), "w", encoding="utf-8") as f:
    f.write("blind A/B assignment -- read AFTER judging\n")
    f.write("=========================================\n")
    for v in ("rect", "polar"):
        f.write("  %s  =  %s%s\n" % (label[v], v,
                                     "   (what the firmware ships)" if v == "rect"
                                     else "   (the section 16 candidate, ~38 us "
                                     "cheaper, no firmware written yet)"))
    f.write("\nab_*_worst.wav are the %d moments where the two disagree most,\n"
            "+-200 ms each, 5 ms fades, concatenated in time order.\n" % len(picks))

# The revealing files, out of the way.
sub = os.path.join(OUTDIR, "after_ab")
os.makedirs(sub, exist_ok=True)
print("  (revealing files -> %s/)" % os.path.basename(sub))

# The model is imported HERE and once: it reads its table path out of sys.argv at
# import time, so the argv it needs has to be in place first (importing it inside the
# chunk loop below, with argv still holding "assemble", is how this failed once).
sys.argv = [sys.argv[0], AK_TABLE, str(SECONDS)]
sys.path.insert(0, os.path.join(ROOT, "tools"))
import avas_type_ty_fixed_model as M              # noqa: E402

# ideal() would allocate 185 x n at once, which is 4 GB at 60 s, so chunk it.
idl = np.empty(n)
CH = 200000
for lo in range(0, n, CH):
    hi = min(n, lo + CH)
    t = np.arange(lo, hi) / float(FS)
    idl[lo:hi] = (M.AMP[:, None]
                  * np.cos(2 * np.pi * M.FRQ[:, None] * t + M.PHA[:, None])).sum(0)
idl *= 0.9 / M.PEAK_ABS

e = {v: y[v] - idl for v in y}
g = 0.9 / max(np.abs(e["rect"]).max(), np.abs(e["polar"]).max())
for v in ("rect", "polar"):
    write_wav(os.path.join(sub, "err_%s.wav" % v), e[v] * g)
print("  error files share a gain of %.1fx (+%.1f dB); err rms rect %.6f polar"
      " %.6f (%.2fx, %+.2f dB)"
      % (g, 20 * np.log10(g), e["rect"].std(), e["polar"].std(),
         e["polar"].std() / e["rect"].std(),
         20 * np.log10(e["polar"].std() / e["rect"].std())))

with open(os.path.join(OUTDIR, "README.txt"), "w", encoding="utf-8") as f:
    f.write("""rect vs polar -- FULL-LENGTH LISTEN, not a difference hunt
=========================================================
THE CRITERION: listen to the whole thing.  If nothing feels off -- and better
still, if it sounds cooler or more pleasant than the real one -- that is the win.
NOT "which is closer to the offline reference".  On the Type_LB engine L1-L8
were offered, L8 had the smallest residual and L3 was chosen, so residual size is
already settled as the wrong axis.

USE THESE:
  ab_X_full.wav      %.0f s, uninterrupted
  ab_Y_full.wav      %.0f s, uninterrupted
  ANSWER.txt         which is which -- open after listening

The two are the same engine.  The only difference is whether the complex envelope
is interpolated in (I,Q) -- what the firmware ships -- or in (A,phi), which is
~38 us/block cheaper and has no firmware written for it yet.  They are unlabelled
so the words "polar" and "rejected" do not prime the ear, not because this is a
detection test.

DO NOT USE THESE TO DECIDE (kept only as diagnostics):
  ab_X_worst.wav / ab_Y_worst.wav
      The %d moments where the two outputs disagree MOST, +-200 ms each.  Answers
      "where is the difference", which does not decide anything.
  after_ab/err_rect.wav / err_polar.wav
      The residual against the offline float reference, both at one shared gain,
      so polar reads ~1.7x louder.  This file embodies exactly the assumption the
      L3-over-L8 choice rejected.  It also gives ANSWER.txt away.
""" % (n / float(FS), n / float(FS), len(picks)))
print("  wrote ANSWER.txt and README.txt (assignment not printed here)")
