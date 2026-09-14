#ifndef NORA_TICK_TIMER_H
#define NORA_TICK_TIMER_H

/*
 * Minimal Timer1-based 1 ms monotonic tick source for dsPIC33CK.
 *
 * API shape follows dspic33ak_tick_timer.h. The interrupt vector remains
 * application-owned; route _T1Interrupt to nora_tick_timer_irq_handler()
 * when this HAL owns Timer1.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NORA_TICK_TIMER_OK = 0,
    NORA_TICK_TIMER_ERR_INVALID_ARG,
    NORA_TICK_TIMER_ERR_NOT_PRESENT,
    NORA_TICK_TIMER_ERR_NOT_INITIALIZED,
    NORA_TICK_TIMER_ERR_OUT_OF_RANGE,
    /*
     * `timer_clk_hz` does not divide exactly by 1000 at any available prescaler, so no
     * PR1 value gives a 1.000 ms period. Distinct from ERR_OUT_OF_RANGE, where the
     * period IS exact but the count does not fit in 16 bits.
     *
     * REFUSING IS THE POINT. The alternative -- rounding to the nearest count and
     * returning OK -- is what an earlier revision of this HAL did, and it is the worse
     * failure: every cadence in a consuming tree ends up expressed in milliseconds of
     * this tick, so a silently approximate tick makes every one of them wrong by the
     * same unstated factor, and the symptom is a timing drift nobody can attribute.
     * A refused init is reported once at bring-up by the profile that asked for it.
     */
    NORA_TICK_TIMER_ERR_INEXACT_PERIOD,
    /*
     * A well-formed request this silicon cannot honour. THIS BACKEND NEVER RETURNS IT --
     * dsPIC33CK's Timer1 can select either value of clock_source, so every well-formed
     * config is honourable here.
     *
     * It exists so that the status enum is one contract across the NORA families rather
     * than two: dsPIC33AK's Timer1 has no FRC input (its T1CON has no TECS at all), so
     * NORA_TICK_TIMER_CLOCK_FRC is refused there with this code, and the reasoning lives
     * at the same enumerator in the AK header. A caller that handles the portable status
     * set therefore compiles and switches identically on both parts; leaving this value
     * out on one side would only move the divergence rather than close it.
     */
    NORA_TICK_TIMER_ERR_NOT_SUPPORTED
} nora_tick_timer_status_t;

#define NORA_TICK_TIMER_DEFAULT_IRQ_PRIORITY   4u

typedef enum {
    NORA_TICK_TIMER_CLOCK_INTERNAL = 0, /* internal instruction cycle clock, i.e. Fcy */
    NORA_TICK_TIMER_CLOCK_FRC           /* the FRC oscillator, independent of Fcy     */
} nora_tick_timer_clock_source_t;

typedef struct {
    /* Actual input clock supplied to Timer1, in Hz. */
    uint32_t timer_clk_hz;

    /* Timer1 clock source. CK demo compatibility uses FRC. */
    nora_tick_timer_clock_source_t clock_source;

    /* CPU interrupt priority for the Timer1 tick. Valid range: 1..7. */
    uint8_t irq_priority;

    /* Keep Timer1 running while the CPU is in Idle mode. */
    bool run_in_idle;
} nora_tick_timer_config_t;

nora_tick_timer_status_t nora_tick_timer_init(
    const nora_tick_timer_config_t *config);

nora_tick_timer_status_t nora_tick_timer_deinit(void);

bool nora_tick_timer_is_present(void);

uint32_t nora_tick_timer_get_ms(void);

bool nora_tick_timer_is_initialized(void);

void nora_tick_timer_irq_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* NORA_TICK_TIMER_H */
