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

/*
 * Modified by SulaoLab, 2026 (Apache-2.0 section 4(b)). What changed is recorded
 * below in full.
 *
 * The pot/RGB/button demo, split out of this board's main.c -- see the header for
 * why it stayed on the board instead of going to app/. The licence above travels
 * with it: the logic below is the inherited Microchip demo, near-unchanged.
 *
 * What DID change in the split:
 *   - the button blanking countdown is now app/button_debounce.{c,h}, one
 *     instance per button, instead of two hand-rolled counters inline
 *   - the LED-follows-button mirror is explicit here rather than buried inside
 *     the debounce function, and still runs on the 1 ms tick so its latency is
 *     unchanged (the foreground iteration is ~20 ms, so mirroring there would have
 *     been visibly slower)
 *
 * And what changed on 2026-08-05, when this board got an interactive console: the
 * ANSI fixed screen became one throttled status line. See the note on
 * demo_rgb_pot_buttons_poll() in the header for why an interactive console and a
 * region repainted ~60 times a second cannot share a terminal. Note the 20 ms above
 * survives that change but for a different reason: it WAS how long a full repaint took
 * at 230400 baud, and it is now this board's stated loop period (main.c).
 */

#ifndef DSPIC33CK_BOARD_DM330030
#error "boards/dm330030/demo_rgb_pot_buttons.c is DM330030-owned. Build it only in the CK256MP508_DM330030 configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "demo_rgb_pot_buttons.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "dm330030_io.h"
#include "dm330030_pot.h"
#include "dm330030_led3_rgb.h"

#include "button_debounce.h"
#include "timer_1ms.h"

/*
 * TWO DOMAINS, AND THIS FILE IS WHERE THEY MEET.
 *
 * The pot reads 12-bit (0..4095, dm330030_pot_read()); dm330030_led3_rgb_set_color() takes 16-bit
 * PWM duty. So the channel values below are kept in the POT's domain -- which is also
 * the domain the screen displays -- and shifted up only at the call that drives the LED.
 *
 * It used to be the other way round: the board's ADC adapter left-shifted into 16 bits
 * and PrintData() shifted every value straight back down to display it. The adapter knew
 * the LED driver's bit depth, which is not a fact about a potentiometer.
 */
#define POT_TO_PWM_SHIFT (4u)
#define PWM_OF(v)        ((uint16_t)((uint16_t)(v) << POT_TO_PWM_SHIFT))

/* Half scale in the PWM domain, i.e. white at 50% duty on all three channels. */
#define WHITE_INTENSITY 0x8000

/* 20 ms of blanking on a 1 ms tick. Long enough for the contact chatter on these
 * switches, short enough that a deliberate double-press still registers. */
#define BUTTON_DEBOUNCE_TIME_MS 20

typedef enum
{
    BUTTON_COLOR_RED = 0,
    BUTTON_COLOR_GREEN = 1,
    BUTTON_COLOR_BLUE = 2
} BUTTON_COLOR;

/* Written by ChangeColor() in the 1 ms tick ISR, read by the foreground poll.
 * volatile for that reason; enum here is a single byte's worth of values and the
 * read cannot straddle the write. */
static volatile BUTTON_COLOR buttonColor;

static uint16_t potentiometer;
static bool     button_s3_is_pressed;

/* Pot domain (0..4095). The same three starting points as the vendor demo, which stated
 * them as 0x4000/0x8000/0xB000 in the PWM domain. */
static uint16_t red = 0x400;
static uint16_t green = 0x800;
static uint16_t blue = 0xB00;

/*
 * NO TRAILING PADDING, unlike the vendor original ("Not Pressed" / "Pressed    ",
 * "Red  " / "Green" / "Blue "). The padding was not cosmetic: the screen was a fixed
 * region rewritten in place, so a shorter value had to overwrite the tail of the longer
 * one it replaced. This line scrolls now -- there is nothing left behind to erase -- and
 * the padding would just be spaces in the middle of a status line.
 */
static const char *const button_strings[2] = { "off", "on" };

static const char *const color_strings[3] = { "Red", "Green", "Blue" };

static button_debounce_t s_sw1;
static button_debounce_t s_sw2;

static void PrintStatusLine(void);

//Helper function that advances the currently selected RGB color channel that
//is to be adjusted next.  This function is called in response to user pushbutton
//press events.
static void ChangeColor(void)
{
    switch(buttonColor)
    {
        case BUTTON_COLOR_RED:
            buttonColor = BUTTON_COLOR_GREEN;
            break;

        case BUTTON_COLOR_GREEN:
            buttonColor = BUTTON_COLOR_BLUE;
            break;

        case BUTTON_COLOR_BLUE:
            buttonColor = BUTTON_COLOR_RED;
            break;

        default:
            buttonColor = BUTTON_COLOR_RED;
            break;
    }
}

/* -------------------------------------------------------------------------- */
/* Buttons: app/button_debounce.c owns the timing, this owns what a press means */
/* -------------------------------------------------------------------------- */

static bool demo_sw_read(uint16_t id)
{
    return dm330030_sw_pressed((dm330030_sw_t)id);
}

static void demo_sw_on_press(uint16_t id)
{
    /* Both buttons advance the channel, which is the original behaviour -- S1 and
     * S2 differ only in which LED mirrors them. */
    (void)id;
    ChangeColor();
}

static const button_debounce_config_t s_sw1_cfg = {
    .read_pressed   = demo_sw_read,
    .on_press       = demo_sw_on_press,
    .id             = (uint16_t)DM330030_SW_1,
    .blanking_ticks = BUTTON_DEBOUNCE_TIME_MS,
};

static const button_debounce_config_t s_sw2_cfg = {
    .read_pressed   = demo_sw_read,
    .on_press       = demo_sw_on_press,
    .id             = (uint16_t)DM330030_SW_2,
    .blanking_ticks = BUTTON_DEBOUNCE_TIME_MS,
};

/*
 * 1 ms tick client. Sampling S1/S2 and mirroring them onto LED1/LED2 -- the LED
 * follows the RAW sample, not the debounced state, so it reports the switch
 * rather than the software's opinion of it. That is the original behaviour and it
 * is the more useful one on a lab board: a chattering switch is visible.
 *
 * S3 is NOT sampled here. It has no edge behaviour and no debounce need -- it is
 * a level that selects white -- so the foreground reads it directly.
 */
static void demo_button_tick(void)
{
    button_debounce_poll(&s_sw1);
    dm330030_led_set(DM330030_LED_1, button_debounce_raw(&s_sw1));

    button_debounce_poll(&s_sw2);
    dm330030_led_set(DM330030_LED_2, button_debounce_raw(&s_sw2));
}

/* -------------------------------------------------------------------------- */

void demo_rgb_pot_buttons_init(void)
{
    buttonColor = BUTTON_COLOR_RED;

    button_debounce_init(&s_sw1, &s_sw1_cfg);
    button_debounce_init(&s_sw2, &s_sw2_cfg);

    //Used for the LED modulation
    TIMER_RequestTick(dm330030_led3_rgb_tick, 1);
    //Register the button sampler, so it gets called periodically when the timer
    //interrupts occur (in this case at 1:1 rate, so it executes once per 1ms).
    TIMER_RequestTick(demo_button_tick, 1);
}

void demo_rgb_pot_buttons_poll(bool report)
{
    // Fetch an ADC sample from the potentiometer.
    potentiometer = dm330030_pot_read();

    //Use the potentiometer ADC value to set the brightness of the currently
    //selected color channel on the RGB LED.  The "currently selected channel"
    //is manually selected by the user at runtime by pressing the pushbuttons.
    switch(buttonColor)
    {
        case BUTTON_COLOR_RED:
            red = potentiometer;
            break;

        case BUTTON_COLOR_GREEN:
            green = potentiometer;
            break;

        case BUTTON_COLOR_BLUE:
            blue = potentiometer;
            break;

        default:
            break;
    }

    button_s3_is_pressed = dm330030_sw_pressed(DM330030_SW_3);

    if( button_s3_is_pressed == true)
    {
        //Set RGB LED color to white if S3 is pressed
        dm330030_led3_rgb_set_color(WHITE_INTENSITY, WHITE_INTENSITY, WHITE_INTENSITY);
    }
    else
    {
        //Update the PWM values controlling the intensity of the RGB LED channels.
        //This is the one place the pot domain becomes the PWM domain.
        dm330030_led3_rgb_set_color(PWM_OF(red), PWM_OF(green), PWM_OF(blue));
    }

    if (report)
    {
        PrintStatusLine();
    }
}

/*
 * The console is UART1 through uart_platform/uart_platform_stdio.c: 230400 baud,
 * 8-N-1, no flow control. (The original comment here said 38400, which stopped
 * being true when this repo took the baud rate from the Clock HAL.)
 *
 * PLAIN SCROLLING TEXT, no escape sequences at all. The three that used to open this
 * function are all gone for stated reasons:
 *
 *   \033[2J  clear screen  -- a scrolling console has boot output above this worth
 *                             keeping (the tick-failure line, and whatever the next
 *                             parity item adds to the banner). Erasing it was only
 *                             right when the screen below was a fixed layout.
 *   \033[0;0f home cursor  -- nothing is addressed absolutely any more.
 *   \033[?25l cursor off   -- ACTIVELY WRONG NOW: this board has an interactive
 *                             console, so there is a prompt to see and characters
 *                             being echoed at it. Hiding the cursor on a terminal
 *                             someone is typing into is the one change here that
 *                             would have been a bug rather than a preference.
 */
void demo_rgb_pot_buttons_print_header(void)
{
    printf("dsPIC33CK Curiosity Development Board demo:"
           " S1/S2 select an RGB channel (and mirror to LED1/LED2),"
           " the pot sets its intensity, S3 forces white\n");
}

/*
 * ONE LINE, and it carries what the eight-line region carried.
 *
 * It also keeps the region's one non-cosmetic job: the pot field doubles as the ADC's
 * ONLY fault report. Without it a dead ADC is indistinguishable from a pot nobody is
 * turning -- the reader just sees a number that does not change. That used to be
 * unsayable at bring-up because the header cleared the screen immediately afterwards;
 * with a scrolling console it would now be sayable there too, but it belongs on the
 * repeating line either way, since the ADC can fail after bring-up.
 */
static void PrintStatusLine(void)
{
    const char *pot_fault = dm330030_pot_fault();

    if (pot_fault != NULL)
    {
        printf("pot=FAULT(%s)", pot_fault);
    }
    else
    {
        printf("pot=%u/4095", (unsigned int)potentiometer);
    }

    printf("  ch=%s  S1=%s S2=%s S3=%s  rgb=(%u,%u,%u)\n",
           color_strings[buttonColor],
           button_strings[button_debounce_pressed(&s_sw1)],
           button_strings[button_debounce_pressed(&s_sw2)],
           button_strings[button_s3_is_pressed],
           (unsigned int)red,
           (unsigned int)green,
           (unsigned int)blue);
}
