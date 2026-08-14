#ifndef UART_PLATFORM_STDIO_H
#define UART_PLATFORM_STDIO_H

/*
 * uart_platform_stdio.h
 * ---------------------
 * The board console's stdio side: brings up the console UART and retargets
 * printf() to it.
 *
 * Was app/console_stdio.{c,h}. Renamed with the move to uart_platform/ because
 * dspic33ak-audio-dsp-sonora has exactly this file under exactly this name
 * (src/uart_platform/uart_platform_stdio.c) doing exactly this job -- the libc
 * write() hook onto one fixed console UART instance through the UART HAL. When the
 * fleet already has a name for a thing, having a second one is just a translation
 * step for whoever reads both.
 *
 * It replaces the last surviving piece of mcc_generated_files/uart1.*. That module
 * was already a thin shim over hal_uart with only one of its eleven functions still
 * called; what actually kept it alive was the write() hook that stdio needs. See the
 * .c file for why this belongs in neither the HAL nor the board layer.
 *
 * BOTH BOARDS BRING THE CONSOLE UART UP THROUGH THIS FILE as of 2026-08-04. What still
 * differs is only how they WRITE: DM330030's console is printf (so the write() hook
 * below is load-bearing there), EV88G73A is a 64 KB part and writes through
 * uart_platform/console_out.h on top of hal_uart. That is a difference in the transport,
 * not in the bring-up -- and the bring-up is what used to be duplicated:
 * ev88g73a_board.c built its own nora_uart_config_t with all eleven fields set to
 * the same values as the one below.
 */

#include "nora_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WHICH UART is the console. One instance, because there is one write() hook and it
 * has nowhere to look a per-call instance up.
 *
 * Exposed rather than kept private so that a board which ALSO names the console
 * instance can assert the two agree instead of drifting: EV88G73A must name it, because
 * its console_out transport addresses the UART directly, and ev88g73a_board.c holds a
 * _Static_assert tying EV88G73A_CONSOLE_UART_INST to this. DM330030 names no instance
 * at all -- everything it prints goes through printf, hence through the hook below.
 */
#define UART_PLATFORM_CONSOLE_UART_INST   (NORA_UART_INST_1)

/*
 * Configure the console UART (8N1, TX+RX, clocked from the Clock HAL's recorded Fcy) and
 * make printf() output go to it. Call after the clock is up and after the console pins
 * have been routed. This is the PERIPHERAL stage of the console on both boards; the pin
 * stage is <board>_uart1_pins_init() and its result is recorded separately, because a
 * mis-routed PPS leaves this one reporting OK while producing no output.
 *
 * Called unconditionally, even when the pin stage failed -- deliberately, and it is
 * EV88G73A that learned why: a UART whose pins did not route is still worth having
 * configured, because a debugger can then read BRG and the enables and tell "wrong rate"
 * from "wrong pin". Skipping it would destroy that distinction.
 *
 * The RATE is the board's argument, not a constant in here. A board whose PLL did not
 * lock must fall back to a rate its fallback clock can actually resolve -- at 4 MHz Fcy
 * the 230400 divisor rounds to 250000 (+8.5%) and every frame is corrupt -- and that
 * decision needs the clock's outcome, which the board has and this file does not.
 */
void uart_platform_stdio_init(uint32_t baudrate);

/*
 * Result of the peripheral-init stage above. Readable from a debugger for the
 * case that matters: the console itself failing to come up, when no printf can
 * report it.
 *
 * Deliberately NOT renamed with the file. It is referenced by name in
 * boards/dm330030/dm330030_board.c's comment and in the bring-up docs as the console's
 * init evidence, and a symbol whose name appears in a written diagnostic procedure
 * is worth more than one that matches its file.
 */
extern volatile nora_uart_status_t g_console_init_status;

#ifdef __cplusplus
}
#endif

#endif /* UART_PLATFORM_STDIO_H */
