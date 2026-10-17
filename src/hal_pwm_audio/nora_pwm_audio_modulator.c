/* SPDX-License-Identifier: MIT-0 */

#include "nora_pwm_audio_modulator.h"

#include <stddef.h>

#define NORA_PWM_AUDIO_Q31_POS_FULL 0x7fffffffu  /* 2^31 - 1 */

bool nora_pwm_audio_modulator_setup(nora_pwm_audio_modulator_t *mod,
                                    uint32_t period, uint32_t min_duty)
{
    if ((mod == NULL) || (period == 0u) || ((period & 1u) != 0u) ||
        (min_duty > (period / 2u)) || (period > 0x80000000u)) {
        return false;
    }
    mod->period = period;
    mod->min_duty = min_duty;
    mod->max_duty = period - min_duty;
    return true;
}

/*
 * amp = period/2 - min_duty < 2^31, so every product below fits in 62 bits.
 *
 * Negative: offset = floor((|x| * amp + 2^30) / 2^31).
 * Positive: offset = round_half_up(x * amp / (2^31 - 1)). The quotient
 *   differs from x * amp / 2^31 by less than amp / 2^31 < 1, so the rounded
 *   estimate q0 = floor((P + 2^30) / 2^31) is either right or one short; it is
 *   one short exactly when 2P >= (2*q0 + 1) * (2^31 - 1). No 64-bit division.
 */
uint32_t nora_pwm_audio_modulate_q31(int32_t sample_q31,
                                     const nora_pwm_audio_modulator_t *mod)
{
    const uint32_t centre = mod->period / 2u;
    const uint32_t amp = centre - mod->min_duty;
    uint64_t p;
    uint32_t offset;

    /* Same result without 64-bit arithmetic when amp fits 16 bits: on 16-bit
     * cores the 64-bit products above are library calls (~3 per sample).
     * Split |x| = hi*2^16 + lo; P = (hi*amp + (lo*amp >> 16))*2^16 + low16,
     * and low16 < 2^16 cannot change bits >= 16 of P + 2^30. r is
     * (P + 2^30) mod 2^31; the positive correction 2P >= (2q0+1)(2^31-1)
     * reduces to r + q0 >= 2^31. Host-checked bit-exact against the 64-bit
     * form (tools/pwm_audio_host). */
    if (amp <= 0xffffu) {
        const uint32_t m = (sample_q31 < 0) ? (uint32_t)0u - (uint32_t)sample_q31
                                            : (uint32_t)sample_q31;
        const uint32_t lo = (uint32_t)(uint16_t)m * (uint16_t)amp;
        const uint32_t phi = (uint32_t)(uint16_t)(m >> 16) * (uint16_t)amp + (lo >> 16);
        const uint32_t t = phi + 0x4000u;
        offset = t >> 15;
        if (sample_q31 < 0) return centre - offset;
        if ((((t & 0x7fffu) << 16) | (lo & 0xffffu)) + offset >= 0x80000000u) ++offset;
        return centre + offset;
    }

    if (sample_q31 < 0) {
        const uint32_t magnitude = (uint32_t)0u - (uint32_t)sample_q31;
        p = (uint64_t)magnitude * amp;
        offset = (uint32_t)((p + 0x40000000u) >> 31);
        return centre - offset;
    }

    p = (uint64_t)(uint32_t)sample_q31 * amp;
    offset = (uint32_t)((p + 0x40000000u) >> 31);
    if ((p << 1) >= (((uint64_t)offset << 1) + 1u) * NORA_PWM_AUDIO_Q31_POS_FULL) {
        ++offset;
    }
    return centre + offset;
}
