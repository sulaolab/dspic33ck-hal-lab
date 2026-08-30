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
 * dm330030_led3_rgb.{c,h} -- the RGB LED, as a soft PWM driven from the 1 ms tick.
 *
 * ITS OWN FILE, and that is the rule rather than an accident: this module holds a STATE
 * MACHINE (three modulation accumulators advanced by dm330030_led3_rgb_tick()), so it is
 * not an accessor and does not belong in dm330030_io.{c,h} with the plain LED/switch/pot
 * reads and writes. A device with state keeps its own file; accessors share one.
 *
 * Pin DIRECTION is dm330030_board.c's, as for every other pin on this board -- the writes
 * below go through the GPIO HAL and never touch TRIS. Before that change each
 * pin_control() rewrote TRIS on every call, and this one is called from the 1 ms tick, so
 * it was doing that thousands of times a second (see dm330030_io.h for the same defect in
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

#ifdef __cplusplus
extern "C" {
#endif

void dm330030_led3_rgb_on(void);
void dm330030_led3_rgb_off(void);
void dm330030_led3_rgb_toggle(void);
void dm330030_led3_rgb_set_color(uint16_t red, uint16_t green, uint16_t blue);

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

void dm330030_led3_rgb_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* DM330030_LED3_RGB_H */
