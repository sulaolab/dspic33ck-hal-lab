/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef EV97U97A_IO_H
#define EV97U97A_IO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV97U97A_LED_1 = 0,
    EV97U97A_LED_2,
    EV97U97A_LED_COUNT
} ev97u97a_led_t;

typedef enum {
    EV97U97A_SW_1 = 0,
    EV97U97A_SW_2,
    EV97U97A_SW_3,
    EV97U97A_SW_COUNT
} ev97u97a_sw_t;

void ev97u97a_led_set(ev97u97a_led_t led, bool on);
void ev97u97a_led_toggle(ev97u97a_led_t led);
bool ev97u97a_led_get(ev97u97a_led_t led);

bool ev97u97a_sw_pressed(ev97u97a_sw_t sw);

#ifdef __cplusplus
}
#endif

#endif
