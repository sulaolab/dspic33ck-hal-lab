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

/* THE ACCUMULATOR SHIM.  The per-sample loop is written once, on the dsPIC's two
 * 40-bit DSP accumulators, and this block says what each step means so the host can
 * run the SAME loop source (tools/host_check/run_noise_check.py compiles this file).
 *
 * Target facts (read, not assumed -- the same three avas_synth_line_ck.c documents):
 * CORCON at reset is fractional (IF = 0), no accumulator saturation (SATA/SATB = 0,
 * 40-bit wrap), saturating store (SATDW = 1).  So `mpy`/`mac`/`msc` add 2*x*y, and
 * `sac acc, #0` stores acc[31:16] = floor(sum / 2^15) -- the model's floor shift --
 * clamped to int16.  The host model below is exactly that, in int64_t.
 *
 * Every operand here is a coefficient in [0, 32767] or a signed int16 state, so the
 * one fractional product that overflows (-32768 * -32768) cannot occur. */
#if defined(__XC_DSC__) || defined(__XC16__)
#pragma GCC diagnostic ignored "-Wvolatile-register-var"
#define NB_ACC(name, reg)   volatile register int16_t name asm(reg)
#define NB_CLR(acc)         ((acc) = __builtin_clr())
#define NB_MPY(acc, x, y)   ((acc) = __builtin_mpy_16((x), (y)))
#define NB_MAC(acc, x, y)   ((acc) = __builtin_mac_16((acc), (x), (y)))
#define NB_MSC(acc, x, y)   ((acc) = __builtin_msc_16((acc), (x), (y)))
#define NB_SAC(acc)         ((int16_t)__builtin_sac_16((acc), 0))
#else
#define NB_ACC(name, reg)   int64_t name
#define NB_CLR(acc)         ((acc) = 0)
#define NB_MPY(acc, x, y)   ((acc) = 2 * (int64_t)(x) * (int64_t)(y))
#define NB_MAC(acc, x, y)   ((acc) += 2 * (int64_t)(x) * (int64_t)(y))
#define NB_MSC(acc, x, y)   ((acc) -= 2 * (int64_t)(x) * (int64_t)(y))
#define NB_SAC(acc)         nb_sac_host(acc)
static int16_t nb_sac_host(int64_t acc)
{
    int64_t v = acc >> 16;      /* arithmetic, as the stored bits [31:16] are */

    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}
#endif

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
    NB_ACC(t, "A");             /* per-band scratch */
    NB_ACC(acc, "B");           /* the gain-weighted sum, across all bands */
    const int16_t *fp = s_ck_type_lb_noise_f_q15;
    const int16_t *gp = b->gain;
    int16_t *lop = b->low;
    int16_t *bdp = b->band;
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

    NB_CLR(acc);
    for (i = 0u; i < AVAS_TYPE_LB_CK_NOISE_BANDS; i++) {
        const int16_t f  = *fp++;
        const int16_t bd = *bdp;
        const int16_t lo = *lop;
        int16_t fb;
        int16_t qb;

        /*  low  += F*band
         *  high  = in - low - Q1*band
         *  band += F*high
         *
         * `high` is never formed, and that is how its width stops mattering.  Its
         * arithmetic bound is |w| + |low| + |Q1*band| ~ 54 800 -- past int16, which is
         * why the C form held it in int32_t and paid a 16x32 multiply per band.  But
         * F*high distributes:
         *
         *   F*high = F*w - F*low_old - F*fb - F*qb,   fb = F*band>>15, qb = Q1*band>>15
         *
         * four 16x16 products summed EXACTLY in the 40-bit accumulator, then one
         * floor shift -- the model's `(fq * hi) >> 15` with hi an unbounded integer,
         * bit for bit.  |2*F*high| <= 2*8874*54800 < 2^31, so the sac neither
         * saturates nor wraps.  `low` and `band` are int16_t and wrap on store, which
         * is the model's specified behaviour (it wraps them explicitly), and the
         * generator measures they never get there (16 925 and 13 655 of 32 767).
         *
         * Before this (2026-10-14) the loop was ~43 instructions a band and the
         * Type_LB block ISR ran out of its 667 us with the bank on. */
        NB_MPY(t, f, bd);
        fb = NB_SAC(t);
        NB_MPY(t, AVAS_TYPE_LB_CK_NOISE_Q1_Q15, bd);
        qb = NB_SAC(t);
        *lop++ = (int16_t)((uint16_t)lo + (uint16_t)fb);
        NB_MPY(t, f, w);
        NB_MSC(t, f, lo);
        NB_MSC(t, f, fb);
        NB_MSC(t, f, qb);
        *bdp = (int16_t)((uint16_t)bd + (uint16_t)NB_SAC(t));

        NB_MAC(acc, *gp++, *bdp++);
    }

    /* Counts -> the engine's A_SCALE units, so the caller can simply ADD this to the
     * carrier sum.  The gain is k * A_SCALE, both measured: there is no free parameter
     * anywhere in this file's level.
     *
     * sac B is the old `acc >>= 15` with a clamp at int16.  The generator's peak is
     * 18 503 counts; past 32 767 the old form would have wrapped in the multiply
     * below, so the clamp only replaces a wrap that was never reached. */
    return (int16_t)(((int32_t)NB_SAC(acc) * (int32_t)AVAS_TYPE_LB_CK_NOISE_GAIN_Q15)
                     >> AVAS_TYPE_LB_CK_NOISE_GAIN_SHIFT);
}
