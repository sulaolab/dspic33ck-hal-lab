#ifndef NORA_ADC_H
#define NORA_ADC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Small dsPIC33CK ADC peripheral HAL.
 *
 * This public surface deliberately follows the dsPIC33AK ADC HAL. The two
 * peripherals use different register models, but their first supported use is
 * identical: software-trigger one single-ended input and poll for a 12-bit
 * result.
 *
 * First-phase scope:
 *   - the CK ADC1/shared-core peripheral;
 *   - polling / software-triggered conversions on AN0..AN31;
 *   - the four hardware averaging/oversampling filters, still polled and still
 *     software-triggered -- see nora_adc_filter_configure() below;
 *   - no board pin ownership, DMA ownership, or interrupt ownership.
 *
 * CK selects an AN number directly instead of an AK-style ADC-module channel
 * plus positive-input mux. To retain the common configuration shape,
 * positive_input must equal channel for this single-ended CK implementation.
 * sample_time_tad is the requested shared-core sample duration in TADCORE
 * cycles. CK encodes that duration as SHRSAMC + 2, so valid first-phase values
 * are 2..255 (the public field intentionally retains the Sonora API shape).
 */

typedef enum {
    NORA_ADC_INSTANCE_1 = 1,
} nora_adc_instance_t;

typedef enum {
    NORA_ADC_RESULT_OK = 0,
    NORA_ADC_RESULT_ERROR,
    NORA_ADC_RESULT_INVALID_ARG,
    NORA_ADC_RESULT_UNSUPPORTED,
    NORA_ADC_RESULT_NOT_INITIALIZED,
    NORA_ADC_RESULT_BUSY,
    NORA_ADC_RESULT_TIMEOUT,
} nora_adc_result_t;

typedef struct {
    nora_adc_instance_t instance;
    uint8_t                  channel;
    uint8_t                  positive_input;
    uint8_t                  sample_time_tad;
    bool                     calibrate;
    uint32_t                 ready_timeout_count;
    uint32_t                 calibration_timeout_count;
} nora_adc_config_t;

typedef struct {
    nora_adc_instance_t instance;
    uint8_t                  channel;
    bool                     initialized;
    bool                     busy;
    nora_adc_result_t   last_result;
    /* Filter state -- private to the implementation, see nora_adc_filter_configure(). */
    uint8_t                  filter;
    uint8_t                  filter_ratio_log2;
    uint8_t                  filter_fractional_bits;
    bool                     filter_active;
} nora_adc_handle_t;

typedef struct {
    bool                   initialized;
    bool                   busy;
    nora_adc_result_t last_result;
} nora_adc_status_t;

bool nora_adc_init(nora_adc_handle_t *handle, const nora_adc_config_t *config);
void nora_adc_deinit(nora_adc_handle_t *handle);

nora_adc_result_t nora_adc_trigger(nora_adc_handle_t *handle, uint8_t channel);
bool nora_adc_is_conversion_complete(const nora_adc_handle_t *handle, uint8_t channel);
nora_adc_result_t nora_adc_get_result(
    nora_adc_handle_t *handle,
    uint8_t channel,
    uint32_t *result);

nora_adc_result_t nora_adc_read_blocking(
    nora_adc_handle_t *handle,
    uint8_t channel,
    uint32_t *result,
    uint32_t timeout_count);

/*
 * HARDWARE AVERAGING / OVERSAMPLING.
 *
 * The CK ADC has four filters that accumulate conversions of one selected input and hand
 * back a single quietened result. They are what a hand-turned pot wants: a wiper on a
 * jumper wire measured 165 counts of peak-to-peak jitter on EV08P02A, against a 24-count
 * deadband, and averaging in hardware is the fix at the source rather than a deadband wide
 * enough to swallow it (which would throw the knob's resolution away to hide the noise).
 *
 * ALLOCATION IS THE CALLER'S, NOT THIS HAL'S. There are only four filters and this driver
 * has no way to know what else on the board wants one, so the caller names the filter it
 * has decided to spend, the same way it names the AN number. What the HAL does instead is
 * refuse a
 * second claim on a filter that is already configured -- so a collision is an
 * NORA_ADC_RESULT_BUSY at bring-up, not two subsystems quietly corrupting one result.
 * Release it with nora_adc_filter_release() (nora_adc_deinit() also releases).
 *
 * Two modes, and they answer in different units:
 *   - AVERAGING returns a plain 12-bit integer, the same units the unfiltered path uses.
 *     A caller can switch to it without rescaling anything.
 *   - OVERSAMPLING returns a WIDER result -- 12 integer bits plus 1..4 fractional bits --
 *     so it buys resolution as well as quiet, at the cost of every downstream constant
 *     changing meaning. nora_adc_filter_fractional_bits() reports the shift; it is 0 in
 *     averaging mode.
 *
 * ratio_log2 is the log2 of the sample count: 1 = 2x through 8 = 256x. It is spelled as
 * an exponent rather than as a ratio because the two modes encode the same set of ratios
 * with different, non-monotonic bit patterns, and one exponent keeps the caller out of
 * that.
 *
 * nora_adc_filter_read_blocking() drives the conversions itself -- the filter is fed by
 * ordinary conversions of its channel, so a filtered read is ratio_log2 worth of them
 * back to back, then one register read. At the default sample time that is about 3.4 us
 * per conversion, so 32x costs roughly 110 us. timeout_count bounds each individual
 * conversion, as it does for the unfiltered call.
 */
typedef enum {
    NORA_ADC_FILTER_MODE_AVERAGING = 0,
    NORA_ADC_FILTER_MODE_OVERSAMPLING,
} nora_adc_filter_mode_t;

/* No channel field: the filter follows the handle's channel, because a handle is already
 * bound to exactly one (nora_adc_trigger() refuses any other). A second channel needs a
 * second handle, which is how the band gap is read beside a pot today. */
typedef struct {
    uint8_t                filter;      /* 0..3 */
    uint8_t                ratio_log2;  /* 1..8 == 2x..256x */
    nora_adc_filter_mode_t mode;
} nora_adc_filter_config_t;

nora_adc_result_t nora_adc_filter_configure(
    nora_adc_handle_t *handle,
    const nora_adc_filter_config_t *config);

nora_adc_result_t nora_adc_filter_release(nora_adc_handle_t *handle);

nora_adc_result_t nora_adc_filter_read_blocking(
    nora_adc_handle_t *handle,
    uint32_t *result,
    uint32_t timeout_count);

/* Fractional bits in the value nora_adc_filter_read_blocking() returns: 0 in averaging
 * mode, 1..4 in oversampling mode, and 0 when no filter is configured. */
uint8_t nora_adc_filter_fractional_bits(const nora_adc_handle_t *handle);

nora_adc_status_t nora_adc_get_status(const nora_adc_handle_t *handle);
nora_adc_result_t nora_adc_get_last_result(const nora_adc_handle_t *handle);
void nora_adc_clear_error(nora_adc_handle_t *handle);

/*
 * The enum as text, for callers that report a failure instead of swallowing it.
 *
 * Beside the enum on purpose, the same way hal_i2c/nora_i2c_status_str() is:
 * every caller reporting an ADC result wants the same words, and a switch next to the
 * enum stops compiling silently when a result is added, which a caller's private copy
 * does not. Never NULL.
 */
const char *nora_adc_result_str(nora_adc_result_t result);

#endif /* NORA_ADC_H */
