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
