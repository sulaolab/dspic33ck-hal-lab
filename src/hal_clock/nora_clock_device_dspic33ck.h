#ifndef NORA_CLOCK_DEVICE_DSPIC33CK_H
#define NORA_CLOCK_DEVICE_DSPIC33CK_H

#include <stdint.h>
#include <stdbool.h>

#include "nora_clock.h"

/*
 * dsPIC33CK oscillator-selection tables: the mapping between the contract's logical
 * source names and this silicon's NOSC/COSC encodings, in both directions.
 *
 * Datasheet transcription only. No sequencing, no policy, no state -- every function
 * here is a pure function of its arguments, so it can be read against DS70005399D
 * Register 9-1 without holding anything else in mind.
 *
 * ONE ASYMMETRY IS THE WHOLE REASON THIS FILE EXISTS
 *   On this silicon the PLL's INPUT is not a separate register field: FRCPLL and
 *   PRIPLL are two different NOSC values. So the logical source PLL_1 does not encode
 *   to a NOSC value on its own -- it needs an input as well, which is why encoding a
 *   PLL is a separate call from encoding everything else. Going the other way the same
 *   fact is a gain: COSC alone tells you both what the system runs on AND, when that
 *   is the PLL, what the PLL runs on.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Logical source -> NOSC, for the sources that are a single mux selection.
 *
 * NORA_CLOCK_SOURCE_PLL_1 is deliberately NOT accepted (see the note above): use
 * nora_clock_device_dspic33ck_pll_input_nosc(). NORA_CLOCK_SOURCE_FRC_DIVIDED is not
 * accepted either -- it is a selection this backend can OBSERVE but does not offer as
 * a destination, because the contract has no way to say which divisor is wanted.
 *
 * Returns NORA_CLOCK_OK, NORA_CLOCK_ERR_NOT_PRESENT (the source does not exist on this
 * family) or NORA_CLOCK_ERR_NOT_SUPPORTED (it exists but is not a system-clock
 * selection here).  *nosc is written only on OK.
 */
nora_clock_status_t nora_clock_device_dspic33ck_encode_source(
    nora_clock_source_t source,
    uint16_t *nosc);

/*
 * PLL input -> the NOSC value that selects "the PLL running from that input".
 * Accepts NORA_CLOCK_SOURCE_FRC and NORA_CLOCK_SOURCE_PRIMARY; everything else is
 * NORA_CLOCK_ERR_NOT_SUPPORTED. This is the same set that
 * nora_clock_pll_input_is_supported() reports, and it is this function that makes it
 * true rather than a second list that has to be kept in step.
 */
nora_clock_status_t nora_clock_device_dspic33ck_pll_input_nosc(
    nora_clock_source_t input,
    uint16_t *nosc);

/*
 * COSC -> the logical source the system is actually running on.
 *
 * Both PLL selections decode to NORA_CLOCK_SOURCE_PLL_1: what the system runs on is
 * the PLL, and which input the PLL uses is a separate question (the next function).
 * FRCDIVN decodes to NORA_CLOCK_SOURCE_FRC_DIVIDED and never to FRC -- reporting it
 * as FRC is what the contract's comment on that enumerator forbids, because it would
 * make a switch to FRC compare equal and silently succeed while the part stays divided.
 * A reserved encoding decodes to NORA_CLOCK_SOURCE_UNKNOWN.
 */
nora_clock_source_t nora_clock_device_dspic33ck_decode_cosc(uint16_t cosc);

/*
 * COSC -> which source the PLL is being fed from, for the two PLL encodings.
 * NORA_CLOCK_SOURCE_UNKNOWN for every other COSC value, including a request that has
 * not completed: while the system is not on the PLL, this silicon exposes no PLL input
 * select to read, and UNKNOWN is that absence rather than an error.
 */
nora_clock_source_t nora_clock_device_dspic33ck_cosc_pll_input(uint16_t cosc);

/*
 * CLKDIV.FRCDIV code -> divisor. Never returns 0, so it is always safe to divide by.
 */
uint16_t nora_clock_device_dspic33ck_frcdiv_divisor(uint16_t code);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CLOCK_DEVICE_DSPIC33CK_H */
