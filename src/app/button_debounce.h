#ifndef BUTTON_DEBOUNCE_H
#define BUTTON_DEBOUNCE_H

/*
 * button_debounce.h -- blanking-interval debounce for a mechanical push-button.
 *
 * WHAT IT IS
 * ----------
 * One instance per button. Sampled from a periodic tick (1 ms on this repo's
 * boards), it turns a raw, chattering pin read into:
 *
 *   - a press EVENT, delivered once per accepted press through on_press()
 *   - a HELD state (button_debounce_pressed()), true from the accepted press
 *     until the button has been continuously released for the blanking interval
 *   - the RAW last sample (button_debounce_raw()), for callers that want to
 *     mirror the button onto an LED without reading the pin a second time
 *
 * The algorithm is a re-arm countdown, not a shift-register match: while the
 * button reads pressed the counter is reloaded, and it only decays once the
 * button reads released. So a press is accepted immediately (no latency) and the
 * NEXT one cannot be accepted until the button has been quiet for the interval.
 * That is what rejects contact chatter on both edges with one counter.
 *
 * WAS boards/dm330030/main.c's ButtonDebounce()
 * ---------------------------------------------
 * Measured before moving it: no pin, no port, no board register -- it read
 * dm330030_sw_pressed() and drove LEDs through dm330030_led_set(), i.e. board APIs, and the
 * only board fact was WHICH button. What is here is the timing logic; which
 * button, and what a press means, stayed with the caller.
 *
 * The original interleaved three things in one function: the countdown, the
 * LED-follows-button mirror, and the colour-channel advance. Only the countdown
 * is generic, so only the countdown moved.
 *
 * ISR/FOREGROUND OWNERSHIP
 * ------------------------
 * button_debounce_poll() is expected to run in the tick ISR; the accessors are
 * expected to run in the foreground. `pressed` and `raw` are therefore volatile
 * bool -- single-byte, so a read cannot straddle a write on this core. Nothing
 * here is a 32-bit publication, so no seqlock is needed (contrast dsp_load.c,
 * which publishes 32-bit counters and does need one).
 *
 * on_press() is called FROM the poll, i.e. in the ISR. Keep it short, and treat
 * anything it writes for the foreground as needing the same volatile treatment.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /*
     * Read the button, active level already resolved: true means pressed. `id` is
     * passed through from the config below so one function can serve several
     * buttons instead of needing a wrapper each.
     */
    bool (*read_pressed)(uint16_t id);

    /*
     * Called once per accepted press, in the tick ISR. NULL for a button whose
     * held state is all the caller wants.
     */
    void (*on_press)(uint16_t id);

    /* Passed to both callbacks. Typically the caller's button index. */
    uint16_t id;

    /*
     * Blanking interval in POLL TICKS, not milliseconds -- this module does not
     * know the tick rate. At the 1 ms tick both boards use, ticks == ms.
     *
     * 0 means no blanking: every tick that reads pressed while the previous read
     * was released fires on_press(). Legal, and occasionally what a lab test
     * wants, but it is not a debounce.
     */
    uint16_t blanking_ticks;
} button_debounce_config_t;

typedef struct {
    const button_debounce_config_t *cfg;

    uint16_t      counter;   /* poll-owned */
    volatile bool pressed;   /* held state, published to the foreground */
    volatile bool raw;       /* last raw sample, published to the foreground */
} button_debounce_t;

/*
 * `cfg` must outlive the instance -- it is stored by pointer, not copied, so a
 * static const config costs no RAM. A NULL cfg leaves the instance inert
 * (poll does nothing, both accessors return false) rather than trapping: this is
 * lab firmware, and a dead button beats a dead board.
 */
void button_debounce_init(button_debounce_t *b, const button_debounce_config_t *cfg);

/* Sample once. Call from the periodic tick, at the rate blanking_ticks counts. */
void button_debounce_poll(button_debounce_t *b);

/* Debounced held state: set on an accepted press, cleared once the button has
 * read released for blanking_ticks consecutive polls. */
bool button_debounce_pressed(const button_debounce_t *b);

/* The last raw sample, undebounced. For an LED that should follow the button
 * exactly, which is a different question from "has a press been accepted". */
bool button_debounce_raw(const button_debounce_t *b);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_DEBOUNCE_H */
