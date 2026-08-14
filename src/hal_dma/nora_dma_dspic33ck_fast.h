#ifndef NORA_DMA_DSPIC33CK_FAST_H
#define NORA_DMA_DSPIC33CK_FAST_H

/*
 * dsPIC33CK DMA hot-path helpers.
 *
 * This header intentionally exposes XC-DSC SFRs and is therefore never part of
 * the public Nora DMA contract. Only the dsPIC33CK backend and backend-aware,
 * measured hot-path consumers include it, where a compile-time-constant channel
 * must fold to direct SFR accesses in an ISR or other measured hot path.
 *
 * Including this header is the consumer's declaration that it is on a measured
 * ISR path. Everything here has an out-of-line twin in nora_dma.h; a consumer
 * that is not on such a path calls the twin and does not include this file.
 *
 * NAMING RULE FOR EVERY NORA ISR FAST PATH
 *   A fast path is a `static inline` function in <module>_<backend>_fast.h, named
 *   <the portable function it shadows>_hot. So nora_dma_read_src() is the
 *   out-of-line portable call and nora_dma_read_src_hot() is the inline one; the
 *   two do the same thing, and the out-of-line version in the backend .c is
 *   literally a call to the inline.
 *
 *   The `_hot` suffix is on the PORTABLE stem on purpose, not a <backend> tag in
 *   the middle. An ISR body written against `_hot` names ports from CK to AK
 *   unchanged -- only the *_fast.h that supplies the inline differs, which is the
 *   same seam every other part of this HAL uses.
 *
 *   Backend-private helpers with NO portable twin keep <module>_<backend>_<name>:
 *   they are not a variant of anything, they are the only form, and the chip
 *   belongs in their name.
 *
 * CK carries one inline the AK backend does not: nora_dma_half_from_status_hot().
 * That is a measurement, not a divergence -- see its comment. The set of inlines
 * in a *_fast.h is per family by construction; only the portable twins in
 * nora_dma.h have to agree across the fleet.
 */

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>

#include "nora_dma.h"
#include "nora_dma_dspic33ck_reg.h"

/* DMAINTn status flags (raw status interpretation) - same bit positions as AK.
 *
 * These are backend-private on purpose: a consumer asks the predicates below (or
 * their out-of-line twins) rather than testing bits, so that the same consumer
 * source compiles on a family whose status word is laid out differently. They
 * live here rather than in the register header because this is the only place
 * that interprets a status snapshot. */
#define NORA_DMA_DSPIC33CK_STAT_HALF         NORA_DMA_DSPIC33CK_INT_HALFIF   /* bit 4 */
#define NORA_DMA_DSPIC33CK_STAT_DONE         NORA_DMA_DSPIC33CK_INT_DONEIF   /* bit 5 */
/* DMAINTn.OVRUNIF: a trigger arrived while the channel still had a pending
 * request -- the primary transport-stall signal (a dropped element on an audio
 * stream). Mirrors AK's NORA_DMA_DSPIC33AK_STAT_OVERRUN so the SPI/TDM diagnostics
 * port across unchanged. isr_snapshot() already clears it as part of the status
 * mask, so it appears in the snapshot the consumer folds into its diag. */
#define NORA_DMA_DSPIC33CK_STAT_OVERRUN      NORA_DMA_DSPIC33CK_INT_OVRUNIF  /* bit 3 */

/* Fast save/mask + restore helpers for short critical sections. A compile-time-
 * constant channel folds to direct _DMAnIE access. */
static inline bool nora_dma_irq_disable_save_hot(nora_dma_channel_t ch)
{
    bool was_enabled;

    switch (ch) {
    case NORA_DMA_CHANNEL_0: was_enabled = (_DMA0IE != 0u); _DMA0IE = 0u; break;
    case NORA_DMA_CHANNEL_1: was_enabled = (_DMA1IE != 0u); _DMA1IE = 0u; break;
    case NORA_DMA_CHANNEL_2: was_enabled = (_DMA2IE != 0u); _DMA2IE = 0u; break;
    case NORA_DMA_CHANNEL_3: was_enabled = (_DMA3IE != 0u); _DMA3IE = 0u; break;
    default: was_enabled = false; break;
    }
    return was_enabled;
}

/* Restoring stores a literal in each arm, as the save side already did. `_DMAnIE = v` with
 * a runtime v folds to one BFINS on dsPIC33C today, but nothing in the C guarantees that --
 * the identical line is a read-modify-write of the whole of IECx on dsPIC33A, which has no
 * BFINS, and IECx carries the tick and the console alongside these bits
 * (a read-modify-write on dsPIC33A, where there is no BFINS). Callers are short
 * critical sections on the
 * audio path, so the cost of the branch is measured there, not assumed. */
static inline void nora_dma_irq_restore_hot(nora_dma_channel_t ch, bool was_enabled)
{
    if (was_enabled) {
        switch (ch) {
        case NORA_DMA_CHANNEL_0: _DMA0IE = 1u; break;
        case NORA_DMA_CHANNEL_1: _DMA1IE = 1u; break;
        case NORA_DMA_CHANNEL_2: _DMA2IE = 1u; break;
        case NORA_DMA_CHANNEL_3: _DMA3IE = 1u; break;
        default: break;
        }
    } else {
        switch (ch) {
        case NORA_DMA_CHANNEL_0: _DMA0IE = 0u; break;
        case NORA_DMA_CHANNEL_1: _DMA1IE = 0u; break;
        case NORA_DMA_CHANNEL_2: _DMA2IE = 0u; break;
        case NORA_DMA_CHANNEL_3: _DMA3IE = 0u; break;
        default: break;
        }
    }
}

/* Read DMASRCn (raw) as a zero-extended 32-bit value, matching the portable API. */
static inline uint32_t nora_dma_read_src_hot(nora_dma_channel_t ch)
{
    /* Widen once after the switch, not per case. With a runtime ch this switch
     * is emitted in full, and returning 32 bits from each case made the TDM RX
     * ISR pay a separate high-word clear per case (measured +16 B). */
    uint16_t src;

    switch (ch) {
    case NORA_DMA_CHANNEL_0: src = DMASRC0; break;
    case NORA_DMA_CHANNEL_1: src = DMASRC1; break;
    case NORA_DMA_CHANNEL_2: src = DMASRC2; break;
    case NORA_DMA_CHANNEL_3: src = DMASRC3; break;
    default: src = 0u; break;
    }
    return src;
}

/* Ordered snapshot: clear _DMAnIF, read DMAINTn, then clear the DMAINTn status
 * flag bits (preserving CHSEL/HALFEN). Returns the raw DMAINTn snapshot,
 * zero-extended to 32 bits to match the portable API. Call with a compile-time-
 * constant ch so the switch folds to direct accesses.
 *
 * The clear is a read-modify-write of the whole register, so it also clears a flag
 * that was raised after the snapshot was taken; the CPU flag was cleared first and
 * still re-asserts, so that event costs an ISR entry whose snapshot maps to
 * NORA_DMA_HALF_NONE rather than being missed outright. See the ordering note on
 * nora_dma_isr_snapshot() in nora_dma.h.
 *
 * The snapshot itself is read and cleared at the register's native 16 bits; only
 * the returned value is widened. The consumers of this value (half_from_status(),
 * status_has_half_done_conflict(), the buffer-half mapping) already take
 * nora_dma_status_t, so widening here removes their promotions rather than adding
 * any. The 16-bit diagnostic store keeps its own narrowing at the call site,
 * where it is documented. */
static inline nora_dma_status_t nora_dma_isr_snapshot_hot(nora_dma_channel_t ch)
{
    uint16_t stat;
    const uint16_t clr = (uint16_t)NORA_DMA_DSPIC33CK_INT_STATUS_MASK;

    switch (ch) {
    case NORA_DMA_CHANNEL_0: _DMA0IF = 0; stat = DMAINT0; DMAINT0 = (uint16_t)(DMAINT0 & (uint16_t)~clr); break;
    case NORA_DMA_CHANNEL_1: _DMA1IF = 0; stat = DMAINT1; DMAINT1 = (uint16_t)(DMAINT1 & (uint16_t)~clr); break;
    case NORA_DMA_CHANNEL_2: _DMA2IF = 0; stat = DMAINT2; DMAINT2 = (uint16_t)(DMAINT2 & (uint16_t)~clr); break;
    case NORA_DMA_CHANNEL_3: _DMA3IF = 0; stat = DMAINT3; DMAINT3 = (uint16_t)(DMAINT3 & (uint16_t)~clr); break;
    default: stat = 0u; break;
    }
    return stat;
}

/*
 * Interpret a DMAINTn snapshot as a ping/pong-half indicator.
 *
 * This looks deliberately small, but it is on the audio RX-DMA ISR path: the
 * SPI/TDM transport calls it once for every completed block to select the RX
 * half handed to the application. Keeping it in nora_dma_dspic33ck.c made that
 * selection an out-of-line call even though the operation is only two tests.
 * On the 16-bit core that also means argument/return register traffic in the
 * deadline-sensitive vector. That measurement is why CK supplies a `_hot` twin
 * here where the AK backend needs none.
 *
 * `always_inline` is intentional here. The status is already a local ISR
 * snapshot, and this routine has no side effects, so putting these exact tests
 * at the call site cannot change acknowledgement or diagnostic ordering. In
 * particular, DONE MUST retain precedence over HALF: seeing both flags is a
 * deadline conflict diagnosed by the transport, but the completed half is
 * still the second (pong) half. A snapshot with neither flag remains HALF_NONE;
 * callers must keep rejecting it rather than inventing a buffer half for an
 * overrun-only/fault interrupt.
 */
static inline __attribute__((always_inline)) nora_dma_half_t
nora_dma_half_from_status_hot(nora_dma_status_t status)
{
    if ((status & NORA_DMA_DSPIC33CK_STAT_DONE) != 0u) {
        return NORA_DMA_HALF_SECOND;
    }
    if ((status & NORA_DMA_DSPIC33CK_STAT_HALF) != 0u) {
        return NORA_DMA_HALF_FIRST;
    }
    return NORA_DMA_HALF_NONE;
}

static inline bool
nora_dma_status_has_half_done_conflict_hot(nora_dma_status_t status)
{
    const uint32_t mask = (uint32_t)(NORA_DMA_DSPIC33CK_STAT_HALF |
                                     NORA_DMA_DSPIC33CK_STAT_DONE);

    return ((status & mask) == mask);
}

static inline bool
nora_dma_status_has_overrun_hot(nora_dma_status_t status)
{
    return ((status & NORA_DMA_DSPIC33CK_STAT_OVERRUN) != 0u);
}

static inline bool
nora_dma_status_has_completed_half_hot(nora_dma_status_t status)
{
    const uint32_t mask = (uint32_t)(NORA_DMA_DSPIC33CK_STAT_HALF |
                                     NORA_DMA_DSPIC33CK_STAT_DONE);

    return ((status & mask) != 0u);
}

/* DONE only: the transfer as a whole. Deliberately not the same question as
 * _has_completed_half_hot(), which is already true at the midpoint. */
static inline bool
nora_dma_status_has_completed_hot(nora_dma_status_t status)
{
    return ((status & NORA_DMA_DSPIC33CK_STAT_DONE) != 0u);
}

#endif /* NORA_DMA_DSPIC33CK_FAST_H */
