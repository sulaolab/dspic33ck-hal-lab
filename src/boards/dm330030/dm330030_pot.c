/*
 * dm330030_pot.c -- see dm330030_pot.h for why this is not part of dm330030_io.
 */

#ifndef DSPIC33CK_BOARD_DM330030
#error "boards/dm330030/dm330030_pot.c is DM330030-owned. Build it only in the CK256MP508_DM330030 configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "dm330030_pot.h"

#include <stddef.h>

#include "nora_adc.h"

/* AN23 is RE3 on this device; the pin itself is dm330030_board.c's (see dm330030_pot.h). */
#define DM330030_POT_AN_CHANNEL      (23u)

/* Shared-core sample duration in TADCORE cycles. Inherited from the vendor demo and
 * generous for a pot: the source impedance of a hand-turned wiper is what sets the
 * minimum, and nothing here is rate-critical. */
#define DM330030_POT_SAMPLE_TIME_TAD (0x30u)

/*
 * TWO TIMEOUTS, NOT ONE. Both are poll counts, and they used to be the same constant --
 * which hid that they answer different questions:
 *
 *   READY  once, at init: has the shared core finished powering up?
 *   READ   every conversion: has this one conversion finished?
 *
 * A conversion at 100 MHz Fcy is a few microseconds, so the read count is insurance
 * against a peripheral that never answers, not a tuned deadline. Separate names mean a
 * future change to one is not silently a change to the other.
 */
#define DM330030_POT_READY_TIMEOUT_COUNT (1000000UL)
#define DM330030_POT_READ_TIMEOUT_COUNT  (1000000UL)

static nora_adc_handle_t s_pot_adc;
static bool                   s_pot_adc_ready;
static uint16_t               s_pot_last_value;
static const char            *s_pot_fault;

bool dm330030_pot_init(void)
{
    const nora_adc_config_t adc_config = {
        .instance = NORA_ADC_INSTANCE_1,
        /* channel == positive_input is required by the CK HAL for a single-ended input;
         * the field pair exists to keep the AK/sonora configuration shape. */
        .channel = DM330030_POT_AN_CHANNEL,
        .positive_input = DM330030_POT_AN_CHANNEL,
        .sample_time_tad = DM330030_POT_SAMPLE_TIME_TAD,
        .calibrate = false,
        .ready_timeout_count = DM330030_POT_READY_TIMEOUT_COUNT,
        .calibration_timeout_count = 0u,
    };

    s_pot_last_value = 0u;
    s_pot_fault      = NULL;
    s_pot_adc_ready  = nora_adc_init(&s_pot_adc, &adc_config);

    if (!s_pot_adc_ready) {
        /* The handle records why, even though init only returns a bare false. */
        s_pot_fault = nora_adc_result_str(nora_adc_get_last_result(&s_pot_adc));
    }

    return s_pot_adc_ready;
}

uint16_t dm330030_pot_read(void)
{
    uint32_t               raw_value = 0u;
    nora_adc_result_t result;

    if (!s_pot_adc_ready) {
        return s_pot_last_value;
    }

    result = nora_adc_read_blocking(&s_pot_adc,
                                         DM330030_POT_AN_CHANNEL,
                                         &raw_value,
                                         DM330030_POT_READ_TIMEOUT_COUNT);
    if (result != NORA_ADC_RESULT_OK) {
        s_pot_fault = nora_adc_result_str(result);
        return s_pot_last_value;
    }

    /* 12-bit, as the converter produces it. Scaling to whatever a caller drives is the
     * caller's business -- see dm330030_pot.h. */
    s_pot_fault      = NULL;
    s_pot_last_value = (uint16_t)raw_value;
    return s_pot_last_value;
}

const char *dm330030_pot_fault(void)
{
    return s_pot_fault;
}
