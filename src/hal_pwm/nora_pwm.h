/* SPDX-License-Identifier: MIT-0 */
#ifndef NORA_PWM_H
#define NORA_PWM_H

/*
 * Minimal NORA PWM (PG) HAL for dsPIC33CK.
 *
 * The surface deliberately follows Sonora's nora_pwm shape: module clock
 * ownership is explicit, generator setup is data-driven, and duty writes are
 * direct.  CK256MP508's PWM1L/PWM2L/PWM3L pins used by DM330030 are fixed
 * peripheral functions, not PPS routes. PG4H/L can also use PPS on MC005;
 * pin routing remains the board's responsibility, outside this HAL.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NORA_PWM_GEN_1 = 0,
    NORA_PWM_GEN_2,
    NORA_PWM_GEN_3,
    NORA_PWM_GEN_4,
    NORA_PWM_GEN_COUNT,
} nora_pwm_generator_id_t;

typedef enum {
    NORA_PWM_MODE_COMPLEMENTARY = 0,
    NORA_PWM_MODE_INDEPENDENT,
    NORA_PWM_MODE_PUSH_PULL,
} nora_pwm_output_mode_t;

/* PCLKCON is shared by all PWM generators. STANDARD selects FOSC for a
 * generator whose CLKSEL is 01; DIVSEL does not divide that direct path. */
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

/* Output override: enable=true forces PWMxH and PWMxL to `high`, enable=false
 * returns them to the PWM. Applied at the generator's next SOC when
 * synchronous, otherwise immediately; both pins change in one register
 * write. Same shape as the dsPIC33AK nora_pwm_generator_override(). */
bool nora_pwm_generator_override(nora_pwm_generator_id_t id, bool enable,
                                 bool high, bool synchronous);

/* Stop or restart an initialized generator (PGxCONL.ON) without
 * reprogramming it. */
bool nora_pwm_generator_run(nora_pwm_generator_id_t id, bool on);

#endif /* NORA_PWM_H */
