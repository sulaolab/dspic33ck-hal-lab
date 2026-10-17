/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef DEMO_RGB_POT_BUTTONS_H
#define DEMO_RGB_POT_BUTTONS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void demo_rgb_pot_buttons_init(void);

void demo_rgb_pot_buttons_print_header(void);

void demo_rgb_pot_buttons_set_rgb_manual_percent(uint8_t red, uint8_t green, uint8_t blue);
void demo_rgb_pot_buttons_use_rgb_pot(void);
bool demo_rgb_pot_buttons_rgb_is_manual(void);
void demo_rgb_pot_buttons_get_rgb_percent(uint8_t *red, uint8_t *green, uint8_t *blue);

void demo_rgb_pot_buttons_poll(bool report);

#ifdef __cplusplus
}
#endif

#endif
