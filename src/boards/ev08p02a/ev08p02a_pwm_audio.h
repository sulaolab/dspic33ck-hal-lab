#ifndef EV08P02A_PWM_AUDIO_H
#define EV08P02A_PWM_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "nora_spi_i2s_tdm.h"

/* Socket 3 MPQ6612A output; called by the WM8904 port and the foreground loop. */
bool ev08p02a_pwm_audio_start(void);
void ev08p02a_pwm_audio_stop(void);
void ev08p02a_pwm_audio_block(const nora_tdm_slot_t *block);
void ev08p02a_pwm_audio_poll(void);
void ev08p02a_pwm_audio_report(void);
bool ev08p02a_pwm_audio_lab_control(bool enabled);
void ev08p02a_pwm_audio_set_neutral(bool neutral);
/* Runtime bench switches: noise-shaper order 0..2, PWM-only gain in 0.1 dB. */
bool ev08p02a_pwm_audio_set_ns_order(uint8_t order);
bool ev08p02a_pwm_audio_set_gain_x10(int16_t db_x10);
/* 0 normal, 1 DMA-stage 1 kHz tone, 2 elastic pass-through, 3/4 20%/80% duty
 * alternating every 1 or 16 carrier periods (scope check of DMA -> PG4DC). */
bool ev08p02a_pwm_audio_set_test_mode(uint8_t mode);
/* Simulated nFAULT: the same EMERGENCY_SAFE + STAGE latch as a real fault. */
void ev08p02a_pwm_audio_inject_stage_fault(void);

#endif
