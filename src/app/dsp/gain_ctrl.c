/*
 * gain_ctrl.c -- see the header for what this is, where it came from, why the file name
 * matches the fleet's while the symbols are prefixed, and why the ramp is exponential and
 * the gain Q15.
 *
 * No board-ownership guard here, unlike the file this was: there is nothing in it that
 * could be wrong on another board. That is the whole reason it moved.
 */

#include "gain_ctrl.h"

#define GAIN_CTRL_DEFAULT_SAMPLE_RATE_HZ (48000u)

/*
 * The smallest state that still produces a non-zero Q15 gain (state >> 16 == 1).
 *
 * It is a working part of the ramp, not a tolerance. Downward: a geometric decay computed
 * with integer shifts STALLS -- once state < 2^sh1 the step evaluates to 0 and the ramp
 * would never reach its target. Upward: a geometric rise out of 0 never leaves 0. So the
 * ramp enters at this floor and exits through it, and both ends are inaudible because the
 * applied gain there is already 1/32768 of unity (-90 dB).
 */
#define GAIN_CTRL_STATE_EPS ((uint32_t)0x00010000)

/*
 * The available ramp curves. One step is `state >> sh1 + state >> sh2`, i.e. a fixed
 * FRACTION of the current gain per block -- shifts and adds, no multiply, no divide,
 * which is what makes gain_ctrl_mute_set() safe to call from the audio ISR.
 *
 * `blocks` is how many steps that fraction needs to cross the full 2^15 range between
 * GAIN_CTRL_STATE_EPS and unity: blocks = ln(32768) / ln(1 + 2^-sh1 + 2^-sh2), computed
 * here rather than at runtime because it is a property of the shift pair alone. Fading
 * DOWN takes about 2% fewer blocks than fading up (the same fraction of a shrinking value
 * is a slightly larger relative step), which is far inside the snapping error below and
 * is not worth a second table.
 *
 * Two shifts rather than one: a single shift only offers ratios of 2 between curves
 * (~180/~360/~720 ms at 48 kHz), and 300 ms -- what this board asks for -- falls between
 * two of them. Adding the second shift halves the spacing at the cost of one add.
 *
 * The table is the ONLY thing that decides which durations exist, so a request the table
 * cannot reach is silently snapped to the nearest entry that it can -- a request for 800 ms
 * against the first four entries delivered 594 ms, which looks like a bug in the caller's
 * config rather than a limit of this table. That is why the fifth entry is here and why
 * adding one is the right way to serve a new duration: a slower ramp costs nothing at run
 * time (the same one add and two shifts per block, just a smaller fraction) and the entry
 * itself costs 4 bytes of flash plus 2 bytes of curve_ms[] RAM.
 */
static const struct {
    uint8_t  sh1;
    uint8_t  sh2;
    uint16_t blocks;
} k_curves[GAIN_CTRL_CURVE_COUNT] = {
    { 5u, 7u,  271u },  /* 1/32 + 1/128 per block -- ~180 ms at 48 kHz / 32 frames */
    { 6u, 7u,  449u },  /* 1/64 + 1/128           -- ~300 ms  (this board's earlier request) */
    { 6u, 8u,  538u },  /* 1/64 + 1/256           -- ~358 ms */
    { 7u, 8u,  892u },  /* 1/128 + 1/256          -- ~594 ms */
    { 7u, 10u, 1188u }, /* 1/128 + 1/1024         -- ~791 ms  (the 800 ms demo ramp) */
};

static uint32_t local_get_valid_sample_rate(uint32_t sample_rate_Hz)
{
    return (sample_rate_Hz != 0u) ? sample_rate_Hz
                                  : GAIN_CTRL_DEFAULT_SAMPLE_RATE_HZ;
}

/*
 * Q31 gain -> internal state. The +1 is what makes unity BIT-EXACT: Q31 unity is
 * 0x7FFFFFFF, one short of the power of two the Q15 scale needs, and 0x7FFFFFFF >> 16 is
 * 0x7FFF -- a gain of 0.99997 that would multiply every steady-state sample instead of
 * passing it through. Everywhere other than unity the +1 is a 1-LSB rounding of a value
 * nobody can hear.
 */
static uint32_t local_q31_to_state(int32_t gain_q31)
{
    return (gain_q31 <= 0) ? 0u : ((uint32_t)gain_q31 + 1u);
}

static int32_t local_state_to_q31(uint32_t state)
{
    return (state >= GAIN_CTRL_STATE_UNITY) ? GAIN_CTRL_UNITY : (int32_t)state;
}

/*
 * x * gain_q15 / 2^15, exactly, with no 64-bit arithmetic.
 *
 * Splitting x into a signed high half and an unsigned low half gives
 *
 *     x * g / 2^15  =  2 * (x_hi * g)  +  (x_lo * g) / 2^15
 *
 * and both halves multiply in ONE cycle each on this core. The form below drops only bit 0
 * of the result -- ((x_lo * g) >> 16) << 1 rather than (x_lo * g) >> 15 -- because the
 * codec word is 24-bit left-justified inside the 32-bit slot, so bit 0 is not an audio bit.
 * If a caller ever needs all 32 bits, OR in ((lo >> 15) & 1u) for two more cycles.
 *
 * The identity, not the instruction count, is the point: this is the SAME arithmetic as
 * the transport header's nora_tdm_slot_scale_q15(), which the audio ISR uses on wire
 * slots. Two code paths that scale audio differently would make the measurement in
 * docs/ck_silicon_findings.md meaningless, so they are kept in step deliberately.
 */
static int32_t local_scale_q15(int32_t x, uint16_t gain_q15)
{
#if defined(__XC16__)
    /* XC16 will not reach the native 16x16 instructions from portable C here: any
     * expression wide enough to hold the product is promoted to 64 bits and becomes a
     * ___muldi3 call (~88 cycles/sample, measured -- see docs/ck_silicon_findings.md).
     * The builtins ARE the fix, so they are written out rather than hoped for. */
    const int32_t  hi = __builtin_mulsu((int16_t)(x >> 16), gain_q15);
    const uint32_t lo = __builtin_muluu((uint16_t)x, gain_q15);

    return (int32_t)(((uint32_t)hi + (lo >> 16)) << 1);
#else
    /* Any other compiler in the fleet (the AK side's XC-DSC targets a 32-bit core, where a
     * 64-bit product is not a library call). Same result, bit for bit, including the
     * dropped bit 0 -- the >>16 <<1 pair is kept rather than written >>15. */
    const int32_t  hi = (int32_t)((int16_t)(x >> 16)) * (int32_t)gain_q15;
    const uint32_t lo = (uint32_t)(uint16_t)x * (uint32_t)gain_q15;

    return (int32_t)(((uint32_t)hi + (lo >> 16)) << 1);
#endif
}

static void local_finish_ramp_now(gain_ctrl_t *g)
{
    g->state  = g->target;
    g->status = GAIN_CTRL_RAMP_IDLE;
}

/* Nearest curve by duration. Four comparisons, no division -- the durations were
 * precomputed for this rate in gain_ctrl_init(). */
static uint8_t local_pick_curve(const gain_ctrl_t *g, uint32_t ramp_ms)
{
    uint32_t best_err = 0xFFFFFFFFu;
    uint8_t  best     = 0u;
    uint8_t  i;

    for (i = 0u; i < (uint8_t)GAIN_CTRL_CURVE_COUNT; i++) {
        const uint32_t ms  = (uint32_t)g->curve_ms[i];
        const uint32_t err = (ms > ramp_ms) ? (ms - ramp_ms) : (ramp_ms - ms);

        if (err < best_err) {
            best_err = err;
            best     = i;
        }
    }

    return best;
}

/* One ramp step = one audio block. Shifts, adds and comparisons only. */
static uint32_t local_advance_one_block(gain_ctrl_t *g)
{
    uint32_t state = g->state;

    if (g->status == GAIN_CTRL_RAMP_IDLE) {
        return state;
    }

    if (g->status == GAIN_CTRL_RAMPING_UP) {
        if (state < GAIN_CTRL_STATE_EPS) {
            state = GAIN_CTRL_STATE_EPS;      /* see GAIN_CTRL_STATE_EPS: 0 cannot grow */
        } else {
            state += (state >> g->sh1) + (state >> g->sh2);
        }

        if (state >= g->target) {
            state = g->target;
            g->status = GAIN_CTRL_RAMP_IDLE;
        }
    } else {
        const uint32_t step = (state >> g->sh1) + (state >> g->sh2);

        state = (state > step) ? (state - step) : 0u;

        /* The EPS floor is the second exit and it is REQUIRED, not a nicety: below it the
         * shifted step is 0 and the decay would stall a few LSBs above the target. */
        if ((state <= g->target) || (state < GAIN_CTRL_STATE_EPS)) {
            state = g->target;
            g->status = GAIN_CTRL_RAMP_IDLE;
        }
    }

    g->state = state;
    return state;
}

void gain_ctrl_init(gain_ctrl_t *g, uint32_t sample_rate_Hz, uint16_t block_frames)
{
    uint32_t rate_kHz;
    uint32_t block_us;
    uint8_t  i;

    if (g == NULL) {
        return;
    }

    if (block_frames == 0u) {
        block_frames = 1u;
    }

    g->mute_on        = false;
    g->sample_rate_Hz = local_get_valid_sample_rate(sample_rate_Hz);
    g->storedGain     = GAIN_CTRL_UNITY;
    g->state          = GAIN_CTRL_STATE_UNITY;
    g->target         = GAIN_CTRL_STATE_UNITY;
    g->sh1            = k_curves[0].sh1;
    g->sh2            = k_curves[0].sh2;
    g->status         = GAIN_CTRL_RAMP_IDLE;

    /*
     * One block's period, in microseconds, and the only division this module performs --
     * once, here, at init. Written as block_frames * 1000 / (rate / 1000) rather than the
     * obvious block_frames * 1000000 / rate so the numerator cannot overflow 32 bits for
     * any block size the transport allows (its own limit is 65535 frames). The rate is
     * rounded to kHz first, which costs 0.2% on 44.1 kHz -- two orders of magnitude
     * smaller than the curve snapping below, so it changes no decision.
     */
    rate_kHz = (g->sample_rate_Hz + 500u) / 1000u;
    if (rate_kHz == 0u) {
        rate_kHz = 1u;
    }

    block_us = ((uint32_t)block_frames * 1000u) / rate_kHz;
    if (block_us > 0xFFFFu) {
        block_us = 0xFFFFu;
    }
    g->block_us = (uint16_t)block_us;

    for (i = 0u; i < (uint8_t)GAIN_CTRL_CURVE_COUNT; i++) {
        uint32_t ms = ((uint32_t)k_curves[i].blocks * block_us) / 1000u;

        if (ms > 0xFFFFu) {
            ms = 0xFFFFu;
        }
        g->curve_ms[i] = (uint16_t)ms;
    }
}

void gain_ctrl_mute_set(gain_ctrl_t *g, bool mute_on, uint32_t ramp_ms)
{
    uint8_t curve;

    if (g == NULL) {
        return;
    }

    g->mute_on = mute_on;
    g->target  = mute_on ? 0u : local_q31_to_state(g->storedGain);

    /* ramp_ms == 0 means "now", and a target we are already at needs no ramp. Both are
     * kept from upstream's mute_set() so the state machine cannot be left RAMPING with
     * nothing to do -- which would cost a step's arithmetic every block, forever. */
    if ((ramp_ms == 0u) || (g->target == g->state)) {
        local_finish_ramp_now(g);
        return;
    }

    curve  = local_pick_curve(g, ramp_ms);
    g->sh1 = k_curves[curve].sh1;
    g->sh2 = k_curves[curve].sh2;

    g->status = (g->target > g->state) ? GAIN_CTRL_RAMPING_UP
                                       : GAIN_CTRL_RAMPING_DOWN;
}

uint16_t gain_ctrl_ramp_ms_effective(const gain_ctrl_t *g, uint32_t ramp_ms)
{
    if ((g == NULL) || (ramp_ms == 0u)) {
        return 0u;
    }

    return g->curve_ms[local_pick_curve(g, ramp_ms)];
}

uint16_t gain_ctrl_next_block_gain_q15(gain_ctrl_t *g)
{
    if (g == NULL) {
        return GAIN_CTRL_GAIN_Q15_UNITY;   /* no state to ramp: pass audio through */
    }

    return (uint16_t)(local_advance_one_block(g) >> 16);
}

int32_t gain_ctrl_next_frame_gain(gain_ctrl_t *g)
{
    if (g == NULL) {
        return GAIN_CTRL_UNITY;            /* no state to ramp: pass audio through */
    }

    return local_state_to_q31(local_advance_one_block(g));
}

void gain_ctrl_process_block(gain_ctrl_t *g,
                             const int32_t *src, int32_t *dst,
                             size_t block_frames, size_t slots_per_fs)
{
    uint16_t gain_q15;
    size_t   i;
    size_t   samples;

    if ((g == NULL) || (src == NULL) || (dst == NULL)) {
        return;
    }

    /*
     * ONE gain for the whole block, hoisted out of the loop, and therefore ONE flat loop
     * instead of the frame/slot nest this used to have: with the gain no longer changing
     * per frame, `base + slot` was just counting contiguous samples the long way round.
     * Frame-major indexing is what makes that safe to flatten.
     */
    gain_q15 = gain_ctrl_next_block_gain_q15(g);
    samples  = block_frames * slots_per_fs;

    for (i = 0u; i < samples; i++) {
        dst[i] = local_scale_q15(src[i], gain_q15);
    }
}
