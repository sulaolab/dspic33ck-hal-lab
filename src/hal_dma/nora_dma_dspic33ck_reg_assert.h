#ifndef NORA_DMA_DSPIC33CK_REG_ASSERT_H
#define NORA_DMA_DSPIC33CK_REG_ASSERT_H

/*
 * nora_dma_dspic33ck_reg_assert.h
 * --------------------------
 *
 * Static assertions that the installed DFP still places the SFR bits where
 * the DMA HAL assumes they are. Nothing here generates code -- if every
 * assertion holds, this header compiles to nothing.
 *
 * WHY IT EXISTS, AND WHY IT IS A SEPARATE FILE
 * --------------------------------------------
 * These checks came from a compile-only feasibility probe (since deleted) written
 * before the HAL existed, to prove the DFP exposed what a CK port would
 * need. The probe was in NO build -- its directory was never referenced -- so
 * all 30 of its assertions had never once been compiled. A safety net that
 * looks present and is in fact inert is worse than none, because it invites the
 * assumption that a DFP change would be caught. Now they are in a header that the
 * HAL includes, so they run on every build and a moved bit position stops the
 * build instead of silently mis-driving the peripheral.
 *
 * Kept out of the HAL sources rather than pasted into them because the assertion
 * SET is device-scoped while the HAL is meant to be device-agnostic: a new CK
 * variant can supply its own without editing HAL logic, and this file can be
 * dropped or replaced when the HAL is reused on another target. (xc.h already makes the
 * HAL DFP-dependent, so keeping DFP knowledge here is not a new dependency --
 * only a tidier place for it.)
 *
 * The probe's other half -- functions that poked the registers directly and
 * hard-coded DM330030 pin numbers -- was deleted, not moved: it is superseded by
 * the HAL it was written to justify, and it did not even compile for CK64MC105.
 */

#include <xc.h>

#if _DMACON_PRSSEL_POSITION != 0
#error "Unexpected DMACON.PRSSEL position"
#endif
#if _DMACON_DMAEN_POSITION != 15
#error "Unexpected DMACON.DMAEN position"
#endif

#if _DMACH0_CHEN_POSITION != 0
#error "Unexpected DMACH0.CHEN position"
#endif
#if _DMACH0_SIZE_POSITION != 1
#error "Unexpected DMACH0.SIZE position"
#endif
#if _DMACH0_TRMODE_POSITION != 2
#error "Unexpected DMACH0.TRMODE position"
#endif
#if _DMACH0_DAMODE_POSITION != 4
#error "Unexpected DMACH0.DAMODE position"
#endif
#if _DMACH0_SAMODE_POSITION != 6
#error "Unexpected DMACH0.SAMODE position"
#endif
#if _DMACH0_CHREQ_POSITION != 8
#error "Unexpected DMACH0.CHREQ position"
#endif
#if _DMACH0_RELOAD_POSITION != 9
#error "Unexpected DMACH0.RELOAD position"
#endif
#if _DMACH0_NULLW_POSITION != 10
#error "Unexpected DMACH0.NULLW position"
#endif

#if _DMAINT0_HALFEN_POSITION != 0
#error "Unexpected DMAINT0.HALFEN position"
#endif
#if _DMAINT0_OVRUNIF_POSITION != 3
#error "Unexpected DMAINT0.OVRUNIF position"
#endif
#if _DMAINT0_HALFIF_POSITION != 4
#error "Unexpected DMAINT0.HALFIF position"
#endif
#if _DMAINT0_DONEIF_POSITION != 5
#error "Unexpected DMAINT0.DONEIF position"
#endif
#if _DMAINT0_LOWIF_POSITION != 6
#error "Unexpected DMAINT0.LOWIF position"
#endif
#if _DMAINT0_HIGHIF_POSITION != 7
#error "Unexpected DMAINT0.HIGHIF position"
#endif
#if _DMAINT0_CHSEL_POSITION != 8
#error "Unexpected DMAINT0.CHSEL position"
#endif
#if _DMAINT0_DBUFWF_POSITION != 15
#error "Unexpected DMAINT0.DBUFWF position"
#endif

#if _DMAINT1_HALFEN_POSITION != 0
#error "Unexpected DMAINT1.HALFEN position"
#endif
#if _DMAINT1_HALFIF_POSITION != 4
#error "Unexpected DMAINT1.HALFIF position"
#endif
#if _DMAINT1_DONEIF_POSITION != 5
#error "Unexpected DMAINT1.DONEIF position"
#endif
#if _DMAINT1_CHSEL_POSITION != 8
#error "Unexpected DMAINT1.CHSEL position"
#endif

#if _IFS0_DMA0IF_POSITION != 4
#error "Unexpected DMA0 interrupt flag position"
#endif
#if _IFS0_DMA1IF_POSITION != 8
#error "Unexpected DMA1 interrupt flag position"
#endif
#if _IEC0_DMA0IE_POSITION != 4
#error "Unexpected DMA0 interrupt enable position"
#endif
#if _IEC0_DMA1IE_POSITION != 8
#error "Unexpected DMA1 interrupt enable position"
#endif
#if _IFS0_SPI1RXIF_POSITION != 9
#error "Unexpected SPI1RX interrupt flag position"
#endif
#if _IFS0_SPI1TXIF_POSITION != 10
#error "Unexpected SPI1TX interrupt flag position"
#endif

#if _SPI1BUFL_DATA_POSITION != 0
#error "Unexpected SPI1BUFL.DATA position"
#endif
#if _SPI1BUFH_DATA_POSITION != 0
#error "Unexpected SPI1BUFH.DATA position"
#endif

#endif /* NORA_DMA_DSPIC33CK_REG_ASSERT_H */
