#include "nora_adc.h"

#include <stddef.h>

#include <xc.h>

#include "nora_adc_dspic33ck_reg.h"

#define DSPIC33CK_ADC_DEFAULT_READY_TIMEOUT       (1000000UL)

/*
 * Slack on the conversion count a filtered read is allowed to spend before it calls the
 * filter dead. The normal case is exactly the ratio -- one conversion in, one sample
 * accumulated -- so this is insurance against a filter that never raises RDY, not a
 * tuned budget. Small on purpose: a wrong OVRSAM encoding would need far more than eight
 * extra conversions to look healthy, so a genuine encoding mistake still fails loudly.
 */
#define DSPIC33CK_ADC_FILTER_CONVERSION_SLACK     (8u)

/*
 * WHICH FILTERS ARE SPENT. Handles are caller-owned structs, so nothing but a
 * module-static record can answer "is filter 2 already someone else's?" -- and answering
 * it is the whole of what this HAL does about a four-way shared resource (see the header:
 * the caller allocates, this driver only refuses a collision).
 */
static uint8_t s_filter_claimed;

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

/*
 * ADFL0CON..ADFL3CON and their DAT registers, reached by a switch on the filter index.
 *
 * NOT as pointers, and not as ADFL0CON + index. The obvious shape here is a helper
 * returning `volatile uint16_t *` -- but xc-dsc v3.31.01 hits an internal compiler error
 * ("in gen_movhi_16, at insn-emit.c") on a STORE through such a pointer once it has been
 * folded to an absolute SFR address, reported against the closing brace of the enclosing
 * function rather than the offending line. Reads through a pointer are fine (that is how
 * adc_wait_for_bit() and the ADCBUF read work); it is the write that breaks. So writes go
 * through a switch of plain SFR assignments, which is what every other register write in
 * this file already is.
 *
 * Four entries also do not need address arithmetic: the headers give these as separate
 * __sfr__ symbols with no documented stride, so assuming one would be an assumption for
 * nothing.
 */
static void adc_filter_con_write(uint8_t filter, uint16_t value)
{
    switch (filter) {
    case 0u:  ADFL0CON = value; break;
    case 1u:  ADFL1CON = value; break;
    case 2u:  ADFL2CON = value; break;
    case 3u:  ADFL3CON = value; break;
    default:                    break;
    }
}

static uint16_t adc_filter_con_read(uint8_t filter)
{
    switch (filter) {
    case 0u:  return ADFL0CON;
    case 1u:  return ADFL1CON;
    case 2u:  return ADFL2CON;
    case 3u:  return ADFL3CON;
    default:  return 0u;
    }
}

static uint16_t adc_filter_dat_read(uint8_t filter)
{
    switch (filter) {
    case 0u:  return ADFL0DAT;
    case 1u:  return ADFL1DAT;
    case 2u:  return ADFL2DAT;
    case 3u:  return ADFL3DAT;
    default:  return 0u;
    }
}

static void adc_filter_drop(nora_adc_handle_t *handle)
{
    if (!handle->filter_active) {
        return;
    }

    /* FLEN = 0 also clears RDY, so nothing stale is left to be read as a result. */
    adc_filter_con_write(handle->filter, 0u);

    s_filter_claimed &= (uint8_t)~(1u << handle->filter);
    handle->filter_active = false;
    handle->filter_fractional_bits = 0u;
}

static void adc_enter_timeout_fault(nora_adc_handle_t *handle)
{
    /* Stop an in-flight conversion rather than allowing its result to leak into a later one. */
    ADCON1L = 0u;
    /* The ADC is now off, so the filter's claim would outlive the thing that feeds it. */
    adc_filter_drop(handle);
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

    /*
     * Before any early return: a re-init must not leave the previous run's filter claimed
     * against a peripheral that is about to be reprogrammed. Guarded by initialized
     * because a first init is handed a handle whose fields mean nothing yet -- the same
     * assumption every other entry point here already makes.
     */
    if (handle->initialized) {
        adc_filter_drop(handle);
    }

    handle->instance = config->instance;
    handle->channel = config->channel;
    handle->initialized = false;
    handle->busy = false;
    handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
    handle->filter = 0u;
    handle->filter_ratio_log2 = 0u;
    handle->filter_fractional_bits = 0u;
    handle->filter_active = false;

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
#if defined(__dsPIC33CK1024MP710__)
    /* DS70005496D ADC: AN0 is dedicated core 0, not the shared core.
     * CLKSEL=0 (Fcy), ADCS=3 (/6), 12-bit unsigned; SAMC delay enabled.
     * Power -> ADON -> ready -> digital enable. No ADC interrupt ownership. */
    ADCON3H = 0u;
    ADCON4Hbits.C0CHS = 0u;
    ADMOD0Lbits.SIGN0 = 0u;
    ADMOD0Lbits.DIFF0 = 0u;
    ADCORE0Lbits.SAMC = shared_samc;
    ADCORE0H = 0u;
    ADCORE0Hbits.ADCS = 3u;
    ADCORE0Hbits.RES = 3u;
    ADCON4Lbits.SAMC0EN = 1u;
    ADTRIG0Lbits.TRGSRC0 = 0u; /* Individual CNVRTCH trigger. */
    ADCON5L = _ADCON5L_C0PWR_MASK;
    ADCON5H = DSPIC33CK_ADC_ADCON5H_SHARED_WARMUP;
    ADCON1L |= DSPIC33CK_ADC_ADCON1L_ADON;
    if (!adc_wait_for_bit(&ADCON5L, _ADCON5L_C0RDY_MASK, ready_timeout)) {
        adc_enter_timeout_fault(handle);
        return false;
    }
    ADCON3Hbits.C0EN = 1u;
#else
    ADCON3H = DSPIC33CK_ADC_ADCON3H_SHARED_CORE;
    ADCON5L = DSPIC33CK_ADC_ADCON5L_SHRPWR;
    ADCON5H = DSPIC33CK_ADC_ADCON5H_SHARED_WARMUP;

    ADCON1L |= DSPIC33CK_ADC_ADCON1L_ADON;
    if (!adc_wait_for_bit(&ADCON5L, DSPIC33CK_ADC_ADCON5L_SHRRDY, ready_timeout)) {
        adc_enter_timeout_fault(handle);
        return false;
    }

#endif

    handle->initialized = true;
    handle->last_result = NORA_ADC_RESULT_OK;
    return true;
}

void nora_adc_deinit(nora_adc_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }

    adc_filter_drop(handle);

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

nora_adc_result_t nora_adc_filter_configure(
    nora_adc_handle_t *handle,
    const nora_adc_filter_config_t *config)
{
    uint16_t value;

    if ((handle == NULL) || (config == NULL)) {
        return NORA_ADC_RESULT_INVALID_ARG;
    }
    if (!handle->initialized) {
        handle->last_result = NORA_ADC_RESULT_NOT_INITIALIZED;
        return handle->last_result;
    }
    if ((config->filter >= DSPIC33CK_ADC_FILTER_COUNT) ||
        (config->ratio_log2 < DSPIC33CK_ADC_FILTER_RATIO_LOG2_MIN) ||
        (config->ratio_log2 > DSPIC33CK_ADC_FILTER_RATIO_LOG2_MAX) ||
        ((config->mode != NORA_ADC_FILTER_MODE_AVERAGING) &&
         (config->mode != NORA_ADC_FILTER_MODE_OVERSAMPLING))) {
        handle->last_result = NORA_ADC_RESULT_INVALID_ARG;
        return handle->last_result;
    }

    /*
     * FLCHSEL is five bits, so it can name AN0..AN31 -- wider than any device here has.
     * Validating against the conversion path's own channel table rather than against 31
     * keeps one answer to "does this AN exist", instead of letting a filter be pointed at
     * a channel the same handle would refuse to convert.
     */
    if (!dspic33ck_adc_reg_channel_is_valid(handle->channel)) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return handle->last_result;
    }

    /* Re-configuring THIS handle's own filter is a reconfigure, not a collision. */
    if (handle->filter_active && (handle->filter == config->filter)) {
        adc_filter_drop(handle);
    }

    if ((s_filter_claimed & (uint8_t)(1u << config->filter)) != 0u) {
        handle->last_result = NORA_ADC_RESULT_BUSY;
        return handle->last_result;
    }

    /* A handle owns at most one filter: moving to a different index gives the old one back
     * rather than leaking it. */
    adc_filter_drop(handle);

    if (config->mode == NORA_ADC_FILTER_MODE_AVERAGING) {
        value = DSPIC33CK_ADC_ADFLCON_MODE_AVERAGING |
                dspic33ck_adc_reg_ovrsam_averaging(config->ratio_log2);
        handle->filter_fractional_bits = 0u;
    } else {
        value = DSPIC33CK_ADC_ADFLCON_MODE_OVERSAMPLING |
                dspic33ck_adc_reg_ovrsam_oversampling(config->ratio_log2);
        handle->filter_fractional_bits =
            dspic33ck_adc_reg_oversampling_fractional_bits(config->ratio_log2);
    }

    value |= (uint16_t)((uint16_t)handle->channel & DSPIC33CK_ADC_ADFLCON_FLCHSEL_MASK);

    /*
     * IE deliberately left clear: this HAL owns no interrupt (see nora_adc.h's scope).
     *
     * Written twice, mode and channel first and FLEN second, so the filter is never
     * momentarily enabled on a channel or ratio it is about to be moved off -- the samples
     * it would accumulate in that window would then be averaged into the first result.
     */
    adc_filter_con_write(config->filter, value);
    adc_filter_con_write(config->filter, value | DSPIC33CK_ADC_ADFLCON_FLEN);

    handle->filter = config->filter;
    handle->filter_ratio_log2 = config->ratio_log2;
    handle->filter_active = true;
    s_filter_claimed |= (uint8_t)(1u << config->filter);

    handle->last_result = NORA_ADC_RESULT_OK;
    return handle->last_result;
}

nora_adc_result_t nora_adc_filter_release(nora_adc_handle_t *handle)
{
    if (handle == NULL) {
        return NORA_ADC_RESULT_INVALID_ARG;
    }

    adc_filter_drop(handle);
    handle->last_result = NORA_ADC_RESULT_OK;
    return handle->last_result;
}

uint8_t nora_adc_filter_fractional_bits(const nora_adc_handle_t *handle)
{
    if ((handle == NULL) || !handle->filter_active) {
        return 0u;
    }

    return handle->filter_fractional_bits;
}

nora_adc_result_t nora_adc_filter_read_blocking(
    nora_adc_handle_t *handle,
    uint32_t *result,
    uint32_t timeout_count)
{
    uint32_t          conversions_left;
    uint32_t          discarded;
    uint16_t          value;
    nora_adc_result_t status;

    if ((handle == NULL) || (result == NULL)) {
        return NORA_ADC_RESULT_INVALID_ARG;
    }
    if (!handle->initialized) {
        handle->last_result = NORA_ADC_RESULT_NOT_INITIALIZED;
        return handle->last_result;
    }
    if (!handle->filter_active) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return handle->last_result;
    }

    /*
     * THE FILTER IS FED BY ORDINARY CONVERSIONS -- it has no trigger of its own. So a
     * filtered read is the unfiltered read run ratio times, with each per-conversion
     * result thrown away; the accumulation happens in the filter beside that path, not
     * instead of it.
     *
     * Each ADCBUF read is what clears the channel's ADSTAT ready bit, which is why the
     * per-conversion result is READ and discarded rather than simply ignored: leaving the
     * flag set would make the next conversion's completion check pass before that
     * conversion had happened, and the loop would spend its budget on nothing.
     */
    conversions_left = (1uL << handle->filter_ratio_log2) +
                       DSPIC33CK_ADC_FILTER_CONVERSION_SLACK;

    while ((adc_filter_con_read(handle->filter) & DSPIC33CK_ADC_ADFLCON_RDY) == 0u) {
        if (conversions_left == 0u) {
            /* Not a stuck conversion -- those return TIMEOUT from the call below. This is
             * the filter never announcing a result, so the ADC is left alone and only the
             * filter is dropped, which keeps a bad OVRSAM setting from looking like dead
             * silicon. */
            adc_filter_drop(handle);
            handle->last_result = NORA_ADC_RESULT_TIMEOUT;
            return handle->last_result;
        }
        conversions_left--;

        discarded = 0u;
        status = nora_adc_read_blocking(handle, handle->channel, &discarded, timeout_count);
        if (status != NORA_ADC_RESULT_OK) {
            return status;
        }
        (void)discarded;
    }

    /* Reading ADFLxDAT is what clears RDY, so the next call starts from an empty filter --
     * which is also why this reads it exactly once. */
    value = adc_filter_dat_read(handle->filter);
    *result = (handle->filter_fractional_bits == 0u)
                  ? (uint32_t)(value & DSPIC33CK_ADC_DATA_MASK_12BIT)
                  : (uint32_t)value;

    handle->last_result = NORA_ADC_RESULT_OK;
    return handle->last_result;
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
