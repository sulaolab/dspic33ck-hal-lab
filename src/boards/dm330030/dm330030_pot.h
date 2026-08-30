#ifndef DM330030_POT_H
#define DM330030_POT_H

/*
 * dm330030_pot.{c,h} -- DM330030's on-board potentiometer, on AN23 / RE3.
 *
 * WHY THIS IS NOT PART OF dm330030_io
 * -----------------------------------
 * It was, briefly, and that was wrong: dm330030_io says "runtime accessors, owns no init",
 * and this module owns an ADC handle, an init, a cached last value and a fault string.
 * Keeping it there forced the header to carry an "except for the pot" clause, and a rule
 * with an exception in it is a rule the next reader will not apply (raised in review,
 * 2026-08-02).
 *
 * So the board's rule is stated once and holds without exceptions:
 *
 *   dm330030_io.*         owns NOTHING -- level reads and writes only (LEDs, switches)
 *   dm330030_pot.*        owns a PERIPHERAL: an ADC handle and its init
 *   dm330030_led3_rgb.*   owns a STATE MACHINE: soft-PWM advanced by the 1 ms tick
 *
 * i.e. one file per thing that has state of its own, and one shared file for the things
 * that have none. That is the same line EV88G73A's ev88g73a_io.{c,h} sits on -- it has
 * only an LED and a button, so it needs no sibling.
 *
 * WAS pot.{c,h}, AND BEFORE THAT adc.{c,h}, WHICH OVERSTATED WHAT IT IS
 * --------------------------------------------------------------------
 * It is not an ADC layer: hal_adc/nora_adc_dspic33ck.c is, and this has always been a thin
 * adapter on top of it (which is why "put this on the ADC HAL" turned out to be nothing to
 * do -- it already was). What it actually knows is one device: which AN number the pot sits
 * on and how long to sample it. So it is named after the device.
 *
 * DIVISION OF RESPONSIBILITY
 * --------------------------
 * Deliberately NO pin configuration here. dm330030_board.c's user-I/O stage makes RE3 an
 * analog input and is its only configurer, exactly as it is for the LEDs and the switches.
 * This file owns the ADC channel and nothing else.
 *
 * That split also removed an ordering hazard that used to be invisible: back when the
 * board had a boot-time stage clearing ANSEL across every port, whatever made the pot pin
 * analog had to run after it -- and while this file configured the pin, that rule spanned
 * two files that merely happened to be called in the right sequence. The sweep itself was
 * deleted on 2026-08-03, so the hazard is gone twice over; what remains, and is the
 * durable half, is that RE3 has exactly one configurer.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the shared-core ADC for the pot's channel. False means the peripheral did not
 * come ready; dm330030_pot_fault() then says why. Called from profile_bring_up().
 *
 * Nothing here prints: this board's console is a fixed screen region that the demo
 * repaints, so a line emitted during bring-up is erased by the header that follows.
 * Report the fault where the value is displayed instead.
 */
bool dm330030_pot_init(void);

/*
 * One blocking conversion, as a RAW 12-bit value (0..4095).
 *
 * Raw on purpose. It used to return the value left-shifted into 16 bits for the RGB LED's
 * PWM, and the caller that wanted a number to display shifted it straight back down --
 * i.e. the board's ADC adapter was carrying the LED driver's bit depth. Scaling belongs
 * where the PWM width is known, which is demo_rgb_pot_buttons.c.
 *
 * On a failed conversion the last good value is returned rather than a zero, so a momentary
 * timeout does not make the display jump to the bottom of the range. The cost is that a
 * persistent fault looks like a still pot, which is what dm330030_pot_fault() is for.
 */
uint16_t dm330030_pot_read(void);

/*
 * NULL while the last init and conversion were both fine; otherwise the HAL's reason as
 * text ("TIMEOUT", "NOT_INITIALIZED", ...).
 *
 * Exists because the alternative was silence: the init result used to be discarded by the
 * caller, and a failed read simply repeated the previous value, so an ADC that never came
 * up was indistinguishable from a potentiometer nobody had turned. Same failure class as
 * the pin-init function that had no caller at all (see dm330030_board.c).
 */
const char *dm330030_pot_fault(void);

#ifdef __cplusplus
}
#endif

#endif /* DM330030_POT_H */
