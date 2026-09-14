/*
 * avas_noise_bank_ck.c -- the L3 noise bank.  See avas_noise_bank_ck.h for what it is
 * and why it is an SVF; this file is the arithmetic, and the arithmetic is the contract.
 *
 * EVERY LINE HERE IS BIT-EXACT AGAINST tools/gen_avas_type_lb_ck_noise.py's run_bank().
 * tools/host_check compares 4096 samples with the gusts off (they cannot be bit-exact
 * across two languages' PRNG draw orders, and are checked statistically instead: the
 * generator measures the realised modulation sd at 1.56 dB against the model's 1.5 dB).
 * If you change a shift here, change it there and re-run the host check -- the noise is
 * not something an ear can audit for a one-bit error.
 */
#include "avas_noise_bank_ck.h"

/* xorshift32 -- the same generator and the same seeds sonora's Type_LB uses, so the
 * two boards' noise is comparable rather than merely similar.  The output is the TOP 16
 * bits: the low bits of an xorshift are the weak ones. */
static int16_t noise_white(uint32_t *state)
{
    uint32_t x = *state;

    x ^= (x << 13);
    x ^= (x >> 17);
    x ^= (x << 5);
    *state = x;
    return (int16_t)(x >> 16);
}

void avas_noise_bank_ck_init(avas_noise_bank_ck_t *b)
{
    uint16_t i;

    for (i = 0u; i < AVAS_TYPE_LB_CK_NOISE_BANDS; i++) {
        b->low[i] = 0;
        b->band[i] = 0;
        b->walk[i] = 0;
        b->gain[i] = s_ck_type_lb_noise_g_q15[i];
    }
    b->pre = 0L;
    b->rng = AVAS_TYPE_LB_CK_NOISE_SEED;
    b->grng = AVAS_TYPE_LB_CK_NOISE_GUST_SEED;
}

void avas_noise_bank_ck_update_gusts(avas_noise_bank_ck_t *b)
{
    uint16_t i;

    for (i = 0u; i < AVAS_TYPE_LB_CK_NOISE_BANDS; i++) {
        int32_t w = (int32_t)b->walk[i];
        int32_t m;
        int32_t g;

        /* A one-pole on a uniform drive: a slow random walk with a bounded sd, which is
         * what "gust" means here -- it moves a band's energy around in time without
         * changing its long-term rms.  The clamp is not cosmetic: it is what bounds
         * AVAS_TYPE_LB_CK_NOISE_GUST_K_Q15 * walk inside int32. */
        /* Both operands are cast to int32_t BEFORE the subtraction, not after.  `int` is
         * 16 bits on this device, so `int16 - int16` is evaluated in 16 bits and a drive
         * of +32767 against a walk of -3802 would wrap -- and a host gcc, whose `int` is
         * 32 bits, would never reproduce it.  Same reason at the tilt below. */
        w += ((int32_t)AVAS_TYPE_LB_CK_NOISE_GUST_A_Q15
              * ((int32_t)noise_white(&b->grng) - (int32_t)b->walk[i])) >> 15;
        if (w > AVAS_TYPE_LB_CK_NOISE_GUST_WALK_LIM) {
            w = AVAS_TYPE_LB_CK_NOISE_GUST_WALK_LIM;
        } else if (w < -AVAS_TYPE_LB_CK_NOISE_GUST_WALK_LIM) {
            w = -AVAS_TYPE_LB_CK_NOISE_GUST_WALK_LIM;
        }
        b->walk[i] = (int16_t)w;

        /* dB to linear, to first order: 10^(x/20) ~= 1 + ln(10)/20 * x, with K carrying
         * the 1.5 dB per sd.  A pow() per band per block is not affordable and would not
         * be more correct -- the target is a modulation SD, and that is what is
         * measured (1.56 dB against 1.5 dB). */
        m = ((int32_t)AVAS_TYPE_LB_CK_NOISE_GUST_K_Q15 * w) >> 15;
        g = (int32_t)s_ck_type_lb_noise_g_q15[i];
        g += (g * m) >> 15;
        if (g < 0L) {
            g = 0L;                 /* a gust deep enough to invert a band is silence */
        } else if (g > 32767L) {
            g = 32767L;
        }
        b->gain[i] = (int16_t)g;
    }
}

int16_t avas_noise_bank_ck_sample(avas_noise_bank_ck_t *b)
{
    int32_t acc = 0L;
    int16_t w;
    uint16_t i;

    /* The source tilt.  It exists because a 2nd-order skirt falls at 6 dB/octave and
     * this target falls at 12.4 dB/octave, so NO vector of gains can fix it -- fitting
     * white-driven bands left five gains at exactly zero and the composite was still
     * +27 dB too loud at 9 kHz.  A gain vector cannot fix a slope.  Tilting the source
     * costs these three instructions ONCE for the whole bank.
     *
     * This is the leaky-integrator form, not the model's `pre += (a*((x<<15)-pre))>>15`:
     * that product is 43 bits and does not exist in int32 on any machine.  Same pole,
     * same unity DC gain, one 16x16 multiply. */
    b->pre += (int32_t)AVAS_TYPE_LB_CK_NOISE_TILT_A_Q15
              * ((int32_t)noise_white(&b->rng) - (int32_t)(int16_t)(b->pre >> 15));
    w = (int16_t)(b->pre >> 15);

    for (i = 0u; i < AVAS_TYPE_LB_CK_NOISE_BANDS; i++) {
        int16_t bd = b->band[i];
        int16_t f = s_ck_type_lb_noise_f_q15[i];
        int32_t lo;
        int32_t hi;

        /*  low  += F*band
         *  high  = in - low - Q1*band
         *  band += F*high
         *
         * `high` is int32_t and that is deliberate.  Its OBSERVED peak is 20 451 over
         * 20 s, which would fit an int16 -- but its arithmetic bound is
         * |w| + |low| + |Q1*band| ~ 54 800, so int16 here would be trusting a 1.6x
         * margin from one 20 s realisation instead of a bound.  A wrap would inject a
         * full-scale impulse into a NOISE signal, which is precisely the defect an ear
         * cannot audit.  int32 also makes this line exactly the reference model's, whose
         * `hi` is an unbounded Python int -- so the cost is a 16x32 multiply, about two
         * instructions a band, and bit-exactness comes free with it.
         *
         * `low` and `band` ARE int16_t, and that is not the same gamble: the reference
         * model wraps them to 16 bits explicitly, so truncation here is the specified
         * behaviour rather than an overflow.  The generator additionally measures that
         * they never get there (16 925 and 13 655 of 32 767). */
        lo = (int32_t)b->low[i] + (((int32_t)f * (int32_t)bd) >> 15);
        b->low[i] = (int16_t)lo;
        hi = (int32_t)w - lo
             - (((int32_t)AVAS_TYPE_LB_CK_NOISE_Q1_Q15 * (int32_t)bd) >> 15);
        bd = (int16_t)((int32_t)bd + (((int32_t)f * (int32_t)hi) >> 15));
        b->band[i] = bd;

        acc += (int32_t)b->gain[i] * (int32_t)bd;
    }
    acc >>= 15;

    /* Counts -> the engine's A_SCALE units, so the caller can simply ADD this to the
     * carrier sum.  The gain is k * A_SCALE, both measured: there is no free parameter
     * anywhere in this file's level. */
    return (int16_t)((acc * (int32_t)AVAS_TYPE_LB_CK_NOISE_GAIN_Q15)
                     >> AVAS_TYPE_LB_CK_NOISE_GAIN_SHIFT);
}
