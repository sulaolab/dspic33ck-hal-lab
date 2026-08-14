#ifndef GAIN_DB_H
#define GAIN_DB_H

#include <stdbool.h>
#include <stdint.h>

#include "gain_db_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * gain_db.h -- a dB-calibrated fixed gain, in integer arithmetic. HEADER ONLY.
 *
 * WHAT THIS IS FOR
 * ----------------
 * The WM8904's analog input (or the circuit ahead of it) is noisy, so the analog gain
 * was turned down to keep that out of the converter -- and the digital signal that
 * arrives is correspondingly small (measured: peak -18.7 dBFS). This lifts it back, by a number of dB stated in the config.
 *
 * It restores LEVEL, NOT SIGNAL-TO-NOISE RATIO. Everything already digitised comes up by
 * the same number of dB, the codec's own noise floor included. The place that buys SNR is
 * the analog PGA ahead of the converter; this is the standard move once that PGA has been
 * set for the circuit's noise rather than for the converter's, and it is worth knowing
 * which of the two problems is being solved. Section 2 of that doc says it at length.
 *
 * WHY THERE IS NO NEW ARITHMETIC KERNEL
 * ------------------------------------
 * gain_ctrl.c's local_scale_q15() and the transport header's
 * nora_tdm_slot_scale_q15() already compute an EXACT scale from two native 16x16
 * multiplies: `t = x * m / 2^16` reconstructed from the high and low halves, then a
 * trailing shift. This is that arithmetic with a wider mantissa and a wider shift:
 *
 *     y = ((x * m) >> 16) << GAIN_DB_SHIFT,   m = round(gain * 2^(16 - GAIN_DB_SHIFT))
 *
 * The dB -> m conversion is NOT done here. It is a logarithm, this part has no FPU, and
 * sonora's `db_to_lin() = powf(10, db/20)` is exactly what must not be ported.
 * tools/gen_gain_db_tables.py does it at build time and gain_db_tables.h is the result --
 * the same rule the AVAS coefficients follow.
 *
 * WHY THE SHIFT IS A CONSTANT (IT WAS NOT, AND THAT COST 30 us)
 * -----------------------------------------------------------
 * The first version of this file split the gain into an octave count `n` and a mantissa in
 * [1, 2), and argued that the octave was free because the helper's trailing `<< 1` became
 * `<< (1 + n)` -- the same instruction with a different immediate. The built image says
 * otherwise, and by a wide margin. With `n` a run-time field:
 *
 *   - `<< (1 + n)` is not a shift instruction. It is a general variable 32-bit shift:
 *     `subr`, a branch, `lsr`, `sl`, `sl`, `ior` -- six or seven instructions per sample.
 *   - the clamp bounds `+-(INT32_MAX >> (1 + n))` become run-time values, and the block
 *     ISR has no registers left to keep them in, so it SPILLS them and re-loads `tmin`
 *     from the frame on every sample.
 *
 * Measured together with the multiply, that was 23-25 cycles a sample and +30 us on the
 * block -- against an estimate of 3-9 us that had priced the octave at zero. The per-sample
 * ledger was measured, not estimated. THE LESSON IS NARROW AND WORTH KEEPING:
 * a shift by a constant is free; a shift by a variable is a subroutine in disguise, and the
 * constants that depend on it stop being immediates.
 *
 * So the octave is gone. GAIN_DB_SHIFT is a compile-time constant, the mantissa carries the
 * whole gain in Q12, and the cost is mantissa resolution: 0.0021 dB at unity, 0.0148 dB at
 * the worst point of the grid (-24.0 dB). The grid itself is 0.5 dB and was found by ear,
 * so this is not a resolution a listener can reach -- it is the deliberate trade
 * (docs section 11.3, lever L2).
 *
 * THE CLAMP, AND WHY IT IS TWO 16-BIT COMPARES
 * -------------------------------------------
 * Attenuation cannot overflow. A boost can, and an overflow on audio is not a loud sample
 * -- it is a SIGN FLIP, which is the one failure mode that sounds like destruction rather
 * than like level. So the shift is guarded.
 *
 * With the shift constant, `t` must satisfy `|t| <= INT32_MAX >> SHIFT`, and that bound is
 * a whole multiple of 65536 -- so testing the HIGH WORD of `t` against a 16-bit immediate is
 * not an approximation of the 32-bit test, it is the SAME test:
 *
 *     t <= 0x07FFFFFF  <=>  (int16)(t >> 16) <=  0x07FF
 *     t >= -0x08000000 <=>  (int16)(t >> 16) >= -0x0800
 *
 * and `t >> 16` costs nothing on a 16-bit core: it is naming the register the high half is
 * already in. Two compares against immediates, no spill, and exact.
 *
 * Because a clamp that fires is a CONFIGURATION error rather than a fact of life, the
 * callers count the samples that hit it and report them (this repo's rule for a survivable
 * fault: ?du for the UART overrun, miss for the block ISR).
 *
 * UNITY
 * -----
 * At 0.0 dB the mantissa is exactly 2^12, so the scale is `(x >> 4) << 4` = x with the low
 * FOUR bits cleared. Those bits are pad, not audio: the codec word is 24-bit left-justified
 * in the 32-bit slot, and gain_ctrl already clears bit 0 at unity for the same reason (see
 * gain_ctrl.h's consequence 3, which this matches rather than re-argues).
 *
 * The caller does better than that: it SKIPS the stage when the mantissa is unity, deciding
 * once per block rather than once per sample, so a 0.0 dB stage now touches nothing at all
 * -- which is both bit-exact and free. The identity above is still proved exhaustively for
 * the scaled path, because a `*ti 0.0` must not produce a different signal from a `*ti`
 * that was never typed.
 *
 * Header-only on purpose: the whole module is a table lookup and one inline scale, and a .c
 * file would have to be added to both configurations in
 * firmware.X/nbproject/configurations.xml (the source list is the build's truth) for no
 * code that a translation unit could not inline anyway.
 */

/*
 * A resolved gain: the multiplier the hot path uses, plus the dB it actually realises so
 * that anything reporting the value reports what is RUNNING and not what was asked for.
 * Keep the dB here rather than recomputing it -- a report derived from a second calculation
 * can agree with the request while the arithmetic disagrees.
 *
 * One integer of arithmetic, where the octave form had two. That also retires a hazard: a
 * (mantissa, shift) pair published from another priority could be read half-updated, and a
 * new mantissa with an old shift is a wrong OCTAVE, not a rounding error. There is no pair
 * left to tear.
 */
typedef struct {
    uint16_t mant;       /* Q12, unity GAIN_DB_MANT_ONE -- always the multiplier */
    int16_t  db_x10;     /* the REALISED gain, in tenths of a dB */
} gain_db_t;

/* Unity, for an initialiser: exactly the identity the comment above describes. */
#define GAIN_DB_UNITY_INIT   { GAIN_DB_MANT_ONE, 0 }

/* True when this gain is the identity, i.e. when the caller may skip the stage. */
#define GAIN_DB_IS_UNITY(m)  ((m) == GAIN_DB_MANT_ONE)

/* The clamp bounds, on the HIGH WORD of t, as the comment above derives them. Named here
 * because the host harness has to state the same two numbers to check them. */
#define GAIN_DB_THI_MAX      ((int16_t)(INT16_MAX >> GAIN_DB_SHIFT))
#define GAIN_DB_THI_MIN      ((int16_t)(-GAIN_DB_THI_MAX - 1))

/* Resolve a half-decibel value off the generated table. Out of range is CLAMPED to the
 * table's ends rather than refused, because the only caller that can be out of range is
 * a config the compiler has already rejected (app_config.h) -- so this is belt and
 * braces, and a clamp keeps it from being a silent read past the array. */
static inline void gain_db_from_half(int16_t half, gain_db_t *g)
{
    int16_t h = half;

    if (h < (int16_t)GAIN_DB_HALF_MIN) { h = (int16_t)GAIN_DB_HALF_MIN; }
    if (h > (int16_t)GAIN_DB_HALF_MAX) { h = (int16_t)GAIN_DB_HALF_MAX; }

    {
        const uint16_t idx = (uint16_t)(h - (int16_t)GAIN_DB_HALF_MIN);

        g->mant   = gain_db_mant[idx];
        g->db_x10 = (int16_t)(h * 5);
    }
}

/*
 * Resolve a request in TENTHS of a dB, which is the console's unit.
 *
 * The table's grid is 0.5 dB, so the request is SNAPPED to the nearest step -- and
 * `g->db_x10` then holds what was realised, not what was asked, so the caller can print
 * the difference instead of hiding it. Same policy as gain_ctrl_mute_set(), which snaps a
 * ramp_ms to an available curve and states which one it picked; a request that snapped
 * somewhere surprising should be visible rather than silent.
 *
 * Returns false, changing nothing, if the request is outside the table's range. Out of
 * range is refused where off-grid is snapped, because the two are different mistakes: a
 * typo of one digit lands off-grid and still means roughly what was intended, while +40 dB
 * does not.
 */
static inline bool gain_db_from_x10(int16_t db_x10, gain_db_t *g)
{
    int16_t half;

    /* Nearest multiple of 5 tenths, rounding away from zero on a tie so that the snap is
     * symmetric about 0 dB (truncation would bias every negative request upward). */
    if (db_x10 >= 0) {
        half = (int16_t)((db_x10 + 2) / 5);
    } else {
        half = (int16_t)(-((-db_x10 + 2) / 5));
    }

    if ((half < (int16_t)GAIN_DB_HALF_MIN) || (half > (int16_t)GAIN_DB_HALF_MAX)) {
        return false;
    }

    gain_db_from_half(half, g);

    return true;
}

/*
 * The hot path. One sample in, one sample out, saturating.
 *
 * `nsat` counts the samples that CLAMPED and is only touched when one does, so the
 * common path pays two comparisons and nothing else. Pass a local and fold it into a
 * persistent counter after the loop -- a pointer straight at a static would make the
 * compiler reload it per sample.
 *
 * The multiply is written out with builtins rather than left to portable C for the reason
 * gain_ctrl.c states at local_scale_q15(): on this toolchain any expression wide enough to
 * hold the product is promoted to 64 bits and becomes a ___muldi3 CALL, measured at ~88
 * cycles per sample. The #else branch is the same arithmetic for the host check and for the
 * AK side's 32-bit core, and is bit-identical by construction.
 */
static inline int32_t gain_db_scale(int32_t x, uint16_t mant, uint16_t *nsat)
{
#if defined(__XC16__)
    const int32_t  hi = __builtin_mulsu((int16_t)(x >> 16), mant);
    const uint32_t lo = __builtin_muluu((uint16_t)x, mant);
#else
    const int32_t  hi = (int32_t)((int16_t)(x >> 16)) * (int32_t)mant;
    const uint32_t lo = (uint32_t)(uint16_t)x * (uint32_t)mant;
#endif
    /* t = x * m / 2^16, exact and always representable: |x| < 2^31 and m < 2^16, so
     * |t| < 2^31. The >>16 is free -- it is naming the high word. */
    const int32_t t = (int32_t)((uint32_t)hi + (lo >> 16));

    /* The clamp, on t's high word against two immediates -- the SAME test as the 32-bit
     * one, because INT32_MAX >> SHIFT is a whole multiple of 65536. See the header
     * comment; this is where the spilled 32-bit bounds used to be. */
    const int16_t thi = (int16_t)(t >> 16);

    if (thi > GAIN_DB_THI_MAX) {
        (*nsat)++;
        return INT32_MAX;
    }
    if (thi < GAIN_DB_THI_MIN) {
        (*nsat)++;
        return INT32_MIN;
    }

    /* Shifted as unsigned: shifting a signed value left into the sign bit is undefined,
     * and the clamp above has already proved this one does not. A CONSTANT shift, which
     * is the whole point of the fixed-shift form. */
    return (int32_t)((uint32_t)t << GAIN_DB_SHIFT);
}

#ifdef __cplusplus
}
#endif

#endif /* GAIN_DB_H */
