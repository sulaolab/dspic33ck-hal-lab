/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef DSPIC33CK_BOARD_EV97U97A
#error "boards/ev97u97a/ev97u97a_pot.c is EV97U97A-owned. Build it only in the CK1024MP710_EV97U97A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "ev97u97a_pot.h"

#include <stddef.h>

#include "nora_adc.h"

#define EV97U97A_POT_AN_CHANNEL      (0u)

#define EV97U97A_POT_SAMPLE_TIME_TAD (0x30u)

#define EV97U97A_POT_READY_TIMEOUT_COUNT (1000000UL)
#define EV97U97A_POT_READ_TIMEOUT_COUNT  (1000000UL)

static nora_adc_handle_t s_pot_adc;
static bool                   s_pot_adc_ready;
static uint16_t               s_pot_last_value;
static const char            *s_pot_fault;

bool ev97u97a_pot_init(void)
{
    const nora_adc_config_t adc_config = {
        .instance = NORA_ADC_INSTANCE_1,

        .channel = EV97U97A_POT_AN_CHANNEL,
        .positive_input = EV97U97A_POT_AN_CHANNEL,
        .sample_time_tad = EV97U97A_POT_SAMPLE_TIME_TAD,
        .calibrate = false,
        .ready_timeout_count = EV97U97A_POT_READY_TIMEOUT_COUNT,
        .calibration_timeout_count = 0u,
    };

    s_pot_last_value = 0u;
    s_pot_fault      = NULL;
    s_pot_adc_ready  = nora_adc_init(&s_pot_adc, &adc_config);

    if (!s_pot_adc_ready) {

        s_pot_fault = nora_adc_result_str(nora_adc_get_last_result(&s_pot_adc));
    }

    return s_pot_adc_ready;
}

uint16_t ev97u97a_pot_read(void)
{
    uint32_t               raw_value = 0u;
    nora_adc_result_t result;

    if (!s_pot_adc_ready) {
        return s_pot_last_value;
    }

    result = nora_adc_read_blocking(&s_pot_adc,
                                         EV97U97A_POT_AN_CHANNEL,
                                         &raw_value,
                                         EV97U97A_POT_READ_TIMEOUT_COUNT);
    if (result != NORA_ADC_RESULT_OK) {
        s_pot_fault = nora_adc_result_str(result);
        return s_pot_last_value;
    }

    s_pot_fault      = NULL;
    s_pot_last_value = (uint16_t)raw_value;
    return s_pot_last_value;
}

const char *ev97u97a_pot_fault(void)
{
    return s_pot_fault;
}
