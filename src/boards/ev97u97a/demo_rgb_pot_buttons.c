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
#error "boards/ev97u97a/demo_rgb_pot_buttons.c is EV97U97A-owned. Build it only in the CK1024MP710_EV97U97A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "demo_rgb_pot_buttons.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_config.h"
#include "ev97u97a_io.h"
#include "ev97u97a_led3_rgb.h"
#include "ev97u97a_pot.h"
#include "button_debounce.h"
#include "timer_1ms.h"
#include "nora_clock.h"

#if DEMO_ENABLE_WM8904_AUDIO
#include "wm8904_audio.h"
#endif

#define BUTTON_DEBOUNCE_TIME_MS 20u

#define AVAS_EVENT_TYPE_TY 0x01u
#define AVAS_EVENT_TYPE_LB 0x02u

#define POT_MAX_COUNTS       4095u
#define POT_EMA_SHIFT        4u
#define POT_ON_THRESHOLD     80u
#define POT_OFF_THRESHOLD    40u
#define POT_OFF_GREEN        12u

#define OUTPUT_ACTIVITY_HOLD_POLLS        5u
#define OUTPUT_ACTIVITY_BLINK_HALF_POLLS  3u

#define AVAS_POT_TOP_CENT 200u
#define AVAS_POT_ZERO_SNAP 82u
#define AVAS_POT_DEADBAND 24u

static uint16_t potentiometer;
static uint32_t s_pot_ema_acc;
static bool     s_pot_ema_init;
static bool     s_rgb_on;
static bool     s_rgb_manual;
static uint16_t s_pot_pitch_ref;
static int16_t  s_avas_pitch_cent;
static bool     s_pot_pitch_seeded;
static uint8_t  s_led1_activity_hold;
static uint8_t  s_led1_blink_count;
static bool     s_led1_blink_on;
static volatile uint8_t s_avas_events;
static button_debounce_t s_sw2;
static button_debounce_t s_sw3;

static bool demo_sw_read(uint16_t id)
{
    return ev97u97a_sw_pressed((ev97u97a_sw_t)id);
}

static void demo_sw_on_press(uint16_t id)
{
    if (id == (uint16_t)EV97U97A_SW_2) {
        s_avas_events |= AVAS_EVENT_TYPE_TY;
    } else if (id == (uint16_t)EV97U97A_SW_3) {
        s_avas_events |= AVAS_EVENT_TYPE_LB;
    }
}

static const button_debounce_config_t s_sw2_cfg = {
    .read_pressed   = demo_sw_read,
    .on_press       = demo_sw_on_press,
    .id             = (uint16_t)EV97U97A_SW_2,
    .blanking_ticks = BUTTON_DEBOUNCE_TIME_MS,
};

static const button_debounce_config_t s_sw3_cfg = {
    .read_pressed   = demo_sw_read,
    .on_press       = demo_sw_on_press,
    .id             = (uint16_t)EV97U97A_SW_3,
    .blanking_ticks = BUTTON_DEBOUNCE_TIME_MS,
};

static void demo_button_tick(void)
{
    button_debounce_poll(&s_sw2);
    button_debounce_poll(&s_sw3);

    ev97u97a_led_set(EV97U97A_LED_2,
                     ev97u97a_sw_pressed(EV97U97A_SW_1) ||
                     ev97u97a_sw_pressed(EV97U97A_SW_2) ||
                     ev97u97a_sw_pressed(EV97U97A_SW_3));
}

static void demo_rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{

    ev97u97a_led3_rgb_set_color((uint16_t)red << 8,
                                (uint16_t)green << 8,
                                (uint16_t)blue << 8);
}

static void demo_rgb_update(uint16_t pot)
{
    uint16_t filtered;

    if (!s_pot_ema_init) {
        s_pot_ema_acc = (uint32_t)pot << POT_EMA_SHIFT;
        s_pot_ema_init = true;
    }
    s_pot_ema_acc += (uint32_t)pot - (s_pot_ema_acc >> POT_EMA_SHIFT);
    filtered = (uint16_t)(s_pot_ema_acc >> POT_EMA_SHIFT);

    if (s_rgb_manual) {
        return;
    }

    if (s_rgb_on) {
        if (filtered < POT_OFF_THRESHOLD) {
            s_rgb_on = false;
        }
    } else if (filtered > POT_ON_THRESHOLD) {
        s_rgb_on = true;
    }

    if (!s_rgb_on) {
        demo_rgb_set(0u, POT_OFF_GREEN, 0u);
    } else {
        uint16_t position;
        uint8_t  red;
        uint8_t  green;
        uint8_t  blue;

        if (filtered <= POT_ON_THRESHOLD) {
            position = 0u;
        } else if (filtered >= POT_MAX_COUNTS) {
            position = 1000u;
        } else {
            position = (uint16_t)(((uint32_t)(filtered - POT_ON_THRESHOLD) * 1000u) /
                                  (POT_MAX_COUNTS - POT_ON_THRESHOLD));
        }

        if (position <= 500u) {

            red   = (uint8_t)((uint32_t)position * 60u / 500u);
            green = red;
            blue  = (uint8_t)(90u - ((uint32_t)position * 30u / 500u));
        } else {

            const uint16_t upper = (uint16_t)(position - 500u);
            red   = (uint8_t)(60u + ((uint32_t)upper * 30u / 500u));
            green = (uint8_t)(60u - ((uint32_t)upper * 60u / 500u));
            blue  = (uint8_t)(60u - ((uint32_t)upper * 60u / 500u));
        }
        demo_rgb_set(red, green, blue);
    }
}

static int16_t demo_pot_to_avas_cent(uint16_t pot)
{
    if (pot <= AVAS_POT_ZERO_SNAP) {
        return 0;
    }

    return (int16_t)(((uint32_t)pot * AVAS_POT_TOP_CENT + (POT_MAX_COUNTS / 2u)) /
                     POT_MAX_COUNTS);
}

static void demo_output_activity_update(void)
{
#if DEMO_ENABLE_WM8904_AUDIO
    if (wm8904_audio_output_activity_take()) {
        s_led1_activity_hold = OUTPUT_ACTIVITY_HOLD_POLLS;
        if (++s_led1_blink_count >= OUTPUT_ACTIVITY_BLINK_HALF_POLLS) {
            s_led1_blink_count = 0u;
            s_led1_blink_on = !s_led1_blink_on;
        }
    } else if (s_led1_activity_hold != 0u) {
        s_led1_activity_hold--;
    }
    ev97u97a_led_set(EV97U97A_LED_1,
                     (s_led1_activity_hold != 0u) && s_led1_blink_on);
#else
    ev97u97a_led_set(EV97U97A_LED_1, false);
#endif
}

void demo_rgb_pot_buttons_init(void)
{
    s_avas_events = 0u;
    s_pot_ema_init = false;
    s_rgb_on = false;
    s_pot_pitch_seeded = false;
    s_rgb_manual = false;
    s_avas_pitch_cent = 0;
    s_led1_activity_hold = 0u;
    s_led1_blink_count = 0u;
    s_led1_blink_on = false;
    button_debounce_init(&s_sw2, &s_sw2_cfg);
    button_debounce_init(&s_sw3, &s_sw3_cfg);
    TIMER_RequestTick(demo_button_tick, 1);

    if (!ev97u97a_led3_rgb_init(nora_clock_get_fcy_hz())) {
        TIMER_RequestTick(ev97u97a_led3_rgb_tick, 1);
    }
    demo_rgb_set(0u, POT_OFF_GREEN, 0u);
}

void demo_rgb_pot_buttons_set_rgb_manual_percent(uint8_t red,
                                                  uint8_t green,
                                                  uint8_t blue)
{
    s_rgb_manual = true;
    ev97u97a_led3_rgb_set_percent(red, green, blue);
}

void demo_rgb_pot_buttons_use_rgb_pot(void)
{
    s_rgb_manual = false;
}

bool demo_rgb_pot_buttons_rgb_is_manual(void)
{
    return s_rgb_manual;
}

void demo_rgb_pot_buttons_get_rgb_percent(uint8_t *red,
                                           uint8_t *green,
                                           uint8_t *blue)
{
    ev97u97a_led3_rgb_get_percent(red, green, blue);
}

void demo_rgb_pot_buttons_print_header(void)
{
    printf("EV97U97A audio controls: SW1 mute/unmute, SW2 AVAS Type_TY on/off, "
           "SW3 AVAS Type_LB on/off; POT AVAS pitch 0 to +200 cent\n");
}

static void print_status_line(void)
{
    const char *pot_fault = ev97u97a_pot_fault();

    if (pot_fault != NULL) {
        printf("pot=FAULT(%s)\n", pot_fault);
    } else {
        printf("pot=%u/4095  AVAS pitch=%+d cent\n", (unsigned int)potentiometer,
               (int)s_avas_pitch_cent);
    }
}

void demo_rgb_pot_buttons_poll(bool report)
{
    uint8_t avas_events;
    uint16_t filtered;

    potentiometer = ev97u97a_pot_read();
    demo_rgb_update(potentiometer);
    demo_output_activity_update();

    filtered = (uint16_t)(s_pot_ema_acc >> POT_EMA_SHIFT);

#if DEMO_ENABLE_WM8904_AUDIO
    {
        int16_t moved;

        if (!s_pot_pitch_seeded) {
            s_pot_pitch_seeded = true;
            s_pot_pitch_ref = filtered;
            s_avas_pitch_cent = demo_pot_to_avas_cent(filtered);
            wm8904_audio_avas_pitch_pot_set(s_avas_pitch_cent);
        } else {
            moved = (int16_t)((int32_t)filtered - (int32_t)s_pot_pitch_ref);
            if (moved < 0) {
                moved = (int16_t)-moved;
            }
            if (moved > (int16_t)AVAS_POT_DEADBAND) {
                s_pot_pitch_ref = filtered;
                s_avas_pitch_cent = demo_pot_to_avas_cent(filtered);
                wm8904_audio_avas_pitch_pot_set(s_avas_pitch_cent);
            }
        }
    }
#endif

    avas_events = s_avas_events;
    s_avas_events = 0u;

#if DEMO_ENABLE_WM8904_AUDIO
    if ((avas_events & AVAS_EVENT_TYPE_TY) != 0u) {
        wm8904_audio_avas_voice_key((uint8_t)WM8904_AUDIO_AVAS_VOICE_TYPE_TY);
    }
    if ((avas_events & AVAS_EVENT_TYPE_LB) != 0u) {
        wm8904_audio_avas_voice_key((uint8_t)WM8904_AUDIO_AVAS_VOICE_TYPE_LB);
    }
#else
    (void)avas_events;
#endif

    if (report) {
        print_status_line();
    }
}
