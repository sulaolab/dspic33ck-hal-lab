/* SPDX-License-Identifier: MIT-0 */
#ifndef NORA_PWM_H
#define NORA_PWM_H

/*
 * Minimal NORA PWM (PG) HAL for dsPIC33CK.
 *
 * The surface deliberately follows Sonora's nora_pwm shape: module clock
 * ownership is explicit, generator setup is data-driven, and duty writes are
 * direct.  CK256MP508's PWM1L/PWM2L/PWM3L pins used by DM330030 are fixed
 * peripheral functions, not PPS routes, so pin routing does not belong here.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NORA_PWM_GEN_1 = 0,
    NORA_PWM_GEN_2,
    NORA_PWM_GEN_3,
    NORA_PWM_GEN_COUNT,
} nora_pwm_generator_id_t;

typedef enum {
    NORA_PWM_MODE_COMPLEMENTARY = 0,
    NORA_PWM_MODE_INDEPENDENT,
    NORA_PWM_MODE_PUSH_PULL,
} nora_pwm_output_mode_t;

/* PCLKCON is shared by all PWM generators.  STANDARD is the CPU peripheral
 * clock (Fcy); no board-specific high-frequency clock is required on CK. */
typedef enum {
    NORA_PWM_MCLK_STANDARD = 0,
} nora_pwm_mclk_source_t;

/* period is the complete count, not the register value: the HAL stores
 * period - 1 in PGxPER.  Duty is clipped to period.  In Independent output
 * mode the CK backend mirrors that one duty onto PWMxH (PGxDC) and PWMxL
 * (PGxTRIGB), rather than exposing two board-specific duties. */
typedef struct {
    uint16_t period;
    uint16_t duty_init;
    bool pen_h;
    bool pen_l;
    bool low_active; /* false: active high; true: invert PWMxL/PWMxH */
    nora_pwm_output_mode_t output_mode;
    bool is_module_master;
} nora_pwm_generator_cfg_t;

bool nora_pwm_module_init(nora_pwm_mclk_source_t source);
bool nora_pwm_generator_init(nora_pwm_generator_id_t id,
                             const nora_pwm_generator_cfg_t *cfg);
void nora_pwm_generator_set_duty(nora_pwm_generator_id_t id, uint16_t duty);
bool nora_pwm_generator_is_present(nora_pwm_generator_id_t id);

#endif /* NORA_PWM_H */
