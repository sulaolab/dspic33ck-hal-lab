/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef DSPIC33CK_BOARD_EV97U97A
#error "boards/ev97u97a/ev97u97a_io.c is EV97U97A-owned. Build it only in the CK1024MP710_EV97U97A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "ev97u97a_io.h"

#include <stddef.h>

#include "ev97u97a_pins.h"
#include "nora_gpio.h"

static const nora_gpio_pin_t s_led_pins[EV97U97A_LED_COUNT] = {
    EV97U97A_LED1_PIN,
    EV97U97A_LED2_PIN
};

static const nora_gpio_pin_t s_sw_pins[EV97U97A_SW_COUNT] = {
    EV97U97A_SW1_PIN,
    EV97U97A_SW2_PIN,
    EV97U97A_SW3_PIN
};

void ev97u97a_led_set(ev97u97a_led_t led, bool on)
{
    if ((unsigned)led >= (unsigned)EV97U97A_LED_COUNT) {
        return;
    }

    (void)nora_gpio_write(s_led_pins[led], on);
}

void ev97u97a_led_toggle(ev97u97a_led_t led)
{
    if ((unsigned)led >= (unsigned)EV97U97A_LED_COUNT) {
        return;
    }

    (void)nora_gpio_toggle(s_led_pins[led]);
}

bool ev97u97a_led_get(ev97u97a_led_t led)
{
    if ((unsigned)led >= (unsigned)EV97U97A_LED_COUNT) {
        return false;
    }

    return nora_gpio_read_output(s_led_pins[led]) == NORA_GPIO_LEVEL_HIGH;
}

bool ev97u97a_sw_pressed(ev97u97a_sw_t sw)
{
    if ((unsigned)sw >= (unsigned)EV97U97A_SW_COUNT) {
        return false;
    }

    return nora_gpio_read(s_sw_pins[sw]) == NORA_GPIO_LEVEL_LOW;
}
