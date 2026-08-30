#ifndef CONSOLE_TASK_H
#define CONSOLE_TASK_H

/*
 * console_task.h -- the console as a main-loop task: bring it up, feed it, print
 * its help.
 *
 * WAS boards/ev88g73a/ev88g73a_console.{c,h}, WHICH IS NOW GONE
 * ------------------------------------------------------------
 * That file started as the whole console and ended as three things wearing a board
 * name: a poll loop, a help string, and the handlers for two module letters. None of
 * them needed the board once uart_platform/console_out.h existed -- a loop that asks "is a
 * character ready, give me the character" has no board knowledge left, and the
 * commands' board knowledge turned out to be four values, now the BOARD SEAM in
 * app/app_traps.h.
 *
 * So the parts moved where they belong: the loop and the help here, module 's' to
 * uart_app/system_console.c, module 'x' to uart_app/traps_console.c. DM330030 gets a console
 * the day it implements console_in_ready()/console_in_read() for real -- which is a
 * board fact -- and not a line of interpreter porting.
 *
 * NOT INTERRUPT-DRIVEN: console_task_poll() drains whatever the UART has and
 * returns. It must be called often enough that the hardware FIFO does not overrun a
 * typed line; EV88G73A calls it from inside its Timer1 wait rather than once per
 * main-loop iteration, for exactly that reason.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the line parser. The console's UART is brought up by board init, since
 * that is where the pins and the baud divisor are. */
void console_task_init(void);

/* Drain the hardware into the parser. Non-blocking; call from the main loop. */
void console_task_poll(void);

/*
 * The one help text, printed once at boot and again on demand as ?gh.
 *
 * One function rather than a boot banner plus a ?gh string. They were two texts
 * before, in two files, and had already drifted: the boot help listed commands the
 * ?gh grammar did not mention. A help text that disagrees with itself is worse than
 * a terse one.
 */
void console_task_print_help(void);

/*
 * Block until everything queued has physically left the transmitter, with a bounded
 * guard so a wedged UART cannot hang the caller.
 *
 * Exists for the two commands that deliberately stop the machine -- the software
 * reset and the forced traps. Their acknowledgement is still in the TX FIFO when the
 * part resets, and a reset discards it, so without this the user sees the command
 * take effect with no output at all: indistinguishable from a command that was
 * silently ignored. On a board with no reset button that difference is the only
 * evidence the command worked.
 *
 * Returns true if the transmitter actually went idle, false if the guard expired.
 */
bool console_task_wait_tx_drain(void);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_TASK_H */
