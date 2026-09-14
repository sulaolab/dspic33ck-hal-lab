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

/*
 * Modified by SulaoLab, 2026 (Apache-2.0 section 4(b)). Changes to Microchip's
 * original: renamed into this board's namespace (was bsp/led3_rgb.h), the pin
 * macros are restated against the GPIO HAL, and the interface notes below were
 * added.
 */

#ifndef DM330030_LED3_RGB_H
#define DM330030_LED3_RGB_H

/*
 * dm330030_led3_rgb.{c,h} -- the RGB LED, with switchable soft/HW PWM.
 *
 * ITS OWN FILE, and that is the rule rather than an accident: this module holds a STATE
 * MACHINE (software modulation accumulators when selected), so it is
 * not an accessor and does not belong in dm330030_io.{c,h} with the plain LED/switch/pot
 * reads and writes. A device with state keeps its own file; accessors share one.
 *
 * Pin DIRECTION is dm330030_board.c's, as for every other pin on this board -- the writes
 * below go through the GPIO HAL and never touch TRIS. Before that change each
 * pin_control() rewrote TRIS on every call, and this one is called from the 4 kHz PWM tick,
 * so it was doing that thousands of times a second (see dm330030_io.h for the same defect in
 * the vendor LED/button headers).
 *
 * The names lost their vendor ALL-CAPS spelling (LED3_RGB_SetColor -> ...set_color) when
 * the board layer was reorganised; the soft-PWM logic in the .c is untouched on purpose.
 * One of those names was also a latent defect: this header declared
 * LED3_GREEND_SetIntensity (note the D) while the .c defined LED3_GREEN_SetIntensity, so
 * the declared function did not exist. Nothing called it, which is why it survived a
 * decade.
 */

#include <stdint.h>
#include <stdbool.h>

/* The supported DM330030 setup has the three J2 jumpers installed: RGB is
 * driven from PWM1H/2H/3H and RE13/14/15 remain High-Z.  Keep that hardware
 * PWM path as the published default.  An unmodified board without the J2
 * links may explicitly select 0 in MPLAB X's preprocessor macros (or with
 * build.ps1 -Define) to restore the legacy software-PWM path. */
#ifndef DM330030_RGB_USE_HARDWARE_PWM
#define DM330030_RGB_USE_HARDWARE_PWM 1
#endif

#if (DM330030_RGB_USE_HARDWARE_PWM != 0) && \
    (DM330030_RGB_USE_HARDWARE_PWM != 1)
#error "DM330030_RGB_USE_HARDWARE_PWM must be 0 (soft) or 1 (hardware)"
#endif

/*
 * Bring-up-only electrical diagnostics, selected independently from the normal
 * soft/HW backend switch:
 *   0 = normal RGB PWM operation
 *   1 = force the enabled PWM pads high through PWM output override
 *   2 = force the J2-connected PWM1H/2H/3H pads to a continuous 50% waveform
 * Modes 1 and 2 deliberately ignore normal RGB duty requests.  Together they
 * separate the physical pad path from the waveform-generation path.
 */
#ifndef DM330030_RGB_PWM_DIAGNOSTIC_MODE
#define DM330030_RGB_PWM_DIAGNOSTIC_MODE 0
#endif

#if (DM330030_RGB_PWM_DIAGNOSTIC_MODE < 0) || \
    (DM330030_RGB_PWM_DIAGNOSTIC_MODE > 2)
#error "DM330030_RGB_PWM_DIAGNOSTIC_MODE must be 0 (normal), 1 (high), or 2 (50% PWM)"
#endif

#if (DM330030_RGB_PWM_DIAGNOSTIC_MODE != 0) && !DM330030_RGB_USE_HARDWARE_PWM
#error "DM330030_RGB_PWM_DIAGNOSTIC_MODE requires DM330030_RGB_USE_HARDWARE_PWM=1"
#endif

#ifdef __cplusplus
extern "C" {
#endif

void dm330030_led3_rgb_on(void);
void dm330030_led3_rgb_off(void);
void dm330030_led3_rgb_toggle(void);
void dm330030_led3_rgb_set_color(uint16_t red, uint16_t green, uint16_t blue);

/*
 * Explicit brightness interface for applications and interactive calibration.
 * Each component is a linear duty request in the inclusive 0..100 percent range;
 * values above 100 are clipped. This board API is deliberately independent of
 * any particular indicator (the POT is only one client of it).
 */
void dm330030_led3_rgb_set_percent(uint8_t red, uint8_t green, uint8_t blue);
void dm330030_led3_rgb_get_percent(uint8_t *red, uint8_t *green, uint8_t *blue);

void dm330030_led3_red_on(void);
void dm330030_led3_red_off(void);
void dm330030_led3_red_toggle(void);
void dm330030_led3_red_set_intensity(uint16_t intensity);

void dm330030_led3_green_on(void);
void dm330030_led3_green_off(void);
void dm330030_led3_green_toggle(void);
void dm330030_led3_green_set_intensity(uint16_t intensity);

void dm330030_led3_blue_on(void);
void dm330030_led3_blue_off(void);
void dm330030_led3_blue_toggle(void);
void dm330030_led3_blue_set_intensity(uint16_t intensity);

/* Starts the selected RGB backend.  HW mode needs no service interrupt. */
bool dm330030_led3_rgb_init(uint32_t fcy_hz);
void dm330030_led3_rgb_tick(void);

/* One-shot boot diagnostic for the selected RGB PWM backend. */
void dm330030_led3_rgb_print_hw_pwm_diagnostic(void);

#ifdef __cplusplus
}
#endif

#endif /* DM330030_LED3_RGB_H */
