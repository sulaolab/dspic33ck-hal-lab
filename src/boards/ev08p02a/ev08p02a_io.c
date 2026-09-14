/*
 * ev08p02a_io.c -- see ev08p02a_io.h for the role split and for why there is no init().
 */

#ifndef DSPIC33CK_BOARD_EV08P02A
#error "boards/ev08p02a/ev08p02a_io.c is EV08P02A-owned. Build it only in the CK256MC005_EV08P02A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "ev08p02a_io.h"

/* Pin numbers come from here and nowhere else, so a wiring change is one file. */
#include "ev08p02a_pins.h"
#include "nora_gpio.h"

void ev08p02a_led0_set(bool on)
{
    /* Active-low: the pin is driven LOW to light the LED. */
    (void)nora_gpio_write(EV08P02A_LED0_PIN, !on);
}

void ev08p02a_led0_toggle(void)
{
    (void)nora_gpio_toggle(EV08P02A_LED0_PIN);
}

bool ev08p02a_sw0_pressed(void)
{
    /* Active-low (DS70005656A Sec.4.2.2): pressed pulls RD13 to GND. read(), not
     * read_output(): this is an input, so the question is the pin level. */
    return nora_gpio_read(EV08P02A_SW0_PIN) == NORA_GPIO_LEVEL_LOW;
}
