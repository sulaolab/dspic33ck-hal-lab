/*
 * Host harness: does masking the ENVELOPE part off actually FREEZE the envelope?
 *
 * The header promises it does -- "env_* keeps whatever it held and env_d* stays
 * zero -- a frozen complex amplitude per cluster, i.e. pure tones".  That is only
 * true if the slope happened to be zero when the mask changed, which is the case
 * straight after a reset and NOT the case if the full engine has been running.
 * The rebuild stops; `env_i += env_di` in the carrier loop does not.
 *
 * This test is the difference between the two orders, because on a speaker a
 * runaway envelope and a wrong coefficient table both just sound wrong:
 *
 *   order A   set_parts(CARRIERS|GATE) from a fresh reset, then render
 *   order B   render with everything on FIRST, then set_parts(CARRIERS|GATE)
 *
 * A frozen envelope means each cluster's |Z| stops moving.  So the test prints,
 * per cluster, how far env_i/env_q travel over a second AFTER the mask was
 * applied.  Zero is the promise; anything large is the slope still running.
 *
 * POLAR (AVAS_TYPE_TY_CK_ENVINTERP == 1) NEEDS A SECOND COLUMN, and that is the point
 * of extending this rather than writing a new test.  Polar's envelope is a magnitude
 * AND a phase, and the phase does not live in a slope variable -- it is folded into
 * the carrier's step.  So zeroing amp_da alone would freeze the magnitude and leave
 * the envelope ROTATING: a detuned carrier, audibly wrong, and completely invisible
 * to the |dZ| column above.  `step off` is that failure, measured directly: how far
 * each carrier's effective step still sits from the nominal one.
 */
#include <stdio.h>
#include <stdlib.h>

#include "avas_synth_line_ck.h"

#define FS 48000L

static void render(avas_line_ck_t *s, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        (void)avas_line_ck_render_sample(s);
    }
}

/* The interpolated envelope quantity the carrier loop multiplies by, as Q15 -- read
 * with the engine's own macro so this test does not restate the state's width (it is
 * the top 16 bits of an int32 at the shipped ENVFRAC, and the whole word at V3's). */
static int32_t env_q15(const avas_line_ck_car_t *c)
{
#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
    return (int32_t)AVAS_TYPE_TY_CK_ENV_Q15(c->amp);
#else
    return (int32_t)AVAS_TYPE_TY_CK_ENV_Q15(c->env_i);
#endif
}

/* How far this carrier's effective step still sits from the nominal one, i.e. how
 * much envelope rotation is left in it.  Always 0 in rect, where the envelope's
 * phase is not carried by the step at all. */
static long step_offset(const avas_line_ck_car_t *c, uint16_t k)
{
#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
    return (long)(avas_type_ty_ck_carph_signed_t)
               (avas_type_ty_ck_carph_t)(c->step_eff - s_ck_car_step[k]);
#else
    (void)c;
    (void)k;
    return 0L;
#endif
}

/* Largest envelope excursion across the clusters over n samples, plus the peak
 * output, which is what a listener actually hears. */
static void measure(const char *label, avas_line_ck_t *s, long n)
{
    int32_t  start[AVAS_TYPE_TY_CK_CLUSTERS];
    int32_t  worst = 0;
    int32_t  peak  = 0;
    long     worst_step = 0;
    uint16_t k;
    long     i;

    for (k = 0u; k < AVAS_TYPE_TY_CK_CLUSTERS; k++) {
        start[k] = env_q15(&s->car[k]);
    }

    for (i = 0; i < n; i++) {
        int32_t y = (int32_t)avas_line_ck_render_sample(s);
        if (y < 0) { y = -y; }
        if (y > peak) { peak = y; }
    }

    for (k = 0u; k < AVAS_TYPE_TY_CK_CLUSTERS; k++) {
        int32_t d = env_q15(&s->car[k]) - start[k];
        long    o = step_offset(&s->car[k], k);
        if (d < 0) { d = -d; }
        if (o < 0) { o = -o; }
        if (d > worst) { worst = d; }
        if (o > worst_step) { worst_step = o; }
    }

    printf("  %-34s worst |dZ| over %ld samples = %7ld Q15   out peak = %6ld"
           "   step off = %6ld\n",
           label, n, (long)worst, (long)peak, worst_step);
}

int main(void)
{
    static avas_line_ck_t s;
    const long sec = FS;

    printf("frozen-envelope check (parts = CARRIERS|GATE = 5), envinterp=%d,"
           " phase %d/%d\n", (int)AVAS_TYPE_TY_CK_ENVINTERP,
           (int)AVAS_TYPE_TY_CK_PHBITS_CAR, (int)AVAS_TYPE_TY_CK_PHBITS_BB);

    /* Order A: mask applied from a fresh reset, where env_d* really is zero. */
    avas_line_ck_init(&s);
    avas_line_ck_set_parts(&s, AVAS_TYPE_TY_CK_PART_CARRIERS | AVAS_TYPE_TY_CK_PART_GATE);
    measure("A: frozen from reset", &s, sec);

    /* Order B: the full engine runs first, so a slope is live when the mask lands.
     * This is what *tb0005 does on a board that is already playing. */
    avas_line_ck_init(&s);
    avas_line_ck_set_parts(&s, AVAS_TYPE_TY_CK_PART_ALL);
    render(&s, sec);
    avas_line_ck_set_parts(&s, AVAS_TYPE_TY_CK_PART_CARRIERS | AVAS_TYPE_TY_CK_PART_GATE);
    measure("B: frozen after running full", &s, sec);

    /* For scale: what the envelope legitimately does with the rebuild ON. */
    avas_line_ck_init(&s);
    avas_line_ck_set_parts(&s, AVAS_TYPE_TY_CK_PART_ALL);
    render(&s, sec);
    measure("(reference) full engine", &s, sec);

    return 0;
}
