#ifndef NORA_ADC_DSPIC33CK_REG_H
#define NORA_ADC_DSPIC33CK_REG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Internal dsPIC33CK ADC register helpers.
 *
 * The CK peripheral has one ADC register block and a shared conversion core.
 * The public HAL intentionally does not expose that layout.
 */

#define DSPIC33CK_ADC_DATA_MASK_12BIT            (0x0FFFu)
#define DSPIC33CK_ADC_SHARED_SAMPLE_TIME_MIN_TAD (2u)

#define DSPIC33CK_ADC_ADCON1L_ADON               (0x8000u)
#define DSPIC33CK_ADC_ADCON1H_12BIT_INTEGER      (0x0060u)
#define DSPIC33CK_ADC_ADCON2L_TADCORE_DIV6       (0x0003u)
#define DSPIC33CK_ADC_ADCON2H_SAMC_MASK          (0x03FFu)
#define DSPIC33CK_ADC_ADCON3L_CNVCHSEL_MASK      (0x003Fu)
#define DSPIC33CK_ADC_ADCON3L_CNVRTCH            (0x0100u)
#define DSPIC33CK_ADC_ADCON3H_SHARED_CORE        (0x0080u)
#define DSPIC33CK_ADC_ADCON5L_SHRPWR             (0x0080u)
#define DSPIC33CK_ADC_ADCON5L_SHRRDY             (0x8000u)
#define DSPIC33CK_ADC_ADCON5H_SHARED_WARMUP      (0x0F00u)

static inline bool dspic33ck_adc_reg_channel_is_valid(uint8_t channel)
{
#if defined(__dsPIC33CK256MP508__)
    /* AN0/AN1 are dedicated-core inputs; AN24/AN25 are internal sources. */
    return (channel >= 2u) && (channel <= 23u);
#elif defined(__dsPIC33CK64MC105__)
    /* AN16/AN17 are internal sources; the remaining AN numbers do not exist. */
    return channel <= 15u;
#else
#error "Unsupported dsPIC33CK ADC device"
#endif
}

static inline uint16_t dspic33ck_adc_reg_ready_mask(uint8_t channel)
{
    return (uint16_t)(1u << (channel & 0x0Fu));
}

#endif /* NORA_ADC_DSPIC33CK_REG_H */
