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

/*
 * ADFLxCON -- the four oversampling/averaging filters.
 *
 * Four on every device this driver builds for: ADFL0CON..ADFL3CON are all declared in
 * p33CK256MC005.h, p33CK64MC105.h and p33CK256MP508.h alike, so unlike the channel table
 * below this count needs no per-device branch.
 *
 * Field layout from DS70005399D Register 12-26 (the CK64MC105 family document; the bit
 * fields in p33CK256MC005.h match it exactly, field for field and width for width, which
 * is the only cross-check available here -- DS70005633B is not in the local datasheet
 * pack).
 */
#define DSPIC33CK_ADC_FILTER_COUNT               (4u)

#define DSPIC33CK_ADC_ADFLCON_FLEN               (0x8000u)
#define DSPIC33CK_ADC_ADFLCON_MODE_MASK          (0x6000u)
#define DSPIC33CK_ADC_ADFLCON_MODE_OVERSAMPLING  (0x0000u)
#define DSPIC33CK_ADC_ADFLCON_MODE_AVERAGING     (0x6000u)
#define DSPIC33CK_ADC_ADFLCON_OVRSAM_MASK        (0x1C00u)
#define DSPIC33CK_ADC_ADFLCON_OVRSAM_POSITION    (10u)
#define DSPIC33CK_ADC_ADFLCON_IE                 (0x0200u)
#define DSPIC33CK_ADC_ADFLCON_RDY                (0x0100u)
#define DSPIC33CK_ADC_ADFLCON_FLCHSEL_MASK       (0x001Fu)

/* Ratio 2x..256x, expressed as its log2 so the two modes' encodings can be derived
 * rather than tabulated twice. */
#define DSPIC33CK_ADC_FILTER_RATIO_LOG2_MIN      (1u)
#define DSPIC33CK_ADC_FILTER_RATIO_LOG2_MAX      (8u)

/*
 * OVRSAM is encoded DIFFERENTLY in the two modes, and the oversampling one is not
 * monotonic -- this is the trap in the register.
 *
 * Averaging (MODE = 11) is the straightforward one: OVRSAM n means 2^(n+1), so
 * 000 = 2x through 111 = 256x. (The datasheet's averaging list prints "110 = 8x" where
 * 010 belongs -- a typo, since 110 also appears against 128x three lines above and the
 * remaining seven entries fix the sequence.)
 *
 * Oversampling (MODE = 00) interleaves two runs: OVRSAM[2] = 0 gives the even powers
 * 4/16/64/256 and OVRSAM[2] = 1 gives the odd ones 2/8/32/128, each run counting up in
 * OVRSAM[1:0]. So the field is not "bigger value = more samples" and must not be treated
 * as one.
 */
static inline uint16_t dspic33ck_adc_reg_ovrsam_averaging(uint8_t ratio_log2)
{
    return (uint16_t)((uint16_t)(ratio_log2 - 1u) << DSPIC33CK_ADC_ADFLCON_OVRSAM_POSITION);
}

static inline uint16_t dspic33ck_adc_reg_ovrsam_oversampling(uint8_t ratio_log2)
{
    /* Even log2 -> the 4x/16x/64x/256x run (OVRSAM[2] = 0); odd -> the 2x/8x/32x/128x
     * run (OVRSAM[2] = 1). */
    const uint16_t field = ((ratio_log2 & 1u) != 0u)
                               ? (uint16_t)(4u + ((ratio_log2 - 1u) / 2u))
                               : (uint16_t)((ratio_log2 - 2u) / 2u);

    return (uint16_t)(field << DSPIC33CK_ADC_ADFLCON_OVRSAM_POSITION);
}

/*
 * Fractional bits in ADFLxDAT. Averaging mode always answers a plain 12-bit integer;
 * oversampling mode widens the result instead of just quietening it -- 2x/4x give 12.1,
 * 8x/16x give 12.2, 32x/64x give 12.3 and 128x/256x give 12.4 -- so a caller cannot scale
 * an oversampled reading without this number.
 */
static inline uint8_t dspic33ck_adc_reg_oversampling_fractional_bits(uint8_t ratio_log2)
{
    return (uint8_t)((ratio_log2 + 1u) / 2u);
}

static inline bool dspic33ck_adc_reg_channel_is_valid(uint8_t channel)
{
#if defined(__dsPIC33CK1024MP710__)
    /* Initial MP710 port: dedicated core 0 / board POT AN0 only. */
    return channel == 0u;
#elif defined(__dsPIC33CK256MP508__)
    /* AN0/AN1 are dedicated-core inputs; AN24/AN25 are internal sources. */
    return (channel >= 2u) && (channel <= 23u);
#elif defined(__dsPIC33CK64MC105__)
    /* AN16/AN17 are internal sources; the remaining AN numbers do not exist. */
    return channel <= 15u;
#elif defined(__dsPIC33CK256MC005__)
    /*
     * 48-pin MC005: AN0-AN19 are external pins and AN20/AN21 are internal, and every
     * one of them reaches the SAME shared core -- so all 22 are valid targets for this
     * driver. DS70005633B Table 13-1 gives the 48-pin part "AN0-AN19" plus ANN0, and
     * Figure 13-2 draws the shared core's multiplexer with AN0..AN19, Temperature
     * Sensor (AN20) and Band Gap 1.2V (AN21) on it. There is no dedicated core on this
     * device (the ADCORE registers do not exist in p33CK256MC005.h), which is why the
     * MP508 line above excludes two channels and this one excludes none.
     *
     * THE TWO INTERNAL CHANNELS ARE DELIBERATELY REACHABLE, unlike MC105's AN16/AN17
     * next door. A band-gap read is the only measurement that proves the converter
     * converts without involving a pin, a Click adapter or any wiring at all -- which
     * is exactly the split a bring-up needs when an external channel reads zero, and
     * it earned its place on the SAM HA1 board by separating a dead reference from
     * dead wiring on the first attempt. AN20 is a forward-biased diode (DS 13.1.1) and
     * needs a two-point calibration to mean a temperature, so it is available but not
     * a thermometer.
     *
     * Nothing has to be enabled to route them: this ADC has no BGOUTEN-style gate --
     * the ADCON5L/ADCON5H bit fields are SHRPWR/SHRRDY/SHRCIE/WARMTIME and nothing
     * else -- so selecting the channel is the whole of it.
     */
    return channel <= 21u;
#else
#error "Unsupported dsPIC33CK ADC device"
#endif
}

static inline uint16_t dspic33ck_adc_reg_ready_mask(uint8_t channel)
{
    return (uint16_t)(1u << (channel & 0x0Fu));
}

#endif /* NORA_ADC_DSPIC33CK_REG_H */
