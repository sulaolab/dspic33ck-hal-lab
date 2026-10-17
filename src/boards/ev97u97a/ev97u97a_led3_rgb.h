/* EV97U97A audio port, adapted from the DM330030 board implementation. */
/*******************************************************************************
Copyright 2016 Microchip Technology Inc. (www.microchip.com)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*******************************************************************************/

#ifndef EV97U97A_LED3_RGB_H
#define EV97U97A_LED3_RGB_H

#include <stdint.h>
#include <stdbool.h>

#ifndef EV97U97A_RGB_USE_HARDWARE_PWM
#define EV97U97A_RGB_USE_HARDWARE_PWM 1
#endif

#if (EV97U97A_RGB_USE_HARDWARE_PWM != 0) && \
    (EV97U97A_RGB_USE_HARDWARE_PWM != 1)
#error "EV97U97A_RGB_USE_HARDWARE_PWM must be 0 (soft) or 1 (hardware)"
#endif

#ifndef EV97U97A_RGB_PWM_DIAGNOSTIC_MODE
#define EV97U97A_RGB_PWM_DIAGNOSTIC_MODE 0
#endif

#if (EV97U97A_RGB_PWM_DIAGNOSTIC_MODE < 0) || \
    (EV97U97A_RGB_PWM_DIAGNOSTIC_MODE > 2)
#error "EV97U97A_RGB_PWM_DIAGNOSTIC_MODE must be 0 (normal), 1 (high), or 2 (50% PWM)"
#endif

#if (EV97U97A_RGB_PWM_DIAGNOSTIC_MODE != 0) && !EV97U97A_RGB_USE_HARDWARE_PWM
#error "EV97U97A_RGB_PWM_DIAGNOSTIC_MODE requires EV97U97A_RGB_USE_HARDWARE_PWM=1"
#endif

#ifdef __cplusplus
extern "C" {
#endif

void ev97u97a_led3_rgb_on(void);
void ev97u97a_led3_rgb_off(void);
void ev97u97a_led3_rgb_toggle(void);
void ev97u97a_led3_rgb_set_color(uint16_t red, uint16_t green, uint16_t blue);

void ev97u97a_led3_rgb_set_percent(uint8_t red, uint8_t green, uint8_t blue);
void ev97u97a_led3_rgb_get_percent(uint8_t *red, uint8_t *green, uint8_t *blue);

void ev97u97a_led3_red_on(void);
void ev97u97a_led3_red_off(void);
void ev97u97a_led3_red_toggle(void);
void ev97u97a_led3_red_set_intensity(uint16_t intensity);

void ev97u97a_led3_green_on(void);
void ev97u97a_led3_green_off(void);
void ev97u97a_led3_green_toggle(void);
void ev97u97a_led3_green_set_intensity(uint16_t intensity);

void ev97u97a_led3_blue_on(void);
void ev97u97a_led3_blue_off(void);
void ev97u97a_led3_blue_toggle(void);
void ev97u97a_led3_blue_set_intensity(uint16_t intensity);

bool ev97u97a_led3_rgb_init(uint32_t fcy_hz);
void ev97u97a_led3_rgb_tick(void);

void ev97u97a_led3_rgb_print_hw_pwm_diagnostic(void);

#ifdef __cplusplus
}
#endif

#endif
