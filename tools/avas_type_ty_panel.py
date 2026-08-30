# -*- coding: utf-8 -*-
"""Render every SHELVED lever to 60 s of audio, unlabelled, for one listening pass.

Why this exists, and why it is not just a bigger A/B
----------------------------------------------------
Section 16 established two things at once: the criterion is a full-length listen
(not residual against the reference), and a 5.1 dB loss on the 48 dB metric was
inaudible over 60 s.  That second fact re-opens levers that were never rejected on
cost -- section 12's V1/V2 in particular, which were shelved because a frequency
error is exactly what the headline metric CANNOT judge.  They were waiting for an
instrument, and the instrument is a WAV file.

So: render each candidate, hide which is which, and listen to each one THROUGH.
The question per file is "does anything feel off, and does anything feel better",
not "can I hear a difference from the shipped one".

`shipped` is in the panel deliberately and unlabelled.  Without it in the set there
is no way to tell "this variant feels fine" from "my ears are agreeable today", and
it costs one more render.

Each variant needs its parameters set BEFORE the model is imported (it computes the
step tables at import time from PHBITS_*), which is why each is a separate process
rather than a loop.

    python tools/avas_type_ty_panel.py render <name> [seconds]
    python tools/avas_type_ty_panel.py assemble [seconds]
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

# name -> (env overrides, ENVI, one-line description for ANSWER.txt)
VARIANTS = {
    "shipped":  ({}, "rect",
                 "what the firmware ships: 32-bit phase, (I,Q) interpolation"),
    "v2":       ({"PHBITS_CAR": "16"}, "rect",
                 "V2: 16-bit CARRIER phase -- the 42 % loop; cluster centres move "
                 "by up to 0.73 Hz"),
    "v1":       ({"PHBITS_BB": "16"}, "rect",
                 "V1: 16-bit BASEBAND phase -- the 22 % loop; each line moves by up "
                 "to 0.023 Hz, which is what the lines beat against each other"),
    "v12":      ({"PHBITS_CAR": "16", "PHBITS_BB": "16"}, "rect",
                 "V1+V2: both phase accumulators 16-bit"),
    "v12polar": ({"PHBITS_CAR": "16", "PHBITS_BB": "16"}, "polar",
                 "V1+V2+polar: the cheapest combination this engine has"),
    "rotq20":   ({"CARRIER": "rotator", "ROTBITS": "20"}, "rect",
                 "rotator at Q20 -- no sine table at all; rejected in section 8 at "
                 "'1 dB down', which is inside section 16's calibration"),
}

MODE = sys.argv[1]
if MODE == "render":
    NAME = sys.argv[2]
    SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
else:
    NAME = None
    SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
OUTDIR = os.path.abspath(os.path.join(ROOT, "..", "_listen_avas_panel"))
N = int(round(SECONDS * FS))
os.makedirs(OUTDIR, exist_ok=True)


def raw_path(name):
    return os.path.join(OUTDIR, "_raw_%s_%ds.npy" % (name, int(SECONDS)))


def write_wav(path, y):
    d = np.clip(np.round(y * 32767.0), -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(struct.pack("<%dh" % len(d), *d.tolist()))


# ---------------------------------------------------------------------------
if MODE == "render":
    env, envi, _ = VARIANTS[NAME]
    os.environ.setdefault("ACC", "acc40")       # the arithmetic the board ships
    for k, v in env.items():
        os.environ[k] = v
    sys.argv = [sys.argv[0], AK_TABLE, str(SECONDS)]
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import avas_type_ty_fixed_model as M          # noqa: E402
    M.ENVI = envi
    M.TRACE_ENV = False
    print("%s: %s, ENVINTERP=%s, %s" % (NAME, env or "(defaults)", envi,
                                        "%.0f s" % SECONDS))
    # The realised frequencies, printed because for V1/V2 they ARE the change being
    # judged -- a listening result is not interpretable without them.
    print("  phase car=%d bb=%d | worst carrier err %.4f Hz | worst line err %.4f Hz"
          % (M.PHBITS_CAR, M.PHBITS_BB, M.CAR_F_ERR.max(), M.BB_F_ERR.max()))
    y = M.run(N)
    np.save(raw_path(NAME), y.astype(np.float64))
    print("  -> %s  peak %.3f rms %.4f" % (os.path.basename(raw_path(NAME)),
                                           np.abs(y).max(), y.std()))
    sys.exit(0)

if MODE != "assemble":
    print("usage: render <%s> [s] | assemble [s]" % "|".join(VARIANTS))
    sys.exit(2)

# ---------------------------------------------------------------------------
have = [k for k in VARIANTS if os.path.exists(raw_path(k))]
missing = [k for k in VARIANTS if k not in have]
if missing:
    print("missing renders: %s" % ", ".join(missing))
    sys.exit(1)

y = {k: np.load(raw_path(k)) for k in have}
n = min(len(a) for a in y.values())
labels = list("ABCDEF")[:len(have)]
order = have[:]
random.SystemRandom().shuffle(order)
assign = dict(zip(order, labels))

for k in have:
    write_wav(os.path.join(OUTDIR, "panel_%s.wav" % assign[k]), y[k][:n])
print("wrote %d files, %.0f s each (assignment not printed here)" % (len(have),
                                                                    n / float(FS)))

with open(os.path.join(OUTDIR, "ANSWER.txt"), "w", encoding="utf-8") as f:
    f.write("panel assignment -- read AFTER listening\n")
    f.write("=======================================\n")
    for k in sorted(have, key=lambda k: assign[k]):
        f.write("  %s = %-9s %s\n" % (assign[k], k, VARIANTS[k][2]))
    f.write("\nAll six are the same engine and the same coefficient table. They\n"
            "differ only in internal arithmetic: how wide the phase accumulators\n"
            "are, which coordinates the envelope is interpolated in, and whether\n"
            "the carrier comes from a table or a rotator.\n")

with open(os.path.join(OUTDIR, "README.txt"), "w", encoding="utf-8") as f:
    f.write("""shelved-lever listening panel -- %d files, %.0f s each
=====================================================
THE CRITERION (unchanged): listen to each one THROUGH. If nothing feels off --
better still, if it sounds cooler or more pleasant than the real one -- it
passes. This is NOT a spot-the-difference test, and residual against the
offline reference is not the axis (L8 had the smallest residual; L3 was chosen).

  panel_A.wav ... panel_%s.wav
  ANSWER.txt        which is which -- open after listening

What the panel is for: several load levers were shelved WITHOUT being costed,
because they change the line frequencies slightly and the 48 dB metric cannot
judge a frequency error -- the mutual beating of lines is the sound itself. A
60 s render can judge it, so they are all here, plus the shipped engine
unlabelled as an anchor.

Useful output per file: "fine" / "something is off (what?)" / "better than the
others (how?)". Naming a file as off is as valuable as naming one as good --
the point is to find which arithmetic can be spent, not to rank them.
""" % (len(have), n / float(FS), labels[-1]))
print("wrote ANSWER.txt and README.txt")
