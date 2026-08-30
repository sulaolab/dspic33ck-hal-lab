/*
 * avas_synth_line_ck.c -- see the header for what this is, what it ports, and how it
 * was verified.  This file is the arithmetic; nothing here touches a register,
 * a pin or a peripheral, which is what lets tools/host_check/ compile it for the
 * host and prove it bit-identical to the model that measured the design.
 *
 * ON THE OPTIMISATION ATTRIBUTES
 * ------------------------------
 * The two hot functions carry __attribute__((optimize("O3"))) rather than
 * relying on the project's level, and that is load-bearing, not a flourish:
 *
 *   - Measured on this repo's hardware (docs/ck_silicon_findings.md, Part 5), a
 *     tight table loop costs 52.7 us at -Os against 16.5 us at -O3 -- 3.2x.  The
 *     estimate for this engine was ~46 % of the block budget at -O3, which becomes
 *     an overrun at -Os. That estimate HELD: the engine's measured increment is
 *     ~289 us of a 667 us block, so -Os would still overrun.
 *     It is not "faster if convenient".
 *   - The project cannot simply be built at -O3: CK64MC105 has 64 KB and -Os ->
 *     -O3 grew the loopback config by 57 %, which does not fit.
 *   - Verified on XC-DSC 3.31.01 for 33CK64MC105: with the project at -Os the
 *     attribute produces code identical to a real -O3 build of the same file
 *     apart from register allocation.  Without it, -Os builds a stack frame and
 *     a measurably worse loop.
 *
 * If a future toolchain drops support for the attribute the build stays correct
 * and silently gets slow, so the load figure is the thing to watch, not the
 * build log.
 */

#include "avas_synth_line_ck.h"

#include <string.h>

#if (AVAS_LINE_CK_HAVE_ENGINE)

/* Only pulled in here: the debug build below (AVAS_CK_VOICE_NONE) has no rebuild
 * loop to feed a ratio into, and an unused static table there would be a warning
 * the -Os build has no ROM to spare arguing with. */
#include "avas_pitch_ck_table.h"

/* ON noinline FOR THE TWO HOT FUNCTIONS
 * -------------------------------------
 * These routines run on every audio block. Keeping each as one compiler-owned
 * out-of-line body avoids a second copy of the inner loop and preserves the
 * measured O3 execution shape in the size-constrained target image. The cost is
 * one call per sample for the carriers, ~4 cycles of 2048.
 */
#if defined(__GNUC__)
#define AVAS_TYPE_TY_CK_ONE_COPY  __attribute__((noinline))
#else
#define AVAS_TYPE_TY_CK_ONE_COPY
#endif

#if defined(__GNUC__)
/* File scope rather than per function, so the `static inline` sine lookup is
 * also compiled at O3 AT ITS INLINE SITES. Marking only the three hot functions
 * left the per-sample wrappers -- and every inline expansion inside them -- at
 * the project's -Os, which was the first suspect when the measured cost came in
 * at 2.5x the estimate. (It was not the cause; see the measurement note at the
 * bottom of this comment block. Kept because it is still the correct scope.) */
#pragma GCC optimize("O3")
#define AVAS_TYPE_TY_CK_HOT  __attribute__((optimize("O3")))
#else
#define AVAS_TYPE_TY_CK_HOT
#endif

/* ON THE DSP ACCUMULATOR (AVAS_TYPE_TY_CK_ACC_MODE)
 * ----------------------------------------------
 * dsPIC33CK has two 40-bit accumulators with single-cycle mac/msc and a
 * store-with-shift-and-round (sac/sac.r).  Both of this engine's hot loops ARE
 * MAC chains, and mode 0 -- what shipped until section 14 -- does not use the unit
 * at all: measured in the listing, zero mac/msc/sac against 51 general multiplies.
 * Three modes so the change is verifiable rather than merely faster:
 *
 *   0  LEGACY, AND NO LONGER THE DEFAULT -- see WHY MODE 0 IS KEPT below.  int32
 *      accumulator, every term truncated by TERMSHIFT before it is summed.  What
 *      shipped first, and what the early quality figures were measured on.
 *   1  REFERENCE.  The 40-bit semantics written in portable C (int64 sum, ONE
 *      shift at the end).  This is the DEFINITION of correctness for mode 2 --
 *      tools/host_check/ compiles it for the host and proves it against the
 *      numpy model.  Never build this for the target: an int64 accumulate drags
 *      in ___muldi3, measured at 88 cycles/sample on this core.
 *   2  DSP ACCUMULATOR.  mac/msc into A (and B), one sac at the end.  Same
 *      arithmetic as mode 1, done by the hardware.
 *
 * WHY MODE 1 AND 2 ARE NOT BIT-EQUAL TO MODE 0, DELIBERATELY.  Mode 0 truncates
 * each of the 22 products before summing; 40 bits holds the untruncated sum
 * (provable bound 11 * 2^31 ~= 2^34.5, and fractional mode doubles it to 2^35.5,
 * so 39 bits with 3.5 spare).  Accumulating without truncating is strictly MORE
 * accurate, so the output changes and AVAS_TYPE_TY_CK_TERMSHIFT stops being needed.
 *
 * WHY MODE 0 IS KEPT, NOW THAT MODE 2 IS THE DEFAULT.  Deleting it -- and TERMSHIFT
 * with it -- was the plan (section 13 step 2) and is deliberately not what happened,
 * so the reason belongs here and not only in the history.  Mode 0 is the only
 * variant that is both PORTABLE and AFFORDABLE.  Mode 1 is portable, but its own
 * warning above says never to build it for the target, and the target measurement
 * agrees: 479.5 us against mode 0's 455.5 us, about 75 cycles per sample, which is
 * the int64 cost that warning predicts.  So a 16-bit core or toolchain that cannot
 * reach the accumulator -- XC16, a future part, a compiler that drops the builtins
 * -- has exactly one cheap path, and this is it.
 *
 * Keeping it is close to free, which is the other half of the decision: an
 * unselected mode generates no code, and mode 0 stays PROVED rather than merely
 * present, because tools/host_check/run_host_check.py takes ACC_MODE 0 as its
 * default and so proves `model == mode 0` on every argument-free run.  What the
 * flip leaves unexercised is only mode 0's TARGET build -- a compile risk, not an
 * arithmetic one.  If that ever stops being worth the three places TERMSHIFT lives
 * (here, the generator, the model), delete all three together: a mode kept without
 * its proof would be worse than no mode at all.
 *
 * THE THREE SILICON FACTS THIS DEPENDS ON, all read rather than assumed:
 *
 *   - CORCON's reset value is 0x0020 (DFP edc:por, DSPIC33CK64MC105.PIC) and
 *     the C runtime only writes CORCON when __enable_fixed is set, which this
 *     project does not use -- so IF = 0 (FRACTIONAL multiply mode), RND = 0
 *     (convergent rounding), SATA = 0 (accumulator A does NOT saturate; it
 *     wraps at 40 bits, which the bound above says it never reaches) and
 *     SATDW = 1 (a sac store to data space DOES saturate).
 *   - Fractional mode makes mac compute A += (a*b) << 1, so A holds twice the
 *     integer sum.  `sac A, #0` stores A[31:16], i.e. A >> 16 = sum >> 15 --
 *     which is exactly this engine's Q15 output scaling.  The doubling is
 *     absorbed for free; no shift instruction is spent on it.
 *   - The compiler pushes and pops ACCAL/ACCAH/ACCAU around any function that
 *     binds an accumulator, so the hazard that made this look like an assembly
 *     project -- A and B are not shadowed on interrupt entry -- is handled by the
 *     toolchain, not by us. Verified in the listing.
 *
 * The saturating sac is a SAFETY IMPROVEMENT, not just a shortcut.  Mode 0's
 * provable output bound is 11 * 32767 = 360437, which the caller then multiplies
 * by NORM_GAIN_Q15 -- that product overflows int32, so mode 0 is only safe
 * because the shipped table's peak is 2.8 % of the headroom.  Mode 2 clamps at
 * the store instead, which is a bound that holds for any table.
 *
 * THE IDIOM, because it took three wrong guesses (DS-50003589C 28.2.9/.41/.56):
 *   - `volatile register int16_t a asm("A")`.  WITHOUT volatile the compiler
 *     rejects it ("Argument 0 should be an accumulator register") at every level
 *     except -O0.
 *   - the accumulator is argument ZERO.  builtins_16.h declares mac_16 as
 *     (int16_t, int16_t, int16_t), which reads like (a, b, acc) and is really
 *     (acc, a, b); passing it last gives "Accumulator register inappropriately
 *     passed".
 */
#ifndef AVAS_TYPE_TY_CK_ACC_MODE
#if defined(__XC_DSC__) || defined(__XC16__)
/*
 * The target default since the hardware gate closed: `sink=` agrees with mode 1 bit
 * for bit, the block ISR measures 404.8 us of its 667 us with miss = 0, and the ear
 * cannot separate it from sonora (section 14).  XC16 shares this arm because the
 * accumulator guard below already treats the two toolchains alike -- no XC16 build
 * exists in this project, and one would owe its own mode 1 == mode 2 check on that
 * silicon before trusting this default, exactly as this one did.
 */
#define AVAS_TYPE_TY_CK_ACC_MODE  2
#else
/*
 * Host: the REFERENCE semantics.  Mode 2 cannot be compiled here at all (the #error
 * below says so) and mode 1 is what defines it, so a host build that says nothing
 * should get the definition rather than the legacy.  This arm decides nothing for
 * the verification chain either way -- run_host_check.py passes
 * -DAVAS_TYPE_TY_CK_ACC_MODE explicitly for both of its cases.
 */
#define AVAS_TYPE_TY_CK_ACC_MODE  1
#endif
#endif

#if (AVAS_TYPE_TY_CK_ACC_MODE == 2) && !defined(__XC_DSC__) && !defined(__XC16__)
#error "avas_type_ty_ck: ACC_MODE 2 is the DSP accumulator and needs XC-DSC. Use mode 1 for the host reference."
#endif

#if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
/* `volatile register x asm("A")` earns -Wvolatile-register-var ("optimization may
 * eliminate reads and/or writes to register variables") once per function.  It is
 * not a real risk here and it cannot be avoided: the volatile is exactly what
 * makes the builtins accept the binding above -O0, and the declaration is
 * Microchip's documented idiom.  The generated code is checked rather than
 * trusted -- the accumulator is never spilled and every term becomes one mac or
 * msc, verified in the listing.
 *
 * Suppressed rather than left to shout because this project treats a warning as a
 * defect (tools/host_check/ fails the build on any), so a permanent known-benign
 * warning would blunt that rule for every future real one. */
#pragma GCC diagnostic ignored "-Wvolatile-register-var"
#endif

/* PIN THE CARRIER LOOP AGAINST BEING UNROLLED -- in polar only, and measured.
 *
 * Polar's loop body is small enough that GCC 8.3.1 completely peels all eleven
 * iterations, and that is a LOSS on both counts:
 *
 *   flash   the function goes 37 -> 287 instructions, and the image 62 937 ->
 *           64 689 B (94 % -> 97 %) on a part where section 10.5 already declared
 *           flash the binding constraint.
 *   speed   it does not even buy cycles.  The looped form walks ONE pointer through
 *           the interleaved carrier state; the peeled form has eleven different
 *           offsets, most too large for a displacement, so it re-materialises a base
 *           address per carrier (`mov #510,w3 / add.w w3,w0,w3 / mov.w w3,w7`).
 *           Executed instructions per sample: ~277 looped against 287 peeled.
 *
 * So the peel costs 1 752 B to execute slightly more.  Rect is not peeled at all
 * (its body is over the threshold), which is why this is scoped to polar rather than
 * applied to a loop it cannot affect -- an unchanged rect image is a byte comparison
 * this way instead of an argument.
 *
 * If a future toolchain drops `#pragma GCC unroll`, the build stays correct and
 * grows; flash is what to watch, the same as with the O3 attributes above.
 */
#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1) && defined(__GNUC__) && (__GNUC__ >= 8)
#define AVAS_TYPE_TY_CK_KEEP_LOOP  _Pragma("GCC unroll 1")
#else
#define AVAS_TYPE_TY_CK_KEEP_LOOP
#endif

/* Q31 1.0.  The gate is a fraction, so full scale is INT32_MAX and not 2^31. */
#define AVAS_TYPE_TY_CK_GATE_ONE   (0x7FFFFFFFL)

/* err_q31 >> 16 gives the Q15 error, so the one-pole's shift is this much less. */
#define AVAS_TYPE_TY_CK_GATE_MULSHIFT  (AVAS_TYPE_TY_CK_GATE_ALPHA_SHIFT - 16u)


/* ------------------------------------------------------------------------- *
 * Q15 sine, from a phase SPLIT INTO an index and a fraction.
 *
 * The split is separated from the lookup on purpose.  Every caller wants both
 * sin and cos of the same phase, and at this table geometry cos is the same
 * fraction at a different index -- so splitting once and looking up twice halves
 * the addressing work.  Taking a uint32 phase per lookup, as the first version
 * did, made the compiler recompute the fraction for cos as well, six
 * instructions to arrive at a value it already had.
 *
 * No wrap and no range reduction: the accumulator's overflow IS the modulo,
 * which is the single biggest reason the fixed-point version is cheaper than the
 * float one it ports (audio_fast_wrap_0_to_2pi() disappears entirely).
 *
 * WHY THE FRACTION IS 16 BITS.  Not for accuracy -- measured against 8 bits it
 * is identical on every figure (48.1 dB, -78.2 dBFS).  It is because `>> 16` of a
 * 32-bit product on a 16-bit core is "use the high word", which costs NOTHING,
 * where `>> 8` costs an sl/lsr/ior on the register pair.  Free precision is a
 * side effect.
 *
 * Interpolation itself is not optional.  Measured with the model: 512 entries
 * interpolated gives a -78 dBFS floor in the line-free bands; the same 512
 * entries WITHOUT interpolation gives -67.3 dBFS, which is worse than the AK
 * engine this replaces.  A 1024-entry table without interpolation is still only
 * -74.8 dBFS and costs 2 KB, so trading interpolation for table size loses twice.
 * ------------------------------------------------------------------------- */
#define AVAS_TYPE_TY_CK_PH_IDX(ph_hi)  \
    ((uint16_t)((uint16_t)(ph_hi) >> AVAS_TYPE_TY_CK_IDXSHIFT_HI))

/* The FRACBITS bits below the index, assembled from the two halves of the phase
 * with 16-bit shifts only -- a 32-bit `ph >> FRACSHIFT` is what the first version
 * wrote, and it costs a shift/shift/ior on the pair plus a mask. */
#define AVAS_TYPE_TY_CK_PH_FRAC(ph_hi, ph_lo)                                   \
    ((uint16_t)((uint16_t)((uint16_t)(ph_hi) << AVAS_TYPE_TY_CK_TABBITS)        \
                | (uint16_t)((uint16_t)(ph_lo) >> AVAS_TYPE_TY_CK_IDXSHIFT_HI)))

/* Same fraction from a 16-BIT phase, where there is no second half to join -- and
 * this is the instruction the narrow accumulators are bought with, not just the
 * add they save.
 *
 * A 16-bit phase leaves 16 - TABBITS = 7 bits below the index, so the model's
 * interpolation is `(d * frac7) >> 7`.  Shifting those 7 bits UP to the top of a
 * 16-bit word instead gives exactly the same integer through the free path:
 * (d * (frac7 << 9)) >> 16 = (d * frac7) >> 7 for either sign, because both are
 * floor divisions by the same power of two.  So the `>> 16` stays "use the high
 * word" and the narrower fraction costs no shift -- one lsl replaces the wide
 * form's shift/shift/ior. */
#define AVAS_TYPE_TY_CK_PH16_FRAC(ph)                                           \
    ((uint16_t)((uint16_t)(ph) << AVAS_TYPE_TY_CK_TABBITS))

/* The signed x unsigned 16x16 -> 32 multiply, as ONE instruction.
 *
 * Writing it as `(int32_t)d * (int32_t)frac` in portable C is not enough: XC-DSC
 * widens both operands first and emits a 32x16 sequence (mul.su to sign extend,
 * then mulw.ss + mul.uu + add), five instructions where the core has a single
 * mul.su.  The builtin is the only way to say it.  Verified by instruction count,
 * and tools/host_check/ proves the fallback computes the same integers.
 */
#if defined(__XC_DSC__) || defined(__XC16__)
#define AVAS_TYPE_TY_CK_MULSU(a, b)   __builtin_mulsu((a), (b))
#else
#define AVAS_TYPE_TY_CK_MULSU(a, b)   ((int32_t)(a) * (int32_t)(uint16_t)(b))
#endif

/* Interpolate at a table entry already pointed at.
 *
 * Takes the POINTER rather than the index so that sin and cos -- a fixed
 * quarter-cycle apart -- share one address computation, the cos entry being a
 * constant displacement off the sin entry.  Passing indices made the compiler
 * build the base address twice per lookup. */
static inline int16_t avas_line_ck_tab_at(const avas_type_ty_ck_sin_ent_t *e,
                                          uint16_t frac)
{
    /* One 32-bit aligned pair: the value and the slope to the next entry.  The
     * value-only table needed two loads, two sign extends and a 32-bit subtract
     * to arrive at that slope. */
    return (int16_t)(e->v + (int16_t)(AVAS_TYPE_TY_CK_MULSU(e->d, frac) >> 16));
}

/* Cos is a quarter cycle further along the SAME table.  The table carries
 * TABN/4 wrapped-around entries past the end for exactly this, so the index
 * needs no mask and the cos entry is `e + QUARTER` -- a displacement, not an
 * address computation.  There is no second table, no second phase accumulator,
 * no 32-bit phase add and no added error. */
#define AVAS_TYPE_TY_CK_COS_AT(e)   ((e) + AVAS_TYPE_TY_CK_QUARTER)


/* ------------------------------------------------------------------------- *
 * One cluster's complex envelope Z_k at the current baseband phase, advancing
 * that cluster's baseband oscillators by one decimated step.
 *
 *     Z_k = sum_{j in cluster} A_j * e^{i * bb_phase[j]}
 *
 * A cluster is a contiguous run of table entries (the generator sorts by
 * frequency for exactly this reason), so no per-line cluster index is needed.
 * bb_step is signed: lines below the carrier rotate backwards.
 *
 * The products are summed in Q30 and shifted once at the end.  A plain int32
 * accumulator is provably enough: the amplitude scale was chosen so that every
 * cluster's amplitudes sum to at most Q15 full scale, and |sum A_j e^{i p}| <=
 * sum A_j, so |acc| <= 32767 * 32767 < 2^31.  That bound is the reason the scale
 * is per-worst-cluster rather than global.
 * ------------------------------------------------------------------------- */
AVAS_TYPE_TY_CK_HOT
static void avas_line_ck_eval_cluster(avas_line_ck_t *s, uint16_t k,
                                      int16_t *out_i, int16_t *out_q)
{
    const avas_line_ck_set_t *set = s->set;
    const uint16_t first = set->cluster_first[k];
    const uint16_t n     = (uint16_t)(set->cluster_first[k + 1u] - first);
    /* Three flash tables and one RAM array, all walked by ascending pointer --
     * the same reason the carrier state is interleaved.  Indexing them by i
     * instead makes the compiler rebuild four addresses per line.  The bases now
     * come out of the descriptor instead of being link-time symbols, which is one
     * load per CLUSTER (not per line) on top of the address arithmetic that was
     * already there. */
    avas_type_ty_ck_bbph_t         *php  = &s->bb_phase[first];
    const avas_type_ty_ck_bbstep_t *stp  = &set->bb_step[first];
    const int16_t               *ampp = &set->amp_q15[first];
#if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
    /* acc_i and acc_q ARE what A and B are for.  Mode 0's listing spills both to
     * the stack on every one of the 185 lines -- 8 of the loop's 38 instructions
     * -- because a 32-bit accumulate-in-place needs a W pair this core has not
     * got spare. */
    volatile register int16_t acc_i asm("A");
    volatile register int16_t acc_q asm("B");
#elif (AVAS_TYPE_TY_CK_ACC_MODE == 1)
    int32_t  acc_i = 0;   /* fits int32: bound is 32767*32767 < 2^31 */
    int32_t  acc_q = 0;
#else
    int32_t  acc_i = 0;
    int32_t  acc_q = 0;
#endif
    uint16_t i;

#if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
    acc_i = __builtin_clr();
    acc_q = __builtin_clr();
#endif

    for (i = 0u; i < n; i++) {
        const avas_type_ty_ck_bbph_t ph = *php;
#if ((AVAS_TYPE_TY_CK_PHBITS_BB) == 16)
        /* No high half to name and no low half to join: the phase IS the word. */
        const uint16_t idx = AVAS_TYPE_TY_CK_PH_IDX(ph);
        const uint16_t fr  = AVAS_TYPE_TY_CK_PH16_FRAC(ph);
#else
        const uint16_t hi  = (uint16_t)(ph >> 16);
        const uint16_t idx = AVAS_TYPE_TY_CK_PH_IDX(hi);
        const uint16_t fr  = AVAS_TYPE_TY_CK_PH_FRAC(hi, (uint16_t)ph);
#endif
        const int16_t  amp = *ampp++;
        const avas_type_ty_ck_sin_ent_t *e = &s_ck_sin_dq15[idx];

        /* The cast is the wrap, at either width: the step is signed and the phase
         * is unsigned, so this is add-modulo-2^PHBITS_BB and nothing else.
         *
         * The pitch ratio is folded in right here rather than into a RAM copy of
         * bb_step: this loop already reads *stp from flash once per DEC samples
         * (see the caller), so multiplying it first costs one extra 16x16 and a
         * shift on a read that was never in the per-sample path to begin with.
         * Q14, not Q15 -- unity is 16384, and at 16384 the shift is exact (no
         * rounding), which is what keeps pitch_ratio_q14's default state
         * bit-identical to the no-pitch-API build tools/host_check/ compares
         * against. */
        *php++ = (avas_type_ty_ck_bbph_t)(ph + (avas_type_ty_ck_bbph_t)
            (((int32_t)*stp++ * (int32_t)s->pitch_ratio_q14) >> 14));

#if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
        acc_i = __builtin_mac_16(acc_i, amp,
                                 avas_line_ck_tab_at(AVAS_TYPE_TY_CK_COS_AT(e), fr));
        acc_q = __builtin_mac_16(acc_q, amp, avas_line_ck_tab_at(e, fr));
#else
        acc_i += (int32_t)amp * (int32_t)avas_line_ck_tab_at(
                                            AVAS_TYPE_TY_CK_COS_AT(e), fr);
        acc_q += (int32_t)amp * (int32_t)avas_line_ck_tab_at(e, fr);
#endif
    }

    /* int16 out, and it is lossless: the amplitude scale was chosen so every
     * cluster's amplitudes sum to at most Q15 full scale, hence |acc| <=
     * 32767*32767 and |acc >> 15| <= 32767.  Returning int16 is what makes the
     * caller's `(int32_t)z << 16` free -- on a 16-bit core that is just which
     * register the value sits in, where a 32-bit `z << 16` is a shift pair.
     *
     * Modes 1 and 2 agree with mode 0 EXACTLY here: this loop never truncated a
     * term, so `sac A, #0` -- which stores A[31:16], and A is twice the integer
     * sum in fractional mode -- is the same `>> 15` written a different way.
     * The envelope is where the accumulator costs nothing at all. */
#if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
    *out_i = __builtin_sac_16(acc_i, 0);
    *out_q = __builtin_sac_16(acc_q, 0);
#else
    *out_i = (int16_t)(acc_i >> 15);
    *out_q = (int16_t)(acc_q >> 15);
#endif
}


#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
/* ------------------------------------------------------------------------- *
 * (I,Q) -> (A, phi), a 16-bit vectoring CORDIC.  Eleven of these per rebuild,
 * i.e. eleven per DEC samples, which the model measured at 1-3 % of the block.
 *
 * WHY A CORDIC AND NOT A DIVIDE OR A TABLE.  The angle is needed to about 12 bits
 * (measured -- see the generated header's note) over the full circle, which is a
 * 2-D lookup, and this core has no divide instruction to build an arctan from.  A
 * CORDIC is shifts and adds, which is exactly what it does have.
 *
 * THE ANGLE COMES OUT IN PHASE UNITS, not radians.  The atan table is generated in
 * the carrier phase's own units, so the result drops straight into a phase
 * accumulator: no scaling, and no quadrant logic either, because the accumulator's
 * overflow is the modulo.  That is also why the pre-rotation can be half a turn --
 * exactly representable at any width -- instead of the quarter-turn-and-swap the
 * textbook form uses.
 *
 * THE INPUT IS HALVED FIRST, and that is the one lossy step here.  A vectoring
 * CORDIC lengthens the vector by K = 1.6468, so K*|Z| with |Z| up to 32767 does not
 * fit int16 -- and int16 x/y is the whole reason this is affordable, one asr plus
 * one add per rotation where int32 would cost a shift pair.  The cost is the bottom
 * bit of A, on a quantity whose interpolation error section 16 measures in tens of
 * Q15 counts.  KINV_Q15 with >> 14 undoes the halving and the CORDIC gain together.
 *
 * Bit-for-bit the same integers as the model's to_polar_cordic(), which
 * tools/host_check/ proves sample for sample -- so this is not "a CORDIC", it is
 * THE one the quality figures describe.
 * ------------------------------------------------------------------------- */
AVAS_TYPE_TY_CK_HOT
static void avas_line_ck_to_polar(int16_t zi, int16_t zq, int16_t *out_a,
                                 avas_type_ty_ck_carph_t *out_p)
{
    int16_t x = (int16_t)(zi >> 1);
    int16_t y = (int16_t)(zq >> 1);
    avas_type_ty_ck_carph_t ang = (avas_type_ty_ck_carph_t)0u;
    int32_t  a;
    uint16_t i;

    /* Right half plane, where the rotations converge.  Negating both components is
     * a rotation by half a turn, so the angle correction is exact. */
    if (x < 0) {
        x   = (int16_t)-x;
        y   = (int16_t)-y;
        ang = AVAS_TYPE_TY_CK_CORDIC_HALF;
    }

    for (i = 0u; i < AVAS_TYPE_TY_CK_CORDIC_N; i++) {
        /* Both displacements come from the OLD x and y -- this is a rotation, not
         * two independent updates. */
        const int16_t dx = (int16_t)(y >> i);
        const int16_t dy = (int16_t)(x >> i);

        if (y >= 0) {
            x   = (int16_t)(x + dx);
            y   = (int16_t)(y - dy);
            ang = (avas_type_ty_ck_carph_t)(ang + s_ck_cordic_atan[i]);
        } else {
            x   = (int16_t)(x - dx);
            y   = (int16_t)(y + dy);
            ang = (avas_type_ty_ck_carph_t)(ang - s_ck_cordic_atan[i]);
        }
    }

    /* x is now K * |Z| / 2 and never exceeds 26979, so nothing above overflowed
     * int16: the rotations only ever multiply the MAGNITUDE by sqrt(1 + 2^-2i), and
     * a component cannot exceed the magnitude.  The clamp is therefore unreachable
     * with this table and kept anyway -- it is one compare per cluster per rebuild,
     * and it is what makes the int16 store safe for ANY table, which is the same
     * argument the saturating sac earns its place with. */
    a = AVAS_TYPE_TY_CK_MULSU(x, AVAS_TYPE_TY_CK_CORDIC_KINV_Q15) >> 14;
    *out_a = (int16_t)((a > 32767L) ? 32767L : a);
    *out_p = ang;
}
#endif /* ENVINTERP == 1 */


/* ------------------------------------------------------------------------- *
 * Rebuild the interpolation slopes for all clusters.  Called once every
 * AVAS_TYPE_TY_CK_DEC samples, which -- with DEC equal to the transport's block
 * size -- is exactly once per block ISR, so the burst is not jitter, it IS the
 * amortised cost.
 *
 * eval_cluster() returns the envelope DEC samples LATER than the sample about to
 * be emitted, because the previous call left the baseband phases one step ahead.
 * The slope walks env_* onto it over exactly DEC increments, so no explicit
 * hand-over is needed.  Computing the target one block ahead is not a
 * refinement: interpolating from the previous target to the current one delays
 * the envelope by a block and measures 15.3 dB below signal instead of 48.
 * ------------------------------------------------------------------------- */
AVAS_TYPE_TY_CK_HOT AVAS_TYPE_TY_CK_ONE_COPY
static void avas_line_ck_rebuild_envelope(avas_line_ck_t *s)
{
    const avas_line_ck_set_t *set = s->set;
    uint16_t k;

    /* Reveal a pending pitch request all at once, before the first cluster reads
     * it, rather than letting the console's write land between two clusters of
     * the SAME call -- that would detune only the ones still to come, a half-old
     * half-new rebuild. This is the one and only place pitch_ratio_q14 changes;
     * everything else in this file only reads it. */
    if (s->pitch_req_pending) {
        s->pitch_ratio_q14   = s->pitch_ratio_req_q14;
        s->pitch_cent        = s->pitch_cent_req;
        s->pitch_req_pending = 0u;
    }

    for (k = 0u; k < set->clusters; k++) {
        avas_line_ck_car_t *c = &s->car[k];
        int16_t next_i;
        int16_t next_q;

        avas_line_ck_eval_cluster(s, k, &next_i, &next_q);

#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
        {
            int16_t a_t;
            avas_type_ty_ck_carph_t p_t;
            avas_type_ty_ck_carph_signed_t dphi;

            avas_line_ck_to_polar(next_i, next_q, &a_t, &p_t);

            c->amp_da = AVAS_TYPE_TY_CK_ENV_SLOPE(AVAS_TYPE_TY_CK_ENV_TGT(a_t), c->amp);

            /* THE UNWRAP IS FREE AND IT IS ALSO THE SCHEME'S ONE BLIND SPOT.
             * Reading the difference of two phase accumulators as a SIGNED value of
             * the same width gives the shortest way round, in (-half, +half], with
             * no branch and no comparison.  What it cannot know is that the true
             * envelope went the LONG way round during the block; measured at 0 to 2
             * blocks per cluster per 2 s, and section 16 shows that is the MINOR
             * half of polar's error -- the major half is that a straight line in
             * (A,phi) fills in the beat nulls.
             *
             * The cast from the unsigned difference is a reduction modulo 2^width on
             * both toolchains (and host_check would fail loudly on a target where it
             * were not), the same argument the phase wrap itself rests on. */
            dphi = (avas_type_ty_ck_carph_signed_t)
                       (avas_type_ty_ck_carph_t)(p_t - c->phi_app);
            dphi = (avas_type_ty_ck_carph_signed_t)(dphi >> AVAS_TYPE_TY_CK_DECSHIFT);

            /* The nominal step comes from FLASH here, not from a RAM copy: this runs
             * once per DEC samples, where the address computation the copy exists to
             * avoid costs nothing. The pitch ratio is folded in on the same read, for
             * the same reason it is folded into the baseband step in eval_cluster()
             * rather than a rewritten RAM table -- this is already the once-per-DEC
             * flash read the ratio needed a hook into.
             *
             * Unsigned multiply on purpose: car_step is a phase increment (always a
             * positive frequency in this model, see the header), and the ratio table
             * is never negative either, so there is no sign to lose by staying in
             * uint32_t all the way through -- which matters at PHBITS_CAR=32, where
             * casting car_step to int32_t first could turn a large-but-legal unsigned
             * step negative before the multiply ever ran. */
            c->step_eff = (avas_type_ty_ck_carph_t)(
                (avas_type_ty_ck_carph_t)(((uint32_t)set->car_step[k]
                                            * (uint32_t)s->pitch_ratio_q14) >> 14)
                + (avas_type_ty_ck_carph_t)dphi);
            /* What the DEC increments will actually ADD, which is not the target:
             * the shift above truncated, and measuring the next difference from the
             * reached value is what keeps that truncation from accumulating.  Shifted
             * as uint32 on purpose -- unsigned overflow is defined, and a signed
             * left shift into the sign bit is not (int is 16 bits on this target and
             * 32 on the host, so this is exactly where the two could differ). */
            c->phi_app = (avas_type_ty_ck_carph_t)
                (c->phi_app + (avas_type_ty_ck_carph_t)
                                  (((uint32_t)(avas_type_ty_ck_carph_t)dphi)
                                   << AVAS_TYPE_TY_CK_DECSHIFT));
        }
#else
        c->env_di = AVAS_TYPE_TY_CK_ENV_SLOPE(AVAS_TYPE_TY_CK_ENV_TGT(next_i), c->env_i);
        c->env_dq = AVAS_TYPE_TY_CK_ENV_SLOPE(AVAS_TYPE_TY_CK_ENV_TGT(next_q), c->env_q);
#endif
    }
}


/* ------------------------------------------------------------------------- *
 * Sum of the running carriers, before the gate.
 *
 *     y = sum_k [ I_k * cos(theta_k) - Q_k * sin(theta_k) ]
 *
 * which is Re{ e^{i theta_k} * Z_k }.  Only these AVAS_TYPE_TY_CK_CLUSTERS
 * oscillators run at fs, and they are ~62 % of this engine's cost.
 *
 * Two cheaper carrier schemes were tried and both were REJECTED by measurement,
 * so this loop is the answer rather than a first draft:
 *
 *   - A per-carrier complex rotator (4 multiplies, no table -- and the scheme
 *     the AK header itself recommends) needs 26-bit state to reach parity.  At
 *     Q14 the state collapses to zero; at Q20 it is 1 dB down.  26-bit state
 *     means 32x32 multiplies, which on this core is not cheaper than the two
 *     interpolated lookups it replaces.
 *   - Dropping the interpolation to buy the shift: see avas_line_ck_sin_q15().
 *
 * TERMSHIFT is deliberately more conservative than measurement requires.  The
 * accumulator's measured peak is 2.8 % of int32, but the provable bound without
 * the shift is 5.5x int32, and an overflow bound that holds only for the table
 * that happens to be shipped is not a bound.  The 11 shifts cost ~22 cycles.
 * ------------------------------------------------------------------------- */
AVAS_TYPE_TY_CK_HOT AVAS_TYPE_TY_CK_ONE_COPY
static int32_t avas_line_ck_process_carriers(avas_line_ck_t *s)
{
#if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
    volatile register int16_t acc asm("A");
#elif (AVAS_TYPE_TY_CK_ACC_MODE == 1)
    int64_t  acc = 0;      /* host reference only: 11 * 2^31 does not fit int32 */
#else
    int32_t  acc = 0;
#endif
    uint16_t k;

    /* The order here is a TOOLCHAIN WORKAROUND, and undoing it breaks the build.
     *
     * Every state update is done FIRST, before the two table lookups and the
     * multiplies, even though the natural order is to accumulate and then advance.
     * With the stores last, XC-DSC 3.31.01 for 33CK64MC105 runs out of W registers
     * across the lookups and emits `add w4,[w0],[w0++]` for the 32-bit
     * accumulate-in-place -- source and destination sharing one pointer register,
     * which its own assembler rejects ("bad code=82", missing right bracket).  It
     * fails loudly rather than miscompiling, and only under this register pressure:
     * the same statement in isolation compiles fine at every optimisation level, so
     * a minimal reproducer does not show it.
     *
     * Doing the stores early costs nothing -- the old phase and envelope are already
     * in registers and that is what the output needs -- and shortens three live
     * ranges. If a later toolchain fixes the register allocator, there is still no
     * reason to move them back.
     */
    avas_line_ck_car_t *c = s->car;

    /* THE COUNT HERE IS A CONSTANT ON PURPOSE, and it is the one place the
     * coefficient-set generalisation deliberately does NOT reach.
     *
     * MEASURED (doc section 11): taking the bound from the descriptor instead --
     * `k < s->set->clusters` hoisted into a register, or the equivalent end-pointer
     * sentinel -- costs +2 / +1 instructions in THIS loop body, which runs once per
     * cluster per sample.  On the Type_TY voice that is 11 x 32 iterations per block,
     * i.e. ~12 us of a 289 us block for a number that cannot change while only one
     * voice is compiled in (design section 3.4).  Everything else the voice owns is
     * read once per DEC samples or once at reset, where the descriptor is free.
     *
     * So while exclusivity is at BUILD time the loop compares against the selected
     * voice's compile-time count and the shipped codegen is unchanged.  Phase 7
     * (both voices resident, run-time exclusive) is where the bound has to become
     * s->set->clusters, and the +1/+2 above is what it will cost -- measured now,
     * before it is a surprise.
     *
     * PHASE 7 IS HERE, and it is spelled AVAS_CK_VOICE_BOTH: in that image the bound
     * comes from the descriptor, because a keypress changes it.  The single-voice
     * image still expands to the literal, so everything above still describes the
     * code it describes -- and that is checked by size rather than by reading, since
     * "unchanged codegen" is a claim a comment cannot keep. */
#if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
    acc = __builtin_clr();
#endif

#if (AVAS_CK_VOICE_BOTH)
    /* HOISTED, and not written as the loop condition: `s->set->clusters` there is two
     * dereferences per iteration, which is not the +1/+2 instructions doc section 11
     * priced -- that number is for the bound held in a register. */
    const uint16_t k_end = AVAS_LINE_CK_LOOP_CLUSTERS(s);

    AVAS_TYPE_TY_CK_KEEP_LOOP
    for (k = 0u; k < k_end; k++) {
#else
    AVAS_TYPE_TY_CK_KEEP_LOOP
    for (k = 0u; k < AVAS_LINE_CK_CLUSTERS; k++) {
#endif
        const avas_type_ty_ck_carph_t ph = c->phase;
#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
        const avas_type_ty_ck_env_t ei = c->amp;
#else
        const avas_type_ty_ck_env_t ei = c->env_i;
        const avas_type_ty_ck_env_t eq = c->env_q;
#endif
#if ((AVAS_TYPE_TY_CK_PHBITS_CAR) == 16)
        /* Nothing to extract: the whole phase is the word the index and the
         * fraction come out of, so the union below -- and the toolchain problem it
         * works around -- does not arise at all at this width. */
        const uint16_t idx = AVAS_TYPE_TY_CK_PH_IDX(ph);
        const uint16_t fr  = AVAS_TYPE_TY_CK_PH16_FRAC(ph);
#else
        /* The high half by naming it, not by shifting.  `(uint16_t)(ph >> 16)` makes
         * XC-DSC 3.31.01 at -O2 materialise the whole 32-bit shift -- a copy of the
         * high word AND a `mov #0` for a high half it immediately truncates away --
         * and it does not eliminate the dead one.  A local union is the same idiom
         * this repo used for the TDM wire slot, and host_check proves the integers
         * are unchanged.  Both the target and the host are little-endian; if that
         * ever stops being true, host_check fails loudly rather than silently. */
        union { uint32_t u32; uint16_t u16[2]; } phu;
        phu.u32 = ph;
        const uint16_t hi  = phu.u16[1];
        const uint16_t idx = AVAS_TYPE_TY_CK_PH_IDX(hi);
        const uint16_t fr  = AVAS_TYPE_TY_CK_PH_FRAC(hi, (uint16_t)ph);
#endif
        const int16_t  i15 = AVAS_TYPE_TY_CK_ENV_Q15(ei);  /* |Z_k| <= 32767: lossless */
#if ((AVAS_TYPE_TY_CK_ENVINTERP) != 1)
        const int16_t  q15 = AVAS_TYPE_TY_CK_ENV_Q15(eq);
#endif
        const avas_type_ty_ck_sin_ent_t *e = &s_ck_sin_dq15[idx];

#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
        /* TWO state updates instead of three, and the step already carries the
         * envelope's phase slope -- that is half of what polar buys.  The other half
         * is the single lookup and single product below. */
        c->phase = (avas_type_ty_ck_carph_t)(ph + c->step_eff);
        c->amp   = AVAS_TYPE_TY_CK_ENV_ADD(ei, c->amp_da);

        /* A * cos(theta + phi).  The sin term is not computed and multiplied by
         * zero, it does not EXIST -- a scheme that computed it would cost what rect
         * costs.  The model writes it as ei*cos - 0*sin only so that one accumulator
         * model covers both schemes; nothing here is multiplied by that zero. */
# if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
        acc = __builtin_mac_16(acc, i15,
                               avas_line_ck_tab_at(AVAS_TYPE_TY_CK_COS_AT(e), fr));
# elif (AVAS_TYPE_TY_CK_ACC_MODE == 1)
        acc += (int32_t)i15 * (int32_t)avas_line_ck_tab_at(
                                           AVAS_TYPE_TY_CK_COS_AT(e), fr);
# else
        /* TERMSHIFT is unchanged and still needed: one product per carrier instead
         * of two halves the bound, but 11 * 2^30 does not fit an int32 either. */
        acc += ((int32_t)i15 * (int32_t)avas_line_ck_tab_at(
                                            AVAS_TYPE_TY_CK_COS_AT(e), fr))
               >> AVAS_TYPE_TY_CK_TERMSHIFT;
# endif
#else
        c->phase = (avas_type_ty_ck_carph_t)(ph + c->step);
        c->env_i = AVAS_TYPE_TY_CK_ENV_ADD(ei, c->env_di);
        c->env_q = AVAS_TYPE_TY_CK_ENV_ADD(eq, c->env_dq);

# if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
        /* No TERMSHIFT: 40 bits holds the untruncated sum, which is the whole
         * point.  mac then msc is Re{e^{i theta} Z} in two instructions. */
        acc = __builtin_mac_16(acc, i15,
                               avas_line_ck_tab_at(AVAS_TYPE_TY_CK_COS_AT(e), fr));
        acc = __builtin_msc_16(acc, q15, avas_line_ck_tab_at(e, fr));
# elif (AVAS_TYPE_TY_CK_ACC_MODE == 1)
        acc += ((int32_t)i15 * (int32_t)avas_line_ck_tab_at(
                                            AVAS_TYPE_TY_CK_COS_AT(e), fr))
               - ((int32_t)q15 * (int32_t)avas_line_ck_tab_at(e, fr));
# else
        acc += (((int32_t)i15 * (int32_t)avas_line_ck_tab_at(
                                            AVAS_TYPE_TY_CK_COS_AT(e), fr))
                - ((int32_t)q15 * (int32_t)avas_line_ck_tab_at(e, fr)))
               >> AVAS_TYPE_TY_CK_TERMSHIFT;
# endif
#endif
        c++;
    }

#if (AVAS_TYPE_TY_CK_ACC_MODE == 2)
    /* A holds twice the integer sum (fractional mode), so A[31:16] IS sum >> 15.
     * One instruction, and the store saturates instead of wrapping -- see the
     * ACC_MODE note: mode 0's output bound does not survive the caller's gain
     * multiply, and this one does. */
    return (int32_t)__builtin_sac_16(acc, 0);
#elif (AVAS_TYPE_TY_CK_ACC_MODE == 1)
    /* Saturate explicitly, because mode 2's sac does and this is its definition. */
    {
        const int64_t y = acc >> 15;
        return (y > 32767) ? 32767L : ((y < -32768) ? -32768L : (int32_t)y);
    }
#else
    return acc >> (15 - AVAS_TYPE_TY_CK_TERMSHIFT);
#endif
}


/* ------------------------------------------------------------------------- *
 * Stop the envelope where it stands.
 *
 * Masking PART_ENVELOPE off stops the REBUILD.  It does not, by itself, stop the
 * interpolation: the carrier loop adds env_d* every sample unconditionally, so
 * without this the last slope computed keeps being applied forever.  Measured
 * with tools/host_check/avas_type_ty_ck_parts_test.c: over one second the envelope
 * travelled 28176 Q15 -- 3.8x what the running engine legitimately moves -- and
 * the output sat pinned at 32767.  Eleven carriers each sweeping full scale and
 * clipping is broadband, and on the board it was described as pink noise.
 *
 * The bug was order-dependent, which is why it survived: applied from a reset
 * (or straight after gate_on(), which resets) the slopes are already zero and the
 * mode is exactly the promised "frozen complex amplitude, i.e. pure tones".  It
 * only misbehaves when the full engine has been running, which is the useful
 * case -- switching *tb0007 -> *tb0005 to hear the difference.
 *
 * Done HERE, on the decimation boundary inside the block ISR, and not in
 * set_parts(): set_parts() is called from the foreground while the ISR is
 * running, and env_d* is 32-bit, so zeroing it there would be a torn write
 * against the ISR's read.  The cost of waiting is bounded and benign -- the
 * slope carries the envelope towards a target one block away, so at most one
 * block's worth of legitimate movement happens before it stops.  That is the
 * difference between a bounded step and an unbounded ramp.
 * ------------------------------------------------------------------------- */
static void avas_line_ck_freeze_envelope(avas_line_ck_t *s)
{
    const avas_line_ck_set_t *set = s->set;
    uint16_t k;

    for (k = 0u; k < set->clusters; k++) {
#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
        /* Polar needs BOTH halves stopped, and the second one is easy to miss: the
         * magnitude stops when amp_da is zeroed, but the envelope's PHASE is riding
         * in the carrier's step, so leaving step_eff alone would leave the frozen
         * envelope rotating -- a detuned carrier, not a held complex amplitude.  Same
         * class of defect as the runaway ramp this function exists for. */
        s->car[k].amp_da   = (avas_type_ty_ck_env_t)0;
        s->car[k].step_eff = set->car_step[k];
#else
        s->car[k].env_di = (avas_type_ty_ck_env_t)0;
        s->car[k].env_dq = (avas_type_ty_ck_env_t)0;
#endif
    }
}


/* ------------------------------------------------------------------------- */
static inline bool avas_line_ck_fully_gated_off(const avas_line_ck_t *s)
{
    return ((s->gate_target <= 0L) && (s->gate <= AVAS_TYPE_TY_CK_GATE_EPS_Q31));
}


void avas_line_ck_reset_phase(avas_line_ck_t *s)
{
    const avas_line_ck_set_t *set = s->set;
    uint16_t i;
    uint16_t k;

#if (AVAS_LINE_CK_HAVE_NOISE)
    /* The noise half is restarted from the same place the tone half is, and for the same
     * reason: "the state the reference WAV starts from" has to mean one instant for both
     * halves or the two are no longer the same sound.  This is also the ONE place
     * that answers "does the sounding voice have a noise half", because it is the one
     * function both init() and voice_set() go through. */
    s->noise_on = (uint8_t)(avas_line_ck_voice(s) == (uint8_t)AVAS_LINE_CK_VOICE_TYPE_LB);
    avas_noise_bank_ck_init(&s->noise);
    avas_noise_bank_ck_update_gusts(&s->noise);
#endif

    /* The measured cos phases as-is: the envelope loop evaluates both cos and
     * sin of them, so the +pi/2 shift a sine-only bank would need is gone. */
    for (i = 0u; i < set->lines; i++) {
        s->bb_phase[i] = set->bb_pha0[i];
    }

    /* Envelope at t = 0, which also leaves the baseband phases one decimated
     * step ahead -- exactly what rebuild_envelope() expects to find. */
    for (k = 0u; k < set->clusters; k++) {
        avas_line_ck_car_t *c = &s->car[k];
        int16_t zi;
        int16_t zq;

#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
        {
            int16_t a0;
            avas_type_ty_ck_carph_t p0;

            /* Called before the phase is written, because in polar the phase comes
             * OUT of the conversion.  The rect branch keeps its original order (and
             * so its own call) for a smaller reason that is still worth having: the
             * reordering alone moved one instruction, and "the shipped path is
             * untouched" is better as a byte comparison than as an argument. */
            avas_line_ck_eval_cluster(s, k, &zi, &zq);
            avas_line_ck_to_polar(zi, zq, &a0, &p0);
            /* The accumulator STARTS AT phi, not at zero, so sample 0 is
             * A*cos(phi) = zi -- the same first sample rect emits.  Both schemes
             * therefore begin from the measured phase set, which is what makes the
             * reference WAV's t = 0 mean the same thing in either build. */
            c->phase    = p0;
            c->phi_app  = p0;
            c->step_eff = set->car_step[k];
            c->amp      = (avas_type_ty_ck_env_t)AVAS_TYPE_TY_CK_ENV_TGT(a0);
            c->amp_da   = (avas_type_ty_ck_env_t)0;
        }
#else
        c->phase  = 0u;
        c->step   = set->car_step[k];
        avas_line_ck_eval_cluster(s, k, &zi, &zq);
        c->env_i  = (avas_type_ty_ck_env_t)AVAS_TYPE_TY_CK_ENV_TGT(zi);
        c->env_q  = (avas_type_ty_ck_env_t)AVAS_TYPE_TY_CK_ENV_TGT(zq);
        c->env_di = (avas_type_ty_ck_env_t)0;
        c->env_dq = (avas_type_ty_ck_env_t)0;
#endif
    }

    /* Rebuild on the very first sample so the slopes are valid from sample 0. */
    s->dec_count = 1u;
}


/* ------------------------------------------------------------------------- *
 * const-cost probe -- present only in a -Define AVAS_TYPE_LB_CK_CONST_PROBE build.
 *
 * Phase 0 of the Type_LB design.  Its whole ROM budget rests on one inferred ratio -- a byte of `.const`
 * costing about 1.5 program bytes, read out of a single map file -- and that
 * inference alone moves the answer by ~800 B, so it is measured here before any
 * table is designed around it.  Same arrangement as AVAS_TYPE_TY_CK_SATPROBE: no
 * normal build contains a byte of this, and the number can be re-measured rather
 * than trusted.
 *
 * Two things it has to defeat, both of which would report a smaller price than
 * the real table will pay:
 *
 *   - constant folding.  The index is volatile, so the reads happen at run time.
 *   - --gc-sections.  isolate-each-function is on, so an uncalled probe would be
 *     dropped; init() is the one entry every image reaches.  (The arrays would
 *     survive anyway -- -fdata-sections is deliberately off, so unreferenced
 *     const inside a live TU is not collected -- but a measurement that depends
 *     on that is measuring the wrong thing.)
 *
 * The masks are `& 15` and `& 3` rather than `% LINES`, so the probe's own CODE
 * is byte-identical at every size and no division helper is dragged in: the
 * difference between two builds is then data and nothing else.  They are that
 * small because the sizes had to be: the image has ~421 B of flash left, so the
 * probe is measured at 16 and 32 lines and the price is read off the SLOPE.  A
 * 132-line probe does not link, which is itself the budget answer.
 * ------------------------------------------------------------------------- */
#if defined(AVAS_TYPE_LB_CK_CONST_PROBE)
#include "avas_type_lb_ck_const_probe.h"

volatile uint16_t g_avas_type_lb_ck_probe_index;
volatile int32_t  g_avas_type_lb_ck_probe_sink;

static void avas_type_lb_ck_const_probe(void)
{
    uint16_t i = (uint16_t)(g_avas_type_lb_ck_probe_index & 15u);
    uint16_t k = (uint16_t)(g_avas_type_lb_ck_probe_index & 3u);

    g_avas_type_lb_ck_probe_sink = (int32_t)s_probe_amp_q15[i]
                               + (int32_t)s_probe_bb_step[i]
                               + (int32_t)s_probe_bb_pha0[i]
                               + (int32_t)s_probe_car_step[k]
                               + (int32_t)s_probe_cluster_first[k];
}
#endif


/* ------------------------------------------------------------------------- *
 * The Type_TY L1 coefficient set, i.e. everything in this engine that knows which
 * vehicle it is.  Built from the generated table header's symbols, so the header
 * stays exactly what it was -- the numbers are not copied, only named.
 *
 * The Type_LB set (design section 3.1) is a second instance of this struct
 * next to a second table header, selected by AVAS_CK_VOICE at build time until the
 * ROM diet lands (section 3.4) and at run time afterwards (section 3.3).  It needs
 * no new line of per-sample code, which is the finding the whole design rests on.
 * ------------------------------------------------------------------------- */
/* The per-sample loop trusts AVAS_LINE_CK_CLUSTERS while the set supplies the
 * tables, so a disagreement between the two would walk off the end of car[] with no
 * symptom until it sounded wrong.  This is what makes that a build failure rather
 * than an assumption now that AVAS_CK_VOICE_TYPE_LB chooses which of the two sets the
 * macro and the descriptor are taken from. */
#if !(AVAS_CK_VOICE_TYPE_LB) || (AVAS_CK_VOICE_BOTH)

static const avas_line_ck_set_t s_avas_type_ty_ck_l1_set = {
    s_ck_amp_q15,
    s_ck_bb_step,
    s_ck_bb_pha0,
    s_ck_car_step,
    s_ck_cluster_first,
    (uint16_t)AVAS_TYPE_TY_CK_LINES,
    (uint16_t)AVAS_TYPE_TY_CK_CLUSTERS,
    (int16_t)AVAS_TYPE_TY_CK_NORM_GAIN_Q15
};

#endif

#if (AVAS_CK_VOICE_TYPE_LB) || (AVAS_CK_VOICE_BOTH)

/* The Type_LB L3 set: 264 lines in AVAS_TYPE_LB_CK_CLUSTERS clusters, the SAME
 * struct, the same engine, and not one line of new per-sample code -- which is the
 * finding the whole design rests on. */
static const avas_line_ck_set_t s_avas_type_lb_ck_l3_set = {
    s_ck_type_lb_amp_q15,
    s_ck_type_lb_bb_step,
    s_ck_type_lb_bb_pha0,
    s_ck_type_lb_car_step,
    s_ck_type_lb_cluster_first,
    (uint16_t)AVAS_TYPE_LB_CK_LINES,
    (uint16_t)AVAS_TYPE_LB_CK_CLUSTERS,
    (int16_t)AVAS_TYPE_LB_CK_NORM_GAIN_Q15
};

#endif

#if (AVAS_CK_VOICE_TYPE_LB)

typedef char avas_line_ck_cluster_count_agrees[
    ((AVAS_LINE_CK_CLUSTERS) == (AVAS_TYPE_LB_CK_CLUSTERS)) ? 1 : -1];

#define AVAS_LINE_CK_ACTIVE_SET     (&s_avas_type_lb_ck_l3_set)

#else

typedef char avas_line_ck_cluster_count_agrees[
    ((AVAS_LINE_CK_CLUSTERS) == (AVAS_TYPE_TY_CK_CLUSTERS)) ? 1 : -1];

#define AVAS_LINE_CK_ACTIVE_SET     (&s_avas_type_ty_ck_l1_set)

#endif

#if (AVAS_CK_VOICE_BOTH)
/* THE TWO RESIDENT VOICES, and the array is what makes them resident.
 *
 * Both descriptors have to be REACHABLE, not merely compiled, and that sentence cost
 * two measurements to learn: the coefficient arrays have internal linkage, so an
 * unreferenced set is dropped by the COMPILER -- before --gc-sections or
 * -fdata-sections get a say -- and the image came out byte-identical to the one-voice
 * build, 62 235 B, twice.  The first attempt emitted both sets and referenced neither
 * (dropped).  The second referenced them through an index the file never wrote, which
 * the compiler folds to its initialiser, leaving the other set unreferenced again
 * (dropped).  Only a genuinely run-time index keeps both alive, which is why the
 * measurement stage of this work used `volatile` as a stand-in for the write that
 * avas_line_ck_voice_set() now really does.
 *
 * This corrects the rule design section 10 states: the unselected voice disappears
 * because of the COMPILER and internal linkage, not because of the linker's
 * --gc-sections, which never gets to see it. */
static const avas_line_ck_set_t *const s_avas_line_ck_voices[AVAS_LINE_CK_VOICE_COUNT] = {
    &s_avas_type_ty_ck_l1_set,
    &s_avas_type_lb_ck_l3_set,
};

/* The last selection, so a transport re-init (*tr, or a rate change) comes back up on
 * the voice that was being listened to rather than silently on the boot one.  It is
 * the boot voice until something writes it, and AVAS_CK_VOICE_TYPE_LB is what names
 * that.  No longer volatile: voice_set() is a real write from reachable code, which
 * is exactly the property the measurement had to fake. */
static uint8_t s_avas_line_ck_voice_idx =
    (uint8_t)((AVAS_CK_VOICE_TYPE_LB) ? 1u : 0u);

/* The state is shared between the voices, so it has to be big enough for both -- a
 * disagreement here walks off the end of car[] or bb_phase[] with no symptom until it
 * sounds wrong, which is the same failure the single-voice assertion above prevents. */
typedef char avas_line_ck_state_fits_both_voices[
    (((AVAS_LINE_CK_MAX_LINES) >= (AVAS_TYPE_TY_CK_LINES)) &&
     ((AVAS_LINE_CK_MAX_LINES) >= (AVAS_TYPE_LB_CK_LINES)) &&
     ((AVAS_LINE_CK_MAX_CLUSTERS) >= (AVAS_TYPE_TY_CK_CLUSTERS)) &&
     ((AVAS_LINE_CK_MAX_CLUSTERS) >= (AVAS_TYPE_LB_CK_CLUSTERS))) ? 1 : -1];
#endif


/* -------------------------------------------------------------------------
 * Which voice, at run time.  See the header for why these exist in every build.
 * ------------------------------------------------------------------------- */
bool avas_line_ck_voice_present(uint8_t voice)
{
#if (AVAS_CK_VOICE_BOTH)
    return (voice < (uint8_t)AVAS_LINE_CK_VOICE_COUNT);
#else
    return (voice == (uint8_t)((AVAS_CK_VOICE_TYPE_LB) ? AVAS_LINE_CK_VOICE_TYPE_LB
                                                     : AVAS_LINE_CK_VOICE_TYPE_TY));
#endif
}


const char *avas_line_ck_voice_name(uint8_t voice)
{
    return (voice == (uint8_t)AVAS_LINE_CK_VOICE_TYPE_LB) ? "type_lb" : "type_ty";
}


uint8_t avas_line_ck_voice(const avas_line_ck_t *s)
{
#if (AVAS_CK_VOICE_BOTH)
    /* From the descriptor the engine is actually reading, not from the remembered
     * index: the two are the same until something re-inits the state, and "what is
     * sounding" is the question every caller is really asking. */
    return (uint8_t)((s->set == &s_avas_type_lb_ck_l3_set) ? AVAS_LINE_CK_VOICE_TYPE_LB
                                                         : AVAS_LINE_CK_VOICE_TYPE_TY);
#else
    (void)s;
    return (uint8_t)((AVAS_CK_VOICE_TYPE_LB) ? AVAS_LINE_CK_VOICE_TYPE_LB
                                           : AVAS_LINE_CK_VOICE_TYPE_TY);
#endif
}


bool avas_line_ck_voice_set(avas_line_ck_t *s, uint8_t voice)
{
    if (!avas_line_ck_voice_present(voice)) {
        return false;
    }

    /* THE ENGINE MUST NOT BE SOUNDING, and this is the module's invariant rather than
     * the caller's promise -- see the header.  A release that has been asked for but
     * not finished still has every oscillator running, and re-seeding them mid-fade is
     * the click this refuses.
     *
     * The test is is_active() and NOT `gate == 0 && gate_target == 0`, which is what
     * this was first written as.  Two states break that form and both are reachable:
     * with the GATE part masked off (*tb) process_sample() pins the gate wide open
     * every sample, so `gate == 0` is never true again and a switch would be refused
     * forever with the console blaming a gate the user had deliberately removed; and
     * at the other end a finished fade sits below GATE_EPS for a moment before it is
     * snapped to zero, where a refusal would be answering about -50 dB of silence.
     * is_active() is the one question that is right in both modes -- it is what the
     * console reports and what the ISR stands the stage down on. */
    if (avas_line_ck_is_active(s)) {
        return false;
    }

#if (AVAS_CK_VOICE_BOTH)
    s_avas_line_ck_voice_idx = (uint8_t)(voice & 1u);
    s->set = s_avas_line_ck_voices[s_avas_line_ck_voice_idx];

    /* The level is per voice (the reference WAV's peak differs), so it is re-read
     * here.  A user gain set with avas_line_ck_set_gain_q15() is NOT preserved
     * through a switch for the same reason: it was a scale on the other voice's
     * reference level, and carrying the product across would be a level the caller
     * never asked for. */
    s->out_gain_q15 = s->set->norm_gain_q15;

    /* Every oscillator from the new voice's measured t = 0 phases.  parts is left
     * alone -- a keypress that silently re-enabled the envelope would undo *tb. */
    avas_line_ck_reset_phase(s);
#endif
    return true;
}


void avas_line_ck_init(avas_line_ck_t *s)
{
    memset(s, 0, sizeof(*s));
    /* Before reset_phase(), which reads the tables through it. */
#if (AVAS_CK_VOICE_BOTH)
    s->set          = s_avas_line_ck_voices[s_avas_line_ck_voice_idx & 1u];
#else
    s->set          = AVAS_LINE_CK_ACTIVE_SET;
#endif
    s->out_gain_q15 = s->set->norm_gain_q15;
    s->parts        = (uint8_t)AVAS_TYPE_TY_CK_PART_ALL;
    avas_line_ck_reset_phase(s);
    s->gate        = 0L;
    s->gate_target = 0L;
    /* memset above already cleared pitch_cent/_req and the two pending flags; the
     * ratio pair has to start at unity explicitly -- 0 there would mean "silence
     * every step", not "no trim". */
    s->pitch_ratio_q14     = (int16_t)AVAS_PITCH_CK_RATIO_Q14_UNITY;
    s->pitch_ratio_req_q14 = (int16_t)AVAS_PITCH_CK_RATIO_Q14_UNITY;
#if defined(AVAS_TYPE_LB_CK_CONST_PROBE)
    avas_type_lb_ck_const_probe();
#endif
}


int16_t avas_line_ck_render_sample(avas_line_ck_t *s)
{
    int32_t y;

    /* 185 baseband oscillators once every DEC samples; absorbed inside one
     * block ISR because DEC is the block size.
     *
     * With ENVELOPE masked off the counter still runs, no rebuild happens, and the
     * slopes are zeroed so env_* really does hold still -- a frozen complex
     * amplitude per cluster, i.e. pure tones. The zeroing is not redundant: see
     * avas_line_ck_freeze_envelope(), where leaving it out was a measured defect.
     *
     * Deliberately not "skip the counter too": leaving it running means switching
     * the part back on resumes on the regular block boundary instead of at an
     * arbitrary phase. */
    s->dec_count--;
    if (s->dec_count == 0u) {
        s->dec_count = (uint16_t)AVAS_TYPE_TY_CK_DEC;
        if ((s->parts & AVAS_TYPE_TY_CK_PART_ENVELOPE) != 0u) {
            avas_line_ck_rebuild_envelope(s);
        } else {
            avas_line_ck_freeze_envelope(s);
        }
#if (AVAS_LINE_CK_HAVE_NOISE)
        /* The gusts ride the SAME control tick the envelope rebuild does -- one control
         * rate for the voice, not two.  It is also where the whole cost of the 1.5 dB
         * modulation lives: 12 bands' worth of work once per 32 samples, after which the
         * per-sample loop just multiplies by a gain that was already going to be there. */
        if (s->noise_on != 0u) {
            avas_noise_bank_ck_update_gusts(&s->noise);
        }
#endif
    }

    if ((s->parts & AVAS_TYPE_TY_CK_PART_CARRIERS) == 0u) {
        return 0;      /* measurement mode: there is no output without carriers */
    }

    y = avas_line_ck_process_carriers(s);

#if (AVAS_LINE_CK_HAVE_NOISE)
    /* The noise is summed HERE -- into the carrier sum, before the output gain and
     * therefore before the gate -- and both of those are deliberate.
     *
     * Before the gain, because the bank's own gain already converts its counts into
     * these A_SCALE units (k * A_SCALE, both measured), so the two halves of L3 are
     * literally one sum and the voice keeps ONE level.  Before the gate, because a fade
     * that covered the tone but not the noise would end in a burst of bare noise, which
     * is the click the gate exists to prevent wearing a different costume.
     *
     * Note what this placement implies and is correct: process_sample() returns early
     * when fully gated off, so a silent bank does not advance -- and every gate-on goes
     * through reset_phase(), which restarts it. */
    if ((s->noise_on != 0u) && ((s->parts & AVAS_TYPE_TY_CK_PART_NOISE) != 0u)) {
        y += (int32_t)avas_noise_bank_ck_sample(&s->noise);
    }
#endif

/* THE SUM CAN CLIP, and only the sum can.  The tone alone is normalised to 0.9 of full
 * scale against a measured 60 s peak, which is why the fast arm below has always been a
 * bare cast.  Adding the noise breaks that: the generator measures tone peak 31 611 plus
 * noise peak 6 002 in A_SCALE units, i.e. 35 090 after the output gain against a full
 * scale of 32 767.  So a build with the noise half must saturate, and a cast would turn
 * that 0.6 dB of overshoot into a full-scale sign flip -- an impulse, in the one signal
 * whose defects an ear cannot localise.  It is two compares on a path that just did
 * 264 oscillators. */
#if ((AVAS_LINE_CK_NORM_SHIFT) == 0) && !(AVAS_LINE_CK_HAVE_NOISE)
    /* The selected voice's output gain fits Q15, so this is one shift and no branch
     * -- what this engine has always done.  Both voices land here at their shipped
     * tables; the arm below exists because a wider cluster span does not (see the
     * header's note on AVAS_LINE_CK_NORM_SHIFT and design section 12). */
    return (int16_t)((y * (int32_t)s->out_gain_q15) >> 15);
#else
    {
        /* Two ways in, one arm.  Either the gain is above unity -- shift less -- or the
         * noise half is compiled and the sum of the two halves can exceed full scale.
         * Both need the SATURATE rather than a wrap: the normalisation targets a 60 s
         * peak that quasi-periodic beating keeps growing past, so the cast would
         * eventually deliver a full-scale click.  With NORM_SHIFT == 0 the shift below
         * is the same >> 15 the fast arm does, so a noise build pays exactly the two
         * compares and nothing else. */
        int32_t o = (y * (int32_t)s->out_gain_q15)
                    >> (15 - (AVAS_LINE_CK_NORM_SHIFT));
        return (int16_t)(o > 32767L ? 32767L : (o < -32768L ? -32768L : o));
    }
#endif
}


void avas_line_ck_set_parts(avas_line_ck_t *s, uint8_t parts)
{
    s->parts = (uint8_t)(parts & AVAS_TYPE_TY_CK_PART_ALL);
}


int16_t avas_line_ck_process_sample(avas_line_ck_t *s)
{
    int32_t err;
    int32_t num;
    int16_t y;

    /* Only true for the samples of a stop's fade tail -- avas_line_ck_gate_off()
     * is what arms this, and it clears itself the first time it fires, so the
     * cost is bounded to that one fade rather than paid every sample forever.
     * avas_line_ck_is_active() rather than fully_gated_off() below because it is
     * the test that is right in BOTH the normal and the GATE-masked *tb
     * configurations (see avas_line_ck_is_active()'s own comment) -- with GATE
     * masked, fully_gated_off() never becomes true at all, and a reset gated on
     * it would then never fire.
     *
     * COMMITTED DIRECTLY, NOT THROUGH pitch_req_pending: once this is true the
     * engine has stopped calling avas_line_ck_rebuild_envelope() altogether (that
     * is what "fully idle" means -- see render_sample()), so a flag left for it
     * to pick up would sit there until the NEXT start and *tc/?tc would keep
     * reporting the old cent in between. There is no rebuild in flight to race
     * with here -- this function IS the context rebuild_envelope() would have
     * been called from -- so writing both pairs straight through is exactly as
     * safe as the request/commit split it is bypassing. */
    if (s->pitch_req_on_silence && !avas_line_ck_is_active(s)) {
        s->pitch_cent           = 0;
        s->pitch_cent_req       = 0;
        s->pitch_ratio_q14      = (int16_t)AVAS_PITCH_CK_RATIO_Q14_UNITY;
        s->pitch_ratio_req_q14  = (int16_t)AVAS_PITCH_CK_RATIO_Q14_UNITY;
        s->pitch_req_pending    = 0u;
        s->pitch_req_on_silence = 0u;
    }

    if (avas_line_ck_fully_gated_off(s)) {
        /* Snap the truncated tail to zero so the stored gate matches what is
         * actually emitted, and a re-enable does not start from -50 dB. */
        s->gate = 0L;
        return 0;
    }

    /* One-pole gate.  ONE 16x16 multiply and a shift: the error's Q15 part times
     * an int16 numerator.  Writing this the obvious way --
     * ((int64_t)alpha_q31 * err) >> 31 -- is what drags in ___muldi3, measured
     * at 88 cycles per sample on this core.  The numerators are exact to
     * 4 decimal places on both time constants; see the generator. */
    if ((s->parts & AVAS_TYPE_TY_CK_PART_GATE) == 0u) {
        /* Masked off: hold the gate wide open rather than at whatever it had
         * reached, so what is removed is the fade and not the signal. */
        s->gate = AVAS_TYPE_TY_CK_GATE_ONE;
        return avas_line_ck_render_sample(s);
    }

    err = s->gate_target - s->gate;
    num = (err > 0L) ? (int32_t)AVAS_TYPE_TY_CK_GATE_ATTACK_NUM
                     : (int32_t)AVAS_TYPE_TY_CK_GATE_RELEASE_NUM;
    s->gate += ((int32_t)(int16_t)(err >> 16) * num) >> AVAS_TYPE_TY_CK_GATE_MULSHIFT;

    y = avas_line_ck_render_sample(s);

    return (int16_t)(((int32_t)y * (s->gate >> 16)) >> 15);
}


void avas_line_ck_set_gain_q15(avas_line_ck_t *s, int16_t gain_q15)
{
    s->out_gain_q15 = (int16_t)(((int32_t)s->set->norm_gain_q15
                                 * (int32_t)gain_q15) >> 15);
}


void avas_line_ck_gate_on(avas_line_ck_t *s)
{
    /* Restart from the measured phase set so every enable produces the same
     * waveform as the reference file's t = 0.  Skipped if the previous release
     * has not finished, where a phase jump would be an audible click. */
    if (s->gate <= AVAS_TYPE_TY_CK_GATE_EPS_Q31) {
        avas_line_ck_reset_phase(s);
    }
    s->gate_target = AVAS_TYPE_TY_CK_GATE_ONE;
}


void avas_line_ck_gate_off(avas_line_ck_t *s)
{
    s->gate_target = 0L;

    /* Arm the reset process_sample() applies once the fade has actually finished
     * (see there) -- not here and not at the next rebuild, either of which would
     * detune the ~3 s tail still playing out. A stop that leaves the trim exactly
     * where it was would surprise the next *ta0001, which is meant to start clean. */
    s->pitch_req_on_silence = 1u;
}


/* -------------------------------------------------------------------------
 * PITCH TRIM.  See the header for why this is a request/commit pair and not a
 * plain write: the ratio is read once per DEC samples by rebuild_envelope(),
 * across every cluster/line in that one call, and a write landing mid-call
 * would detune only whichever of them were still to come.
 * ------------------------------------------------------------------------- */
void avas_line_ck_request_pitch_cent(avas_line_ck_t *s, int16_t cent)
{
    /* Clamps to +/-AVAS_LINE_CK_PITCH_LIMIT_CENT (equal to the table's own
     * domain -- see avas_pitch_ck_table.h) and rounds to its 5 cent grid, so
     * pitch_cent_req stores the value that is actually going to sound rather
     * than the raw argument -- *tc/?tc must not claim a pitch this table
     * cannot produce. */
    cent = avas_pitch_ck_snap_cent(cent);

    s->pitch_cent_req      = cent;
    s->pitch_ratio_req_q14 = avas_pitch_ck_cent_to_ratio_q14(cent);
    /* Value first, flag last -- rebuild_envelope() only looks at the flag, so the
     * two writes have to be visible in this order for it to never see a stale
     * ratio paired with a fresh flag. */
    s->pitch_req_pending   = 1u;
}

int16_t avas_line_ck_get_pitch_cent(const avas_line_ck_t *s)
{
    return s->pitch_cent;
}

int16_t avas_line_ck_get_pitch_cent_req(const avas_line_ck_t *s)
{
    return s->pitch_cent_req;
}


/* The predicate the chain stage runs on, and the same one process_sample() uses for
 * its early-out -- deliberately the same expression rather than a second rule, so
 * "still rendering" and "still costing" cannot drift apart.
 *
 * EXCEPT WHEN THE GATE PART IS MASKED OFF, where the shared expression is not merely
 * different but unable to terminate: process_sample() pins the gate wide open every
 * sample in that mode, so fully_gated_off() can never become true and this answered
 * "still rendering" forever.  The header records what that cost -- the chain's AVAS
 * stage never stood down after a stop.
 *
 * gate_target is the right thing to read instead, and it keeps the no-drift property
 * for the reason that matters: in this mode the gate carries no information at all
 * (it is a constant), so the request is the ONLY state that distinguishes running
 * from stopped.  It is also still exactly "does process_sample() emit anything",
 * which is what both callers mean -- with the gate masked, output stops the moment
 * the request does, because there is no fade left to render. */
bool avas_line_ck_is_active(const avas_line_ck_t *s)
{
    if ((s->parts & AVAS_TYPE_TY_CK_PART_GATE) == 0u) {
        return (s->gate_target > 0L);
    }

    return !avas_line_ck_fully_gated_off(s);
}

#else /* !AVAS_LINE_CK_HAVE_ENGINE -- AVAS_CK_VOICE_NONE, the debug mode */

/* -------------------------------------------------------------------------
 * THE DEBUG MODE'S ENGINE: none.
 *
 * The whole point is to hand the flash back, so what stands in for the engine has to
 * be the smallest thing that keeps the REST of the firmware compiling unchanged.  That
 * is why these are stubs behind the real API rather than `#if`s at the call sites:
 * wm8904_audio.c calls sixteen of these functions from the block ISR, the console and
 * the bring-up, and teaching each of those call sites about a build mode would spread
 * the switch across the file that is hardest to review -- and would have to be undone
 * the next time the mode changes.  Here, the mode is one `#if` in one file.
 *
 * is_active() answering false is the load-bearing stub: the chain asks it before it
 * asks for a sample, so passthrough audio survives an AVAS-OFF image.  The stub
 * process/render pair still returns 0 rather than being left to trap, because "asked
 * for a sample anyway" should be silence and not noise -- a debug image that squeals
 * is a debug image nobody leaves running.
 *
 * voice_present() answers false for EVERY voice, so an AVAS voice key refuses with
 * a reason instead of appearing to select something. That is the same contract the
 * single-voice image already uses for the absent voice; this mode is just the case
 * where both are absent.
 * ------------------------------------------------------------------------- */

void avas_line_ck_init(avas_line_ck_t *s)
{
    if (s != 0) {
        (void)memset(s, 0, sizeof(*s));
    }
}

void avas_line_ck_reset_phase(avas_line_ck_t *s)
{
    (void)s;
}

bool avas_line_ck_voice_present(uint8_t voice)
{
    (void)voice;
    return false;
}

const char *avas_line_ck_voice_name(uint8_t voice)
{
    (void)voice;
    return "none (AVAS_CK_VOICE_NONE)";
}

uint8_t avas_line_ck_voice(const avas_line_ck_t *s)
{
    (void)s;
    return 0u;
}

bool avas_line_ck_voice_set(avas_line_ck_t *s, uint8_t voice)
{
    (void)s;
    (void)voice;
    return false;
}

int16_t avas_line_ck_render_sample(avas_line_ck_t *s)
{
    (void)s;
    return 0;
}

int16_t avas_line_ck_process_sample(avas_line_ck_t *s)
{
    (void)s;
    return 0;
}

void avas_line_ck_set_parts(avas_line_ck_t *s, uint8_t parts)
{
    (void)s;
    (void)parts;
}

void avas_line_ck_gate_on(avas_line_ck_t *s)
{
    (void)s;
}

void avas_line_ck_gate_off(avas_line_ck_t *s)
{
    (void)s;
}

bool avas_line_ck_is_active(const avas_line_ck_t *s)
{
    (void)s;
    return false;
}

void avas_line_ck_request_pitch_cent(avas_line_ck_t *s, int16_t cent)
{
    (void)s;
    (void)cent;
}

int16_t avas_line_ck_get_pitch_cent(const avas_line_ck_t *s)
{
    (void)s;
    return 0;
}

int16_t avas_line_ck_get_pitch_cent_req(const avas_line_ck_t *s)
{
    (void)s;
    return 0;
}

#endif /* AVAS_LINE_CK_HAVE_ENGINE */
