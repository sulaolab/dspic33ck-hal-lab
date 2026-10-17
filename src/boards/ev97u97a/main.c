/* EV97U97A audio port, adapted from the DM330030 board implementation. */
/*******************************************************************************
Copyright 2019 Microchip Technology Inc. (www.microchip.com)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*******************************************************************************/

#ifndef DSPIC33CK_BOARD_EV97U97A
#error "boards/ev97u97a/main.c is EV97U97A-owned. Build it only in the CK1024MP710_EV97U97A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include <stdbool.h>
#include <stdio.h>

#include "ev97u97a_board.h"
#include "profile_main.h"
#include "app_console.h"
#if defined(EV97U97A_HAVE_BUILD_ID_H)
#include "ev97u97a_build_id.h"
#endif

#include "timer_app.h"
#include "timer_1ms.h"

#include "nora_clock.h"

#include "ev97u97a_io.h"
#include "ev97u97a_pins.h"
#include "ev97u97a_pot.h"

#include "demo_rgb_pot_buttons.h"
#include "ev97u97a_led3_rgb.h"

#include "app_config.h"

#if DEMO_ENABLE_CONSOLE_COMMANDS
#include "console_task.h"
#endif
#include "console_out.h"

#if DEMO_ENABLE_I2C_PROBE
#include "i2c_probe.h"

static const i2c_probe_t s_i2c1_probe = {
    .inst        = NORA_I2C_INST_3,
    .addr7       = 0x1Au,
    .reg_pointer = 0x00u,
    .bus_init    = ev97u97a_i2c3_init,
    .where       = EV97U97A_I2C3_WHERE_STR,
};
#endif

#if DEMO_ENABLE_TDM_MASTER_LOOPBACK
#include "demo_tdm_master_loopback.h"

static const demo_tdm_master_loopback_port_t s_tdm_loopback_port = {
    .configure_pins = ev97u97a_tdm_pins_init,
    .wiring         = EV97U97A_TDM_LOOPBACK_WIRING_STR,
};
#endif

#if DEMO_ENABLE_WM8904_AUDIO
#include "wm8904_audio.h"

static bool ev97u97a_sw1_pressed(void)
{
    return ev97u97a_sw_pressed(EV97U97A_SW_1);
}

static const wm8904_audio_port_t s_wm8904_port = {
    .configure_pins      = ev97u97a_tdm_pins_init,
    .i2c_init            = ev97u97a_i2c3_init,

    .mute_button_pressed = ev97u97a_sw1_pressed,

    .wiring              = EV97U97A_AUDIO_WIRING_STR,
};

static const wm8904_audio_config_t s_wm8904_audio = {
    .port            = &s_wm8904_port,
    .i2c_inst_legacy = 3u,

    .dspic_is_master = false,
    .brg             = 0u,
    .sample_rate_hz  = 48000u,
    .gain_ramp_ms    = 800u,

    .status_period_ms      = 0u,

    .idle_report_period_ms = 40000u,
};
#endif

#define EV97U97A_POLL_PERIOD_MS (20u)

#define EV97U97A_STATUS_PRINT_PERIOD_MS (2000u)

static bool s_tick_start_ok;
static bool s_tick_clients_ok;

void profile_bring_up(void)
{
    ev97u97a_board_init();

    (void)ev97u97a_pot_init();

    s_tick_start_ok   = timer_app_start_from_frc(NORA_CLOCK_FRC_HZ);
    s_tick_clients_ok = TIMER_SetConfiguration(TIMER_CONFIGURATION_1MS);

    demo_rgb_pot_buttons_init();
}

void profile_report(void)
{
#if defined(EV97U97A_HAVE_BUILD_ID_H)
    printf("EV97U97A CK1024MP710 build=%s\n", EV97U97A_BUILD_ID);
#endif
    printf("EV97U97A clock: Fcy=%lu on_target=%u diag=%u UART=%lu pins=%u IO=%u\n",
           (unsigned long)nora_clock_get_fcy_hz(), (unsigned)g_ev97u97a_clock_on_target,
           (unsigned)g_ev97u97a_clock_diag, (unsigned long)g_ev97u97a_console_baud,
           (unsigned)g_ev97u97a_uart1_pins_init_ok, (unsigned)g_ev97u97a_user_io_pins_init_ok);
    demo_rgb_pot_buttons_print_header();
    ev97u97a_led3_rgb_print_hw_pwm_diagnostic();

    if (!s_tick_start_ok || !s_tick_clients_ok)
    {
        printf("1 ms tick FAILED (%s) -- RGB fade, buttons and the status line are dead "
               "(console still answers)\n",
               s_tick_start_ok ? "client registry" : "Timer1");
    }

#if DEMO_ENABLE_CONSOLE_COMMANDS
    console_task_init();
    console_task_print_help();
#endif
}

void profile_start(void)
{
#if DEMO_ENABLE_I2C_PROBE
    i2c_probe_run(&s_i2c1_probe);
#endif
#if DEMO_ENABLE_TDM_MASTER_LOOPBACK
    demo_tdm_master_loopback_start(&s_tdm_loopback_port);
#endif
#if DEMO_ENABLE_WM8904_AUDIO

#if WM8904_AUDIO_ENABLE_TDM_DIAG
    (void)wm8904_audio_lifecycle_probe_unconfigured();
#endif

    wm8904_audio_start(&s_wm8904_audio);
    console_out_str("      'a' / 'A' alone (no Enter) start the AVAS synth's"
                    " type_ty / type_lb voice, exclusive; while one is sounding EITHER"
                    " key stops it (fades out over ~3 s);"
                    " up/down arrow trim pitch +-5 cent;"
                    " Classic *cy00 is the Type_TY structured toggle\n");
#endif
}

#if DEMO_ENABLE_WM8904_AUDIO

void ev97u97a_transport_console_onmsg(app_console_msg_t *msg);

void ev97u97a_transport_console_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

    if (msg->name == 'd') {
        if (msg->kind == '?') {
            console_out_str(" ?td: declick restart strategy bitmask (one-shot, *td<NN>):\n"
                            "   DEFAULT(0x00) shutdown = vendor WSEQ; startup = manual\n"
                            "   bit0 0x01 ordered HP disable; bit1 0x02 warm servo\n"
                            "   bit2 0x04 soft unmute; bit3 0x08 WSEQ shutdown\n"
                            "   bit4 0x10 soft shutdown; bit5 0x20 WSEQ startup\n"
                            "   bit6 0x40 legacy quench; e.g. *td00 *td20 *td40\n");
            wm8904_audio_declick_print_status();
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_OK;
            return;
        }
        if (msg->kind != '*') {
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            return;
        }
        {

            const uint8_t mask = (msg->data_len >= 1u) ? msg->data[0] : 0x00u;
            console_out_str(" *td: declick one-shot restart mask=0x");
            console_out_hex16((uint16_t)mask);
            console_out_str("\n");
            msg->data_len = 0u;
            msg->status = wm8904_audio_restart_declick(mask)
                            ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        }
        return;
    }

    if (msg->name == 'p') {
        if (msg->kind == '?') {
            wm8904_audio_path_report();
            msg->status = APP_CONSOLE_OK;
            return;
        }

        if (msg->data_len == 0u) {
            wm8904_audio_path_next();
            msg->status = APP_CONSOLE_OK;
            return;
        }

        if (msg->data_len > 2u) {
            console_out_str(" *tp: value is 0000..0004, or omit it to cycle\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }

        {
            const uint16_t value =
                (msg->data_len == 2u)
                    ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                    : (uint16_t)msg->data[0];

            msg->status = wm8904_audio_path_set(value) ? APP_CONSOLE_OK
                                                       : APP_CONSOLE_ERR_BAD_DATA;
        }
        return;
    }

#if WM8904_AUDIO_ENABLE_TDM_DIAG
    if (msg->name == 'b') {
        if (msg->kind == '?') {
            wm8904_audio_avas_parts_report();
            msg->status = APP_CONSOLE_OK;
            return;
        }
        {
            uint16_t value;

            if ((msg->data_len == 0u) || (msg->data_len > 2u)) {
                wm8904_audio_avas_parts_usage();
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }
            value = (msg->data_len == 2u)
                        ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                        : (uint16_t)msg->data[0];

            if (!wm8904_audio_avas_parts_valid(value)) {
                wm8904_audio_avas_parts_usage();
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                return;
            }
            wm8904_audio_avas_parts_set((uint8_t)value);
        }
        msg->status = APP_CONSOLE_OK;
        return;
    }
#endif

    if ((msg->name == 'i') || (msg->name == 'o')) {
        const bool post = (msg->name == 'o');

        if (msg->kind == '?') {
            if (msg->data_len != 0u) {
                console_out_str(post ? " ?to: takes no value\n" : " ?ti: takes no value\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }
            if (post) {
                wm8904_audio_post_gain_report();
            } else {
                wm8904_audio_pre_gain_report();
            }
            msg->status = APP_CONSOLE_OK;
            return;
        }

        {
            uint16_t value;

            if ((msg->data_len == 0u) || (msg->data_len > 2u)) {
                console_out_str(post ? " *to" : " *ti");
                console_out_str(": needs tenths of a dB, signed hex -- 0000 = 0.0 dB,"
                                " 003C = +6.0 dB, FFF6 = -1.0 dB (0.5 dB grid)\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }

            value = (msg->data_len == 2u)
                        ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                        : (uint16_t)(int16_t)(int8_t)msg->data[0];

            msg->status = (post ? wm8904_audio_post_gain_set((int16_t)value)
                                : wm8904_audio_pre_gain_set((int16_t)value))
                              ? APP_CONSOLE_OK
                              : APP_CONSOLE_ERR_BAD_DATA;
        }
        return;
    }

    if (msg->name == 'c') {
        if (msg->kind == '?') {
            if (msg->data_len != 0u) {
                console_out_str(" ?tc: takes no value\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }
            wm8904_audio_avas_pitch_report();
            msg->status = APP_CONSOLE_OK;
            return;
        }

        {
            uint16_t value;

            if ((msg->data_len == 0u) || (msg->data_len > 2u)) {
                console_out_str(" *tc: needs cent, signed hex -- 0000 = 0, 00C8 = +200,"
                                " FF38 = -200 (the trim's full range)\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }

            value = (msg->data_len == 2u)
                        ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                        : (uint16_t)(int16_t)(int8_t)msg->data[0];

            (void)wm8904_audio_avas_pitch_set((int16_t)value);
        }
        msg->status = APP_CONSOLE_OK;
        return;
    }

    if (msg->name == 'q') {
        if (msg->kind == '?') {
            wm8904_audio_load_line_report();
            msg->status = APP_CONSOLE_OK;
            return;
        }

        {
            uint16_t value;

            if (msg->kind != '*') {
                msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
                return;
            }

            if (msg->data_len == 0u) {
                wm8904_audio_load_line_set(false);
                msg->status = APP_CONSOLE_OK;
                return;
            }

            value = (msg->data_len >= 2u)
                        ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                        : (uint16_t)msg->data[0];

            if (value == 2u) {
                console_out_str(" *tq0002YYYY: telemetry-period selection is unsupported; "
                                "CK TDM1 uses its board-profile-fixed period\n");
                msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
                return;
            }

            if (msg->data_len > 2u) {
                console_out_str(" *tq: takes no period -- use *tq or *tq0000 = off, "
                                "*tq0001 = on\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }

            if (value > 1u) {
                console_out_str(" *tq: value must be 0000 or 0001 (or omit it for off)\n");
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                return;
            }

            wm8904_audio_load_line_set(value != 0u);
        }
        msg->status = APP_CONSOLE_OK;
        return;
    }

    if (msg->name == 's') {

        if (msg->data_len != 0u) {
            console_out_str(" *ts/?ts: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }

        if (msg->kind == '?') {
            wm8904_audio_stop_report();
        } else {
            wm8904_audio_stop();
        }
        msg->status = APP_CONSOLE_OK;
        return;
    }

    if (msg->name == 'r') {
        if (msg->data_len != 0u) {
            console_out_str(" *tr: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }
        if (msg->kind == '?') {
            console_out_str(" ?tr: no query form -- ?ts reports the state\n");
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            return;
        }

        msg->status = wm8904_audio_restart() ? APP_CONSOLE_OK
                                            : APP_CONSOLE_ERR_OPERATION_FAILED;
        return;
    }

#if WM8904_AUDIO_ENABLE_TDM_DIAG
    if (msg->name == 'l') {
        if (msg->data_len != 0u) {
            console_out_str(" *tl: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }
        if (msg->kind == '?') {
            console_out_str(" ?tl: no query form -- *tl performs the calls it reports\n");
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            return;
        }

        msg->status = wm8904_audio_lifecycle_probe() ? APP_CONSOLE_OK
                                                    : APP_CONSOLE_ERR_OPERATION_FAILED;
        return;
    }
#endif

#if WM8904_AUDIO_ENABLE_SYSTEM_PROBE

    if (msg->name == 'm') {
        if (msg->data_len != 0u) {
            console_out_str(" *tm: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }
        if (msg->kind == '?') {
            console_out_str(" ?tm: no query form -- *tm performs the calls it reports\n");
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            return;
        }

        msg->status = wm8904_audio_system_probe() ? APP_CONSOLE_OK
                                                 : APP_CONSOLE_ERR_OPERATION_FAILED;
        return;
    }
#endif

    msg->status = APP_CONSOLE_ERR_NOT_FOUND;
}

void ev97u97a_classic_console_onmsg(app_console_msg_t *msg)
{
    uint8_t subcode;

    if (msg == NULL) {
        return;
    }

    if (msg->name != 'y') {
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        return;
    }

    if (msg->kind != '*') {
        console_out_str(" ?cy: no query form -- *cy00 toggles the Type_TY AVAS voice\n");
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    if (msg->data_len != 1u) {
        console_out_str(" *cy: needs one synth subcode -- 00 = Type_TY AVAS toggle\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    subcode = msg->data[0];
    if (subcode == 0u) {
        console_out_str(" *cy00: Type_TY AVAS toggle\n");
        wm8904_audio_avas_voice_key((uint8_t)WM8904_AUDIO_AVAS_VOICE_TYPE_TY);
        msg->status = APP_CONSOLE_OK;
        return;
    }

    if (subcode <= 4u) {
        console_out_str(" *cy: Sonora synth subcode is unsupported on CK (only 00 = Type_TY AVAS)\n");
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    console_out_str(" *cy: unknown synth subcode\n");
    msg->status = APP_CONSOLE_ERR_BAD_DATA;
}

bool ev97u97a_transport_console_hotkey(uint8_t ch)
{
    static uint8_t s_esc_state;

    if (s_esc_state == 2u) {
        s_esc_state = 0u;
        if (ch == (uint8_t)'A') {
            wm8904_audio_avas_pitch_step(+5);
            return true;
        }
        if (ch == (uint8_t)'B') {
            wm8904_audio_avas_pitch_step(-5);
            return true;
        }
        return true;
    }

    if (s_esc_state == 1u) {
        s_esc_state = (ch == (uint8_t)'[') ? 2u : 0u;
        return true;
    }

    if (ch == 0x1Bu) {
        s_esc_state = 1u;
        return true;
    }

    if (ch == (uint8_t)'a') {
        wm8904_audio_avas_voice_key((uint8_t)WM8904_AUDIO_AVAS_VOICE_TYPE_TY);
        return true;
    }

    if (ch == (uint8_t)'A') {
        wm8904_audio_avas_voice_key((uint8_t)WM8904_AUDIO_AVAS_VOICE_TYPE_LB);
        return true;
    }

    return false;
}
#endif

bool profile_wait_next_tick(void)
{
    static uint32_t status_last_ms;
    uint32_t        wait_start_ms = GetTicks();
    bool            report;

    while ((uint32_t)(GetTicks() - wait_start_ms) < EV97U97A_POLL_PERIOD_MS)
    {
#if DEMO_ENABLE_CONSOLE_COMMANDS
        console_task_poll();
#endif
    }

    report = ((uint32_t)(GetTicks() - status_last_ms) >= EV97U97A_STATUS_PRINT_PERIOD_MS);
    if (report)
    {
        status_last_ms = GetTicks();
    }

    return report;
}

void profile_poll(bool report)
{

    demo_rgb_pot_buttons_poll(report);

#if DEMO_ENABLE_I2C_PROBE

    i2c_probe_poll(&s_i2c1_probe, report);
#endif

#if DEMO_ENABLE_TDM_MASTER_LOOPBACK
    demo_tdm_master_loopback_poll();
#endif

#if DEMO_ENABLE_WM8904_AUDIO

    wm8904_audio_poll(report);
#endif
}
