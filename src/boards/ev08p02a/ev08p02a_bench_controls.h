#ifndef EV08P02A_BENCH_CONTROLS_H
#define EV08P02A_BENCH_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { EV08P02A_BUTTON_NONE, EV08P02A_BUTTON_SHORT,
               EV08P02A_BUTTON_LONG } ev08p02a_button_action_t;
typedef struct {
    bool raw, stable, long_sent;
    uint32_t raw_ms, press_ms;
} ev08p02a_button_state_t;

/* 15 ms debounce, one 1500 ms hold action, no short action after long release. */
static inline ev08p02a_button_action_t ev08p02a_button_update(
    ev08p02a_button_state_t *s, bool raw, uint32_t now)
{
    if (raw != s->raw) { s->raw = raw; s->raw_ms = now; }
    if (raw != s->stable && (uint32_t)(now - s->raw_ms) >= 15u) {
        s->stable = raw;
        if (raw) { s->press_ms = now; s->long_sent = false; }
        else if (!s->long_sent) return EV08P02A_BUTTON_SHORT;
    }
    if (s->stable && raw && !s->long_sent &&
        (uint32_t)(now - s->press_ms) >= 1500u) {
        s->long_sent = true;
        return EV08P02A_BUTTON_LONG;
    }
    return EV08P02A_BUTTON_NONE;
}

typedef struct { bool seeded, muted; uint32_t mode_ms; } ev08p02a_led_state_t;

static inline bool ev08p02a_led_update(ev08p02a_led_state_t *s,
                                      bool muted, uint32_t now)
{
    if (!s->seeded || s->muted != muted) {
        s->seeded = true; s->muted = muted; s->mode_ms = now;
    }
    return ((uint32_t)(now - s->mode_ms) % 1000u) < (muted ? 100u : 500u);
}
#endif
