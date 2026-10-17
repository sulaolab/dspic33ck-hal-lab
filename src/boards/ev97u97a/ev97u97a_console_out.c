/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef DSPIC33CK_BOARD_EV97U97A
#error "boards/ev97u97a/ev97u97a_console_out.c is EV97U97A-owned. Build it only in the CK1024MP710_EV97U97A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "console_out.h"

#include <stdio.h>

#include "app_console.h"
#include "demo_rgb_pot_buttons.h"

#include "nora_uart.h"
#include "uart_platform_stdio.h"

void console_out_char(char c)
{
    (void)putchar((int)(unsigned char)c);
}

void console_out_str(const char *s)
{
    if (s == NULL) {
        return;
    }

    (void)fputs(s, stdout);
}

void console_out_u32(uint32_t value)
{

    (void)printf("%lu", (unsigned long)value);
}

void console_out_hex16(uint16_t value)
{
    (void)printf("%04X", (unsigned int)value);
}

bool console_out_idle(void)
{

    return nora_uart_tx_done(UART_PLATFORM_CONSOLE_UART_INST);
}

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

#if DEMO_ENABLE_WM8904_AUDIO
#include "wm8904_audio.h"
void ev97u97a_transport_console_onmsg(app_console_msg_t *msg);
void ev97u97a_classic_console_onmsg(app_console_msg_t *msg);
#endif
void console_board_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

#if DEMO_ENABLE_WM8904_AUDIO
    if (msg->module == (uint8_t)'t') { ev97u97a_transport_console_onmsg(msg); return; }
    if (msg->module == (uint8_t)'c') { ev97u97a_classic_console_onmsg(msg); return; }
#endif
    if ((msg->module != (uint8_t)'l') || (msg->name != (uint8_t)'r')) {
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        return;
    }

    if (msg->kind == (uint8_t)'?') {
        if (msg->data_len != 0u) {
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }
    } else if (msg->kind == (uint8_t)'*') {
        if (msg->data_len == 0u) {
            demo_rgb_pot_buttons_use_rgb_pot();
        } else if (msg->data_len == 3u) {
            if ((msg->data[0] > 100u) || (msg->data[1] > 100u) ||
                (msg->data[2] > 100u)) {
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                return;
            }
            demo_rgb_pot_buttons_set_rgb_manual_percent(msg->data[0], msg->data[1],
                                                         msg->data[2]);
        } else {
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }
    } else {
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    msg->data[0] = demo_rgb_pot_buttons_rgb_is_manual() ? 1u : 0u;
    demo_rgb_pot_buttons_get_rgb_percent(&msg->data[1], &msg->data[2], &msg->data[3]);
    msg->data_len = 4u;
    msg->status = APP_CONSOLE_OK;
}

#if DEMO_ENABLE_WM8904_AUDIO
bool ev97u97a_transport_console_hotkey(uint8_t ch);
#endif

bool console_board_hotkey(uint8_t ch)
{
#if DEMO_ENABLE_WM8904_AUDIO
    return ev97u97a_transport_console_hotkey(ch);
#else
    (void)ch;

    return false;
#endif
}
