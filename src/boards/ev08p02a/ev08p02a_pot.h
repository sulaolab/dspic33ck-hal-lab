#ifndef EV08P02A_POT_H
#define EV08P02A_POT_H

/*
 * ev08p02a_pot.{c,h} -- a mikroBUS POT Click read through the shared-core ADC.
 *
 * THE DEVICE THIS NAMES
 * ---------------------
 * A MIKROE-3402 POT Click in a socket of the Curiosity Nano Base for Click boards
 * (DS50002839B), which this Nano plugs into. The Click's AN pin is the only signal
 * used; RST/CS/SCK/MISO/MOSI/INT/PWM are left alone.
 *
 * WHICH SOCKET, AND WHY THAT DECIDES THE CHANNEL
 * ----------------------------------------------
 * The base board wires each socket's AN pin to a different Nano edge pin, and the
 * dsPIC33CK256MC005 Curiosity Nano user guide (DS70005656A) prints the resulting map
 * for THIS part in section 9.4, Figure 9-6:
 *
 *     socket 1   AN -> RA2  = AN9
 *     socket 2   AN -> RA3  = AN3      <- EV08P02A_POT_SOCKET default
 *     socket 3   AN -> RA4  = AN4
 *
 * Cross-checked against the silicon rather than trusted from one drawing: the device
 * description file DSPIC33CK256MC005.PIC lists RA3's functions as
 * DACOUT1/AN3/CMP1C/IOCA3, RA2's as AN9/IOCA2 and RA4's as OA3OUT/AN4/IBIAS3/IOCA4.
 * All three are plain analog inputs on the shared core; none is shared with anything
 * this board configures.
 *
 * So the socket is the ONE thing a user changes, and the AN number follows from it.
 * Set EV08P02A_POT_SOCKET to 1, 2 or 3; anything else is an #error rather than a
 * silently wrong channel, because a pot that reads a floating pin looks exactly like
 * a pot that is not turning.
 *
 * WHY THIS IS A FILE OF ITS OWN, and not part of ev08p02a_io
 * ---------------------------------------------------------
 * The same line DM330030 draws (dm330030_pot.h states it at length): ev08p02a_io owns
 * nothing and only reads and writes levels, while this owns a peripheral -- an ADC
 * handle, an init, a cached value and a fault string. Ported from dm330030_pot.{c,h},
 * which reads that board's soldered-on pot on AN23 through the same HAL.
 *
 * DIVISION OF RESPONSIBILITY
 * --------------------------
 * NO pin configuration here. ev08p02a_board.c's user-I/O table makes the pot's pin an
 * analog input and is its only configurer, exactly as it is for LED0 and SW0. This
 * file owns the ADC channel and the console module, and nothing else.
 *
 * WHAT IT DOES NOT DO
 * -------------------
 * No averaging, no filtering, no calibration, no DMA. One blocking conversion per
 * read, as the converter produces it. The bring-up question is whether turning the
 * knob moves the number.
 */

#include <stdint.h>
#include <stdbool.h>

#include "app_console.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * EV08P02A_POT_SOCKET (1, 2 or 3) selects the socket, and with it the pin AND the AN
 * channel. It is defined in ev08p02a_pins.h beside both halves of that map -- see the
 * note there for why they are not in two files.
 */

/*
 * 12-bit, single-ended against AVDD (ev08p02a_pot.c's REFSEL choice). A second consumer
 * (ev08p02a_avas_pot.c, turning counts into a pitch) needs the same domain, so it is a
 * fact about the peripheral rather than something each caller retypes as its own 4095
 * that could silently stop matching this one.
 */
#define EV08P02A_ADC_FULL_SCALE (4095u)

/*
 * Bring up the shared-core ADC for the pot's channel. False means the peripheral never
 * came ready, and ev08p02a_pot_fault() then says why in the HAL's own words.
 *
 * Unlike DM330030's, this one may print through the console: this board's console is a
 * scrolling log, not a repainted screen region, so a bring-up line survives.
 */
bool ev08p02a_pot_init(void);

/* One blocking 12-bit conversion of the pot channel. On failure the previous value is
 * returned and ev08p02a_pot_fault() is set -- a read never blocks the caller forever
 * and never invents a number. */
uint16_t ev08p02a_pot_read(void);

/*
 * The internal 1.2 V band gap (AN21) -- the measurement that proves the converter
 * converts with no pin, no Click board and no wiring involved.
 *
 * Returns false and leaves *counts untouched when no conversion happened, rather than
 * handing back a number from somewhere else: it needs its own ADC handle because the CK
 * HAL binds one handle to one channel, and an earlier version that shared the pot's
 * handle printed the POT reading under the heading "bandgap" (measured on silicon --
 * the HAL answered UNSUPPORTED and the number shown was the knob).
 */
bool ev08p02a_pot_read_bandgap(uint16_t *counts);

/* NULL when the last operation succeeded, otherwise the HAL result as text. */
const char *ev08p02a_pot_fault(void);

/* One line naming the socket, the pin, the AN number and the current reading. Used by
 * the boot report and by ?ai. */
void ev08p02a_pot_report(void);

/*
 * Console module 'a' -- ADC. Routed from console_board_onmsg() in
 * ev08p02a_console_out.c, because what it reads is a board's socket.
 *
 *   ?ai   what is configured: socket, pin, AN number, and the fault if any
 *   ?av   one reading: raw counts and millivolts
 *   *av1 / *av0   start / stop the periodic reading line
 *   *ab   the internal 1.2 V band gap -- PASS/FAIL against a wide sanity band
 *
 * The single-key AVAS hotkeys 'a' and 'A' are NOT in the way: a hotkey is only taken
 * when no command line is being built, and '?' or '*' has already started one by the
 * time the 'a' arrives (app_console_is_idle(), app_console.h).
 */
void ev08p02a_pot_console_onmsg(app_console_msg_t *msg);

/* Emits the periodic line when *av1 is on. Called from profile_poll(); `report` is the
 * shared status-line throttle and is ignored -- this module has a period of its own so
 * that a knob being turned looks continuous. */
void ev08p02a_pot_poll(bool report);

#ifdef __cplusplus
}
#endif

#endif /* EV08P02A_POT_H */
