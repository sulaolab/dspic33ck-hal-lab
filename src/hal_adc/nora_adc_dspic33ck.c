#include "nora_adc.h"

#include <stddef.h>

#include <xc.h>

#include "nora_adc_dspic33ck_reg.h"

#define DSPIC33CK_ADC_DEFAULT_READY_TIMEOUT       (1000000UL)

const char *nora_adc_result_str(nora_adc_result_t result)
{
    switch (result) {
    case NORA_ADC_RESULT_OK:              return "OK";
    case NORA_ADC_RESULT_ERROR:           return "ERROR";
    case NORA_ADC_RESULT_INVALID_ARG:     return "INVALID_ARG";
    case NORA_ADC_RESULT_UNSUPPORTED:     return "UNSUPPORTED";
    case NORA_ADC_RESULT_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case NORA_ADC_RESULT_BUSY:            return "BUSY";
    case NORA_ADC_RESULT_TIMEOUT:         return "TIMEOUT";
    default:                                   return "?";
    }
}

static uint32_t adc_timeout_or_default(uint32_t timeout_count)
{
    return (timeout_count == 0u) ? DSPIC33CK_ADC_DEFAULT_READY_TIMEOUT : timeout_count;
}

static bool adc_sample_time_tad_to_samc(uint8_t sample_time_tad, uint16_t *samc)
{
    if ((samc == NULL) || (sample_time_tad < DSPIC33CK_ADC_SHARED_SAMPLE_TIME_MIN_TAD)) {
        return false;
    }

    /* SHRSAMC register value 0 represents two TADCORE cycles. */
    *samc = (uint16_t)sample_time_tad - DSPIC33CK_ADC_SHARED_SAMPLE_TIME_MIN_TAD;
    return true;
}

static bool adc_wait_for_bit(volatile uint16_t *reg, uint16_t mask, uint32_t timeout_count)
{
    uint32_t remaining = timeout_count;

    while ((*reg & mask) == 0u) {
        if (remaining == 0u) {
            return false;
        }
        remaining--;
    }

    return true;
}

static bool adc_channel_is_ready(uint8_t channel)
{
    const uint16_t mask = dspic33ck_adc_reg_ready_mask(channel);

    if (channel < 16u) {
        return (ADSTATL & mask) != 0u;
    }

    return (ADSTATH & mask) != 0u;
}

static bool adc_handle_channel_is_valid(const nora_adc_handle_t *handle, uint8_t channel)
{
    return (handle != NULL) && handle->initialized &&
           dspic33ck_adc_reg_channel_is_valid(channel) && (channel == handle->channel);
}

static void adc_enter_timeout_fault(nora_adc_handle_t *handle)
{
    /* Stop an in-flight conversion rather than allowing its result to leak into a later one. */
    ADCON1L = 0u;
    handle->initialized = false;
    handle->busy = false;
    handle->last_result = NORA_ADC_RESULT_TIMEOUT;
}

bool nora_adc_init(nora_adc_handle_t *handle, const nora_adc_config_t *config)
{
    uint32_t ready_timeout;
    uint16_t shared_samc;

    if ((handle == NULL) || (config == NULL)) {
        return false;
    }

    handle->instance = config->instance;
    handle->channel = config->channel;
    handle->initialized = false;
    handle->busy = false;
    handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;

    if ((config->instance != NORA_ADC_INSTANCE_1) ||
        !dspic33ck_adc_reg_channel_is_valid(config->channel) ||
        (config->positive_input != config->channel) ||
        !adc_sample_time_tad_to_samc(config->sample_time_tad, &shared_samc)) {
        handle->last_result = NORA_ADC_RESULT_INVALID_ARG;
        return false;
    }

    /* CK's shared-core path has no CALREQ/CALRDY equivalent. */
    if (config->calibrate) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return false;
    }

    ready_timeout = adc_timeout_or_default(config->ready_timeout_count);

    ADCON1L = 0u;
    ADCON1H = DSPIC33CK_ADC_ADCON1H_12BIT_INTEGER;
    ADCON2L = DSPIC33CK_ADC_ADCON2L_TADCORE_DIV6;
    ADCON2H = shared_samc & DSPIC33CK_ADC_ADCON2H_SAMC_MASK;
    ADCON3L = 0u;
    ADCON3H = DSPIC33CK_ADC_ADCON3H_SHARED_CORE;
    ADCON5L = DSPIC33CK_ADC_ADCON5L_SHRPWR;
    ADCON5H = DSPIC33CK_ADC_ADCON5H_SHARED_WARMUP;

    ADCON1L |= DSPIC33CK_ADC_ADCON1L_ADON;
    if (!adc_wait_for_bit(&ADCON5L, DSPIC33CK_ADC_ADCON5L_SHRRDY, ready_timeout)) {
        adc_enter_timeout_fault(handle);
        return false;
    }

    handle->initialized = true;
    handle->last_result = NORA_ADC_RESULT_OK;
    return true;
}

void nora_adc_deinit(nora_adc_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }

    if (handle->instance == NORA_ADC_INSTANCE_1) {
        ADCON1L = 0u;
        ADCON1H = 0u;
        ADCON2L = 0u;
        ADCON2H = 0u;
        ADCON3L = 0u;
        ADCON3H = 0u;
        ADCON5L = 0u;
        ADCON5H = 0u;
    }

    handle->initialized = false;
    handle->busy = false;
    handle->last_result = NORA_ADC_RESULT_OK;
}

nora_adc_result_t nora_adc_trigger(nora_adc_handle_t *handle, uint8_t channel)
{
    if (handle == NULL) {
        return NORA_ADC_RESULT_INVALID_ARG;
    }
    if (!handle->initialized) {
        handle->last_result = NORA_ADC_RESULT_NOT_INITIALIZED;
        return handle->last_result;
    }
    if (!adc_handle_channel_is_valid(handle, channel)) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return handle->last_result;
    }
    if (handle->busy) {
        handle->last_result = NORA_ADC_RESULT_BUSY;
        return handle->last_result;
    }

    ADCON3L = (ADCON3L & (uint16_t)~DSPIC33CK_ADC_ADCON3L_CNVCHSEL_MASK) |
              ((uint16_t)channel & DSPIC33CK_ADC_ADCON3L_CNVCHSEL_MASK);
    ADCON3L |= DSPIC33CK_ADC_ADCON3L_CNVRTCH;

    handle->busy = true;
    handle->last_result = NORA_ADC_RESULT_OK;
    return handle->last_result;
}

bool nora_adc_is_conversion_complete(const nora_adc_handle_t *handle, uint8_t channel)
{
    if (!adc_handle_channel_is_valid(handle, channel) || !handle->busy) {
        return false;
    }

    return adc_channel_is_ready(channel);
}

nora_adc_result_t nora_adc_get_result(
    nora_adc_handle_t *handle,
    uint8_t channel,
    uint32_t *result)
{
    volatile uint16_t *result_register;

    if ((handle == NULL) || (result == NULL)) {
        return NORA_ADC_RESULT_INVALID_ARG;
    }
    if (!handle->initialized) {
        handle->last_result = NORA_ADC_RESULT_NOT_INITIALIZED;
        return handle->last_result;
    }
    if (!adc_handle_channel_is_valid(handle, channel)) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return handle->last_result;
    }
    if (!handle->busy) {
        handle->last_result = NORA_ADC_RESULT_INVALID_ARG;
        return handle->last_result;
    }
    if (!adc_channel_is_ready(channel)) {
        handle->last_result = NORA_ADC_RESULT_BUSY;
        return handle->last_result;
    }

    result_register = (&ADCBUF0) + channel;
    *result = (uint32_t)(*result_register & DSPIC33CK_ADC_DATA_MASK_12BIT);
    handle->busy = false;
    handle->last_result = NORA_ADC_RESULT_OK;
    return handle->last_result;
}

nora_adc_result_t nora_adc_read_blocking(
    nora_adc_handle_t *handle,
    uint8_t channel,
    uint32_t *result,
    uint32_t timeout_count)
{
    nora_adc_result_t status;
    uint32_t remaining = adc_timeout_or_default(timeout_count);

    status = nora_adc_trigger(handle, channel);
    if (status != NORA_ADC_RESULT_OK) {
        return status;
    }

    while (!nora_adc_is_conversion_complete(handle, channel)) {
        if (remaining == 0u) {
            adc_enter_timeout_fault(handle);
            return handle->last_result;
        }
        remaining--;
    }

    return nora_adc_get_result(handle, channel, result);
}

nora_adc_status_t nora_adc_get_status(const nora_adc_handle_t *handle)
{
    nora_adc_status_t status = {
        .initialized = false,
        .busy = false,
        .last_result = NORA_ADC_RESULT_INVALID_ARG,
    };

    if (handle != NULL) {
        status.initialized = handle->initialized;
        status.busy = handle->busy;
        status.last_result = handle->last_result;
    }

    return status;
}

nora_adc_result_t nora_adc_get_last_result(const nora_adc_handle_t *handle)
{
    if (handle == NULL) {
        return NORA_ADC_RESULT_INVALID_ARG;
    }

    return handle->last_result;
}

void nora_adc_clear_error(nora_adc_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }

    /* A timeout is a hardware fault: clear_error() never substitutes for re-init. */
    if (handle->initialized && (handle->last_result != NORA_ADC_RESULT_OK)) {
        handle->last_result = NORA_ADC_RESULT_OK;
    }
}
