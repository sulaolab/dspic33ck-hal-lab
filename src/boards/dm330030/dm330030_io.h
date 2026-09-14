#ifndef DM330030_IO_H
#define DM330030_IO_H

/*
 * dm330030_io.{c,h} -- RUNTIME I/O on DM330030: the two discrete LEDs and the three
 * push-buttons. Read and written every loop iteration, and OWNING NOTHING -- no handle, no
 * init, no cached state, not one direction register.
 *
 * Was led_sw.{c,h}. THE POT IS NOT HERE, and the reason is the rule: this board's runtime
 * devices divide by what they own, which is a line a later reader can re-derive.
 *
 *   dm330030_io.*         owns NOTHING -- level reads and writes only   <- this file
 *   dm330030_pot.*        owns a PERIPHERAL: an ADC handle and its init
 *   dm330030_led3_rgb.*   owns a STATE MACHINE: soft-PWM advanced by the 1 ms tick
 *
 * The pot did live here for one commit, which forced this header to carry an "except for
 * the pot, which does have an init" clause -- and a rule with an exception in it is a rule
 * the next reader will not apply (raised in review, 2026-08-02). One file per thing with
 * state of its own; one shared file for the things with none.
 *
 * The three ROLES are unchanged by that, and are the same on both boards:
 *
 *   dm330030_pins.h     facts      compile time only, no code
 *   dm330030_board.*    bring-up   once, in order
 *   dm330030_io.*       runtime    every iteration                      <- this file
 *
 * WHAT THIS REPLACED, AND THE DEFECT IT FIXES
 * -------------------------------------------
 * Five Microchip-2016 vendor headers (led1.h, led2.h, button_s1..s3.h), each with inline
 * functions writing LATx/TRISx/PORTx bits directly. They were not merely old-fashioned:
 * they were a SECOND OWNER of pins that the board file already owns through the GPIO HAL.
 *
 *   dm330030_pins.h     DM330030_LED1_PIN = port E pin 6
 *   dm330030_board.c    nora_gpio_config_digital_output(DM330030_LED1_PIN, false)
 *   led1.h              LATEbits.LATE6 / TRISEbits.TRISE6      <- same pin, no HAL
 *
 * Same for LED2 (E5) and SW1/SW2/SW3 (E7/E8/E9) -- dm330030_pins.h already had every one
 * of them, so the vendor headers were pure duplication of pin facts. The concrete bug:
 * LED1_On() rewrote TRIS on EVERY call, as did BUTTON_S1_IsPressed(), so direction was
 * re-asserted from a second place behind the HAL's back.
 *
 * DIVISION OF RESPONSIBILITY HERE
 * -------------------------------
 * Deliberately NO dm330030_io_init(). dm330030_board_init() configures direction,
 * pull-ups and the pot's analog bit, and remains their ONLY configurer; an init here would
 * recreate the two-owners problem this file exists to remove.
 *
 * (The AK fleet's board_components/led_sw.c does own its init, because there is no
 * separate board file doing it there. Same job, one less owner.)
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Indices, not pins: the pin numbers live in dm330030_pins.h and nowhere else. */
typedef enum {
    DM330030_LED_1 = 0,
    DM330030_LED_2,
    DM330030_LED_COUNT
} dm330030_led_t;

typedef enum {
    DM330030_SW_1 = 0,
    DM330030_SW_2,
    DM330030_SW_3,
    DM330030_SW_COUNT
} dm330030_sw_t;

/* Active high: these LEDs are driven from the pin, not sunk to it. Out-of-range
 * indices are ignored rather than asserted -- this is board glue on a lab board,
 * and a silent no-op beats a hang in a blink loop. */
void dm330030_led_set(dm330030_led_t led, bool on);
void dm330030_led_toggle(dm330030_led_t led);
bool dm330030_led_get(dm330030_led_t led);

/*
 * Active low (pressed = pin low), matching the pull-ups dm330030_board.c enables.
 *
 * NOT debounced. The callers sample this once per 1 ms tick and do their own
 * edge/hold logic, which is where the timing knowledge lives; a debounce here
 * would either duplicate or fight that (see app/button_debounce.h). Out-of-range
 * returns false.
 */
bool dm330030_sw_pressed(dm330030_sw_t sw);

#ifdef __cplusplus
}
#endif

#endif /* DM330030_IO_H */
