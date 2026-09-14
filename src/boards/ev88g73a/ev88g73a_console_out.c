/*
 * ev88g73a_console_out.c -- EV88G73A's implementation of uart_platform/console_out.h,
 * and this board's ONLY console transport.
 *
 * It used to be five delegations onto a parallel board API (ev88g73a_uart_write_*,
 * _rx_ready, _read_byte, _tx_done). That API is gone and its bodies are here. The
 * measurement that ended it: four of its seven functions had exactly one caller --
 * this file -- while the other three were called 45 times from this board's own
 * main.c and audio module, which is to say the console's output had two names for one
 * UART and the shared modules' seam was being bypassed by the board's own code.
 *
 * What was board-specific about any of it was WHICH UART: EV88G73A_CONSOLE_UART_INST
 * in ev88g73a_board.h, because the Nano's debugger CDC is wired to UART1's RC10/RC11.
 * Decimal digit generation and fixed-width hex never were. Bring-up (pins, PPS,
 * clock-derived baud) stays in ev88g73a_board.c -- this file only moves bytes.
 *
 * Note what is NOT here: printf. This is a 64 KB part and its own code avoids stdio.
 * See uart_platform/console_out.h.
 */

#ifndef DSPIC33CK_BOARD_EV88G73A
#error "boards/ev88g73a/ev88g73a_console_out.c is EV88G73A-owned. Build it only in the CK64MC105_EV88G73A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "console_out.h"

#include <stddef.h>

#include "app_config.h"       /* DEMO_* switches, resolved and checked */
#include "app_console.h"
#include "ev88g73a_board.h"   /* EV88G73A_CONSOLE_UART_INST */
#include "nora_uart.h"

/*
 * This board's ONE output byte, and the one place a line ending is made.
 *
 * Sources carry a bare '\n' and the CR is added here, so the wire still carries CR LF
 * -- the whole argument is in uart_platform/console_out.h. Everything below routes
 * through this, including the digit and hex-nibble writers that can never emit a '\n':
 * one call site for the transport is smaller than four, and it leaves nowhere for a
 * second, untranslated path to appear later.
 */
static void console_put(char c)
{
    if (c == '\n') {
        (void)nora_uart_write_byte(EV88G73A_CONSOLE_UART_INST, (uint8_t)'\r');
    }

    (void)nora_uart_write_byte(EV88G73A_CONSOLE_UART_INST, (uint8_t)c);
}

void console_out_char(char c)
{
    console_put(c);
}

void console_out_str(const char *s)
{
    if (s == NULL) {
        return;
    }

    while (*s != '\0') {
        console_put(*s);
        s++;
    }
}

void console_out_u32(uint32_t value)
{
    char digits[10];
    uint8_t count = 0u;

    if (value == 0u) {
        console_put('0');
        return;
    }

    /* Generated least-significant first, so it is emitted in reverse below. The
     * bound on `count` is the array, not a digit count: a 32-bit value cannot exceed
     * ten decimal digits, and if that ever changed the loop would truncate rather
     * than write past the buffer. */
    while ((value != 0u) && (count < (uint8_t)sizeof(digits))) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (count-- != 0u) {
        console_put(digits[count]);
    }
}

void console_out_hex16(uint16_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    int8_t nibble;

    for (nibble = 3; nibble >= 0; nibble--) {
        console_put(hex[(value >> (nibble * 4)) & 0xFu]);
    }
}

bool console_out_idle(void)
{
    return nora_uart_tx_done(EV88G73A_CONSOLE_UART_INST);
}

bool console_in_ready(void)
{
    return nora_uart_rx_ready(EV88G73A_CONSOLE_UART_INST);
}

bool console_in_read(uint8_t *out)
{
    if (out == NULL) {
        return false;
    }

    return nora_uart_read_byte(EV88G73A_CONSOLE_UART_INST, out) ==
           NORA_UART_OK;
}

uint32_t console_in_overrun_recovered(void)
{
    nora_uart_rx_status_t status;

    if (nora_uart_rx_status_get(EV88G73A_CONSOLE_UART_INST, &status) !=
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

    /* No partial fill on failure: the caller is told "cannot answer" and prints
     * that, because an all-zero tuple is a legitimate healthy reading too. */
    if (nora_uart_rx_status_get(EV88G73A_CONSOLE_UART_INST, &status) !=
        NORA_UART_OK) {
        return false;
    }
    if (nora_uart_rx_regs_get(EV88G73A_CONSOLE_UART_INST, &regs) !=
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
 * Board hook for uart_app/console_dispatch.c: a module letter that needs board knowledge.
 *
 * 's' (reset, reset cause) and 'x' (traps) were both here, in ev88g73a_console.c, and
 * both moved to app/ once the four values they actually needed became the BOARD SEAM in app/app_traps.h.
 * For a long time this board owned NO letter as a result, and answering "not found" was
 * the accurate answer.
 *
 * It now owns TWO when WM8904 audio is enabled: 't' (audio transport path and load
 * controls) and the narrow Classic compatibility form `*cy00` (Type_TY AVAS toggle).
 * This is the hook working as intended -- the command attaches to a board file instead
 * of teaching the dispatcher about a board, and the handlers live next to the config
 * they control (boards/ev88g73a/main.c) rather than here.
 *
 * Declared at the point of use, which is console_dispatch.c's stated convention for a
 * module handler with exactly one caller.
 */
#if DEMO_ENABLE_WM8904_AUDIO
void ev88g73a_transport_console_onmsg(app_console_msg_t *msg);   /* main.c */
void ev88g73a_classic_console_onmsg(app_console_msg_t *msg);     /* main.c */
bool ev88g73a_transport_console_hotkey(uint8_t ch);              /* main.c */
#endif

void console_board_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

#if DEMO_ENABLE_WM8904_AUDIO
    if (msg->module == 't') {
        ev88g73a_transport_console_onmsg(msg);
        return;
    }

    if (msg->module == 'c') {
        ev88g73a_classic_console_onmsg(msg);
        return;
    }
#endif

    msg->status = APP_CONSOLE_ERR_NOT_FOUND;
}

/*
 * Board hook for uart_app/console_task.c: single-key hotkeys, routed the same way and
 * for the same reason as the 't' module above. The keys live next to the commands they
 * duplicate (boards/ev88g73a/main.c) so that a key and its command cannot drift apart;
 * this file still knows about the console and nothing about TDM.
 */
bool console_board_hotkey(uint8_t ch)
{
#if DEMO_ENABLE_WM8904_AUDIO
    return ev88g73a_transport_console_hotkey(ch);
#else
    (void)ch;

    return false;
#endif
}
