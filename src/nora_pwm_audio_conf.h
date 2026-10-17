#ifndef NORA_PWM_AUDIO_CONF_H
#define NORA_PWM_AUDIO_CONF_H

/*
 * nora_pwm_audio_conf.h -- project config for hal_pwm_audio (dspic33ck-hal-lab).
 *
 * The HAL folder ships only nora_pwm_audio_conf.h_example; this project file
 * sits beside nora_spi_i2s_tdm_conf.h, outside the HAL folder. Board backend:
 * boards/ev08p02a/ev08p02a_pwm_audio_hal.c (EV08P02A_PWM_AUDIO=1).
 */

#include <stdint.h>

#include "app_config.h"

#if defined(DSPIC33CK_BOARD_EV08P02A) && EV08P02A_PWM_AUDIO
#define NORA_PWM_AUDIO_ENABLE 1
#else
#define NORA_PWM_AUDIO_ENABLE 0
#endif

#define NORA_PWM_AUDIO_SAMPLE_RATE_HZ   48000u
#define NORA_PWM_AUDIO_FRAMES_PER_BLOCK 32u
#define NORA_PWM_AUDIO_MAX_CHANNELS     1u   /* socket 3 MPQ6612A, L only */

/* The core holds up to three blocks. The ECPLL backend consumes them directly;
 * the optional FRCPLL backend copies them to its own elastic duty FIFO. */
#define NORA_PWM_AUDIO_FIFO_BLOCKS  4u
#define NORA_PWM_AUDIO_PRIME_BLOCKS 2u

/* PG4DC is 16-bit; the period is 4096 (ECPLL) or 4166 (FRCPLL) counts. */
#define NORA_PWM_AUDIO_DUTY_T uint16_t

#endif /* NORA_PWM_AUDIO_CONF_H */
