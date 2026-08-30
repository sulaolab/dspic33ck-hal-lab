#ifndef AVAS_SYNTH_LINE_CK_H
#define AVAS_SYNTH_LINE_CK_H

/*
 * avas_synth_line_ck -- a LINE-MODEL AVAS engine, fixed-point, for dsPIC33CK.
 *
 * It was written as the Type_TY port (`avas_type_ty_ck`) and renamed when the
 * Type_LB L3 voice turned out to be the same arithmetic with a different
 * coefficient set: 264 lines in 4 clusters instead of 185 in 11.  Nothing about
 * the numbers changed at the rename; what changed is that the tables arrive
 * through avas_line_ck_set_t instead of being addressed by name.  Naming from
 * sonora, which splits the identities the same way (engine per vehicle, table per
 * model label) -- note the one deliberate asymmetry: the build-time KNOBS keep their
 * AVAS_TYPE_TY_CK_* spelling, because they are what every -Define line,
 * run_host_check.py, cost_probe and both docs already say.
 *
 * A port of dspic33ak-audio-dsp-sonora's src/apps/classic/dsp/avas_synth_type_ty.c,
 * which runs on dsPIC33AK.  The ALGORITHM is unchanged and the coefficients are the
 * same file; the ARITHMETIC is a rewrite, because AK has an FPU and CK does not.
 *
 *     y(t) = sum_j AMP[j] * cos(2*pi*FRQ[j]*t + PHA[j])      j = 1..lines
 *          = sum_k Re{ e^{i 2 pi FC[k] t} * Z_k(t) }         k = 1..clusters
 *
 * The two voices model AVAS sounds of the kind already in public use on the road.
 * The line and noise data are the project's own.
 *
 * For the Type_TY L1 set: 185 spectral lines of the reference sound's
 * fixed-pitch opening, grouped into 11 clusters no wider than 200 Hz.  Only the
 * carriers run at fs; each cluster's complex envelope Z_k is rebuilt every AVAS_TYPE_TY_CK_DEC
 * samples and linearly interpolated in between.  The rebuild targets the
 * envelope ONE BLOCK AHEAD -- interpolating towards a value already reached
 * delays it by a block and costs 33 dB.  See the AK file's header comment for
 * why the model is a line list and not a harmonic grid, and why trimming the
 * line count is not a way to buy load.
 *
 * WHY FIXED POINT IS THE WHOLE PORT
 * ---------------------------------
 * CK has half AK's cycles per sample (2048 vs 4166 at 48 kHz) and no FPU.  The
 * float engine costs 1900 cycles/sample WITH an FPU, so an instruction-for-
 * instruction port needs 93 % of the budget before the first soft-float call --
 * and one library multiply on this core was measured at 88 cycles/sample
 * (docs/ck_silicon_findings.md, Part 5).  Fixed point is not an optimisation
 * here, it is the reason the port is possible: a Q15 multiply-accumulate is one
 * cycle, and the phase wrap becomes free because integer overflow IS the modulo.
 *
 * The representation choices (Q15 amplitudes scaled per worst cluster, the phase
 * accumulators, envelope as an int32 whose top 16 bits are the Q15 value, the
 * gate's int16-numerator one-pole) are all documented where they are derived:
 * tools/avas_type_ty_fixed_model.py and tools/gen_avas_type_ty_ck_tables.py.
 *
 * The phase accumulators are 32 bits or 16 bits, chosen at build time with
 * AVAS_TYPE_TY_CK_PHBITS_CAR / AVAS_TYPE_TY_CK_PHBITS_BB and defaulted in the generated
 * table header, which carries a step table for each.  16 bits is cheaper on a
 * 16-bit core and costs frequency resolution; what it realises, and the listening
 * test that accepted it, are recorded next to those defines.
 *
 * The envelope's interpolation COORDINATES are the same kind of choice:
 * AVAS_TYPE_TY_CK_ENVINTERP, 0 rect / 1 polar, defaulted in that same header.  Polar
 * halves the carrier loop's lookups and multiplies and pays a CORDIC per cluster per
 * rebuild; it is measurably less accurate and was accepted by ear, which is written
 * down where the default is.
 *
 * VERIFIED, AND HOW
 * -----------------
 * Every accuracy figure for the AK engine came from an offline model, never
 * from hardware.  A fixed-point rewrite can fail in ways float cannot -- a
 * wrong headroom shift, a table too coarse -- and on a speaker those look
 * exactly like a wrong coefficient table.  So:
 *
 *   1. tools/avas_type_ty_fixed_model.py models this arithmetic in numpy and
 *      measures it against the same reference the AK engine was measured
 *      against: 48.1 dB below signal, line-free floor -79.0 dBFS (AK float:
 *      48.9 dB, -71.9 dBFS -- parity on accuracy, 7 dB better on the floor).
 *   2. tools/host_check/ compiles THIS file for the host and asserts the C
 *      output is bit-identical to that model, sample for sample.
 *
 * So the numbers in (1) describe this code, not a paper design.
 *
 * SAMPLE RATE IS BAKED INTO THE TABLE, AND THE GUARD IS A RUNTIME ONE
 * -------------------------------------------------------------------
 * bb_step and car_step are both scaled by fs, so a table generated for 48000 and
 * played at 48828 is 1.7 % sharp -- 29 cents, which on a synthesised engine note
 * is exactly the kind of wrong that sounds fine.  Regenerate for the other rate:
 *     python tools/gen_avas_type_ty_ck_tables.py src/app/dsp/avas_type_ty_ck_tables.h <ak_table.h> 48828
 *
 * WHAT ACTUALLY CATCHES A MISMATCH IS avas_rate_matches() IN wm8904_audio.c, which
 * refuses to start the AVAS stage and says so on the console.  It has to be a
 * runtime check, because the rate is a runtime property of the transport config:
 * EV88G73A runs 48828.125 Hz as dsPIC master (BCLK/BRG derived) and 48000 Hz as
 * codec master, and both are legitimate on the same image.
 *
 * The #if below is therefore an OPT-IN build-time assertion for a consumer that
 * does fix fs at compile time -- tools/host_check/, or a board with one rate.
 * NOTHING IN THIS PROJECT DEFINES AVAS_TYPE_TY_CK_APP_FS_HZ, so on this firmware the
 * #error never fires; do not read it as the safety net.  This paragraph exists
 * because the earlier wording claimed it was one, which is worse than no guard:
 * it invites the next reader to skip writing the check that does the work.
 */

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * WHICH VOICE THIS IMAGE RENDERS.  0 = Type_TY L1, 1 = Type_LB L3.
 *
 * Build-time and not run-time, on the owner's instruction, until the ROM diet
 * lands: the image is at 99 % of flash, and the two coefficient sets do not both
 * fit.  The flip to sonora's
 * run-time exclusivity is then this `#if` becoming an `if` -- nothing else in the
 * design moves, which is why it is built this way round.
 *
 * IT IS DECIDED HERE, BEFORE EITHER TABLE HEADER IS READ, for a reason that is not
 * style: the unselected voice's line tables have to be removed by the PREPROCESSOR.
 * `--gc-sections` is on but `-fdata-sections` is deliberately off (measured worse),
 * so unreferenced `const` inside a live translation unit is NOT collected -- proved
 * from the other side in design section 10, where the const probe's code and RAM
 * globals were collected and its arrays rode along.  So each generated table header
 * tests this macro around its arrays, and it must already be defined when it does.
 * ------------------------------------------------------------------------- */
#ifndef AVAS_CK_VOICE_TYPE_LB
#define AVAS_CK_VOICE_TYPE_LB         (0)
#endif
#if ((AVAS_CK_VOICE_TYPE_LB) != 0) && ((AVAS_CK_VOICE_TYPE_LB) != 1)
#error "AVAS_CK_VOICE_TYPE_LB must be 0 (Type_TY L1) or 1 (Type_LB L3) -- one voice is compiled per image until the ROM diet lands."
#endif

/* BOTH VOICES RESIDENT, chosen at RUN TIME (design section 3.4, phase 7).  It began
 * as a measurement switch -- "how many bytes do two coefficient sets cost" -- and
 * kept the name when the answer turned out to fit; what changed is that the engine
 * now really renders whichever voice was last selected, so the sets have to be
 * REACHABLE and not merely compiled.  That distinction is the whole finding behind
 * this macro and it is recorded where the selection lives (avas_synth_line_ck.c):
 * the coefficient arrays have internal linkage, so an unreferenced set is dropped by
 * the COMPILER, before --gc-sections or -fdata-sections get a say.
 *
 * AVAS_CK_VOICE_TYPE_LB still means something when this is set: it is the BOOT voice.
 * Unset, the image holds exactly one voice and avas_line_ck_voice_set() refuses the
 * other one with a reason rather than silently doing nothing.
 *
 * WHAT IT COSTS, so the single-voice image can stay the reference: the per-sample
 * carrier loop's bound has to come from the descriptor instead of a literal (+1 to
 * +2 instructions per cluster per sample, measured in doc section 11), and the state
 * arrays are sized for the wider voice.  Both are inside `#if` so an image built
 * without this is unchanged -- proved by size, not by argument. */
#ifndef AVAS_CK_VOICE_BOTH
#define AVAS_CK_VOICE_BOTH          (0)
#endif

/* -------------------------------------------------------------------------
 * NO VOICE AT ALL -- the debug mode, on the owner's instruction:
 * 「こういう難解なデバッグをするときにすぐ、という意味です。デバッグモード用 AVAS 全 OFF
 * モード」.  It is a STANDING facility, not scaffolding for one bug.
 *
 * WHAT IT IS FOR.  This image sits at 98 % of flash with both voices resident, and
 * that ceiling has already decided technical questions on its own -- section 11's
 * `*tm` had to become lab-only to buy 4 287 B, and the ROM diet exists at all because
 * a feature would otherwise not link.  The cost that is easy to miss is the one paid
 * during DEBUGGING: an investigation that needs instrumentation (counters on a
 * periodic line, a register snapshot, a history ring) is not merely inconvenient at
 * 912 B free, it is abandoned.  This macro is the escape hatch for exactly that, and
 * the console-RX deafness investigation is its first customer.
 *
 * WHY IT IS NOT ANOTHER "WHICH VOICE" VALUE.  AVAS_CK_VOICE_TYPE_LB and _BOTH answer
 * "which", and every downstream `#if` -- here, in both table headers, in the engine --
 * is written in those terms.  "None" is a different question ("whether"), so it gets
 * its own macro and then FORCES the others to a consistent answer below, rather than
 * being smuggled in as a third value that six existing conditions would each have to
 * learn about.
 *
 * WHAT IT DOES NOT RECLAIM: RAM.  The state arrays are still sized from the table
 * macros, because those macros are what the struct is written in terms of and the
 * engine that would index them is gone anyway.  Flash is what this part is short of;
 * if an investigation ever needs the RAM too, shrink it then and measure it then.
 * ------------------------------------------------------------------------- */
#ifndef AVAS_CK_VOICE_NONE
#define AVAS_CK_VOICE_NONE          (0)
#endif
#if ((AVAS_CK_VOICE_NONE) != 0) && ((AVAS_CK_VOICE_NONE) != 1)
#error "AVAS_CK_VOICE_NONE must be 0 (an AVAS voice is compiled) or 1 (debug mode: no voice at all)."
#endif

/* The engine's own switch, derived once so the engine and the tables test the same
 * thing.  Named HAVE_ engine to read like AVAS_LINE_CK_HAVE_NOISE below. */
#define AVAS_LINE_CK_HAVE_ENGINE    ((AVAS_CK_VOICE_NONE) == 0)

#if (AVAS_CK_VOICE_NONE)
/* NONE wins, and it wins HERE -- before either table header is read, for the same
 * preprocessor reason the voice choice is made here (see the comment above
 * AVAS_CK_VOICE_TYPE_LB).  Forcing the selectors to "one voice, Type_TY, no noise" means
 * every existing condition downstream stays valid and unread-modified; the arrays are
 * then dropped by the one guard in avas_type_ty_ck_tables.h that tests NONE, and the
 * code that would reference them is dropped by AVAS_LINE_CK_HAVE_ENGINE in the .c.
 * AVAS_CK_VOICE_TYPE_LB_NOISE needs no forcing: AVAS_LINE_CK_HAVE_NOISE is already an
 * AND with the two selectors, so it falls out at 0. */
#undef  AVAS_CK_VOICE_TYPE_LB
#define AVAS_CK_VOICE_TYPE_LB         (0)
#undef  AVAS_CK_VOICE_BOTH
#define AVAS_CK_VOICE_BOTH          (0)
#endif

#include "avas_type_ty_ck_tables.h"
#if (AVAS_CK_VOICE_TYPE_LB) || (AVAS_CK_VOICE_BOTH)
#include "avas_type_lb_ck_tables.h"
#endif

/* -------------------------------------------------------------------------
 * THE NOISE HALF, and why it has its own switch rather than riding on
 * AVAS_CK_VOICE_TYPE_LB.
 *
 * L3 = tone + noise, and the noise is 80.2 % of the reference sound's energy -- so a
 * Type_LB without it is not a quiet Type_LB, it is a different instrument.
 * It is nonetheless separable, because the two costs are separable: the bank is
 * about 100 bytes of RAM and 12 x 3 multiplies a sample, and this part's binding
 * constraint is flash, not either of those.  Being able to build the tone half alone
 * is what makes "what did the bank cost" a subtraction instead of an estimate.
 *
 * Default ON wherever the Type_LB is compiled: shipping the tone half by default
 * would mean the default build is the one nobody wants to listen to.
 * ------------------------------------------------------------------------- */
#ifndef AVAS_CK_VOICE_TYPE_LB_NOISE
#define AVAS_CK_VOICE_TYPE_LB_NOISE   (1)
#endif

#define AVAS_LINE_CK_HAVE_NOISE                                                     \
    (((AVAS_CK_VOICE_TYPE_LB) || (AVAS_CK_VOICE_BOTH)) && (AVAS_CK_VOICE_TYPE_LB_NOISE))

#if (AVAS_LINE_CK_HAVE_NOISE)
#include "avas_noise_bank_ck.h"
#endif

/* Opt-in only -- see the header comment.  A build that never defines
 * AVAS_TYPE_TY_CK_APP_FS_HZ is guarded by avas_rate_matches() at run time instead. */
#if defined(AVAS_TYPE_TY_CK_APP_FS_HZ) && \
    ((AVAS_TYPE_TY_CK_APP_FS_HZ) != (AVAS_TYPE_TY_CK_TABLE_FS_HZ))
#error "avas_type_ty_ck: the coefficient table's fs does not match the app's -- regenerate it (see this header). A silent mismatch is a pitch error, not a failure."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Part mask -- which halves of the engine actually run.
 *
 * WHAT THIS IS FOR NOW, WHICH IS NOT WHAT IT WAS FOR. It was written when the
 * whole engine did not fit -- 768 us per 32-frame block against a 667 us period,
 * i.e. 115 % -- and running part of it was the only way to exercise the transport
 * at all. THAT IS CLOSED: the engine fits with margin (see the summary at
 * boards/ev88g73a/main.c's *tb), and the 115 % turned out to be generated code rather than the algorithm.
 *
 * The mask is kept because switching ENVELOPE on and off is how the 185 lines'
 * contribution is HEARD rather than argued about. It is no longer a way to make
 * the engine fit.
 *
 *   CARRIERS  the AVAS_TYPE_TY_CK_CLUSTERS full-rate oscillators. Without these
 *             there is no output at all, so this is a measurement mode only.
 *   ENVELOPE  the 185 baseband oscillators, rebuilt every DEC samples. Without
 *             these the envelope FREEZES -- at whatever it had reached, NOT at its
 *             t=0 value, and the slopes are zeroed to make that true (see
 *             avas_line_ck_freeze_envelope(); this comment used to claim "the
 *             slopes stay zero", which was only true from a reset, and the missing
 *             zeroing turned *tb0007 -> *tb0005 into a runaway ramp that clipped
 *             and sounded like pink noise). The engine then degrades to CLUSTERS
 *             pure tones at the cluster centroid frequencies. That still makes
 *             sound -- it is close to the "harmonic grid" model the AK work
 *             abandoned for sounding thin, which is a fair description of what to
 *             expect from it, and switching the part on and off is the cheapest way
 *             to HEAR what the 185 lines are contributing.
 *   GATE      the one-pole fade. Cheap; separable only so its cost is known
 *             rather than assumed.
 *   NOISE     the Type_LB's 12-band noise bank -- 80.2 % of that reference sound's
 *             energy, and the one part whose contribution is genuinely hard to
 *             predict by reading a spectrum, so being able to A/B it on the board is
 *             worth more here than for any other part.  Present in every build (it
 *             is a mask bit, not code) but only ACTS when the sounding voice is the
 *             Type_LB: the Type_TY set has no noise half to switch off.
 * ------------------------------------------------------------------------- */
#define AVAS_TYPE_TY_CK_PART_CARRIERS  (0x01u)
#define AVAS_TYPE_TY_CK_PART_ENVELOPE  (0x02u)
#define AVAS_TYPE_TY_CK_PART_GATE      (0x04u)
#define AVAS_TYPE_TY_CK_PART_NOISE     (0x08u)
#define AVAS_TYPE_TY_CK_PART_ALL       (0x0Fu)

/*
 * The console's own description of the mask, in ONE place. It used to be spelled
 * out at each print site, and the copies drifted the moment NOISE was added: the
 * board answered `?tb: parts = 15` while listing only `1=carriers 2=envelope
 * 4=gate`, and offered `*tb0007 = all` for a mask whose all is now 0x0F. A help
 * string that contradicts the value printed next to it is worse than none, and
 * three literals is exactly how that happens twice.
 */
#define AVAS_TYPE_TY_CK_PART_HELP      "1=carriers 2=envelope 4=gate 8=noise"

/* -------------------------------------------------------------------------
 * One carrier's state, INTERLEAVED -- and the interleaving is a measured
 * decision, not a style preference.
 *
 * Held as five parallel arrays (the obvious layout) the carrier loop needs a
 * separate base address per array, and this core has too few W registers to keep
 * them: XC-DSC spilled the phase to the stack and re-read it three times per
 * carrier, and rebuilt each array address from a constant offset every iteration
 * (`mov #40,w0 / add.w w0,w12,w0` twice per envelope update).  Measured, that
 * layout cost 112 instructions per carrier per sample.
 *
 * Interleaved, ONE pointer walks the whole thing instead of five.  Note what that
 * does NOT buy, because this comment claimed it until the listing was read: a
 * fixed displacement is a single-instruction addressing mode for `mov`, but NOT
 * for `add`, which takes `[Ws]` and no displacement at all.  So each 32-bit update
 * still pays `add.w w1,#8,w4` to build a pointer first -- 5 such instructions per
 * carrier per sample.
 *
 * Reordering the fields so each accumulator is immediately followed by its delta
 * was tried and MEASURED AS WORTHLESS -- 5 pointer builds either way, because the
 * compiler builds a fresh pointer per update regardless of how close the operands
 * sit.  The order below is therefore the original one.  Nothing about the
 * arithmetic changed.
 *
 * env_* (and polar's amp) is avas_type_ty_ck_env_t, which at the SHIPPED
 * AVAS_TYPE_TY_CK_ENVFRAC of 0 (V3, section 25) IS the int16 Q15 value: one word per
 * interpolated quantity, one 16-bit add per carrier per sample, and no read-out shift
 * because there is nothing below the value to shift off.
 *
 * At ENVFRAC=16 the same field is an int32 whose TOP 16 BITS are the Q15 value -- the
 * carrier loop reads that part as the high word, which on a 16-bit core is free, while
 * the low 16 bits carry the slope's fraction so a slow cluster's slope does not
 * quantise.  Section 8 justified that wide state as "a Q15-only slope would truncate
 * to zero for the weak clusters" and section 19 MEASURED that sentence to be wrong on
 * both counts: what quantises is set by lines per cluster, not by level, and nothing
 * truncates to zero at any width.  What the narrow state actually costs is a BIAS,
 * because C's >> floors asymmetrically -- which AVAS_TYPE_TY_CK_ENVROUND removes for one
 * add per cluster per rebuild, and that pairing is enforced in the generated header.
 * The width is a build-time choice for that reason and not this comment's.
 *
 * POLAR (AVAS_TYPE_TY_CK_ENVINTERP == 1) carries a DIFFERENT state, which is why the
 * fields are #if'd rather than reused under the old names: a wrong access then
 * fails to compile instead of quietly meaning something else.  It is also smaller
 * -- one interpolated quantity instead of two -- and what replaces the second one
 * is not a state at all but a modification of the carrier's own step.
 * ------------------------------------------------------------------------- */
typedef struct
{
    /* Full-rate carrier phase; the full cycle is 2^AVAS_TYPE_TY_CK_PHBITS_CAR, and
     * the accumulator's overflow IS the modulo at either width.  In polar this
     * accumulator carries theta + phi, which is the whole trick: the envelope's
     * phase rides in the carrier's own phase and costs no add of its own. */
    avas_type_ty_ck_carph_t phase;
#if ((AVAS_TYPE_TY_CK_ENVINTERP) == 1)
    /* The carrier step with the envelope's phase SLOPE folded in, i.e.
     * s_ck_car_step[k] + dphi, recomputed once per rebuild.  This is where the
     * nominal step's RAM copy went: the loop reads this instead, and the rebuild
     * reads the nominal one straight out of flash (once per DEC samples, where an
     * address computation costs nothing). */
    avas_type_ty_ck_carph_t step_eff;
    /* The phi the accumulator has actually REACHED, which is not the phi the last
     * rebuild aimed at: dphi is truncated by >> DECSHIFT, and taking the next
     * difference from the reached value is what stops that truncation
     * accumulating into a pitch error. */
    avas_type_ty_ck_carph_t phi_app;
    avas_type_ty_ck_env_t amp;      /* |Z_k|, Q15 above AVAS_TYPE_TY_CK_ENVFRAC fraction bits */
    avas_type_ty_ck_env_t amp_da;   /* per-sample slope towards the next rebuild */
#else
    /* A COPY of s_ck_car_step[k], 44 bytes of RAM (22 at 16-bit phase) to hold
     * what flash already holds.  It buys the loop two W registers -- the flash
     * table's walking pointer and its end sentinel -- on a core that had too few
     * to keep the output accumulator out of memory. */
    avas_type_ty_ck_carph_t step;
    avas_type_ty_ck_env_t env_i;
    avas_type_ty_ck_env_t env_q;
    avas_type_ty_ck_env_t env_di;   /* per-sample slope towards the next rebuild */
    avas_type_ty_ck_env_t env_dq;
#endif
} avas_line_ck_car_t;

/* -------------------------------------------------------------------------
 * ONE VOICE'S COEFFICIENTS, and why they are a struct rather than symbols.
 *
 * This engine is not "the Type_TY engine with a table" -- everything that makes it
 * fast (the per-worst-cluster Q15 scale, 16-bit phase whose overflow IS the
 * modulo, the shared {value, difference} sine table, the 40-bit accumulator, the
 * polar interpolation, the 16-bit envelope) is arithmetic that does not know which
 * vehicle it is rendering.  What is vehicle-specific is exactly this: five flash
 * arrays, two counts and one level.  Naming them through a descriptor is what lets
 * a second voice cost ZERO new per-sample code.
 *
 * The cost of the indirection is a register-pressure question and not a
 * cycle-count one, which on this core is the same sentence as "it has to be
 * measured": three earlier layout changes here were priced by spills rather than
 * by arithmetic (sections 15 and 19 of the Type_TY doc).  Where it lands matters --
 * the per-SAMPLE loop needs only `clusters`, the three line tables are read once
 * per DEC samples, and the rest is reset-time -- so measure it, do not assume it.
 * ------------------------------------------------------------------------- */
typedef struct
{
    const int16_t               *amp_q15;        /* [lines]        Q15 line amplitudes */
    const avas_type_ty_ck_bbstep_t *bb_step;        /* [lines]        signed baseband step */
    const avas_type_ty_ck_bbph_t   *bb_pha0;        /* [lines]        measured start phases */
    const avas_type_ty_ck_carph_t  *car_step;       /* [clusters]     carrier steps */
    const uint16_t              *cluster_first;  /* [clusters + 1] cluster boundaries */
    uint16_t lines;
    uint16_t clusters;
    int16_t  norm_gain_q15;      /* the reference WAV's level for THIS voice */
} avas_line_ck_set_t;

/* The state is sized for the widest voice COMPILED IN, not for the one selected --
 * so with both resident it is a max() over the two sets, which is where this comment
 * said it would end up.  RAM is not the constraint here -- 770 B against 2 682 B
 * free -- so the sets are not unioned; that would trade a measured non-problem for a
 * reset-ordering bug.  Nor is the max() taken per voice pair by hand: a third voice
 * would then size the state from whichever two were remembered. */
#if (AVAS_CK_VOICE_BOTH)
#define AVAS_LINE_CK_MAX_LINES                                                      \
    (((AVAS_TYPE_LB_CK_LINES) > (AVAS_TYPE_TY_CK_LINES)) ? (AVAS_TYPE_LB_CK_LINES)          \
                                                    : (AVAS_TYPE_TY_CK_LINES))
#define AVAS_LINE_CK_MAX_CLUSTERS                                                   \
    (((AVAS_TYPE_LB_CK_CLUSTERS) > (AVAS_TYPE_TY_CK_CLUSTERS)) ? (AVAS_TYPE_LB_CK_CLUSTERS) \
                                                          : (AVAS_TYPE_TY_CK_CLUSTERS))
#elif (AVAS_CK_VOICE_TYPE_LB)
#define AVAS_LINE_CK_MAX_LINES      (AVAS_TYPE_LB_CK_LINES)
#define AVAS_LINE_CK_MAX_CLUSTERS   (AVAS_TYPE_LB_CK_CLUSTERS)
#else
#define AVAS_LINE_CK_MAX_LINES      (AVAS_TYPE_TY_CK_LINES)
#define AVAS_LINE_CK_MAX_CLUSTERS   (AVAS_TYPE_TY_CK_CLUSTERS)
#endif

/* The SELECTED voice's cluster count, which is a compile-time constant for exactly
 * as long as one voice is compiled per image (design section 3.4).  The per-sample
 * carrier loop compares against THIS and not against the descriptor, because the
 * descriptor's version was measured at +1 to +2 instructions per cluster per sample
 * (doc section 11) for a number that cannot change in such an image.  The
 * descriptor still owns the tables, the line count and the level -- everything read
 * per DEC samples or at reset.  When both voices become resident this has to give
 * way to s->set->clusters, and the engine has a static assertion where the set is
 * defined so the two cannot silently disagree in the meantime.
 *
 * AVAS_CK_VOICE_BOTH IS THAT "AFTERWARDS", and the two forms are separated here so
 * the single-voice image keeps the codegen this paragraph is about.  In a both-voices
 * image AVAS_LINE_CK_CLUSTERS still names the BOOT voice's count -- it is what the
 * build-time assertions are written against -- while the loop takes its bound from
 * AVAS_LINE_CK_LOOP_CLUSTERS(), i.e. from the descriptor, because in that image the
 * number really can change between one block and the next. */
#if (AVAS_CK_VOICE_TYPE_LB)
#define AVAS_LINE_CK_CLUSTERS       (AVAS_TYPE_LB_CK_CLUSTERS)
#else
#define AVAS_LINE_CK_CLUSTERS       (AVAS_TYPE_TY_CK_CLUSTERS)
#endif

#if (AVAS_CK_VOICE_BOTH)
#define AVAS_LINE_CK_LOOP_CLUSTERS(s)   ((s)->set->clusters)
#else
#define AVAS_LINE_CK_LOOP_CLUSTERS(s)   ((uint16_t)(AVAS_LINE_CK_CLUSTERS))
#endif

/* -------------------------------------------------------------------------
 * THE OUTPUT GAIN'S LEFT SHIFT, which is a property of the coefficient SET and not
 * of the engine -- so it comes out of the selected voice's table header, like the
 * gain itself.
 *
 * A voice whose output gain is below unity has shift 0, and then the return
 * statement below is textually the single `>> 15` this engine has always done: the
 * Type_TY image is unaffected, which is what keeps it byte-identical.  A voice whose
 * gain EXCEEDS unity needs the shift and a CLAMP, and the Type_LB is such a
 * voice at some spans: A_SCALE is set by the widest cluster's amplitude sum while
 * the gain's numerator is the peak the whole sum reaches, and those two are only
 * nearly equal when the clusters are narrow (Type_TY: ratio 1.026, gain 0.877).
 *
 * The clamp is not decoration.  The normalisation targets a 60 s peak and
 * quasi-periodic beating keeps growing past it, so a gain above unity WILL be asked
 * to produce more than full scale eventually -- and an int16 cast of that is a
 * wrap, i.e. a full-scale click, where saturation is a moment of soft distortion.
 * ------------------------------------------------------------------------- */
#if (AVAS_CK_VOICE_TYPE_LB)
#define AVAS_LINE_CK_NORM_SHIFT     (AVAS_TYPE_LB_CK_NORM_SHIFT)
#else
#define AVAS_LINE_CK_NORM_SHIFT     (AVAS_TYPE_TY_CK_NORM_SHIFT)
#endif

/* THE ONE THING THAT DID NOT BECOME RUN-TIME, and it is deliberate.  The gain itself
 * is per voice and lives in the descriptor (norm_gain_q15), but the shift sits in the
 * return statement's `>> (15 - shift)` -- making it a variable would put a variable
 * shift in the output path of every sample to express a number that is 0 for both
 * voices we have.  So both resident voices must agree on it, and disagreeing is a
 * build failure with the fix named.  A voice whose gain exceeds unity is what would
 * break the tie; when one turns up, the shift joins the descriptor and this goes. */
#if (AVAS_CK_VOICE_BOTH) && ((AVAS_TYPE_TY_CK_NORM_SHIFT) != (AVAS_TYPE_LB_CK_NORM_SHIFT))
#error "avas_line_ck: the two resident voices need different output shifts -- move NORM_SHIFT into avas_line_ck_set_t (see this header), do not pick one."
#endif

typedef struct
{
    /* Which voice this state is rendering.  Read once per sample (for the cluster
     * count) and per DEC samples (for the tables); never NULL after init(). */
    const avas_line_ck_set_t *set;

    /* One baseband oscillator per line, running at fs/DEC.  Phase only: the
     * step is a flash constant (it depends on nothing but f, fc, DEC and fs) and
     * the amplitude is read straight out of the table, so neither costs RAM.
     * Left as a plain array: this one IS walked linearly by a single pointer, so
     * it has none of the addressing problem the carrier state had.
     *
     * The width is AVAS_TYPE_TY_CK_PHBITS_BB: 740 bytes at 32 bits, 370 at 16, which
     * on a part sitting at 5700 of 8192 bytes of data memory is the largest single
     * RAM item this engine owns. */
    avas_type_ty_ck_bbph_t bb_phase[AVAS_LINE_CK_MAX_LINES];

    avas_line_ck_car_t car[AVAS_LINE_CK_MAX_CLUSTERS];

    uint16_t dec_count;      /* samples left before the next envelope rebuild */

    int16_t out_gain_q15;    /* set->norm_gain_q15 scaled by user gain */

    int32_t gate;            /* Q31 */
    int32_t gate_target;     /* Q31: 0 or INT32_MAX */

    uint8_t parts;           /* AVAS_TYPE_TY_CK_PART_* mask; init sets ALL */

    /* Pitch trim, cent and Q14 ratio, in the committed/requested pairs the rest of
     * this file already uses for cross-context handoff (out_gain_q15 has no such
     * pair because a caller writing it mid-block is not a defect -- gain has no
     * "boundary" to respect the way a step table does).  See
     * avas_line_ck_request_pitch_cent() for why pitch needs one. */
    int16_t pitch_cent;            /* committed: what is actually sounding */
    int16_t pitch_cent_req;        /* requested: what the next rebuild will apply */
    int16_t pitch_ratio_q14;       /* committed ratio, unity = AVAS_PITCH_CK_RATIO_Q14_UNITY */
    int16_t pitch_ratio_req_q14;   /* requested ratio */
    uint8_t pitch_req_pending;     /* set => copy the *_req pair in at the next rebuild */
    uint8_t pitch_req_on_silence;  /* set => reset to 0 cent once truly silent (stop) */

#if (AVAS_LINE_CK_HAVE_NOISE)
    /* The Type_LB's noise half.  Costs about 100 bytes of RAM and is present in
     * the state of a both-voices build even while the Type_TY is sounding: the
     * alternative is a union, and a union would make "switch voices" a question about
     * lifetimes in a struct the ISR walks.  RAM is what this part has (2 592 bytes
     * free); the thing it does not have is flash. */
    avas_noise_bank_ck_t noise;

    /* Cached per voice-selection rather than recomputed per sample: whether the
     * sounding voice HAS a noise half.  The alternative -- comparing s->set against
     * the Type_LB descriptor inside the sample loop -- is a pointer load and a
     * compare 48 000 times a second to answer a question that changes only when
     * someone presses a key. */
    uint8_t noise_on;
#endif
} avas_line_ck_t;

/* Zero the state, load the measured phases, set the level to the reference
 * WAV's (0.9 peak), and leave the gate closed. */
void avas_line_ck_init(avas_line_ck_t *s);

/* Restart every oscillator from its measured phase.  The measured phases are
 * only mutually meaningful at t = 0 of the analysed segment, so this is how the
 * synth is put back into the state the reference WAV starts from. */
void avas_line_ck_reset_phase(avas_line_ck_t *s);

/* -------------------------------------------------------------------------
 * WHICH VOICE, at run time.
 *
 * These exist in EVERY build, including the single-voice ones, and that is the
 * point: a caller asks for a voice and is told yes or no, instead of the caller
 * carrying an `#if` to know which question it is allowed to ask.  In a one-voice
 * image avas_line_ck_voice_set() answers false for the absent voice and the console
 * layer turns that into a printed reason -- the same shape as "the rate does not
 * match", which is the other thing that makes starting the synth refuse.
 *
 * SWITCHING IS NOT CROSSFADING.  voice_set() re-seeds every oscillator from the new
 * voice's measured t = 0 phases, so calling it while the engine is sounding would be
 * a discontinuity in every one of them at once -- a click, and a loud one at these
 * levels.  It therefore refuses unless the gate is fully closed, which makes the
 * exclusivity the caller wants an invariant of this module rather than a rule the
 * caller is trusted to remember.  "Silent" is avas_line_ck_is_active() and not
 * `gate == 0`, which matters in both directions and is argued where it is tested.
 * The `parts` mask is preserved across a switch, because losing *tb to a keypress
 * would be its own surprise while listening; the LEVEL is not, since it was a scale
 * on the other voice's reference peak.
 * ------------------------------------------------------------------------- */
#define AVAS_LINE_CK_VOICE_TYPE_TY     (0u)
#define AVAS_LINE_CK_VOICE_TYPE_LB    (1u)
#define AVAS_LINE_CK_VOICE_COUNT    (2u)

/* false = not in this image, index out of range, or the engine is still sounding.  It
 * does not distinguish them; the caller that wants to print a reason asks the two
 * questions it can answer itself (voice_present, is_active). */
bool avas_line_ck_voice_set(avas_line_ck_t *s, uint8_t voice);

/* Which voice the state is rendering -- read from s->set, not from a remembered
 * index, so it cannot drift from what is actually sounding. */
uint8_t avas_line_ck_voice(const avas_line_ck_t *s);

/* Is that voice compiled into this image at all? */
bool avas_line_ck_voice_present(uint8_t voice);

/* "type_ty" / "type_lb", for console text.  Valid for any index, in any build --
 * naming a voice does not require having it. */
const char *avas_line_ck_voice_name(uint8_t voice);

/* One sample, Q15, gate applied.  Returns 0 without touching any oscillator
 * once the release fade has finished, which is what makes the cost actually
 * drop to zero rather than merely become inaudible. */
int16_t avas_line_ck_process_sample(avas_line_ck_t *s);

/* One sample, Q15, WITHOUT the gate -- the engine proper.  This is the function
 * tools/host_check/ compares against the numpy model, which has no gate; keeping
 * it separate is what makes that comparison bit-exact rather than approximate. */
int16_t avas_line_ck_render_sample(avas_line_ck_t *s);

/* Linear Q15, applied on top of the reference level: 32767 = the level of
 * out_lines_L1.wav.  Deliberately linear and not dB -- a dB entry point would
 * need a log/exp or a conversion table this module has no other use for, and the
 * one caller that wants dB can own that table. */
void avas_line_ck_set_gain_q15(avas_line_ck_t *s, int16_t gain_q15);

/* -------------------------------------------------------------------------
 * PITCH TRIM, cent (100 cent = 1 semitone), 0 = the table's own pitch.
 *
 * Unlike gain, this cannot just be written and picked up whenever the hot loop
 * next reads it: car_step/bb_step are read once per AVAS_TYPE_TY_CK_DEC samples,
 * ALL clusters/lines in that one rebuild call, and every one of them has to see
 * the same ratio -- a value that changed mid-call would detune only the clusters
 * still to come. So the write goes through a request/commit pair exactly like
 * this module's other cross-context state, and takes effect at the top of the
 * next avas_line_ck_rebuild_envelope() call, not inside this function.
 *
 * Clamped to +/-AVAS_LINE_CK_PITCH_LIMIT_CENT -- a caller does not have to know
 * the limit to be safe, only the console layer that reports the range has to.
 * ------------------------------------------------------------------------- */
#define AVAS_LINE_CK_PITCH_LIMIT_CENT   (200)

void    avas_line_ck_request_pitch_cent(avas_line_ck_t *s, int16_t cent);

/* The committed value (what is actually sounding right now) and the requested
 * one (what the next rebuild will apply) -- separate so a caller can tell
 * "still catching up" from "settled", the same distinction the PRE/POST gain
 * report makes. (Spelled out rather than written as the two console command
 * names, because those two side by side contain a comment-open sequence.) */
int16_t avas_line_ck_get_pitch_cent(const avas_line_ck_t *s);
int16_t avas_line_ck_get_pitch_cent_req(const avas_line_ck_t *s);

void avas_line_ck_gate_on(avas_line_ck_t *s);
void avas_line_ck_gate_off(avas_line_ck_t *s);

/* Which parts run. Setting this does not reset any oscillator, so it can be
 * changed between blocks; switching ENVELOPE back on resumes from wherever the
 * frozen envelope was left, which is continuous rather than a click. */
void avas_line_ck_set_parts(avas_line_ck_t *s, uint8_t parts);

/*
 * True while this engine still renders anything, release tail included.
 *
 * WITH PART_GATE MASKED OFF THIS ANSWERS FROM THE REQUEST, not from the gate, and
 * that is a fix rather than a special case.  Masking the gate off pins `gate` wide
 * open (process_sample() rewrites it every sample, so that what is removed is the
 * fade and not the signal).  A gate that cannot fall cannot report a finished fade
 * -- so this used to answer TRUE FOREVER once the gate part was masked, and a
 * caller that stops the synth and waits for this to go false would wait forever.
 *
 * That is not hypothetical: wm8904_audio.c's chain clears s_chain_avas_run only
 * when this goes false, so `*tb0003` (gate masked) followed by an AVAS stop left the
 * stage summing into the chain for good, with no way back short of a restart.
 */
bool avas_line_ck_is_active(const avas_line_ck_t *s);

#ifdef __cplusplus
}
#endif

#endif /* AVAS_SYNTH_LINE_CK_H */
