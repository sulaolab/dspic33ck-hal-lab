#ifndef NORA_HIGH_RES_TIMER_H
#define NORA_HIGH_RES_TIMER_H

/*
 * nora_high_res_timer.h
 * --------------------------
 * CK sibling of dspic33ak_high_res_timer: a free-running high-resolution counter
 * for profiling and short interval measurement (used by the SPI/I2S/TDM block-ISR
 * load monitor). Built on SCCP1 in 32-bit timer mode, clocked at Fcy with 1:1
 * prescale, to keep AK's 32-bit effectively-non-wrapping semantics (~42.9 s at
 * Fcy 100 MHz).
 *
 * NOT Timer2/3: this file used to claim "Timer1 is owned by the 1 ms tick;
 * Timer2/3 are free", which is false for this family -- dsPIC33CK replaced
 * Timer2-Timer5 with SCCP/MCCP, so neither CK256MP508 nor CK64MC105 has T2CON at
 * all, and the HAL silently compiled out to "not present" on both. See the .c
 * file for the details.
 *
 * The API matches the AK HAL 1:1 so diag.c stays unchanged.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NORA_HIGH_RES_TIMER_OK = 0,
    NORA_HIGH_RES_TIMER_ERR_INVALID_ARG,
    NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT,
    NORA_HIGH_RES_TIMER_ERR_NOT_INITIALIZED
} nora_high_res_timer_status_t;

typedef struct {
    /* Actual input clock supplied to the timer, in Hz. Fcy with this backend,
     * which selects the peripheral clock (SCCP1 CLKSEL = 0b000) at 1:1. */
    uint32_t timer_clk_hz;
    /* Keep the counter running while the CPU is in Idle mode. */
    bool run_in_idle;
} nora_high_res_timer_config_t;

/* Configure and start the family's 32-bit time base as a free-running counter --
 * SCCP1 in 32-bit mode here, the natively 32-bit Timer2 on dsPIC33AK (see the
 * file header). On success this HAL owns that peripheral until deinit(). */
nora_high_res_timer_status_t nora_high_res_timer_init(
    const nora_high_res_timer_config_t *config);

nora_high_res_timer_status_t nora_high_res_timer_deinit(void);

bool nora_high_res_timer_is_present(void);
bool nora_high_res_timer_is_initialized(void);

/* Raw 32-bit counter value (one count = one timer input-clock period). 0 when the
 * timer is absent or the HAL is not initialized -- indistinguishable from a valid
 * count of 0, so use is_present()/is_initialized() to tell them apart. */
uint32_t nora_high_res_timer_get_count(void);

/* Wraparound-safe while the measured interval is shorter than one 32-bit cycle,
 * i.e. 2^32 / timer_clk_hz -- ~42.9 s at Fcy = 100 MHz. */
uint32_t nora_high_res_timer_elapsed_count(uint32_t start_count);

/* Convert raw counts to integer microseconds (truncated, saturates to UINT32_MAX). */
uint32_t nora_high_res_timer_count_to_us(uint32_t count);

/* Convert raw counts to 0.1 us units (e.g. 1234 == 123.4 us; 64-bit division). */
uint32_t nora_high_res_timer_count_to_us_x10(uint32_t count);

uint32_t nora_high_res_timer_elapsed_us(uint32_t start_count);
uint32_t nora_high_res_timer_elapsed_us_x10(uint32_t start_count);

/*
 * The divisor every count_to_*() uses, and a way to CORRECT it against a better
 * reference than the one init() was handed.
 *
 * init() is told the clock the caller BELIEVES is feeding the timer, and on this
 * board that belief is Fcy computed from the FRC's nominal 8 MHz. The FRC is not
 * a reference: measured against the audio clock, the true Fcy here is about
 * 0.59 % high, so every absolute microsecond figure printed from this timer is
 * 0.59 % long. Percentages are unaffected -- they are a ratio of two numbers
 * scaled by the same wrong divisor, which is why the load display was never
 * wrong, only its `last`/`deadline`/`margin` microseconds.
 *
 * A caller that has a genuine frequency reference can therefore hand back a
 * measured clock. The counter itself is NOT touched: only the divisor changes, so
 * intervals already timed stay comparable and nothing is discontinuous except the
 * scale factor. See dsp_load_set_block_period_ref_us_x100(), which does this with the
 * codec crystal as the reference.
 *
 * Refuses 0 (that is the "unknown clock" sentinel) and refuses to run before
 * init(). Deliberately no band check here: this HAL cannot know what is
 * plausible, and the caller that owns the reference is the one that can.
 */
uint32_t nora_high_res_timer_clk_hz(void);
nora_high_res_timer_status_t nora_high_res_timer_set_clk_hz(uint32_t clk_hz);

#ifdef __cplusplus
}
#endif

#endif /* NORA_HIGH_RES_TIMER_H */
