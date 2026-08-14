#ifndef DMA_SELFTEST_H
#define DMA_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>

#include "nora_dma.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dma_selftest.h -- standalone proof that the DMA controller itself moves data.
 *
 * WHY THIS EXISTS AS ITS OWN STEP
 * -------------------------------
 * When a DMA-fed peripheral transport does not stream, the symptom ("no transfers ever
 * happened") is identical whether the DMA controller is misconfigured, the trigger
 * source is wrong, or the peripheral never raises its event. One flash cycle cannot
 * distinguish them by staring at the transport. This test removes the peripheral from
 * the loop entirely: RAM -> RAM, triggered by software (DMACHn.CHREQ). A PASS narrows a
 * remaining transport failure to the trigger/peripheral side; a FAIL says stop looking
 * at the SPI. That ordering is what found two of the three DMA defects in
 * docs/ck_silicon_findings.md.
 *
 * WAS boards/ev88g73a/ev88g73a_dma_selftest.{c,h}
 * ----------------------------------------------
 * Measured before moving it: zero pins, zero ports, zero board registers. Its only
 * board-shaped content was the CHANNEL NUMBER -- and a channel number is not a board
 * fact either, it is an allocation the application makes (the SPI/TDM transport owns 0
 * and 1, see nora_spi_i2s_tdm_conf.h). So the channel became the argument below,
 * and the caller states its own allocation.
 *
 * Output goes through app/console_out.h, so it works on any board that implements the
 * seam rather than on the one whose UART function it used to call by name.
 */

/*
 * Run the test on `channel`. Prints its own one-line result and returns true on PASS.
 *
 * The channel must be one nothing else is using: it is configured, enabled, triggered,
 * then left disabled with its status and IRQ flag cleared, so it is safe to run before
 * a transport starts -- but not safe to point at a channel a running transport owns.
 */
bool dma_selftest_run(nora_dma_channel_t channel);

#ifdef __cplusplus
}
#endif

#endif /* DMA_SELFTEST_H */
