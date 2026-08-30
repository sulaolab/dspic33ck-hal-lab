# -*- coding: utf-8 -*-
"""Render the model's output to WAV so the polar question can also be JUDGED BY EAR.

Why this is a fair listening test, and where it stops being one
---------------------------------------------------------------
`tools/host_check/` proves `avas_type_ty_ck.c` bit-identical to the model, so the
"rect" file below is not an approximation of what the board plays -- it IS what the
board plays, sample for sample, ahead of the codec's own conversion.  The "polar"
file is what a polar build WOULD play, at the same standard: the model computes the
same integer operations that firmware would.

What it cannot reproduce is the analogue end -- the WM8904, the amplifier and the
speaker on EV88G73A -- so a difference that survives here may or may not survive
there.  That is the honest limit, and it is the reason the board comparison (`a`
toggles the shipped engine) stays the final word on timbre.

The error files are the useful ones.  A -43 dB error is nearly impossible to hear
against the signal that hides it; subtracting the offline reference leaves the error
alone, and BOTH error files are scaled by the SAME factor, so polar really does come
out 1.79x louder rather than being normalised back to a tie.

    python tools/avas_type_ty_render_wav.py [seconds] [outdir]
"""
import os
import struct
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
AK_TABLE = os.path.join(ROOT, "tools", "host_check", "ref",
                        "avas_synth_type_ty_tables.h")
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0
OUTDIR = (sys.argv[2] if len(sys.argv) > 2
          else os.path.join(ROOT, "..", "_listen_avas"))

os.environ.setdefault("ACC", "acc40")      # the arithmetic the board ships
sys.argv = [sys.argv[0], AK_TABLE, str(SECONDS)]
sys.path.insert(0, os.path.join(ROOT, "tools"))
import avas_type_ty_fixed_model as M      # noqa: E402


def write_wav(path, y, fs=48000):
    """16-bit mono PCM.  Clipped, not scaled: a file that had to be turned down to
    fit would no longer be the thing the firmware emits."""
    d = np.clip(np.round(y * 32767.0), -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(struct.pack("<%dh" % len(d), *d.tolist()))
    print("  %-28s %6.2f s  peak %.3f" % (os.path.basename(path), len(d) / float(fs),
                                          np.abs(y).max()))


def variant(envi):
    M.ENVI = envi
    M.TRACE_ENV = False
    return M.run(M.N)


os.makedirs(OUTDIR, exist_ok=True)
print("rendering %.1f s to %s" % (SECONDS, os.path.abspath(OUTDIR)))
idl = M.ideal(M.N)
y_rect = variant("rect")
y_pol = variant("polar")

write_wav(os.path.join(OUTDIR, "avas_rect_ships.wav"), y_rect)
write_wav(os.path.join(OUTDIR, "avas_polar_rejected.wav"), y_pol)
write_wav(os.path.join(OUTDIR, "avas_reference_float.wav"), idl)

# One common gain for both error files, chosen so the LOUDER of the two just fills
# the file.  The ratio between them is then the measured 1.79x, audible as level.
e_r, e_p = y_rect - idl, y_pol - idl
g = 0.9 / max(np.abs(e_r).max(), np.abs(e_p).max())
print("  error files share a gain of %.1fx (+%.1f dB)" % (g, 20 * np.log10(g)))
write_wav(os.path.join(OUTDIR, "avas_err_rect.wav"), e_r * g)
write_wav(os.path.join(OUTDIR, "avas_err_polar.wav"), e_p * g)
print("  err rms rect %.6f  polar %.6f  (%.2fx, %+.2f dB)"
      % (e_r.std(), e_p.std(), e_p.std() / e_r.std(),
         20 * np.log10(e_p.std() / e_r.std())))
