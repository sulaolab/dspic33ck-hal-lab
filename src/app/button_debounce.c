/*
 * button_debounce.c -- see the header for the algorithm and the ownership rules.
 *
 * No board-ownership guard: there is not a pin, a port or a board register in
 * here. What it reads and what it reports are both the caller's callbacks. That
 * is why it left boards/dm330030/main.c.
 */

#include "button_debounce.h"

#include <stddef.h>

void button_debounce_init(button_debounce_t *b, const button_debounce_config_t *cfg)
{
    if (b == NULL) {
        return;
    }

    b->cfg     = cfg;
    b->counter = 0u;
    b->pressed = false;
    b->raw     = false;
}

void button_debounce_poll(button_debounce_t *b)
{
    bool now;

    if ((b == NULL) || (b->cfg == NULL) || (b->cfg->read_pressed == NULL)) {
        return;
    }

    now    = b->cfg->read_pressed(b->cfg->id);
    b->raw = now;

    if (now) {
        /*
         * Accept the press only when the counter has fully decayed, i.e. the
         * button has been continuously released for the blanking interval. This
         * is what rejects both press chatter and release chatter with one
         * counter: chatter arrives well inside the interval.
         */
        if (b->counter == 0u) {
            b->pressed = true;
            if (b->cfg->on_press != NULL) {
                b->cfg->on_press(b->cfg->id);
            }
        }

        /* Reload on every pressed sample, so the interval is measured from the
         * LAST time the button was down, not from the first. A held button
         * therefore never re-arms. */
        b->counter = b->cfg->blanking_ticks;
    } else {
        if (b->counter != 0u) {
            b->counter--;
        } else {
            b->pressed = false;
        }
    }
}

bool button_debounce_pressed(const button_debounce_t *b)
{
    return ((b != NULL) && (b->cfg != NULL)) ? b->pressed : false;
}

bool button_debounce_raw(const button_debounce_t *b)
{
    return ((b != NULL) && (b->cfg != NULL)) ? b->raw : false;
}
