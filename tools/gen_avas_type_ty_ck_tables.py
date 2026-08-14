# -*- coding: utf-8 -*-
"""Emit the fixed-point Type_TY AVAS coefficient tables the CK firmware compiles.

GENERATED FILE -- do not hand-edit the output.

Single source of truth, twice over:

  * The 185 spectral lines and the 11 clusters come from the AK firmware's own
    generated header (`avas_synth_type_ty_tables.h`).  There is no second copy of
    the coefficients anywhere; re-run the analysis on the AK side and this
    follows.
  * Every derived integer here is computed by IMPORTING
    `avas_type_ty_fixed_model.py`, the bit-accurate model that measured the design.
    The C tables and the model therefore cannot disagree -- if they could, the
    model's 48.1 dB would say nothing about the firmware.

The scale factors, the phase representation and why each is what it is are
documented in that model's header comment, not repeated here.

    python tools/gen_avas_type_ty_ck_tables.py [out.h] [table.h] [fs_hz]
    (default out: src/app/dsp/avas_type_ty_ck_tables.h)
"""
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "src/app/dsp/avas_type_ty_ck_tables.h"
HDR = (sys.argv[2] if len(sys.argv) > 2
       else "tools/host_check/ref/avas_synth_type_ty_tables.h")
FS_HZ = int(sys.argv[3]) if len(sys.argv) > 3 else 48000

# The model reads its design parameters from the environment at import time, so
# they are pinned here rather than left to whatever the shell happens to carry.
# These are the values the sweep settled on.
os.environ.setdefault("DEC", "32")
os.environ.setdefault("TABBITS", "9")
os.environ.setdefault("FRACBITS", "16")
os.environ.setdefault("TERMSHIFT", "4")
os.environ.setdefault("CARRIER", "table")

# WHAT THE ENGINE DEFAULTS TO when nothing passes -Define: the envelope's
# interpolation coordinates, 0 rect / 1 polar (the model's ENVINTERP).  This is the
# one place the shipped choice lives, which is why it is here and not in the engine --
# same argument as the phase widths just below.  It moves only after a listen on the
# board, and the header text says so.
#
# It has moved: 1 (polar) since doc section 21 -- block ISR 298.9 us / 44.5 %, miss=0
# over 456 k+ blocks, listen PASS, and mode 1 == mode 2 on silicon (sink=68957 both).
# The offline metric still prefers rect (48.1 dB vs 42.9), and section 16's rule is why
# that does not decide it.  rect stays reachable with -Define AVAS_TYPE_TY_CK_ENVINTERP=0.
ENVINTERP_DEFAULT = int(os.environ.get("ENVINTERP_DEFAULT", 1))
assert ENVINTERP_DEFAULT in (0, 1), "ENVINTERP_DEFAULT is 0 (rect) or 1 (polar)"

# THE ENVELOPE STATE'S WIDTH, same argument for living here: 16 fraction bits below
# the Q15 value (the wide int32 pair) or 0 (V3, an int16 state).  The model's
# ENVFRAC, and only the two widths it measured are offered -- an intermediate width
# would need the 32-bit state anyway, so it would cost what 16 costs and buy what 0
# buys, which is nothing.
#
# It has moved: 0 (V3) since doc section 25 -- block ISR 289.2 us / 43.0 %, miss=0
# over the listen, margin 382.2 us, and 135 B of flash / 44 B of RAM returned
# (section 24, measured against the wide state on one HEAD with only this -Define
# between the two images).  The offline metric prefers the wide state by 0.4 dB and
# section 16's rule is why that does not decide it -- the ear passed V3 on this
# board's own speaker.  16 stays reachable with -Define AVAS_TYPE_TY_CK_ENVFRAC=16.
ENVFRAC_DEFAULT = int(os.environ.get("ENVFRAC_DEFAULT", 0))
assert ENVFRAC_DEFAULT in (0, 16), "ENVFRAC_DEFAULT is 0 (V3, shipped) or 16 (the wide 32-bit state)"

# WHETHER THE SLOPE IS ROUNDED, the model's ENVROUND.  Not an independent choice: at
# ENVFRAC=0 the floor is a BIAS (the state settles below its target and that offset
# lands as a coherent line at each cluster's own frequency), so V3 is only ever a
# candidate with this on -- doc section 19, -6.3 dB rect / a 0.63 dB cluster droop in
# polar without it, -2.0 / -0.4 dB with it.  At ENVFRAC=16 it is a measured no-op on
# the output samples (section 20), so it stays off there.  Hence: it follows the width
# unless something overrides it deliberately -- which since section 25 means the
# shipped build has it ON, and the -Define'd wide build still has it off.
ENVROUND_DEFAULT = int(os.environ.get(
    "ENVROUND_DEFAULT", 1 if ENVFRAC_DEFAULT == 0 else 0))
assert ENVROUND_DEFAULT in (0, 1), "ENVROUND_DEFAULT is 0 (floor) or 1 (round)"

sys.argv = [sys.argv[0], HDR, "0.0"]        # the model parses argv for its table path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import avas_type_ty_fixed_model as M           # noqa: E402

assert M.CARRIER == "table", "the rotator variant was measured and rejected; see the doc"
assert M.OUT_SHIFT == 0, (
    "this set's output gain no longer fits Q15 (shift %d) -- that is legal, the engine"
    " supports it, but it changes the Type_TY output path and needs re-listening"
    % M.OUT_SHIFT)
assert M.FS == float(FS_HZ), (
    "the model is hard-wired to %g Hz but %d Hz was requested; the step tables bake fs in"
    % (M.FS, FS_HZ))

# ---------------------------------------------------------------------------
# The phase-accumulator widths -- BOTH of them, and both out of the model.
#
# The engine is built with either 32-bit or 16-bit phase accumulators (V2 = the 11
# carriers, V1 = the 185 baseband oscillators), chosen by a -Define, so this header has to
# carry both step tables.  Narrowing the SHIPPED 32-bit integers is not an option
# and that is arithmetic, not tidiness: round(f * 2**32) >> 16 and round(f * 2**16)
# differ by an LSB on plenty of lines, and one LSB is enough to break the bit-exact
# host check -- so each width's step has to be rounded from the FREQUENCY.
#
# importlib.reload() rebinds the same module object, so the values are snapshotted
# out before the module is rebuilt underneath them.  Neither set is a
# reimplementation of the model; both ARE the model, run twice.
# ---------------------------------------------------------------------------
import importlib                                # noqa: E402


def phase_tables(bits_car, bits_bb):
    os.environ["PHBITS_CAR"] = str(bits_car)
    os.environ["PHBITS_BB"] = str(bits_bb)
    importlib.reload(M)
    assert M.FS == float(FS_HZ), "the reloaded model changed fs"
    return {
        "bb_step":  [int(v) for v in M.BB_STEP],
        "bb_pha0":  [int(v) for v in M.BB_PHA0],
        "car_step": [int(v) for v in M.CAR_STEP],
        "car_err":  float(M.CAR_F_ERR.max()),
        "bb_err":   float(M.BB_F_ERR.max()),
        "frac":     int(M._fracbits(bits_bb)),
        # atan(2**-i) in CARRIER PHASE units, so it is per-width for the same reason
        # the step tables are -- and for the stronger reason that the firmware's
        # CORDIC accumulates into a phase accumulator, not into radians.
        "cordic":   [int(v) for v in M.CORDIC_ATAN],
    }


PH32 = phase_tables(32, 32)
PH16 = phase_tables(16, 16)
phase_tables(32, 32)     # leave M holding the wide values, which the rest reads


def rows(vals, per_line, fmt):
    out = []
    for i in range(0, len(vals), per_line):
        out.append("    " + " ".join(fmt % v for v in vals[i:i + per_line]))
    return "\n".join(out)


def pair_rows(a, b, per_line):
    """{value, delta} initialisers, four pairs to a line."""
    items = ["{ %6d, %5d }," % (int(x), int(y)) for x, y in zip(a, b)]
    return "\n".join("    " + " ".join(items[i:i + per_line])
                     for i in range(0, len(items), per_line))


# The gate's one-pole coefficient, as an int16 numerator plus a fixed shift, so
# the per-sample update is ONE 16x16 multiply and a shift -- no 32x32, no
# library call.  (The naive `(int64_t)alpha * err >> 31` is what pulls in
# ___muldi3, measured at 88 cycles/sample in docs/ck_silicon_findings.md.)
#
#   err_hi = err_q31 >> 16          (int16, Q15 of the error)
#   d_gate = (err_hi * ALPHA_NUM) >> (ALPHA_SHIFT - 16)
#          = err_q31 * ALPHA_NUM / 2**ALPHA_SHIFT
# ALPHA_SHIFT = 27 is the largest shift that keeps BOTH numerators inside int16.
ALPHA_SHIFT = 27
ATTACK_S, RELEASE_S = 4.000, 0.500


def alpha_num(tau_s):
    import math
    a = 1.0 - math.exp(-1.0 / (M.FS * tau_s))
    n = int(round(a * (1 << ALPHA_SHIFT)))
    assert 0 < n <= 32767, "alpha numerator %d does not fit int16" % n
    return n, a


ATT_N, ATT_A = alpha_num(ATTACK_S)
REL_N, REL_A = alpha_num(RELEASE_S)

# -50 dB, the point where the release tail is declared finished and this engine
# stops costing anything.  Same value and same reasoning as the AK engine.
GATE_EPS_Q31 = int(round(0.0031623 * (1 << 31)))

txt = """/* =========================================================================
 * Type_TY AVAS L1 line model -- FIXED-POINT coefficient tables for dsPIC33CK
 * GENERATED by tools/gen_avas_type_ty_ck_tables.py -- DO NOT EDIT
 *
 * Source of the coefficients:
 *     %s
 * Source of every derived integer below:
 *     tools/avas_type_ty_fixed_model.py  (imported by the generator, so the model
 *     that measured this design and the firmware that runs it share one set of
 *     numbers by construction)
 *
 * Design point, as measured by that model against the offline reference:
 *     %d lines, %d clusters, DEC=%d, %d-entry Q15 sine table, %d-bit interpolation
 *     48.1 dB below signal, line-free floor -79.0 dBFS
 *   (the AK float engine it ports: 48.9 dB, -71.9 dBFS -- so this is parity on
 *    accuracy and 7 dB better on the noise floor, because a Q15 interpolated
 *    table beats the parabolic sine approximation AK uses)
 *
 * fs is BAKED IN: bb_step and car_step are both scaled by it.  A rate change cannot
 * pass silently -- regenerate instead.  What catches it is avas_rate_matches() in
 * wm8904_audio.c, at RUN time, because the rate is a runtime property of the
 * transport config; avas_type_ty_ck.h also offers an opt-in #error, but nothing in this
 * project defines the macro it needs.  See that header's SAMPLE RATE section.
 * ========================================================================= */

#ifndef AVAS_TYPE_TY_CK_TABLES_H
#define AVAS_TYPE_TY_CK_TABLES_H

#include <stdint.h>

#define AVAS_TYPE_TY_CK_TABLE_FS_HZ    (%du)
#define AVAS_TYPE_TY_CK_LINES          (%du)
#define AVAS_TYPE_TY_CK_CLUSTERS       (%du)
#define AVAS_TYPE_TY_CK_DEC            (%du)
#define AVAS_TYPE_TY_CK_DECSHIFT       (%du)   /* log2(DEC): the slope divide is a shift */
#define AVAS_TYPE_TY_CK_TABBITS        (%du)
#define AVAS_TYPE_TY_CK_TABN           (%du)
#define AVAS_TYPE_TY_CK_FRACBITS       (%du)
#define AVAS_TYPE_TY_CK_IDXSHIFT       (%du)   /* 32 - TABBITS */
#define AVAS_TYPE_TY_CK_FRACSHIFT      (%du)   /* 32 - TABBITS - FRACBITS */
#define AVAS_TYPE_TY_CK_TERMSHIFT      (%du)   /* per-carrier shift before summing y */

/* The engine splits the phase into an index and a fraction using 16-bit
 * operations only, so these are stated relative to the phase's HIGH WORD rather
 * than to the 32-bit accumulator.  The point is that nothing in the per-sample
 * path performs a 32-bit shift: idx is one lsr of the high word, and the
 * fraction is the remaining bits of the high word joined to the top of the low
 * word.  (FRACBITS is 16 for the same reason -- see the model's note.) */
#define AVAS_TYPE_TY_CK_IDXSHIFT_HI    (%du)   /* 16 - TABBITS */
#define AVAS_TYPE_TY_CK_TABMASK        (%du)   /* TABN - 1 */
#define AVAS_TYPE_TY_CK_QUARTER        (%du)   /* TABN/4: the cos index offset */

/* -------------------------------------------------------------------------
 * PHASE ACCUMULATOR WIDTHS -- a build-time choice, and BOTH step tables below are
 * generated for it.  AVAS_TYPE_TY_CK_PHBITS_BB is V1 (the 185 baseband oscillators),
 * AVAS_TYPE_TY_CK_PHBITS_CAR is V2 (the 11 full-rate carriers).
 *
 * THE DEFAULT IS 16/16 since the hardware gate closed: the block ISR measures
 * 368.5 us of its 667 us -- 54.9 %%, margin 302.5 us, miss = 0 -- and the engine was
 * listened to through the board's own codec, amplifier and speaker and called
 * perfect.  Build 32/32 with -Define to get the width the early figures were
 * measured at; it is kept, proved and
 * flashable for the same reason ACC_MODE 0 is (it is the width a part with cycles to
 * spare should prefer -- the narrow one buys load with frequency resolution, and
 * only this line table and this listener say that trade is free).
 *
 * WHY IT IS CHEAPER.  On a 16-bit core a 16-bit accumulator is one add instead of
 * add/addc, and the index/fraction split stops having to join the two halves of a
 * 32-bit word -- which is the per-line cost this engine is made of.  V1 also
 * returns 2 bytes of RAM per line (%d B) and both shrink flash.
 *
 * WHAT IT COSTS: frequency RESOLUTION, because the step must be an integer --
 * fs/2^bits for a carrier, (fs/DEC)/2^bits for a baseband oscillator.  Realised
 * with THIS line table, computed by the model rather than bounded:
 *
 *     width      worst carrier error    worst baseband error    interp fraction
 *      32/32     %-19.6f  %-19.6f  %2d bits
 *      16/16     %-19.6f  %-19.6f  %2d bits
 *
 * (The fraction narrows because a 16-bit phase has only 16 - TABBITS bits left
 * below the index.  Measured identical to 16 bits on every figure; the resolution
 * is the whole cost.)
 *
 * HOW THAT WAS JUDGED, because the headline metric cannot do it: a baseband error
 * changes how a cluster's lines beat against each other, and that beating IS the
 * sound, so 48 dB-below-signal says nothing useful about it.  Six 60 s renders,
 * unlabelled, with the shipped engine hidden among them as an anchor
 * (tools/avas_type_ty_panel.py): indistinguishable, including 16/16.  Then the same
 * width was heard on the board itself, which is the criterion -- a full listen
 * through the speaker this engine drives, not an A/B and not a residual.  The metric
 * keeps its job as the DEFECT watchdog -- host_check still proves C == model bit for
 * bit at whatever width it is told -- but the number to compare against moves with
 * the width, and that is expected rather than a regression.
 * ------------------------------------------------------------------------- */
#ifndef AVAS_TYPE_TY_CK_PHBITS_CAR
#define AVAS_TYPE_TY_CK_PHBITS_CAR     (16)
#endif
#ifndef AVAS_TYPE_TY_CK_PHBITS_BB
#define AVAS_TYPE_TY_CK_PHBITS_BB      (16)
#endif
#if ((AVAS_TYPE_TY_CK_PHBITS_CAR) != 32) && ((AVAS_TYPE_TY_CK_PHBITS_CAR) != 16)
#error "AVAS_TYPE_TY_CK_PHBITS_CAR must be 32 or 16 -- those are the two step tables this header carries."
#endif
#if ((AVAS_TYPE_TY_CK_PHBITS_BB) != 32) && ((AVAS_TYPE_TY_CK_PHBITS_BB) != 16)
#error "AVAS_TYPE_TY_CK_PHBITS_BB must be 32 or 16 -- those are the two step tables this header carries."
#endif

/* -------------------------------------------------------------------------
 * WHICH COORDINATES THE ENVELOPE IS INTERPOLATED IN -- 0 rect, 1 polar, and the
 * same choice the model calls ENVINTERP=rect|polar.
 *
 *   0  rect   I_k*cos(theta) - Q_k*sin(theta).  TWO interpolated lookups, two
 *             multiplies and two 32-bit envelope adds per carrier per sample.
 *   1  polar  A_k*cos(theta + phi_k), with phi_k folded into that carrier's own
 *             phase accumulator so it costs no add of its own: ONE lookup, ONE
 *             multiply, ONE 32-bit add.  The rebuild pays a CORDIC per cluster
 *             (the table below) and the state loses env_q/env_dq.
 *
 * The identity is exact AT the rebuild instants; the difference is entirely in the
 * %d samples between them.  A straight line in (I,Q) passes near the origin, so a
 * beat null is reproduced; a straight line in (A,phi) walks around the origin and
 * fills the null in.  MEASURED, and this is the whole reason this is a switch and
 * not a rewrite: the error is 1.8x larger and the line-free floor rises ~4 dB
 * (both the numbers and the load were measured), and then the ear did not hear
 * it -- 60 s blind, on this board's own
 * speaker, "honestly the same, both sound like the real Type_TY".  So the metric
 * rejects it, the criterion accepts it, and both records are kept.
 * ------------------------------------------------------------------------- */
#ifndef AVAS_TYPE_TY_CK_ENVINTERP
#define AVAS_TYPE_TY_CK_ENVINTERP      (%d)
#endif
#if ((AVAS_TYPE_TY_CK_ENVINTERP) != 0) && ((AVAS_TYPE_TY_CK_ENVINTERP) != 1)
#error "AVAS_TYPE_TY_CK_ENVINTERP must be 0 (rect) or 1 (polar)."
#endif

/* -------------------------------------------------------------------------
 * HOW WIDE THE ENVELOPE STATE IS -- 16 fraction bits below the Q15 value, or none.
 * The model calls this ENVFRAC and the document calls it V3.
 *
 *   16  the state is an int32 whose TOP 16 BITS are the Q15 value, and the bottom
 *       16 carry the slope's fraction so a slow cluster's slope does not quantise.
 *   0   the state IS the int16 Q15 value: one word per interpolated quantity, one
 *       16-bit add per carrier per sample instead of a 32-bit one, and the read-out
 *       shift disappears because there is nothing below the value to shift off.
 *
 * WHY 0 IS NOT FREE, and this is the part that is easy to get wrong.  The slope is
 * (target - now) >> DECSHIFT and C's >> floors, so with no fraction bits a block
 * that needs to RISE by less than DEC freezes while one that needs to FALL by the
 * same amount moves a full DEC.  The truncation is not symmetric, the state settles
 * BELOW its target, and a constant on the envelope multiplies the carrier into a
 * spurious line at that cluster's own centroid -- coherent, so it costs far more on
 * the metric than its size suggests.  MEASURED: rect loses 6.3 dB, and polar instead
 * droops one cluster's level by 0.63 dB (its state holds a magnitude, where rect's
 * two signed components could let a floored I cancel a floored Q).
 *
 * That is what AVAS_TYPE_TY_CK_ENVROUND buys back, which is why the two knobs are not
 * independent: ENVROUND follows ENVFRAC unless it is overridden, and ENVFRAC=0 with
 * ENVROUND=0 is the variant the document measured AND REJECTED, kept reachable only
 * so that the rejection can be reproduced.  See section 19.
 *
 * 0 IS WHAT SHIPS, since section 25.  With ENVROUND on it costs 0.4 dB of the offline
 * metric and buys, MEASURED on the board against the wide state built from the same
 * commit: block ISR 298.9 -> 289.2 us (44.5 -> 43.0 per cent of the block), margin
 * 382.2 us, miss = 0, 135 B of flash and 44 B of RAM returned -- and the listener
 * passed it on this board's own speaker (section 24).  The 0.4 dB is recorded and did
 * not decide it, for the reason section 16 gives.  The wide state is still a build
 * away: -Define AVAS_TYPE_TY_CK_ENVFRAC=16 (ENVROUND then follows back to 0).
 * ------------------------------------------------------------------------- */
#ifndef AVAS_TYPE_TY_CK_ENVFRAC
#define AVAS_TYPE_TY_CK_ENVFRAC        (%d)
#endif
#if ((AVAS_TYPE_TY_CK_ENVFRAC) != 16) && ((AVAS_TYPE_TY_CK_ENVFRAC) != 0)
#error "AVAS_TYPE_TY_CK_ENVFRAC must be 0 (V3, the shipped int16 state) or 16 (the wide 32-bit state)."
#endif

/* ROUNDING THE SLOPE: add DEC/2 before the shift, which makes the truncation above
 * zero-mean.  One add per interpolated quantity per cluster per REBUILD -- 11
 * instructions per DEC samples in polar, 22 in rect -- against the per-carrier
 * per-SAMPLE work that ENVFRAC=0 removes, so it is bought with small change.
 *
 * At ENVFRAC=16 it is a MEASURED NO-OP on the output samples (section 20: the slope's
 * LSB is 2**-16 of a Q15 count there, and the two floors that do bias the engine at
 * that width are the read-out and the target, not this one) -- so the shipped build,
 * which is ENVFRAC=0, is the one that needs it and the only one where it does work.
 *
 * The pairing is enforced rather than defaulted, which is this engine's habit for a
 * combination that would otherwise build quietly and be wrong: -Define ENVFRAC=0 on
 * its own does not silently get the floored variant, it does not compile.  Reproducing
 * the rejection is a MODEL run (tools/avas_type_ty_env16_study.py), not a firmware build,
 * so the firmware loses nothing by refusing to build it. */
#ifndef AVAS_TYPE_TY_CK_ENVROUND
#define AVAS_TYPE_TY_CK_ENVROUND       (%d)
#endif
#if ((AVAS_TYPE_TY_CK_ENVROUND) != 0) && ((AVAS_TYPE_TY_CK_ENVROUND) != 1)
#error "AVAS_TYPE_TY_CK_ENVROUND must be 0 (floor, C's >>) or 1 (round, add DEC/2 first)."
#endif
#if ((AVAS_TYPE_TY_CK_ENVFRAC) == 0) && ((AVAS_TYPE_TY_CK_ENVROUND) == 0)
#error "ENVFRAC=0 (V3) needs ENVROUND=1: floored, the narrow state biases itself low -- rect loses 6.3 dB and polar droops a cluster by 0.63 dB (doc section 19).  Add AVAS_TYPE_TY_CK_ENVROUND=1."
#endif

/* The phase words, as NAMED TYPES rather than #if'd struct fields and #if'd
 * locals: the state, the step tables and the loop's copies then cannot disagree
 * about the width, which is the one way this change could go wrong silently.
 *
 * The signed twin is what reads a DIFFERENCE of two phases: polar's rebuild needs
 * `p_target - p_reached` as a signed value of the same width, which is the shortest
 * way round with no branch and no comparison -- the unwrap comes free out of the
 * same wrap-is-overflow property the phase itself relies on. */
#if ((AVAS_TYPE_TY_CK_PHBITS_CAR) == 16)
typedef uint16_t avas_type_ty_ck_carph_t;
typedef int16_t  avas_type_ty_ck_carph_signed_t;
#else
typedef uint32_t avas_type_ty_ck_carph_t;
typedef int32_t  avas_type_ty_ck_carph_signed_t;
#endif
#if ((AVAS_TYPE_TY_CK_PHBITS_BB) == 16)
typedef uint16_t avas_type_ty_ck_bbph_t;
typedef int16_t  avas_type_ty_ck_bbstep_t;
#else
typedef uint32_t avas_type_ty_ck_bbph_t;
typedef int32_t  avas_type_ty_ck_bbstep_t;
#endif

/* The envelope accumulator, as a NAMED TYPE for the same reason the phase words are:
 * the struct field, the loop's local copy and the rebuild's slope then cannot
 * disagree about the width.  int16 at ENVFRAC=0 -- and the wrap that the narrow type
 * brings with it is not incidental, it is what the model reproduces (_env()), because
 * V3's whole risk is what the low end of the state does. */
#if ((AVAS_TYPE_TY_CK_ENVFRAC) == 0)
typedef int16_t  avas_type_ty_ck_env_t;
#else
typedef int32_t  avas_type_ty_ck_env_t;
#endif

/* The three places the width shows through, written once here so the engine reads the
 * same at both widths and neither variant is a second copy of the loop.
 *
 *   _TGT   scale a Q15 target into the accumulator's units.  At 16 this is the `<< 16`
 *          that costs nothing (eval_cluster returns int16, so it is which register the
 *          value sits in); at 0 it is nothing at all.  int32 EITHER WAY, so that the
 *          difference below cannot overflow: two Q15 values can differ by 65534, and
 *          the model computes that difference in full width before it shifts.
 *   _Q15   take the Q15 value back out for the multiply -- the high word, or the value.
 *   _SLOPE (target - now) >> DECSHIFT, rounded per ENVROUND, wrapped to the state's
 *          width by the cast, which is the model's _env() on the same expression.
 *   _ADD   the per-sample accumulate, and the ONE place the two widths need different
 *          text rather than a different constant.  At 16 it is the signed add this
 *          engine has always done.  At 0 the sum can leave int16 (a target near full
 *          scale plus one slope step), and the model WRAPS there -- so it is spelled as
 *          an unsigned add, which is the only wrap C defines, and read back signed by
 *          the same modulo-2^width cast the phase unwrap in the engine rests on.  One
 *          instruction either way; host_check is what proves the two agree. */
#define AVAS_TYPE_TY_CK_ENV_TGT(z)     (((int32_t)(z)) << (AVAS_TYPE_TY_CK_ENVFRAC))
#define AVAS_TYPE_TY_CK_ENV_Q15(e)     ((int16_t)((e) >> (AVAS_TYPE_TY_CK_ENVFRAC)))
#define AVAS_TYPE_TY_CK_ENV_RND \\
    ((AVAS_TYPE_TY_CK_ENVROUND) ? (int32_t)((AVAS_TYPE_TY_CK_DEC) / 2u) : (int32_t)0)
#define AVAS_TYPE_TY_CK_ENV_SLOPE(tgt, now)                                      \\
    ((avas_type_ty_ck_env_t)((((tgt) - (int32_t)(now)) + (AVAS_TYPE_TY_CK_ENV_RND))  \\
                          >> (AVAS_TYPE_TY_CK_DECSHIFT)))
#if ((AVAS_TYPE_TY_CK_ENVFRAC) == 0)
#define AVAS_TYPE_TY_CK_ENV_ADD(e, d)  \\
    ((int16_t)(uint16_t)((uint16_t)(e) + (uint16_t)(d)))
#else
#define AVAS_TYPE_TY_CK_ENV_ADD(e, d)  ((int32_t)(e) + (int32_t)(d))
#endif

/* Maps the accumulated y (in the scaled units the amplitude table uses) to Q15
 * output, reproducing the AK engine's level: the measured 60 s peak of the full
 * 185-line sum normalised to 0.9 of full scale. */
#define AVAS_TYPE_TY_CK_NORM_GAIN_Q15  (%d)

/* The gain's left SHIFT, which is 0 for this set and is stated rather than assumed.
 * A_SCALE is set by the worst CLUSTER's amplitude sum while the gain's numerator is
 * the peak the WHOLE sum reaches; with 11 narrow clusters those are nearly equal
 * (0.3758 against 0.3858, ratio 1.026), so the gain is 0.877 and Q15 holds it.  A
 * coefficient set whose clusters are fat does NOT get that for free -- the
 * Type_LB's is such a set at some spans -- so the shift is part of a voice's
 * table and the engine reads it from whichever voice is selected.  At 0 the output
 * path is the single `>> 15` this engine has always done. */
#define AVAS_TYPE_TY_CK_NORM_SHIFT     (%d)

/* Gate one-pole, attack %.3f s / release %.3f s at %d Hz.  See the generator for
 * why this is an int16 numerator and a shift rather than a Q31 multiply. */
#define AVAS_TYPE_TY_CK_GATE_ALPHA_SHIFT   (%du)
#define AVAS_TYPE_TY_CK_GATE_ATTACK_NUM    (%d)
#define AVAS_TYPE_TY_CK_GATE_RELEASE_NUM   (%d)
#define AVAS_TYPE_TY_CK_GATE_EPS_Q31       (%dL)

/* -------------------------------------------------------------------------
 * Q15 sine, one full cycle, as {value, delta-to-next} PAIRS.
 *
 * The pair form is what makes the interpolation cheap, and the reason is the
 * instruction set rather than the algorithm: a 4-byte aligned pair is ONE mov.d,
 * where reading tab[idx] and tab[idx+1] separately costs two address
 * computations, two loads, two sign extends and a 32-bit subtract.  Measured on
 * XC-DSC 3.31.01 for 33CK64MC105, one interpolated lookup went from 22
 * instructions to 6.
 *
 * cos is this same table read a quarter cycle further along, which at this
 * geometry is the exact index offset AVAS_TYPE_TY_CK_TABN/4 -- so cos and sin share
 * one phase accumulator AND one interpolation fraction, and the quarter-cycle
 * add disappears from the per-sample path entirely.
 *
 * The table therefore runs TABN + TABN/4 entries: the first TABN/4 are repeated
 * past the end so that `idx + QUARTER` NEVER NEEDS A WRAP MASK.  That costs %d
 * bytes of flash (of ~23 KB free) and removes an and-with-511 plus the register
 * holding the mask from the per-carrier path.  Delta fits int16 with room to
 * spare: the largest step of a Q15 sine sampled %d times is 402.
 * ------------------------------------------------------------------------- */
typedef struct
{
    int16_t v;      /* Q15 sine at this index          */
    int16_t d;      /* Q15 sine at index+1, minus v    */
} avas_type_ty_ck_sin_ent_t;

#define AVAS_TYPE_TY_CK_TABENTS    (AVAS_TYPE_TY_CK_TABN + AVAS_TYPE_TY_CK_QUARTER)

/* aligned(4): the pair is fetched as a 32-bit double word. */
static const avas_type_ty_ck_sin_ent_t s_ck_sin_dq15[AVAS_TYPE_TY_CK_TABENTS]
    __attribute__((aligned(4))) = {
%s
};

/* -------------------------------------------------------------------------
 * THE TYPE_TY L1 COEFFICIENT SET ITSELF -- five arrays, and the ONLY part of this
 * header that is vehicle-specific.  Everything above and below (the knobs, the sine
 * table, the CORDIC constants, the gate) is shared with the Type_LB voice.
 *
 * IT IS COMPILED OUT OF A TYPE_LB IMAGE, and by the preprocessor rather than by
 * the linker, because that is the only thing that actually frees the bytes:
 * --gc-sections is on but -fdata-sections is deliberately off, so unreferenced
 * `const` inside a live translation unit is not collected -- MEASURED from the other
 * side.  1 156 data bytes = 1 734 program bytes, on an image at 99 %% of flash.
 * ------------------------------------------------------------------------- */
/* AVAS_CK_VOICE_NONE is the debug mode (see avas_synth_line_ck.h): no voice is
 * compiled, so these arrays go too -- they are the bulk of what that mode reclaims.
 * Tested raw rather than through AVAS_LINE_CK_HAVE_ENGINE so that this header stays
 * usable on its own (generators, tools/host_check): an undefined macro reads as 0,
 * which is the permissive answer. */
#if !(AVAS_CK_VOICE_NONE) && (!(AVAS_CK_VOICE_TYPE_LB) || (AVAS_CK_VOICE_BOTH))

/* -------------------------------------------------------------------------
 * Per line, frequency ascending.  Each cluster is a contiguous run, which is
 * why no per-line cluster index exists.
 *
 *   amp_q15  amplitude, scaled so the strongest cluster's amplitude SUM fills
 *            Q15.  That bound is what makes |Z_k| <= 32767 and lets the
 *            envelope accumulate in a plain int32.
 *   bb_step  (f - fc)*DEC/fs as a phase increment, signed: lines below their
 *            carrier rotate backwards.  Derived here rather than in the firmware,
 *            so it costs flash instead of RAM.
 *   bb_pha0  the measured cos phase, as a whole cycle of 2^PHBITS_BB.
 *
 * Both are rounded FROM THE FREQUENCY at the selected width -- the narrow set is
 * not the wide set shifted down, because those two disagree by an LSB on plenty of
 * lines and host_check is bit-exact.
 * ------------------------------------------------------------------------- */
static const int16_t s_ck_amp_q15[AVAS_TYPE_TY_CK_LINES] = {
%s
};

static const avas_type_ty_ck_bbstep_t s_ck_bb_step[AVAS_TYPE_TY_CK_LINES] = {
#if ((AVAS_TYPE_TY_CK_PHBITS_BB) == 16)
%s
#else
%s
#endif
};

static const avas_type_ty_ck_bbph_t s_ck_bb_pha0[AVAS_TYPE_TY_CK_LINES] = {
#if ((AVAS_TYPE_TY_CK_PHBITS_BB) == 16)
%s
#else
%s
#endif
};

/* -------------------------------------------------------------------------
 * Per cluster: the full-rate carrier increment, and where the cluster's lines
 * start.  s_ck_cluster_first has one extra entry so a cluster's line range is
 * [first[k], first[k+1]) with no count array.
 * ------------------------------------------------------------------------- */
static const avas_type_ty_ck_carph_t s_ck_car_step[AVAS_TYPE_TY_CK_CLUSTERS] = {
#if ((AVAS_TYPE_TY_CK_PHBITS_CAR) == 16)
%s
#else
%s
#endif
};

static const uint16_t s_ck_cluster_first[AVAS_TYPE_TY_CK_CLUSTERS + 1] = {
%s
};

#endif /* !AVAS_CK_VOICE_TYPE_LB -- the Type_TY coefficient set */

#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
/* -------------------------------------------------------------------------
 * CORDIC constants for polar's (I,Q) -> (A,phi), which runs %d times per rebuild.
 *
 * atan(2**-i) EXPRESSED AS A PHASE, not as an angle: the conversion accumulates
 * straight into the same units the carrier phase uses, so nothing in the firmware
 * ever holds radians and the wrap is again just the accumulator's overflow.  That
 * makes the table per-width, like the step tables.
 *
 * %d iterations.  Each resolves about one bit of angle and the model's sweep put the
 * knee at 10 (42.84 dB against 42.87 at 12, i.e. flat) -- so 12 sits in the flat
 * region with two iterations of margin, and dropping to 10 is a measured lever worth
 * about 0.4 %% of the block if flash or load ever needs it.  It is also where the
 * table stops saying anything at a 16-bit phase: atan(2**-13) is 1.3 phase units.
 *
 * KINV_Q15 is 1/K for the CORDIC's fixed vector growth K = %.7f.  The engine halves
 * the input first (K*32767 does not fit int16, and 16-bit x/y is what makes this
 * affordable), so the >> 14 that goes with this constant undoes both scalings at
 * once -- see avas_type_ty_ck_to_polar().
 * ------------------------------------------------------------------------- */
#define AVAS_TYPE_TY_CK_CORDIC_N           (%du)
#define AVAS_TYPE_TY_CK_CORDIC_KINV_Q15    (%d)
/* Half a turn at this width: the pre-rotation that puts the vector in the right
 * half plane, where a vectoring CORDIC converges. */
#define AVAS_TYPE_TY_CK_CORDIC_HALF   \
    ((avas_type_ty_ck_carph_t)1u << ((AVAS_TYPE_TY_CK_PHBITS_CAR) - 1))

static const avas_type_ty_ck_carph_t s_ck_cordic_atan[AVAS_TYPE_TY_CK_CORDIC_N] = {
#if ((AVAS_TYPE_TY_CK_PHBITS_CAR) == 16)
%s
#else
%s
#endif
};
#endif /* ENVINTERP == 1 */

#endif /* AVAS_TYPE_TY_CK_TABLES_H */
""" % (
    HDR,
    M.NLINE, M.K, M.DEC, M.TABN, M.FRACBITS,
    FS_HZ, M.NLINE, M.K, M.DEC, M.DECSHIFT,
    M.TABBITS, M.TABN, M.FRACBITS, M._IDXSH, M._FRSH, M.TERMSHIFT,
    16 - M.TABBITS, M.TABN - 1, M.TABN // 4,
    2 * M.NLINE,
    PH32["car_err"], PH32["bb_err"], PH32["frac"],
    PH16["car_err"], PH16["bb_err"], PH16["frac"],
    M.DEC, ENVINTERP_DEFAULT,
    ENVFRAC_DEFAULT, ENVROUND_DEFAULT,
    M.OUT_GAIN_Q15, M.OUT_SHIFT,
    ATTACK_S, RELEASE_S, FS_HZ,
    ALPHA_SHIFT, ATT_N, REL_N, GATE_EPS_Q31,
    4 * (M.TABN // 4), M.TABN,
    pair_rows(list(M.SINTAB[:M.TABN]) + list(M.SINTAB[:M.TABN // 4]),
              list(M.SINDELTA) + list(M.SINDELTA[:M.TABN // 4]), 4),
    rows([int(v) for v in M.AMP_Q15], 10, "%6d,"),
    rows(PH16["bb_step"], 10, "%7d,"),
    rows(PH32["bb_step"], 6, "%12d,"),
    rows(PH16["bb_pha0"], 10, "%6uu,"),
    rows(PH32["bb_pha0"], 6, "%11uu,"),
    rows(PH16["car_step"], 8, "%6uu,"),
    rows(PH32["car_step"], 5, "%11uu,"),
    rows([int(v) for v in list(M.FIRST) + [M.NLINE]], 12, "%4du,"),
    M.K, M.CORDIC_N, M.CORDIC_K, M.CORDIC_N, M.CORDIC_KINV_Q15,
    rows(PH16["cordic"], 6, "%6uu,"),
    rows(PH32["cordic"], 5, "%11uu,"),
)

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write(txt)

print("wrote %s" % OUT)
print("  fs=%d  lines=%d  clusters=%d  DEC=%d  sin table=%d entries (v,d pairs)"
      % (FS_HZ, M.NLINE, M.K, M.DEC, M.TABN))
print("  A_SCALE=%.1f  amp_q15 %d..%d  norm gain Q15=%d"
      % (M.A_SCALE, M.AMP_Q15.min(), M.AMP_Q15.max(), M.OUT_GAIN_Q15))
print("  gate alpha: attack %d rel %d (>>%d)  = tau %.3f s / %.3f s"
      % (ATT_N, REL_N, ALPHA_SHIFT,
         1.0 / (M.FS * ATT_N / float(1 << ALPHA_SHIFT)),
         1.0 / (M.FS * REL_N / float(1 << ALPHA_SHIFT))))
print("  flash: sin(v,d) %d B + amp %d B + bb_step %d B + bb_pha0 %d B + cluster %d B = %d B"
      % (4 * (M.TABN + M.TABN // 4), 2 * M.NLINE, 4 * M.NLINE, 4 * M.NLINE, 4 * M.K + 2 * (M.K + 1),
         4 * M.TABN + 2 * M.NLINE + 8 * M.NLINE + 4 * M.K + 2 * (M.K + 1)))
# Only ONE of each pair is compiled -- the unselected width is preprocessed out --
# so the header carrying both costs nothing in the image.
print("  phase widths: 32/32 and 16/16, both emitted (#if AVAS_TYPE_TY_CK_PHBITS_*)")
print("    32/32  carrier err <= %.6f Hz  baseband err <= %.6f Hz  frac %d bits"
      % (PH32["car_err"], PH32["bb_err"], PH32["frac"]))
print("    16/16  carrier err <= %.6f Hz  baseband err <= %.6f Hz  frac %d bits"
      % (PH16["car_err"], PH16["bb_err"], PH16["frac"]))
print("    16/16 saves %d B flash (steps+phases) and %d B RAM (bb_phase[])"
      % (2 * M.NLINE + 2 * M.NLINE + 2 * M.K, 2 * M.NLINE))
