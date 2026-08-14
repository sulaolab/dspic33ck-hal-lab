#ifndef GAIN_CTRL_H
#define GAIN_CTRL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * gain_ctrl.h -- exponential mute/gain ramp. Q15 gain, advanced once per BLOCK.
 *
 * WAS boards/ev88g73a/ev88g73a_gain_ctrl.{c,h}, AND HAD NO BUSINESS BEING THERE
 * ---------------------------------------------------------------------------
 * Measured before moving it: zero pins, zero ports, zero board registers -- and zero
 * HAL calls of any kind. The whole file is integer arithmetic over a state struct the
 * caller owns. Only the names said EV88G73A.
 *
 * File named after dspic33ak-audio-dsp-sonora's src/apps/classic/dsp/gain_ctrl.c,
 * which this is a fixed-point port of: same state-machine shape (a ramp restarts from
 * the current gain toward the new target; ramp_ms == 0 applies immediately), same
 * mute_on/storedGain policy.
 *
 * The SYMBOLS are prefixed gain_ctrl_* rather than copying upstream's bare
 * gain_init() / mute_set() / audiogain_t, and that is deliberate: upstream's are
 * float-based and this is fixed-point, so the two could not share a struct anyway -- and
 * if the float original is ever vendored into this repo alongside, identical global names
 * would collide at link time. Same file name for grep, distinct symbols for the linker.
 *
 * WHAT CHANGED 2026-08-06: Q31-per-frame LINEAR -> Q15-per-block EXPONENTIAL
 * -------------------------------------------------------------------------
 * The previous version stored gain as Q31 and applied it as
 * `((int64_t)sample * gain) >> 31`. That >>31 is the whole problem: it forces the
 * multiply to 64 bits, and XC16 implements it as a call to `___muldi3` -- a full 64x64
 * signed multiply, measured at about 88 cycles PER SAMPLE, 63% of the block ISR (see
 * docs/ck_silicon_findings.md, "___muldi3: a typing decision costing 88 cycles per
 * sample"). On this core a 16x16 multiply is ONE cycle. The multiply was never the cost;
 * the shift WIDTH was.
 *
 * So gain is now Q15, where unity is 0x8000 and `>>16` is free (it is just naming the
 * high word of a product pair). Two native 16x16 multiplies reconstruct the exact
 * product -- see gain_ctrl.c's local_scale_q15() and the transport header's
 * nora_tdm_slot_scale_q15(), which carry the identity and the proof.
 *
 * Three consequences, all deliberate:
 *
 *   1. THE RAMP ADVANCES ONCE PER BLOCK, not once per frame. A block-invariant gain is
 *      what lets a caller hoist the value into a register and run one flat loop over the
 *      block; per-frame advance forced a call and a fresh load every frame. The cost is
 *      ramp resolution: one step per block (0.67 ms at 48 kHz / 32 frames) instead of one
 *      per sample. That is inaudible on a mute ramp and was accepted explicitly.
 *
 *   2. THE CURVE IS EXPONENTIAL, not linear. A linear ramp needed a per-sample step, and
 *      computing it needed `gain_diff / ramp_samples` -- a 64-bit divide (`___divdi3`)
 *      that ran INSIDE THE AUDIO ISR, because mute_set() is called from the block
 *      callback (that is where the button level is applied). It put a several-hundred-
 *      cycle spike on exactly the block where the user pressed the button. The
 *      exponential curve needs no multiply and no divide at all: each step is
 *      `state +- ((state >> sh1) + (state >> sh2))`, shifts and adds only. It is also
 *      the better curve for a mute ramp -- constant dB per unit time rather than constant
 *      amplitude per unit time.
 *
 *   3. UNITY IS BIT-EXACT. gain == GAIN_CTRL_GAIN_Q15_UNITY reproduces every bit of the
 *      input that carries audio (only bit 0 of the 32-bit slot is dropped, and the codec
 *      word is 24-bit left-justified, so bit 0 is not an audio bit). This matters because
 *      unity IS the steady state -- 100% of the running time on a passthrough -- and the
 *      whole point of putting the gain stage in the default path is to measure its cost,
 *      not to degrade the audio it measures. gain == 0 is a true digital zero with no
 *      special case and no branch.
 *
 * Gain is applied linearly to the 32-bit slot value, so it still does not care what the
 * codec's real audio bit depth is inside that slot.
 */

/* Q31 unity, kept as the unit of storedGain and of gain_ctrl_next_frame_gain()'s return
 * so the Q31-facing surface still means what it always did. */
#define GAIN_CTRL_UNITY ((int32_t)0x7FFFFFFF)

/* The unit the hot path actually uses: Q15, unity = 0x8000, 0 = silence. 0x8000 (not
 * 0x7FFF) because unity must be an exact power of two for the scale to be bit-exact. */
#define GAIN_CTRL_GAIN_Q15_UNITY ((uint16_t)0x8000)

/*
 * Internal ramp state unit: unity = 2^31, so the applied Q15 gain is simply state >> 16.
 * Exposed only because the struct below is -- callers use the accessors, not this.
 */
#define GAIN_CTRL_STATE_UNITY ((uint32_t)0x80000000)

/* Number of ramp curves the ms request is snapped to. See gain_ctrl.c's table -- that table
 * is where a new duration is added, and this count follows it. */
#define GAIN_CTRL_CURVE_COUNT 5

typedef enum {
    GAIN_CTRL_RAMP_IDLE = 0,
    GAIN_CTRL_RAMPING_UP,
    GAIN_CTRL_RAMPING_DOWN,
} gain_ctrl_ramp_status_t;

typedef struct {
    bool     mute_on;
    int32_t  storedGain;      /* Q31: the gain unmute returns to. Unity by default. */

    uint32_t state;           /* current gain, GAIN_CTRL_STATE_UNITY = unity */
    uint32_t target;          /* where the ramp is heading, same unit */
    uint8_t  sh1;             /* one step = state >> sh1 + state >> sh2 (see the table) */
    uint8_t  sh2;

    uint32_t sample_rate_Hz;
    uint16_t block_us;        /* one block's period; what makes the ms request meaningful */
    uint16_t curve_ms[GAIN_CTRL_CURVE_COUNT];   /* what each curve takes AT THIS RATE */

    gain_ctrl_ramp_status_t status;
} gain_ctrl_t;

/*
 * Starts at unity gain, ramp idle.
 *
 * block_frames is the caller's DMA block size (NORA_TDM_BLOCK_FRAMES), and it is
 * required rather than optional: the ramp now steps once per block, so a duration in
 * milliseconds is meaningless without knowing how long a block is. It is used ONCE, here,
 * to precompute each curve's duration at this rate -- which is what keeps the division
 * out of mute_set(), i.e. out of the audio ISR. 0 is treated as 1.
 */
void gain_ctrl_init(gain_ctrl_t *g, uint32_t sample_rate_Hz, uint16_t block_frames);

/*
 * Mirrors upstream gain_ctrl.c's mute_set(): mute_on selects target 0 vs storedGain, then
 * starts (or immediately applies, if ramp_ms == 0) a ramp.
 *
 * ramp_ms is snapped to the nearest available curve rather than realised exactly -- the
 * curves are the shift pairs in gain_ctrl.c's table, and at 48 kHz/32 frames they land on
 * roughly 180 / 300 / 358 / 594 / 791 ms. Snapping is what buys a ramp with no multiply and no
 * divide anywhere; an exact duration would put one of them back in the ISR. The chosen
 * curve is stated once at init, so a request that snapped somewhere surprising is visible
 * rather than silent.
 *
 * SAFE TO CALL FROM THE AUDIO ISR, which is where the mute button's level is applied:
 * shifts, adds and four comparisons, no 64-bit helper call.
 */
void gain_ctrl_mute_set(gain_ctrl_t *g, bool mute_on, uint32_t ramp_ms);

/*
 * What ramp_ms will ACTUALLY take, in ms, after snapping -- for a bring-up banner. Pure:
 * it picks the same curve mute_set() would and changes no state. Returns 0 for a NULL
 * state or a ramp_ms of 0, both of which mean "no ramp".
 */
uint16_t gain_ctrl_ramp_ms_effective(const gain_ctrl_t *g, uint32_t ramp_ms);

/*
 * Advances the ramp by ONE BLOCK and returns the Q15 gain to apply to every slot of that
 * block. This is the hot-path accessor: hoist it out of the loop, then apply the returned
 * value to all block_frames * slots_per_fs slots.
 *
 * Returns GAIN_CTRL_GAIN_Q15_UNITY for a NULL state, i.e. audio passes through unchanged.
 */
uint16_t gain_ctrl_next_block_gain_q15(gain_ctrl_t *g);

/*
 * The same advance, reported in Q31 -- kept because the Q31 gain value is this module's
 * fleet-facing unit (storedGain, GAIN_CTRL_UNITY) and a caller that wants to print or
 * compare a gain should not have to know about the Q15 hot-path representation.
 *
 * IT IS NO LONGER A PER-FRAME CALL. The name is retained deliberately: the AK/sonora side
 * has a function of this name and this signature, and changing it here would break the
 * grep that ties them together. One call still means "advance one step and give me the
 * gain"; a step is now a block. Do not use it in a per-slot loop -- that is what
 * gain_ctrl_next_block_gain_q15() is for, and mixing the two would advance the ramp twice.
 */
int32_t gain_ctrl_next_frame_gain(gain_ctrl_t *g);

/*
 * Applies one block's gain to every slot of a block, frame-major indexing
 * buf[frame * slots_per_fs + slot]. Advances the ramp once (one call = one block).
 *
 * Runs in the audio block ISR on the callers that use it; it allocates nothing and
 * touches no peripheral, which is what makes that safe.
 *
 * Takes plain int32_t sample buffers. On dsPIC33CK the TRANSPORT's buffer element is not
 * an int32_t (it is a wire slot -- see nora_tdm_slot_t, defect 7), so a caller
 * feeding this straight from a TDM ping/pong half will not compile. That is deliberate:
 * this module is a fixed-point port of sonora's float gain_ctrl and is shared in spirit
 * with the AK side, where no such type exists. Coupling it to one part's DMA layout would
 * undo the reason it left boards/ev88g73a/ in the first place -- zero HAL dependencies.
 * Such a caller should use gain_ctrl_next_block_gain_q15() above and do its own traversal,
 * scaling with nora_tdm_slot_scale_q15() as it goes.
 */
void gain_ctrl_process_block(gain_ctrl_t *g,
                             const int32_t *src, int32_t *dst,
                             size_t block_frames, size_t slots_per_fs);

#ifdef __cplusplus
}
#endif

#endif /* GAIN_CTRL_H */
