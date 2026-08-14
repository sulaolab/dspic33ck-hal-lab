/*
 * dm330030_io.c -- the LEDs and switches, owning nothing. See dm330030_io.h for what this
 * replaced, why the pot is dm330030_pot.c instead, and why there is no direction register
 * anywhere below.
 */

#ifndef DSPIC33CK_BOARD_DM330030
#error "boards/dm330030/dm330030_io.c is DM330030-owned. Build it only in the CK256MP508_DM330030 configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "dm330030_io.h"

#include <stddef.h>

#include "dm330030_pins.h"
#include "nora_gpio.h"

/*
 * Index -> pin, in one place. dm330030_pins.h holds the pin numbers; this table only
 * gives them an order, so adding a third LED is one line here and one line there
 * rather than a new function.
 */
static const nora_gpio_pin_t s_led_pins[DM330030_LED_COUNT] = {
    DM330030_LED1_PIN,
    DM330030_LED2_PIN
};

static const nora_gpio_pin_t s_sw_pins[DM330030_SW_COUNT] = {
    DM330030_SW1_PIN,
    DM330030_SW2_PIN,
    DM330030_SW3_PIN
};

void dm330030_led_set(dm330030_led_t led, bool on)
{
    if ((unsigned)led >= (unsigned)DM330030_LED_COUNT) {
        return;
    }

    /* write() only, never set_direction(): dm330030_board.c owns direction. That is the
     * whole point of this file -- see dm330030_io.h. */
    (void)nora_gpio_write(s_led_pins[led], on);
}

void dm330030_led_toggle(dm330030_led_t led)
{
    if ((unsigned)led >= (unsigned)DM330030_LED_COUNT) {
        return;
    }

    (void)nora_gpio_toggle(s_led_pins[led]);
}

bool dm330030_led_get(dm330030_led_t led)
{
    if ((unsigned)led >= (unsigned)DM330030_LED_COUNT) {
        return false;
    }

    /* read_output(), not read(): the question is what the pin is being DRIVEN to,
     * which for an output is the latch. Reading the port instead would return
     * whatever the pin is actually at, and answer a different question if
     * something external ever loaded it. */
    return nora_gpio_read_output(s_led_pins[led]) == NORA_GPIO_LEVEL_HIGH;
}

bool dm330030_sw_pressed(dm330030_sw_t sw)
{
    if ((unsigned)sw >= (unsigned)DM330030_SW_COUNT) {
        return false;
    }

    return nora_gpio_read(s_sw_pins[sw]) == NORA_GPIO_LEVEL_LOW;
}
