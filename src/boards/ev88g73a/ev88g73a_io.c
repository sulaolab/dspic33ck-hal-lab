/*
 * ev88g73a_io.c -- see ev88g73a_io.h for the role split and for why there is no init().
 */

#ifndef DSPIC33CK_BOARD_EV88G73A
#error "boards/ev88g73a/ev88g73a_io.c is EV88G73A-owned. Build it only in the CK64MC105_EV88G73A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "ev88g73a_io.h"

/* Pin numbers come from here and nowhere else, so a wiring change is one file. */
#include "ev88g73a_pins.h"
#include "nora_gpio.h"

void ev88g73a_led0_set(bool on)
{
    /* Active-low: the pin is driven LOW to light the LED. */
    (void)nora_gpio_write(EV88G73A_LED0_PIN, !on);
}

void ev88g73a_led0_toggle(void)
{
    (void)nora_gpio_toggle(EV88G73A_LED0_PIN);
}

bool ev88g73a_sw0_pressed(void)
{
    /* Active-low (DS70005517B Sec.4.2.2): pressed pulls RD13 to GND. read(), not
     * read_output(): this is an input, so the question is the pin level. */
    return nora_gpio_read(EV88G73A_SW0_PIN) == NORA_GPIO_LEVEL_LOW;
}
