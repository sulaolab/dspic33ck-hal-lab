#ifndef NORA_GPIO_TABLE_H
#define NORA_GPIO_TABLE_H

/*
 * A board's fixed pins as a table, applied in one call.
 *
 * WHAT THIS IS FOR
 * ----------------
 * A board's user-I/O stage is a list: this pin an output starting low, that one an
 * input with a pull-up, this one analog. Spelled as straight-line code it becomes
 * one `if (!...) return false;` per pin -- eight of them on DM330030, two on
 * EV88G73A -- and the LIST, which is the actual board fact, is buried in the
 * control flow that walks it.
 *
 * As a table the fact is the data and the walk is shared. That is the same move the
 * app-layer seams (wm8904_audio_port_t, i2c_probe_t) already make one level up, and
 * the point of it here is that two boards' pin stages stop being two functions that
 * merely resemble each other and become two tables with one applier.
 *
 * A DELIBERATELY THIN INTERFACE
 * -----------------------------
 * No failure index is reported, only pass/fail. It was tempting -- a table makes
 * "which entry refused" nearly free to compute -- but the straight-line code it
 * replaces did not report it either, no caller has asked, and this links into a
 * 64 KB part with `remove-unused-sections` off, so unused reporting is not free.
 * Add it when something wants it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "nora_gpio.h"

typedef struct {
    nora_gpio_pin_t           pin;
    const nora_gpio_config_t *config;
} nora_gpio_table_entry_t;

/*
 * Apply every entry in order, stopping at the first refusal.
 *
 * ENTRIES ARE INDEPENDENT, and that is a property worth keeping. The walk is top to
 * bottom because it is a loop, but no board's table may rely on it: each entry states
 * one pin completely (direction, pull, analog, open-drain, initial level), so applying
 * them in any order gives the same result.
 *
 * This paragraph used to say the opposite -- "ORDER IS PART OF THE DATA", because
 * DM330030 cleared ANSEL across all ports at boot and then had to make its
 * potentiometer pin analog AGAIN, which only worked if that entry came last. That
 * sweep was deleted on 2026-08-03 (see the note in boards/dm330030/dm330030_board.c),
 * and with it the one reason a table here was order-sensitive. If a new board needs
 * pin B configured after pin A, that is a sign the pin descriptions are incomplete;
 * fix the description rather than relying on the sequence.
 */
bool nora_gpio_table_apply(const nora_gpio_table_entry_t *table,
                                uint8_t                             count);

/*
 * The four descriptions every board in this repo was writing out for itself.
 *
 * EV88G73A had `sw0_input_pullup` and DM330030 had `input_pullup`: the same five
 * fields, differing only in the name of the local. Sharing them is not just less
 * text -- a pull-up that is stated once cannot be stated inconsistently, and on
 * EV88G73A the pull-up is load-bearing (DS70005517B Sec.4.2.2: SW0 ties RD13 to GND
 * through the button and nothing else, so without it the pin floats when released).
 *
 * A board that needs something these do not describe still writes its own struct
 * and points an entry at it; nothing here is exhaustive.
 */
extern const nora_gpio_config_t nora_gpio_cfg_output_low;
extern const nora_gpio_config_t nora_gpio_cfg_output_high;
extern const nora_gpio_config_t nora_gpio_cfg_input_pullup;
/* Analog input with NO pull: a potentiometer is a divider driven from both rails,
 * and an internal pull would sit across one leg of it and skew the reading. */
extern const nora_gpio_config_t nora_gpio_cfg_analog_input;

#endif /* NORA_GPIO_TABLE_H */
