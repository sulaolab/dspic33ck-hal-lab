/*
 * dm330030_console_out.c -- DM330030's implementation of uart_platform/console_out.h.
 *
 * This board already prints with printf(), retargeted onto the console UART by
 * uart_platform/uart_platform_stdio.c's write() hook, and main.c uses it in 22 places. So the
 * cheapest correct implementation is to delegate, rather than introduce a second
 * output path alongside the one the board already has.
 *
 * That is the opposite choice from EV88G73A, which avoids stdio because it is a
 * 64 KB part -- and it is exactly why this is a per-board file rather than one
 * shared implementation. See uart_platform/console_out.h.
 *
 * IN USE SINCE 2026-08-05: this board runs uart_app/console_task.* now
 * (DEMO_ENABLE_CONSOLE_COMMANDS=1 in board_profile.h), so all seven functions below have
 * a caller -- output from the command handlers, input from console_task_poll(), and
 * console_out_idle() from the software-reset acknowledgement. What it still LACKS is trap
 * reporting: this configuration is APP_TRAPS_POLICY=2 (spin), so a trap halts silently
 * and the forced-trap commands (*xa, *xm, *xs) stop the board instead of reporting. That
 * is the next
 * parity item, not a property of this file. See docs/ck_hardware_notes.md.
 *
 * Its hardware verification is deferred, so nothing here has run on the board.
 */

#ifndef DSPIC33CK_BOARD_DM330030
#error "boards/dm330030/dm330030_console_out.c is DM330030-owned. Build it only in the CK256MP508_DM330030 configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "console_out.h"

#include <stdio.h>

#include "app_console.h"

/* For nora_uart_tx_done() in console_out_idle() and UART_PLATFORM_CONSOLE_UART_INST,
 * the instance stdio is already retargeted to -- see uart_platform_stdio.h. */
#include "nora_uart.h"
#include "uart_platform_stdio.h"

/*
 * LINE ENDINGS: not here, and deliberately not here. All four output functions below
 * are stdio, so they all land in uart_platform_stdio.c's write() hook, which is where
 * this board's '\n' -> CR LF translation lives -- one place, and the same place that
 * already serves the 22 direct printf() calls in this board's main.c. Adding a
 * translation here as well would double the CR. See uart_platform/console_out.h.
 */

void console_out_char(char c)
{
    (void)putchar((int)(unsigned char)c);
}

void console_out_str(const char *s)
{
    if (s == NULL) {
        return;
    }

    /* fputs rather than printf: the argument is data, not a format string, and
     * passing data as a format is how a '%' in a device name becomes a crash. */
    (void)fputs(s, stdout);
}

void console_out_u32(uint32_t value)
{
    /* %lu, not %u: uint32_t is unsigned long on this 16-bit target. */
    (void)printf("%lu", (unsigned long)value);
}

void console_out_hex16(uint16_t value)
{
    (void)printf("%04X", (unsigned int)value);
}

bool console_out_idle(void)
{
    /*
     * WHAT THE NAME PROMISES, and it is the only thing it may mean: software buffer,
     * TX FIFO and shift register all empty, last stop bit gone. The callers are the
     * software-reset acknowledgement and the trap report, and both are about to destroy
     * the machine that would finish the transmission -- "approximately empty" there loses
     * exactly the line that says why the board reset.
     *
     * UNTIL 2026-08-05 THIS RETURNED TRUE UNCONDITIONALLY, argued as a safe limitation:
     * nothing on this board called it, and reporting idle loses at most a line whereas
     * returning false forever hangs the caller. Both halves of that were fine and the
     * conclusion still made the function a promise it did not keep -- and it was about to
     * acquire a caller, since this board's console RX pin is now routed.
     *
     * printf stays as the FORMATTER (uart_platform_stdio.c's write() hook is already on
     * hal_uart, so there is no second transport here) while the WAIT goes to the HAL,
     * which is the one thing stdio cannot answer. Same question, same answer, same
     * instance as ev88g73a_console_out.c's.
     */
    return nora_uart_tx_done(UART_PLATFORM_CONSOLE_UART_INST);
}

/*
 * INPUT, POLLED -- BOARD_CONSOLE_USE_RX_ISR is 0 in this board's board_profile.h and
 * app_config.h refuses a 1 while no configuration links the vectors.
 *
 * UNTIL 2026-08-05 BOTH OF THESE RETURNED false, with a comment that said this board's
 * "console UART RX is read by the HAL ISR ring rather than polled". That was not true of
 * any build: hal_uart/nora_uart_dspic33ck_isr.c is excluded from this configuration, so
 * nothing ever filled the ring -- the receiver was enabled (uart_platform_stdio_init()
 * sets enable_rx) and its characters were simply dropped on the floor. Returning "no
 * character" was accurate about the OUTCOME and wrong about the reason, which is the
 * shape of mistake that let this board look incapable of console input for months.
 *
 * Identical to ev88g73a_console_out.c's pair, on the same HAL calls, differing only in
 * the instance constant -- and that constant is UART_PLATFORM_CONSOLE_UART_INST here
 * rather than a board-named one because stdio is already retargeted to it, so a second
 * name for the same UART is exactly what console_out.h exists to prevent.
 */
bool console_in_ready(void)
{
    return nora_uart_rx_ready(UART_PLATFORM_CONSOLE_UART_INST);
}

bool console_in_read(uint8_t *out)
{
    if (out == NULL) {
        return false;
    }

    return nora_uart_read_byte(UART_PLATFORM_CONSOLE_UART_INST, out) ==
           NORA_UART_OK;
}

uint32_t console_in_overrun_recovered(void)
{
    nora_uart_rx_status_t status;

    if (nora_uart_rx_status_get(UART_PLATFORM_CONSOLE_UART_INST, &status) !=
        NORA_UART_OK) {
        return 0u;
    }

    return status.rx_overrun_recovered_count;
}

bool console_in_health(console_in_health_t *out)
{
    nora_uart_rx_status_t status;
    nora_uart_rx_regs_t regs;

    if (out == NULL) {
        return false;
    }

    if (nora_uart_rx_status_get(UART_PLATFORM_CONSOLE_UART_INST, &status) !=
        NORA_UART_OK) {
        return false;
    }
    if (nora_uart_rx_regs_get(UART_PLATFORM_CONSOLE_UART_INST, &regs) !=
        NORA_UART_OK) {
        return false;
    }

    out->bytes   = status.rx_byte_count;
    out->overrun = status.rx_overrun_recovered_count;
    out->framing = status.framing_error_count + status.parity_error_count;
    out->mode    = regs.mode;
    out->sta     = regs.sta;
    out->stah    = regs.stah;
    return true;
}

/*
 * Board hook for uart_app/console_dispatch.c. DM330030 has no command interpreter yet,
 * so it owns no module letters; reporting "not found" is the accurate answer rather
 * than a placeholder success. When this board gains commands they attach here.
 */
void console_board_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

    msg->status = APP_CONSOLE_ERR_NOT_FOUND;
}

/*
 * Board hook for uart_app/console_task.c: single-key hotkeys. This board owns none, so
 * every character stays with the line parser and behaves exactly as it did before the
 * hook existed -- which is the point of answering false rather than swallowing keys.
 */
bool console_board_hotkey(uint8_t ch)
{
    (void)ch;

    return false;
}
