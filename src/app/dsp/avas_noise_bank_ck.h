/*
 * avas_noise_bank_ck.h -- the NOISE half of the Type_LB L3 voice.
 *
 * L3 = tone + noise(mod_db=1.5).  The tone half is 264 sine lines in
 * avas_synth_line_ck.c; this is the other 80.2 % of the reference sound's energy, which no
 * number of lines can represent because it is not periodic.
 *
 * WHAT IT IS: one white source (xorshift32), tilted once by a single Q15 one-pole, into
 * 12 Chamberlin state-variable bandpasses summed with fitted gains.  Every coefficient
 * comes from avas_type_lb_ck_noise_tables.h, which is generated and carries its own
 * measurements; nothing here is tunable by hand.
 *
 * WHY AN SVF AND NOT A BIQUAD.  These bands go down to 24 Hz, and a direct-form
 * biquad's `a1` there is -1.998817: in Q14 that is -32748 vs -32749, i.e. the POLE
 * ANGLE quantises by about 100 % and the centre frequency becomes arbitrary.  The SVF
 * tunes on F = 2 sin(pi f0 / fs), which quantises the frequency itself -- 0.3 % at the
 * bottom band.  It is the same three multiplies either way.
 *
 * COST, and where it is spent: 3 multiplies + the accumulate per band per sample.  Two
 * of the multiplies are 16x16; the third is 16x32 because `high` is held wide on
 * purpose -- see the comment at that line, which is about a bound rather than a
 * measurement.  The gusts cost NOTHING per sample: they fold into the gain the bank
 * already multiplies by, and are recomputed once per control block.
 */
#ifndef AVAS_NOISE_BANK_CK_H
#define AVAS_NOISE_BANK_CK_H

#include <stdint.h>

#include "avas_type_lb_ck_noise_tables.h"

typedef struct {
    /* The two SVF state variables per band.  int16_t is not a guess: the generator
     * measures the peak of both over 20 s of this exact arithmetic and asserts they
     * never reach 32 767 (they peak near 15 000), so nothing here wraps. */
    int16_t  low[AVAS_TYPE_LB_CK_NOISE_BANDS];
    int16_t  band[AVAS_TYPE_LB_CK_NOISE_BANDS];
    /* The fitted gain with this block's gust folded in -- the only thing the per-sample
     * loop reads, which is what makes the gusts free. */
    int16_t  gain[AVAS_TYPE_LB_CK_NOISE_BANDS];
    /* The gust random walk, one per band, in int16 drive units. */
    int16_t  walk[AVAS_TYPE_LB_CK_NOISE_BANDS];
    int32_t  pre;                   /* the source tilt's state, Q15 of an int16 */
    uint32_t rng;                   /* the white source */
    uint32_t grng;                  /* the gust drive, deliberately a SEPARATE stream */
} avas_noise_bank_ck_t;

/* Reset to silence and the fitted gains with no gust applied.  Called on every gate-on,
 * because a bank that is not running has no state worth keeping. */
void avas_noise_bank_ck_init(avas_noise_bank_ck_t *b);

/* One block's worth of gust update.  Call at the control rate, before the samples. */
void avas_noise_bank_ck_update_gusts(avas_noise_bank_ck_t *b);

/* One sample, in the engine's A_SCALE units -- i.e. already scaled to be ADDED to the
 * carrier sum, before the voice's output gain.  Returns int16_t-ranged values but the
 * caller must accumulate in int32_t: tone peak + noise peak exceeds full scale (the
 * generator measures 34 487 after the output gain), so the SUM is what clamps. */
int16_t avas_noise_bank_ck_sample(avas_noise_bank_ck_t *b);

#endif /* AVAS_NOISE_BANK_CK_H */
