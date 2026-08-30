# -*- coding: utf-8 -*-
"""Emit the Type_LB L3 coefficient set the CK firmware compiles.

GENERATED FILE -- do not hand-edit the output.

WHAT IS AND IS NOT IN HERE, because that IS the design: a voice is five flash arrays, two counts and one level.  Everything else
the engine needs -- the build-time knobs, the {value, delta} sine table, the CORDIC
constants, the gate's one-pole, the phase and envelope typedefs -- is arithmetic that
does not know which vehicle it is rendering, so it stays in
`avas_type_ty_ck_tables.h` and is SHARED.  This header includes that one and adds
nothing that already exists there.  If a future edit finds itself copying a knob into
this file, the copy is the bug.

Single source of truth, twice over, exactly as on the Type_TY side:

  * The 264 lines come from `tools/L3_params_type_lb.txt`, generated straight out
    of the analysis cache by `tools/gen_avas_type_lb_lines.py` (the same
    `flat_pitch_window` + `pick_lines` calls a16_lines.py stages 1-2 make).  There is
    no second copy of the coefficients.
  * Every derived integer here is computed by IMPORTING `avas_type_ty_fixed_model.py`,
    the bit-accurate model that measured the design -- including the clustering,
    which for this voice happens in the model rather than upstream because there is
    no AK Type_LB table to inherit it from.

    python tools/gen_avas_type_lb_ck_tables.py [out.h] [max_span_hz] [fs_hz]
    (default out: src/app/dsp/avas_type_lb_ck_tables.h)
"""
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "src/app/dsp/avas_type_lb_ck_tables.h"
# MAX_SPAN_HZ, and it is a MEASURED choice rather than a round number: see section 12
# of the design doc for the four-way comparison.  100 Hz wins on all four quantities
# the span moves at once -- interpolation error, amplitude resolution, output headroom
# (it is the only span whose output gain stays below unity, so the engine's output
# path is the same single shift the Type_TY voice uses) -- and costs three more
# full-rate carriers, which this part has the cycles for.
MAX_SPAN_HZ = float(sys.argv[2]) if len(sys.argv) > 2 else 100.0
FS_HZ = int(sys.argv[3]) if len(sys.argv) > 3 else 48000

HERE = os.path.dirname(os.path.abspath(__file__))
PARAMS = os.path.join(HERE, "L3_params_type_lb.txt")

# The design parameters, pinned here rather than left to the shell -- the same values
# and the same reason as the Type_TY generator.  Note what is NOT pinned: the knobs
# that only affect the SHARED tables (TABBITS) or the engine's own arithmetic
# (ENVINTERP, ENVFRAC, ACC_MODE).  They do not reach a single integer in this file,
# which is what "the coefficient set is five arrays" means in practice.
os.environ.setdefault("DEC", "32")
os.environ.setdefault("FRACBITS", "16")
os.environ.setdefault("CARRIER", "table")
os.environ["MAX_SPAN_HZ"] = str(MAX_SPAN_HZ)

sys.argv = [sys.argv[0], PARAMS, "0.0"]     # the model parses argv for its table path
sys.path.insert(0, HERE)
import avas_type_ty_fixed_model as M           # noqa: E402

assert M.SOURCE_IS_LINE_LIST, "the Type_LB set comes from the line list, not an AK header"
assert M.FS == float(FS_HZ), (
    "the model is hard-wired to %g Hz but %d Hz was requested; the step tables bake fs in"
    % (M.FS, FS_HZ))

import importlib                             # noqa: E402
import numpy as np                           # noqa: E402


def phase_tables(bits):
    """Both widths, BOTH out of the model -- never the wide set shifted down.

    round(f * 2**32) >> 16 and round(f * 2**16) differ by an LSB on plenty of lines,
    and one LSB fails the bit-exact host check.  Same argument, same code shape, as
    the Type_TY generator.
    """
    os.environ["PHBITS_CAR"] = str(bits)
    os.environ["PHBITS_BB"] = str(bits)
    importlib.reload(M)
    assert M.FS == float(FS_HZ), "the reloaded model changed fs"
    assert M.MAX_SPAN_HZ == MAX_SPAN_HZ, "the reloaded model changed the span"
    return {
        "bb_step":  [int(v) for v in M.BB_STEP],
        "bb_pha0":  [int(v) for v in M.BB_PHA0],
        "car_step": [int(v) for v in M.CAR_STEP],
        "car_err":  float(M.CAR_F_ERR.max()),
        "bb_err":   float(M.BB_F_ERR.max()),
    }


PH32 = phase_tables(32)
PH16 = phase_tables(16)
phase_tables(32)         # leave M holding the wide values, as the Type_TY generator does


def rows(vals, per_line, fmt):
    out = []
    for i in range(0, len(vals), per_line):
        out.append("    " + " ".join(fmt % v for v in vals[i:i + per_line]))
    return "\n".join(out)


# The cluster decomposition, written into the header as a comment: it is how a reader
# tells what this voice actually is (four carriers or seven, and where), and AK's own
# generator records the same table for the same reason.
COUNTS = np.diff(np.append(M.FIRST, M.NLINE))
E_TOT = float((M.AMP ** 2).sum())
clines = []
for k in range(M.K):
    f, c = int(M.FIRST[k]), int(COUNTS[k])
    seg_f, seg_a = M.FRQ[f:f + c], M.AMP[f:f + c]
    clines.append(
        " *  %2d   %9.2f      %3d   %7.2f     %7.2f     %6.2f"
        % (k, M.FC[k], c, float(seg_f.max() - seg_f.min()),
           float(np.abs(seg_f - M.FC[k]).max()), 100.0 * float((seg_a ** 2).sum()) / E_TOT))
CLUSTER_TABLE = "\n".join(clines)

# Cents, not hertz, is the readable form of a carrier's frequency error for this
# voice: the same ~0.3 Hz of 16-bit resolution error that was 0.16 % on a Type_TY
# carrier is worth more on a 67 Hz one, and that is the whole reason the width is
# re-examined here instead of inherited (design section 12).
CENTS16 = float(np.max(1200.0 * np.abs(np.log2(
    (np.array(PH16["car_step"]) / 2.0 ** 16 * M.FS) / M.FC))))

txt = """/* =========================================================================
 * Type_LB AVAS L3 line model -- FIXED-POINT coefficient set for dsPIC33CK
 * GENERATED by tools/gen_avas_type_lb_ck_tables.py -- DO NOT EDIT
 *
 * Source of the coefficients:
 *     tools/L3_params_type_lb.txt  (all %d detected lines of the flat-pitch
 *     window -- L3 is the UNTRUNCATED tone model; L4/L5/L8 take the top 64)
 * Source of every derived integer below:
 *     tools/avas_type_ty_fixed_model.py  (imported by the generator, so the model that
 *     measured this design and the firmware that runs it share one set of numbers by
 *     construction).  That module is the ENGINE's model, driven with this voice's
 *     table -- see tools/avas_type_lb_fixed_model.py.
 *
 * WHAT IS NOT IN THIS FILE, and that is the point: the sine table, the CORDIC
 * constants, the gate's one-pole, the phase/envelope typedefs and every build-time
 * knob live in avas_type_ty_ck_tables.h and are SHARED by both voices.  L3's tone part
 * is the same equation the Type_TY voice computes (design section 2), so a voice is
 * five arrays, two counts and one level -- nothing more.
 *
 * fs is BAKED IN: bb_step and car_step are both scaled by it.  Regenerate for
 * another rate rather than converting.
 * ========================================================================= */

#ifndef AVAS_TYPE_LB_CK_TABLES_H
#define AVAS_TYPE_LB_CK_TABLES_H

#include <stdint.h>

/* The shared half of the engine's tables, and the typedefs the arrays below are
 * declared with.  Including it here rather than relying on include order means this
 * header is self-sufficient. */
#include "avas_type_ty_ck_tables.h"

#define AVAS_TYPE_LB_CK_TABLE_FS_HZ   (%du)
#define AVAS_TYPE_LB_CK_MODEL_LABEL   "L3"    /* lines + 1.5 dB noise movement; the
                                             * noise bank is a separate stage */
#define AVAS_TYPE_LB_CK_LINES         (%du)
#define AVAS_TYPE_LB_CK_CLUSTERS      (%du)
#define AVAS_TYPE_LB_CK_MAX_SPAN_HZ   (%.0f)

#if defined(AVAS_TYPE_TY_CK_APP_FS_HZ) && \\
    ((AVAS_TYPE_TY_CK_APP_FS_HZ) != (AVAS_TYPE_LB_CK_TABLE_FS_HZ))
#error "avas_type_lb_ck: this coefficient set's fs does not match the app's -- regenerate it.  A silent mismatch is a pitch error, not a failure."
#endif

/* -------------------------------------------------------------------------
 * THE CLUSTER DECOMPOSITION, at MAX_SPAN_HZ = %.0f.
 *
 * Greedy contiguous runs of ascending frequency no wider than the span, carrier at
 * the amplitude-weighted centroid so the smallest baseband offset lands on the
 * strongest lines.  Only these %d carriers run at fs; each cluster's complex
 * envelope is rebuilt every AVAS_TYPE_TY_CK_DEC samples and interpolated in between.
 *
 *   #   carrier Hz    lines   span Hz   max |f-fc|   energy %%
%s
 *
 * WHY %.0f Hz AND NOT WIDER, measured four ways in design section 12: the span sets
 * the interpolation error (max |f-fc| above), the amplitude resolution (A_SCALE is
 * 32767 / the WIDEST cluster's amplitude sum, so every line in the table pays for
 * the fattest one), the output headroom (the same sum is the output gain's
 * denominator -- above %.0f Hz the gain exceeds unity and the engine needs a shift
 * and a clamp) and the load (%.2f us per extra carrier per block, MEASURED on the
 * Type_TY voice).  All four point the same way here, which is why the doc's original
 * "200 or 150" is not the answer.
 *
 * REALISED FREQUENCY ERROR at the two phase widths, computed by the model:
 *     32/32   carrier <= %.6f Hz   baseband <= %.6f Hz
 *     16/16   carrier <= %.6f Hz   baseband <= %.6f Hz   (<= %.2f cents)
 * Cents is the readable form for this voice: its carriers sit at %.0f-%.0f Hz where
 * the Type_TY's sit at 234-2169, so the same absolute resolution error is worth
 * several times more of a pitch shift.  What that is worth is a listening question
 * (phase 5) and the numbers are here so it is not answered by a comment.
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * THE OUTPUT LEVEL, and the one place this voice needs arithmetic the Type_TY voice
 * does not.
 *
 * The gain maps the accumulated y -- in the scaled units the amplitude table uses --
 * to Q15, reproducing the reference WAV's level: the measured 60 s peak of the full
 * %d-line sum (%.6f) normalised to 0.9 of full scale.
 *
 * Q15 CANNOT ALWAYS HOLD IT.  A_SCALE is set by the worst CLUSTER's amplitude sum
 * while the numerator is the peak the WHOLE sum reaches, and those are independent:
 * Type_TY's 11 narrow clusters make them nearly equal (ratio 1.026, gain 0.877),
 * while a few fat Type_LB clusters do not.  So the gain is carried as a Q15
 * mantissa plus a left SHIFT -- an octave is free on this core, only the mantissa
 * costs a multiply -- and AVAS_TYPE_LB_CK_NORM_SHIFT is 0 whenever the gain fits, in
 * which case the engine's output path is textually the shift it has always done.
 *
 * At this span the shift is %d and the linear gain is %.4f.
 * ------------------------------------------------------------------------- */
#define AVAS_TYPE_LB_CK_NORM_GAIN_Q15 (%d)
#define AVAS_TYPE_LB_CK_NORM_SHIFT    (%d)

/* -------------------------------------------------------------------------
 * Per line, frequency ascending.  Each cluster is a contiguous run, which is why no
 * per-line cluster index exists.  Same three arrays, same meanings and the same
 * types as the Type_TY set -- these are literally the other instance of
 * avas_line_ck_set_t.
 *
 *   amp_q15  amplitude, scaled so the strongest cluster's amplitude SUM fills Q15
 *            (%.1f here; the weakest line lands on %d counts).
 *   bb_step  (f - fc)*DEC/fs as a phase increment, signed.
 *   bb_pha0  the measured cos phase, as a whole cycle of 2^PHBITS_BB.
 *
 * Both step tables are rounded FROM THE FREQUENCY at their own width.
 * ------------------------------------------------------------------------- */
static const int16_t s_ck_type_lb_amp_q15[AVAS_TYPE_LB_CK_LINES] = {
%s
};

static const avas_type_ty_ck_bbstep_t s_ck_type_lb_bb_step[AVAS_TYPE_LB_CK_LINES] = {
#if ((AVAS_TYPE_TY_CK_PHBITS_BB) == 16)
%s
#else
%s
#endif
};

static const avas_type_ty_ck_bbph_t s_ck_type_lb_bb_pha0[AVAS_TYPE_LB_CK_LINES] = {
#if ((AVAS_TYPE_TY_CK_PHBITS_BB) == 16)
%s
#else
%s
#endif
};

/* Per cluster: the full-rate carrier increment, and where the cluster's lines start.
 * s_ck_type_lb_cluster_first has one extra entry so a cluster's line range is
 * [first[k], first[k+1]) with no count array. */
static const avas_type_ty_ck_carph_t s_ck_type_lb_car_step[AVAS_TYPE_LB_CK_CLUSTERS] = {
#if ((AVAS_TYPE_TY_CK_PHBITS_CAR) == 16)
%s
#else
%s
#endif
};

static const uint16_t s_ck_type_lb_cluster_first[AVAS_TYPE_LB_CK_CLUSTERS + 1] = {
%s
};

#endif /* AVAS_TYPE_LB_CK_TABLES_H */
""" % (
    M.NLINE,
    FS_HZ, M.NLINE, M.K, MAX_SPAN_HZ,
    MAX_SPAN_HZ, M.K, CLUSTER_TABLE, MAX_SPAN_HZ, MAX_SPAN_HZ, 11.88,
    PH32["car_err"], PH32["bb_err"], PH16["car_err"], PH16["bb_err"], CENTS16,
    float(M.FC.min()), float(M.FC.max()),
    M.NLINE, M.PEAK_ABS,
    M.OUT_SHIFT, M._OUT_GAIN_LIN,
    M.OUT_GAIN_Q15, M.OUT_SHIFT,
    M.A_SCALE, int(M.AMP_Q15.min()),
    rows([int(v) for v in M.AMP_Q15], 10, "%6d,"),
    rows(PH16["bb_step"], 10, "%7d,"),
    rows(PH32["bb_step"], 6, "%12d,"),
    rows(PH16["bb_pha0"], 10, "%6uu,"),
    rows(PH32["bb_pha0"], 6, "%11uu,"),
    rows(PH16["car_step"], 8, "%6uu,"),
    rows(PH32["car_step"], 5, "%11uu,"),
    rows([int(v) for v in list(M.FIRST) + [M.NLINE]], 12, "%4du,"),
)

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write(txt)

print("wrote %s" % OUT)
print("  fs=%d  lines=%d  clusters=%d (MAX_SPAN_HZ=%.0f)  DEC=%d"
      % (FS_HZ, M.NLINE, M.K, MAX_SPAN_HZ, M.DEC))
print("  A_SCALE=%.1f  amp_q15 %d..%d  peak_abs=%.6f -> gain %.4f = %d >> %d"
      % (M.A_SCALE, M.AMP_Q15.min(), M.AMP_Q15.max(), M.PEAK_ABS,
         M._OUT_GAIN_LIN, M.OUT_GAIN_Q15, 15 - M.OUT_SHIFT))
# The price, in the units phase 0 measured: a byte of .const costs exactly 1.5
# program bytes on this part, so the flash figure is quoted both ways.
DATA_B = 2 * M.NLINE + 4 * M.NLINE + 2 * M.K + 2 * (M.K + 1)     # 16-bit phase widths
print("  flash at 16/16: amp %d + bb_step %d + bb_pha0 %d + cluster %d = %d data B"
      " = %d program B (x1.5, MEASURED in design section 10)"
      % (2 * M.NLINE, 2 * M.NLINE, 2 * M.NLINE, 2 * M.K + 2 * (M.K + 1),
         DATA_B, int(round(1.5 * DATA_B))))
print("  the sine table, the CORDIC table, the gate constants and every knob are "
      "SHARED with the Type_TY set -- not emitted here")
