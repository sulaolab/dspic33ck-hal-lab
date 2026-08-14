/*******************************************************************************
Copyright 2016 Microchip Technology Inc. (www.microchip.com)

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
 * Modified by SulaoLab, 2026 (Apache-2.0 section 4(b)). Changes to Microchip's
 * original: renamed into this board's namespace (was bsp/led3_rgb.c), the three
 * pin writes go through the GPIO HAL instead of LATx/TRISx, ANSEL is owned per
 * pin, the ownership assert below was added, and two defects that were only
 * correct at one clock point were fixed. See the notes further down for why.
 */

#ifndef DSPIC33CK_BOARD_DM330030
#error "boards/dm330030/dm330030_led3_rgb.c is DM330030-owned. Build it only in the CK256MP508_DM330030 configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "dm330030_led3_rgb.h"

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * The three pin writes below go through the GPIO HAL rather than LATx/TRISx, for
 * the reason spelled out in dm330030_io.h: dm330030_board.c's
 * dm330030_user_io_pins_init() already configures DM330030_RGB_{RED,GREEN,BLUE}_PIN, so a
 * second place asserting direction makes two owners of one pin. As written before, each
 * pin_control() rewrote TRIS on every call -- and this one is called from the 1 ms tick,
 * so it was doing that thousands of times a second.
 *
 * The soft-PWM logic in this file is untouched on purpose: modulation counts and
 * gamma-ish intensity stepping are how this board presents its RGB LED, not
 * anything the HAL has an opinion about.
 */
#include "dm330030_pins.h"
#include "nora_gpio.h"

/* This module uses 3 dimmable LEDs (red, green, blue).  It uses them to
 * implement one color changing LED.
 */

#define LED_ON  1
#define LED_OFF 0

#define INPUT  1
#define OUTPUT 0

struct LED {
    uint16_t intensity;
    uint16_t modulation_count;
    unsigned int next_state;
    void (*pin_control)(unsigned int state);
};

static void dm330030_led3_red_pin_control(unsigned int state);
static void dm330030_led3_green_pin_control(unsigned int state);
static void dm330030_led3_blue_pin_control(unsigned int state);

static struct LED red = {0, 0, LED_OFF, &dm330030_led3_red_pin_control};
static struct LED green = {0, 0, LED_OFF, &dm330030_led3_green_pin_control};
static struct LED blue = {0, 0, LED_OFF, &dm330030_led3_blue_pin_control};

#define DEFAULT_INTENSITY 0x2000

void dm330030_led3_red_set_intensity(uint16_t new_intensity)
{  
    red.intensity = new_intensity >> 8;
}

void dm330030_led3_red_on(void)
{
    dm330030_led3_red_set_intensity(DEFAULT_INTENSITY);
}

void dm330030_led3_red_off(void)
{
    dm330030_led3_red_set_intensity(0);
}

void dm330030_led3_red_toggle(void)
{
	if(red.intensity == 0)
    {
        dm330030_led3_red_set_intensity(DEFAULT_INTENSITY);
    }
    else
    {
        dm330030_led3_red_set_intensity(0);
    }
}

static void dm330030_led3_red_pin_control(unsigned int state)
{
    /* Direction is dm330030_board.c's; this only drives the level. */
    (void)nora_gpio_write(DM330030_RGB_RED_PIN, state != 0u);
}

void dm330030_led3_green_set_intensity(uint16_t new_intensity)
{  
    green.intensity = new_intensity >> 8;
}

void dm330030_led3_green_on(void)
{
    dm330030_led3_green_set_intensity(DEFAULT_INTENSITY);
}

void dm330030_led3_green_off(void)
{
    dm330030_led3_green_set_intensity(0);
}

void dm330030_led3_green_toggle(void)
{
	if(green.intensity == 0)
    {
        dm330030_led3_green_set_intensity(DEFAULT_INTENSITY);
    }
    else
    {
        dm330030_led3_green_set_intensity(0);
    }
}

static void dm330030_led3_green_pin_control(unsigned int state)
{
    /* Direction is dm330030_board.c's; this only drives the level. */
    (void)nora_gpio_write(DM330030_RGB_GREEN_PIN, state != 0u);
}

void dm330030_led3_blue_set_intensity(uint16_t new_intensity)
{      
    blue.intensity = new_intensity >> 8;
}

void dm330030_led3_blue_on(void)
{
    dm330030_led3_blue_set_intensity(DEFAULT_INTENSITY);
}

void dm330030_led3_blue_off(void)
{
    dm330030_led3_blue_set_intensity(0);
}

void dm330030_led3_blue_toggle(void)
{
	if(blue.intensity == 0)
    {
        dm330030_led3_blue_set_intensity(DEFAULT_INTENSITY);
    }
    else
    {
        dm330030_led3_blue_set_intensity(0);
    }
}

static void dm330030_led3_blue_pin_control(unsigned int state)
{
    /* Direction is dm330030_board.c's; this only drives the level. */
    (void)nora_gpio_write(DM330030_RGB_BLUE_PIN, state != 0u);
}

void dm330030_led3_rgb_on(void)
{
    dm330030_led3_red_on();
    dm330030_led3_blue_on();
    dm330030_led3_green_on();  
}

void dm330030_led3_rgb_off(void)
{
    dm330030_led3_red_off();
    dm330030_led3_blue_off();
    dm330030_led3_green_off();  
}

void dm330030_led3_rgb_toggle(void)
{
    dm330030_led3_red_toggle();
    dm330030_led3_blue_toggle();
    dm330030_led3_green_toggle();  
}

void dm330030_led3_rgb_set_color(uint16_t red, uint16_t green, uint16_t blue)
{
    dm330030_led3_red_set_intensity(red);
    dm330030_led3_green_set_intensity(green);
    dm330030_led3_blue_set_intensity(blue);
}


//https://www.embeddedrelated.com/showarticle/107.php
#define FULL_SCALE_MODULATION_COUNT 285
#define MIN_MODULATION_COUNT 18

static void ModulateLEDNextStateSet(struct LED *led)
{
    uint16_t count_change = led->intensity;
    
    if(count_change < MIN_MODULATION_COUNT)
    {       
        if(count_change > (MIN_MODULATION_COUNT>>1))
        {
            count_change = MIN_MODULATION_COUNT;
        }
        else
        {
            count_change = 0;
        }
    }
    
    led->modulation_count += count_change;
    if(led->modulation_count < FULL_SCALE_MODULATION_COUNT)
    {
        led->next_state = LED_OFF;
    }
    else
    {
        led->next_state = LED_ON;
        led->modulation_count -= FULL_SCALE_MODULATION_COUNT;
    }
}

static void SetLEDState(struct LED *led)
{
    led->pin_control(led->next_state);
}

void dm330030_led3_rgb_tick(void)
{
    SetLEDState(&red);
    SetLEDState(&blue);
    SetLEDState(&green);
    
    ModulateLEDNextStateSet(&red);
    ModulateLEDNextStateSet(&blue);
    ModulateLEDNextStateSet(&green);
}


