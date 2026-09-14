#ifndef EV88G73A_IO_H
#define EV88G73A_IO_H

/*
 * ev88g73a_io.{c,h} -- RUNTIME I/O. LED0 and SW0, read and written every loop
 * iteration.
 *
 * Split out of board_ev88g73a.c, which held all three board roles at once. The dividing
 * line is WHEN a thing matters: bring-up runs once and in a fixed order
 * (ev88g73a_board.h), pin facts are compile-time only (ev88g73a_pins.h), and what is
 * here is called forever afterwards.
 *
 * DELIBERATELY NO ev88g73a_io_init(). ev88g73a_board_init() configures LED0's direction
 * and SW0's input+pull-up and is their ONLY configurer. Adding an init here would make
 * two owners of one pin -- the exact defect that DM330030's vendor led1.h/button_s1.h
 * had, where every LED1_On() rewrote TRIS behind the HAL's back. Nothing below touches a
 * direction register.
 *
 * DM330030's dm330030_io.{c,h} is the same role under the same name on the other board
 * (its led_sw + pot, merged for exactly this reason).
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LED0 is ACTIVE-LOW (DS70005517B: RD10 sinks the LED), which is why this takes a
 * logical "on" and inverts rather than exposing the pin level. */
void ev88g73a_led0_set(bool on);
void ev88g73a_led0_toggle(void);

/*
 * SW0, active-low, RAW -- not debounced here.
 *
 * The pull-up it depends on is configured by ev88g73a_board_init() and is load-bearing:
 * RD13 ties to GND through the button and nothing else (DS70005517B Sec.4.2.2), so
 * without it the pin floats when released and this function is meaningless.
 *
 * Callers that need a stable edge debounce against their own polling cadence, which is
 * where the timing knowledge lives (app/wm8904_audio.c's mute toggle does exactly that).
 */
bool ev88g73a_sw0_pressed(void);

#ifdef __cplusplus
}
#endif

#endif /* EV88G73A_IO_H */
