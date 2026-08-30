/* -------------------------------------------------------------------------
 * COST PROBE: what a per-carrier complex ROTATOR costs, in generated code.
 *
 * The order of work ends with:
 *
 *     rotator Q20 -- quality is cleared, so re-cost it: Q20 state does not fit
 *     a 16x16 multiply, which is what section 8 rejected it for.  If the
 *     increment can stay Q15 while the state is Q20, it is a 32x16 and the
 *     arithmetic is worth re-deriving; if not, it stays closed on cost.
 *
 * "Re-deriving" it on paper is what section 8 did, and section 18's peeled loop
 * is the standing reminder that this compiler's output is not derivable.  So
 * this file asks the compiler instead: four carrier loops, same shape, same
 * flags, counted the same way section 15 counted the shipped one.
 *
 * The two TAB_ variants are not padding -- they are the CALIBRATION.  They
 * reproduce the shipped rect and polar loops, whose bodies are independently
 * measured at 36 and 21 instructions (section 18's table).  If they come out at
 * those numbers, the rotator counts from the same harness can be compared
 * against them; if they do not, nothing here means anything.
 *
 * NOT part of the firmware: nothing includes this, and the tables below are
 * `extern` declarations so no data is emitted.  Compile with -S and count.
 *
 *   PROBE=1  TAB_RECT   the shipped rect loop:  two lookups, mac + msc
 *   PROBE=2  TAB_POLAR  the shipped polar loop: one lookup,  mac
 *   PROBE=3  ROT_Q15    rotator, Q15 int16 state -- section 8's cheap version,
 *                       the one the model measures as collapsing to zero
 *   PROBE=4  ROT_Q20    rotator, Q20 int32 state, written in portable C
 *                       ((int64_t)a * b >> 20) -- what a first draft writes
 *   PROBE=5  ROT_Q20    the same arithmetic with the 32x16 SPLIT BY HAND, which is
 *                       the multiply the doc's re-cost question is actually about
 *   PROBE=6  ROT_Q15    rotator paired with POLAR's single accumulator, i.e. the
 *                       cheapest rotator that can exist here -- half the state and
 *                       one mac, with the sine simply discarded
 *
 * The rotator is paired with the RECT envelope in 3, 4 and 5, which is the fair
 * pairing: a rotator produces cos AND sin in the same update, so it replaces rect's
 * two lookups.  PROBE=6 measures the unfair one anyway, because "the sine is wasted"
 * is a reason to expect a loss, not a measurement of one.
 * ------------------------------------------------------------------------- */
#include <stdint.h>

#define CLUSTERS   11u
#define TABBITS    9
#define IDXSHIFT   (16 - TABBITS)      /* 16-bit phase: idx is the top TABBITS */

typedef struct { int16_t v; int16_t d; } sin_ent_t;

/* Declared, never defined: the probe measures code, not data. */
extern const sin_ent_t s_sin_dq15[(1 << TABBITS) + 1];

#define COS_AT(e)  ((e) + (1 << (TABBITS - 2)))

/* The interpolated lookup, byte for byte the engine's own (avas_synth_line_ck.c's
 * tab_at + MULSU): a signed-times-unsigned product whose >> 16 is free.
 *
 * `__builtin_mulsu` and not `(int32_t)d * (int32_t)frac` -- the doc's MULSU note
 * says the portable spelling costs five instructions instead of one, so writing it
 * portably here would inflate BOTH calibration loops and quietly recalibrate the
 * harness against the wrong baseline. */
static inline int16_t tab_at(const sin_ent_t *e, uint16_t frac)
{
    return (int16_t)(e->v + (int16_t)(__builtin_mulsu(e->d, frac) >> 16));
}

#if (PROBE == 1) || (PROBE == 2) || (PROBE == 7) || (PROBE == 8)
/* ---- the two calibration loops, and V3 ------------------------------------- */
/*
 * PROBE=7 and 8 are the same two loops with a 16-BIT envelope accumulator, i.e.
 * section 19's V3 measured on its COST side.  The quality side is the model's
 * (tools/avas_type_ty_env16_study.py); this is the other half of the same question,
 * and it is here rather than in the engine because V3 is not implemented -- the
 * point is to find out what it would be worth before writing it.
 *
 * With the fraction bits gone, `ei >> 16` is not a shift but an identity: the state
 * word IS the Q15 the mac wants.  That is what V3 buys, on top of the narrower
 * loads and the missing `addc`.
 */
#if (PROBE == 7) || (PROBE == 8)
typedef int16_t env_t;
#define ENV_Q15(x)  (x)                 /* the state already IS Q15 */
#else
typedef int32_t env_t;
#define ENV_Q15(x)  ((int16_t)((x) >> 16))
#endif

typedef struct {
    uint16_t phase;
#if (PROBE == 2) || (PROBE == 8)
    uint16_t step_eff;
    uint16_t phi_app;
    env_t    amp;
    env_t    amp_da;
#else
    uint16_t step;
    env_t    env_i;
    env_t    env_q;
    env_t    env_di;
    env_t    env_dq;
#endif
} car_t;

int32_t probe_carriers(car_t *car)
{
    volatile register int16_t acc asm("A");
    car_t *c = car;
    uint16_t k;

    acc = __builtin_clr();
    _Pragma("GCC unroll 1")
    for (k = 0u; k < CLUSTERS; k++) {
        const uint16_t ph  = c->phase;
        const uint16_t idx = (uint16_t)(ph >> IDXSHIFT);
        const uint16_t fr  = (uint16_t)(ph << TABBITS);
        const sin_ent_t *e = &s_sin_dq15[idx];
#if (PROBE == 2) || (PROBE == 8)
        const env_t    ei  = c->amp;
        const int16_t  i15 = ENV_Q15(ei);
        c->phase = (uint16_t)(ph + c->step_eff);
        c->amp   = (env_t)(ei + c->amp_da);
        acc = __builtin_mac_16(acc, i15, tab_at(COS_AT(e), fr));
#else
        const env_t    ei  = c->env_i;
        const env_t    eq  = c->env_q;
        const int16_t  i15 = ENV_Q15(ei);
        const int16_t  q15 = ENV_Q15(eq);
        c->phase = (uint16_t)(ph + c->step);
        c->env_i = (env_t)(ei + c->env_di);
        c->env_q = (env_t)(eq + c->env_dq);
        acc = __builtin_mac_16(acc, i15, tab_at(COS_AT(e), fr));
        acc = __builtin_msc_16(acc, q15, tab_at(e, fr));
#endif
        c++;
    }
    return (int32_t)__builtin_sac_16(acc, 0);
}

#else
/* ---- the rotator ---------------------------------------------------------- */
/*
 *   [c']   [ C  -S ] [c]
 *   [s'] = [ S   C ] [s]        C = cos(step), S = sin(step), both Q15
 *
 * No table, no phase accumulator, no interpolation -- and no index arithmetic,
 * which is the part section 8 did not count when it rejected this.  What it buys
 * back is four multiplies where the table pays two, plus the state's own width.
 *
 * (C, S) is per-carrier state rather than a constant because the shipped engine
 * re-derives the step every rebuild in polar (step_eff) and could in rect: a
 * rotator can only change frequency by changing these two, which is a per-block
 * cost the loop never sees.
 */
#if (PROBE == 3) || (PROBE == 6)
typedef int16_t rot_t;                  /* Q15 state, 16x16 products */
#define ROTSHIFT 15
#else
typedef int32_t rot_t;                  /* Q20 state, 32x16 products */
#define ROTSHIFT 20
#endif

/* PROBE=5: the 32x16 written OUT, because PROBE=4's `(int64_t)a * b` is not the
 * multiply the doc asked about -- it is a 64-bit library call, and rejecting Q20 on
 * that spelling would be rejecting the spelling, not the arithmetic.
 *
 *   a (int32, Q20) * b (int16, Q15), then >> 20:
 *     split a into a signed high word and an unsigned low word;
 *     hi = ahi * b            -- one mul.ss, and it stands for hi * 2^16
 *     lo = alo * b            -- one mul.su, the part below that
 *     (hi + (lo >> 16)) >> 4  -- the >>16 is "use the high word", so it is free,
 *                                and the >>4 is what is left of the >>20
 * Truncating lo's low 16 bits loses < 1 LSB of the Q20 result, which is 2^-20 of
 * full scale: below the Q15 the product is about to be rounded to anyway. */
#if (PROBE == 5)
static inline int32_t mul_q20_q15(int32_t a, int16_t b)
{
    union { int32_t s32; uint16_t u16[2]; } au;
    au.s32 = a;
    return (__builtin_mulss((int16_t)au.u16[1], b)
            + (int16_t)(__builtin_mulsu(b, au.u16[0]) >> 16)) >> 4;
}
#endif

typedef struct {
    rot_t    cr;             /* cos(theta), Q15 or Q20 */
    rot_t    ci;             /* sin(theta) */
    int16_t  rc;             /* cos(step), Q15 -- the increment */
    int16_t  rs;             /* sin(step), Q15 */
#if (PROBE == 6)
    int32_t  amp;            /* polar's ONE accumulator */
    int32_t  amp_da;
#else
    int32_t  env_i;
    int32_t  env_q;
    int32_t  env_di;
    int32_t  env_dq;
#endif
} car_t;

int32_t probe_carriers(car_t *car)
{
    volatile register int16_t acc asm("A");
    car_t *c = car;
    uint16_t k;

    acc = __builtin_clr();
    _Pragma("GCC unroll 1")
    for (k = 0u; k < CLUSTERS; k++) {
        const rot_t   cr = c->cr;
        const rot_t   ci = c->ci;
        const int16_t rc = c->rc;
        const int16_t rs = c->rs;
#if (PROBE == 6)
        const int32_t ei = c->amp;
#else
        const int32_t ei = c->env_i;
        const int32_t eq = c->env_q;
        const int16_t q15 = (int16_t)(eq >> 16);
#endif
        const int16_t i15 = (int16_t)(ei >> 16);

        /* The rotation.  At Q15 both products are 16x16 and the >> 15 is the
         * high word.  At Q20 each product is 32x16, which this core does not
         * have: the compiler must split it, and how it splits it is the number
         * this probe exists to obtain. */
#if (PROBE == 3) || (PROBE == 6)
        c->cr = (rot_t)(((int32_t)cr * rc - (int32_t)ci * rs) >> ROTSHIFT);
        c->ci = (rot_t)(((int32_t)cr * rs + (int32_t)ci * rc) >> ROTSHIFT);
#elif (PROBE == 5)
        c->cr = mul_q20_q15(cr, rc) - mul_q20_q15(ci, rs);
        c->ci = mul_q20_q15(cr, rs) + mul_q20_q15(ci, rc);
#else
        c->cr = (rot_t)(((int64_t)cr * rc - (int64_t)ci * rs) >> ROTSHIFT);
        c->ci = (rot_t)(((int64_t)cr * rs + (int64_t)ci * rc) >> ROTSHIFT);
#endif
#if (PROBE == 6)
        c->amp = ei + c->amp_da;
#else
        c->env_i = ei + c->env_di;
        c->env_q = eq + c->env_dq;
#endif

        /* Q15 cos/sin for the output product, which is where the state width is
         * paid for a second time: a Q20 state has to come DOWN to Q15 to feed a
         * 16x16 mac. */
        acc = __builtin_mac_16(acc, i15, (int16_t)(cr >> (ROTSHIFT - 15)));
#if (PROBE != 6)
        acc = __builtin_msc_16(acc, q15, (int16_t)(ci >> (ROTSHIFT - 15)));
#endif
        c++;
    }
    return (int32_t)__builtin_sac_16(acc, 0);
}

/* Renormalisation, once per carrier per REBUILD (not per sample): the rotator's
 * magnitude drifts, and this is the cost that has no counterpart in the table
 * scheme at all.  Counted separately and amortised by DEC = 32. */
void probe_renorm(car_t *car)
{
    car_t *c = car;
    uint16_t k;

    for (k = 0u; k < CLUSTERS; k++) {
        const rot_t cr = c->cr;
        const rot_t ci = c->ci;
#if (PROBE == 3) || (PROBE == 6)
        const int32_t m2  = ((int32_t)cr * cr + (int32_t)ci * ci) >> ROTSHIFT;
        const int32_t err = (((int32_t)1 << ROTSHIFT) - m2) >> 1;
        c->cr = (rot_t)(cr + (rot_t)(((int32_t)cr * err) >> ROTSHIFT));
        c->ci = (rot_t)(ci + (rot_t)(((int32_t)ci * err) >> ROTSHIFT));
#elif (PROBE == 5)
        /* Same shape, with the Q20 operand reduced to Q15 for the second factor so
         * every product is the split 32x16 rather than a 32x32. */
        const int32_t m2  = mul_q20_q15(cr, (int16_t)(cr >> 5))
                          + mul_q20_q15(ci, (int16_t)(ci >> 5));
        const int32_t err = (((int32_t)1 << ROTSHIFT) - m2) >> 1;
        c->cr = cr + mul_q20_q15(cr, (int16_t)(err >> 5));
        c->ci = ci + mul_q20_q15(ci, (int16_t)(err >> 5));
#else
        const int32_t m2  = (int32_t)(((int64_t)cr * cr + (int64_t)ci * ci)
                                      >> ROTSHIFT);
        const int32_t err = (((int32_t)1 << ROTSHIFT) - m2) >> 1;
        c->cr = (rot_t)(cr + (rot_t)(((int64_t)cr * err) >> ROTSHIFT));
        c->ci = (rot_t)(ci + (rot_t)(((int64_t)ci * err) >> ROTSHIFT));
#endif
        c++;
    }
}
#endif
