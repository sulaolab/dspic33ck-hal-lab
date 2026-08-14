/*
 * console_task.c -- see console_task.h for what this is and what it replaced.
 */

#include "console_task.h"

#include <stdint.h>

#include "app_console.h"
#include "app_traps.h"
#include "console_out.h"

void console_task_init(void)
{
    app_console_init();
}

void console_task_print_help(void)
{
    console_out_str(
        " console: <kind><module><name>[hex payload]\n"
        "   kind    * = set, ? = query\n"
        "   modules g general, s system, d diagnostics, x exceptions\n"
        "   ?gv build ID       ?gh this help\n"
        "   *sr reset          ?sr reset cause\n"
        "   ?dt timer read test    ?du console RX overruns recovered\n");
#if APP_TRAP_TEST_CMDS
    console_out_str(
        "   ?xl last trap      *xa/*xm/*xs force address/math/stack trap\n");
#else
    console_out_str(
        "   ?xl last trap      (*xa/*xm/*xs trap tests not built in)\n");
#endif
}

bool console_task_wait_tx_drain(void)
{
    uint32_t guard;

    /*
     * A bounded spin on the transmitter's own done flag rather than a flat delay.
     * sonora spends a flat 150 ms in the equivalent place; waiting on the flag is
     * neither slower nor shorter than it needs to be, and the guard is what keeps a
     * wedged UART from turning "the reset command" into "the hang command".
     *
     * The count is worth roughly 40 ms at Fcy 100 MHz -- far longer than the ~0.5 ms
     * a line needs at 230400 baud, and deliberately not derived from the clock: this
     * is an upper bound on a failure path, not a timing service, and a board running
     * slower than it thinks is one of the failures it has to survive.
     *
     * Deliberately a busy-wait rather than borrowing a timer: the caller is on its
     * way to a reset or a trap, and coupling that path to the main loop's timer for
     * the sake of a few milliseconds would be the wrong trade.
     */
    for (guard = 0u; guard < 1000000u; guard++) {
        if (console_out_idle()) {
            return true;
        }
    }

    return false;
}

/*
 * Single-key hotkeys, owned by the board because the things worth binding to one
 * keystroke are board hardware. Returns true when the key was consumed.
 *
 * Declared at the point of use, which is console_dispatch.c's stated convention for a
 * board hook with exactly one caller -- and the same shape as console_board_onmsg():
 * the board that has no hotkeys answers false, exactly as it answers "not found".
 *
 * NOT sonora's three-valued result. There (uart_app/app_debug.c, apps/sonora_app_console.h)
 * a hotkey may report HANDLED_FLUSH, meaning the action blocked long enough -- a codec
 * mute settle, a synth blip -- that keystrokes queued behind it must be dropped. No CK
 * hotkey blocks, so the third state would be an unused mechanism, and the flush it asks
 * for is not free of consequence: it discards input nobody has looked at yet.
 */
bool console_board_hotkey(uint8_t ch);

void console_task_poll(void)
{
    uint8_t ch;

    /*
     * Drain the hardware into the parser. The parser owns the line: it echoes,
     * handles backspace, enforces the line limit and emits the response, so this
     * loop hands characters over and arbitrates exactly one thing -- whether a
     * character is a single-key hotkey or part of a command line.
     *
     * THE GATE IS app_console_is_idle(), WHICH IS WHAT IT WAS FOR: hotkeys are offered
     * only while the line buffer is empty. Otherwise a hotkey letter steals a character
     * out of the middle of a command -- 'a' appears twice in `*ta0001` -- and the
     * command silently becomes a different one. sonora carries the scar: a refactor
     * that merged its hotkeys and its console into one switch lost this distinction
     * and 'C' began eating hex nibbles. Its gate is a `static bool` armed by '*'/'?';
     * this parser already answers the same question about its own buffer, so asking it
     * is both shorter and correct for a line that never began with '*' or '?' at all.
     */
    while (console_in_ready()) {
        if (!console_in_read(&ch)) {
            return;
        }

        if (app_console_is_idle() && console_board_hotkey(ch)) {
            continue;
        }

        (void)app_console_feed_char(ch);
    }
}
