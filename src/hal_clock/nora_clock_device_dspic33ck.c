#include "nora_clock_device_dspic33ck.h"

#include <stddef.h>

#include "nora_clock_dspic33ck_reg.h"

/*
 * See nora_clock_device_dspic33ck.h. Tables from DS70005399D Register 9-1 (OSCCON)
 * and Register 9-2 (CLKDIV).
 */

nora_clock_status_t nora_clock_device_dspic33ck_encode_source(
    nora_clock_source_t source,
    uint16_t *nosc)
{
    uint16_t encoded;

    if (nosc == NULL) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    switch (source) {
    case NORA_CLOCK_SOURCE_FRC:
        encoded = NORA_CLOCK_DSPIC33CK_NOSC_FRC;
        break;
    case NORA_CLOCK_SOURCE_BFRC:
        encoded = NORA_CLOCK_DSPIC33CK_NOSC_BFRC;
        break;
    case NORA_CLOCK_SOURCE_PRIMARY:
        encoded = NORA_CLOCK_DSPIC33CK_NOSC_PRI;
        break;
    case NORA_CLOCK_SOURCE_LPRC:
        encoded = NORA_CLOCK_DSPIC33CK_NOSC_LPRC;
        break;

    /*
     * Exists on this silicon, is observable, is not offered as a destination.
     * A caller asking for it could not say WHICH divisor it wants -- the contract has
     * no field for one -- so honouring the request would mean picking a divisor on the
     * caller's behalf and reporting a frequency it never asked for.
     */
    case NORA_CLOCK_SOURCE_FRC_DIVIDED:
        return NORA_CLOCK_ERR_NOT_SUPPORTED;

    /*
     * A PLL is not a single mux selection here. Refused rather than handled so that a
     * caller cannot reach the PLL without going through the path that knows its input.
     */
    case NORA_CLOCK_SOURCE_PLL_1:
        return NORA_CLOCK_ERR_NOT_SUPPORTED;

    /*
     * Absent from the mux entirely, which is a different answer from "unsupported
     * routing" and the contract keeps the two apart.
     */
    case NORA_CLOCK_SOURCE_PLL_2:
    case NORA_CLOCK_SOURCE_PLL1_VCO_FRACDIV:
    case NORA_CLOCK_SOURCE_PLL2_VCO_FRACDIV:
    case NORA_CLOCK_SOURCE_REFI1:
    case NORA_CLOCK_SOURCE_REFI2:
        return NORA_CLOCK_ERR_NOT_PRESENT;

    default:
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    *nosc = encoded;
    return NORA_CLOCK_OK;
}

nora_clock_status_t nora_clock_device_dspic33ck_pll_input_nosc(
    nora_clock_source_t input,
    uint16_t *nosc)
{
    if (nosc == NULL) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    switch (input) {
    case NORA_CLOCK_SOURCE_FRC:
        *nosc = NORA_CLOCK_DSPIC33CK_NOSC_FRCPLL;
        return NORA_CLOCK_OK;
    case NORA_CLOCK_SOURCE_PRIMARY:
        *nosc = NORA_CLOCK_DSPIC33CK_NOSC_PRIPLL;
        return NORA_CLOCK_OK;
    default:
        /* BFRC and LPRC reach the mux but not the PLL: the mux has no encoding that
         * says "PLL from BFRC". Not a routing this backend declined to write -- one
         * the silicon does not have. */
        return NORA_CLOCK_ERR_NOT_SUPPORTED;
    }
}

nora_clock_source_t nora_clock_device_dspic33ck_decode_cosc(uint16_t cosc)
{
    switch (cosc & 0x07u) {
    case NORA_CLOCK_DSPIC33CK_NOSC_FRC:      return NORA_CLOCK_SOURCE_FRC;
    case NORA_CLOCK_DSPIC33CK_NOSC_FRCPLL:   return NORA_CLOCK_SOURCE_PLL_1;
    case NORA_CLOCK_DSPIC33CK_NOSC_PRI:      return NORA_CLOCK_SOURCE_PRIMARY;
    case NORA_CLOCK_DSPIC33CK_NOSC_PRIPLL:   return NORA_CLOCK_SOURCE_PLL_1;
    case NORA_CLOCK_DSPIC33CK_NOSC_LPRC:     return NORA_CLOCK_SOURCE_LPRC;
    case NORA_CLOCK_DSPIC33CK_NOSC_BFRC:     return NORA_CLOCK_SOURCE_BFRC;
    case NORA_CLOCK_DSPIC33CK_NOSC_FRCDIVN:  return NORA_CLOCK_SOURCE_FRC_DIVIDED;
    default:                                 return NORA_CLOCK_SOURCE_UNKNOWN;
    }
}

nora_clock_source_t nora_clock_device_dspic33ck_cosc_pll_input(uint16_t cosc)
{
    switch (cosc & 0x07u) {
    case NORA_CLOCK_DSPIC33CK_NOSC_FRCPLL:   return NORA_CLOCK_SOURCE_FRC;
    case NORA_CLOCK_DSPIC33CK_NOSC_PRIPLL:   return NORA_CLOCK_SOURCE_PRIMARY;
    default:                                 return NORA_CLOCK_SOURCE_UNKNOWN;
    }
}

uint16_t nora_clock_device_dspic33ck_frcdiv_divisor(uint16_t code)
{
    /*
     * DS70005399D Register 9-2, CLKDIV<10:8>. Powers of two up to 64, then the last
     * encoding jumps to 256 -- there is no /128. Written as a table rather than a
     * shift precisely because of that jump.
     *
     * Only code 0 (the reset value, /1) is exercised on this bench: EV88G73A boots on
     * the FRCDIVN selection with FRCDIV untouched, which is 8 MHz undivided. The rest
     * are transcription. Nothing in this repo writes FRCDIV, so a wrong entry could
     * only ever be reached by external code changing it -- and would then show up as a
     * reported frequency, never as a mis-programmed clock.
     */
    static const uint16_t divisor[8] = { 1u, 2u, 4u, 8u, 16u, 32u, 64u, 256u };
    return divisor[code & 0x07u];
}

/*
 * The two capability predicates of the contract.
 *
 * They live in this file because they are the same datasheet knowledge as the tables
 * above, and keeping them here is what stops them from becoming a second list that
 * drifts: each one answers by asking the encoder that the switch path itself uses, so
 * "supported" cannot disagree with "accepted".
 */

bool nora_clock_system_source_is_supported(nora_clock_source_t source)
{
    uint16_t nosc;

    /* The one destination that is not a plain mux selection, and so cannot be asked of
     * the encoder. Supported: nora_clock_switch_source() accepts it once the PLL has
     * been configured. */
    if (source == NORA_CLOCK_SOURCE_PLL_1) {
        return true;
    }

    return (nora_clock_device_dspic33ck_encode_source(source, &nosc) == NORA_CLOCK_OK);
}

bool nora_clock_pll_input_is_supported(nora_clock_pll_t pll, nora_clock_source_t source)
{
    uint16_t nosc;

    if (pll != NORA_CLOCK_PLL_1) {
        return false;
    }

    return (nora_clock_device_dspic33ck_pll_input_nosc(source, &nosc) == NORA_CLOCK_OK);
}
