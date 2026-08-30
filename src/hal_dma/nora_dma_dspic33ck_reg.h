#ifndef NORA_DMA_DSPIC33CK_REG_H
#define NORA_DMA_DSPIC33CK_REG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Minimal CK DMA register masks and 16-bit accessors for the DMA HAL backend.
 *
 * Intentionally small: it captures only the DFP-visible register shape the
 * backend and its ISR fast path actually drive. Backend-private -- nothing here
 * is part of the nora_dma.h contract, and no consumer includes it.
 */

/* ---- DMACON fields ---- */
#define NORA_DMA_DSPIC33CK_CON_PRSSEL  (1u << 0)
#define NORA_DMA_DSPIC33CK_CON_DMAEN   (1u << 15)

/* ---- DMACHn fields ---- */
#define NORA_DMA_DSPIC33CK_CH_CHEN         (1u << 0)
#define NORA_DMA_DSPIC33CK_CH_SIZE         (1u << 1)
#define NORA_DMA_DSPIC33CK_CH_TRMODE_POS   2u
#define NORA_DMA_DSPIC33CK_CH_TRMODE_MASK  (0x3u << NORA_DMA_DSPIC33CK_CH_TRMODE_POS)
#define NORA_DMA_DSPIC33CK_CH_DAMODE_POS   4u
#define NORA_DMA_DSPIC33CK_CH_DAMODE_MASK  (0x3u << NORA_DMA_DSPIC33CK_CH_DAMODE_POS)
#define NORA_DMA_DSPIC33CK_CH_SAMODE_POS   6u
#define NORA_DMA_DSPIC33CK_CH_SAMODE_MASK  (0x3u << NORA_DMA_DSPIC33CK_CH_SAMODE_POS)
#define NORA_DMA_DSPIC33CK_CH_CHREQ        (1u << 8)
#define NORA_DMA_DSPIC33CK_CH_RELOAD       (1u << 9)
#define NORA_DMA_DSPIC33CK_CH_NULLW        (1u << 10)

/* ---- DMAINTn fields ---- */
#define NORA_DMA_DSPIC33CK_INT_HALFEN      (1u << 0)
#define NORA_DMA_DSPIC33CK_INT_OVRUNIF     (1u << 3)
#define NORA_DMA_DSPIC33CK_INT_HALFIF      (1u << 4)
#define NORA_DMA_DSPIC33CK_INT_DONEIF      (1u << 5)
#define NORA_DMA_DSPIC33CK_INT_LOWIF       (1u << 6)
#define NORA_DMA_DSPIC33CK_INT_HIGHIF      (1u << 7)
#define NORA_DMA_DSPIC33CK_INT_CHSEL_POS   8u
#define NORA_DMA_DSPIC33CK_INT_CHSEL_MASK  (0x7Fu << NORA_DMA_DSPIC33CK_INT_CHSEL_POS)
#define NORA_DMA_DSPIC33CK_INT_DBUFWF      (1u << 15)

/*
 * DMAINTn holds both the trigger selector (CHSEL) / HALFEN control and the
 * per-channel status flags. Clearing status must touch ONLY the flag bits, so
 * the CHSEL and HALFEN configuration survives a status clear.
 */
#define NORA_DMA_DSPIC33CK_INT_STATUS_MASK (NORA_DMA_DSPIC33CK_INT_OVRUNIF | \
                                       NORA_DMA_DSPIC33CK_INT_HALFIF  | \
                                       NORA_DMA_DSPIC33CK_INT_DONEIF  | \
                                       NORA_DMA_DSPIC33CK_INT_LOWIF   | \
                                       NORA_DMA_DSPIC33CK_INT_HIGHIF  | \
                                       NORA_DMA_DSPIC33CK_INT_DBUFWF)

static inline void nora_dma_reg_set16(volatile uint16_t *reg, uint16_t mask)
{
    *reg = (uint16_t)(*reg | mask);
}

static inline void nora_dma_reg_clear16(volatile uint16_t *reg, uint16_t mask)
{
    *reg = (uint16_t)(*reg & (uint16_t)~mask);
}

static inline void nora_dma_reg_write_field16(volatile uint16_t *reg,
                                                   uint16_t mask,
                                                   uint16_t pos,
                                                   uint16_t value)
{
    *reg = (uint16_t)((*reg & (uint16_t)~mask) | ((uint16_t)(value << pos) & mask));
}

static inline bool nora_dma_reg_is_set16(volatile uint16_t *reg, uint16_t mask)
{
    return ((*reg & mask) != 0u);
}

/* Bit positions this header hands out are only valid if the DFP still agrees.
 * Included last so the assertions are checked wherever these accessors are used;
 * it emits no code. See the header for why the checks live in their own file. */
#include "nora_dma_dspic33ck_reg_assert.h"

#endif /* NORA_DMA_DSPIC33CK_REG_H */
