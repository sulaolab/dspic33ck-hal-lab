#ifndef DEMO_RGB_POT_BUTTONS_H
#define DEMO_RGB_POT_BUTTONS_H

/*
 * DM330030's small board-control module. Its filename is retained because it is
 * already part of every MPLAB configuration. It owns the DM330030-only physical
 * controls: POT -> AVAS gain/RGB status, LED1 -> final-output activity, LED2 -> switch
 * mirror, and S2/S3's debounced AVAS voice-toggle events.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the S2/S3 debounce sampler after the 1 ms timer is configured. */
void demo_rgb_pot_buttons_init(void);

/* Print the current audio-control assignment once at boot. */
void demo_rgb_pot_buttons_print_header(void);

/*
 * POT RGB source control. Manual percentages hold the colour for visual
 * calibration; restoring automatic mode hands the LED back to the POT ramp.
 */
void demo_rgb_pot_buttons_set_rgb_manual_percent(uint8_t red, uint8_t green, uint8_t blue);
void demo_rgb_pot_buttons_use_rgb_pot(void);
bool demo_rgb_pot_buttons_rgb_is_manual(void);
void demo_rgb_pot_buttons_get_rgb_percent(uint8_t *red, uint8_t *green, uint8_t *blue);

/* Sample the pot, dispatch pending AVAS button events, and print one status line when due. */
void demo_rgb_pot_buttons_poll(bool report);

#ifdef __cplusplus
}
#endif

#endif /* DEMO_RGB_POT_BUTTONS_H */
