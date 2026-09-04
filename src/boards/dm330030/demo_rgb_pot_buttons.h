#ifndef DEMO_RGB_POT_BUTTONS_H
#define DEMO_RGB_POT_BUTTONS_H

/*
 * demo_rgb_pot_buttons.h -- DM330030's inherited Microchip demo: the
 * potentiometer sets the intensity of one RGB-LED channel, S1/S2 select which
 * channel, S3 forces white, and a fixed screen region reports all of it.
 *
 * WHY THIS IS boards/dm330030/ AND NOT app/
 * -----------------------------------------
 * The move test the rest of this tree uses (see docs/ck_source_layout.md) is: grep
 * the file for a pin, a port, a board register; if what comes back is VALUES, it
 * can move to app/ behind a seam. What comes back here is neither pins nor values
 * but three BOARD APIs -- dm330030_led3_rgb_set_color(), dm330030_led_*()/dm330030_sw_*(),
 * dm330030_pot_read(). Putting this in app/ would need a four-hook
 * seam (read_pot / set_rgb / sw_pressed / led_set) whose only implementer would
 * be this board, permanently: no other board in this repo has an RGB LED, a
 * potentiometer and three buttons. A seam with one possible implementation
 * removes no coupling; it only adds indirection.
 *
 * So this is a SPLIT, not a promotion. What it buys is that boards/dm330030/main.c
 * is once again only the five profile hooks, the way boards/ev88g73a/main.c is --
 * previously 380 of its 412 lines were this demo, and the demo's 1 ms tick
 * registrations were sitting in the board's bring-up phase.
 *
 * The one piece that DID pass the test moved: the button blanking countdown is
 * now app/button_debounce.{c,h}, used twice from here.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register this demo's two 1 ms tick clients (the RGB soft-PWM modulator and the
 * button sampler). Call after TIMER_SetConfiguration(TIMER_CONFIGURATION_1MS) and
 * before profile_report(), i.e. from profile_bring_up() -- that ordering is the
 * original's and it matters: the RGB LED is dark until its modulator ticks.
 */
void demo_rgb_pot_buttons_init(void);

/* The key legend, printed once as ordinary scrolling text. */
void demo_rgb_pot_buttons_print_header(void);

/*
 * One iteration: sample the pot, apply it to the selected channel, handle S3, and
 * -- when `report` -- emit ONE status line.
 *
 * THE ANSI FIXED SCREEN IS GONE (2026-08-05), and with it the "unthrottled by
 * design" that used to be documented here. It rewrote rows 8..15 by cursor
 * addressing on every iteration, which cost this board an interactive console: a
 * command echo and its reply land in the middle of a region that is repainted
 * ~60 times a second, and the repaint saturated the transmitter the console needs.
 *
 * So this board's console now scrolls, exactly like EV88G73A's, and the pot/button
 * state is one throttled line instead of a live region. The ACTUATION is unchanged
 * and still runs on every call -- the pot still drives the RGB channel at the loop
 * rate and S1/S2/S3 still work at their own cadences -- because only the PRINTING
 * is behind `report`. See boards/dm330030/main.c for the two periods, and
 * the parity contract (the ANSI layout is explicitly outside it, so this was a
 * free choice, not a required one).
 */
void demo_rgb_pot_buttons_poll(bool report);

#ifdef __cplusplus
}
#endif

#endif /* DEMO_RGB_POT_BUTTONS_H */
