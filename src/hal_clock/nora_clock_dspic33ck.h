#ifndef NORA_CLOCK_DSPIC33CK_H
#define NORA_CLOCK_DSPIC33CK_H

#include <stdint.h>
#include <stdbool.h>

#include "nora_clock.h"

/*
 * dsPIC33CK backend of the NORA clock contract -- the part that cannot be portable.
 *
 * nora_clock.h is the whole public API.  What is here is what a portable consumer
 * must NOT use: this silicon's diagnostic codes, and the raw oscillator control
 * words for a post-mortem.  Nothing here is required to build against the contract.
 *
 * WHERE THE CONTRACT COMES FROM, AND HOW DRIFT IS DETECTED
 *   nora_clock.h in this directory is a VENDORED, BYTE-IDENTICAL copy of
 *   dspic33ak-audio-dsp-sonora-dev:src/hal_clock/nora_clock.h at commit
 *   a7de05b on branch design/nora-clock-contract (repo
 *   sulaolab/dspic33ak-audio-dsp-sonora-dev).
 *   Byte-identical is the point rather than a convenience: it makes divergence a
 *   diff instead of a reading exercise, and it is checked by comparing the STORED
 *   blob on both sides -- not the working tree, because .gitattributes checks these
 *   files out with CRLF while the repository stores LF:
 *
 *     git rev-parse HEAD:src/hal_clock/nora_clock.h
 *     git -C ../dspic33ak-audio-dsp-sonora-dev rev-parse a7de05b:src/hal_clock/nora_clock.h
 *
 *   Both must print ed4d2f8de121e1179fc12479f06f23f5c1057d86.  Note that this repo
 *   is a CONSUMER of that file: a CK-side improvement to the contract goes to the AK
 *   owner and comes back as a new snapshot.  Editing the copy is how the two families
 *   quietly stop implementing the same contract.
 *
 * FIVE SENTENCES IN THAT COPY ARE THE AK BACKEND'S AND READ FALSE HERE
 *   They are left in place deliberately -- byte-identity is worth more than five
 *   sentences, and patching them locally would forfeit the check above.  Reported to
 *   the AK owner as a wording item so a later snapshot is family-neutral.  A reader
 *   of the copy should mentally substitute:
 *
 *     "the dsPIC33AK512MPS512 DFP 1.3.185 device facts"   the preamble's description
 *         of where the generic core's limits come from.  Here they come from the
 *         dsPIC33/PIC24 FRM "Oscillator Module with High-Speed PLL" DS70005255B
 *         section 8.0, transcribed in nora_clock_dspic33ck.c.
 *     "The CLKGEN blocks moved to nora_clock_dspic33ak.h"   true as history, and the
 *         reason it matters here is the sentence after it: CK has no CLKGEN, so there
 *         is no CK implementation of those calls to write.
 *     "nora_clock_dspic33ak_raw_capture()"   on this backend,
 *         nora_clock_dspic33ck_raw_capture(), below.
 *     "8 MHz on the dsPIC33AK parts this backend builds for"   8 MHz on the
 *         dsPIC33CK parts too (DS70005399D section 9.4); the tuning-scope statement
 *         around it applies here via OSCTUN.TUN[5:0] and is equally unwritten.
 *     "nora_clock_dspic33ak_diag_t"   on this backend,
 *         nora_clock_dspic33ck_diag_t, below.
 *
 * WHAT THIS SILICON DOES NOT HAVE, so that its absence is not read as an omission:
 *   - No PLL2.  The dsPIC33CK256MP508's Auxiliary PLL exists but is not a system-clock
 *     selection in the NOSC mux, so NORA_CLOCK_PLL_2 answers NORA_CLOCK_ERR_NOT_PRESENT
 *     rather than being mapped onto it.
 *   - No writable system divider on the paths this contract exposes.  CLKDIV.FRCDIV
 *     divides only the separate FRCDIVN oscillator SELECTION, which is why that
 *     selection is reported as NORA_CLOCK_SOURCE_FRC_DIVIDED and not as FRC.
 *   - No pre-switch PLL readiness bit.  OSCCON.LOCK cannot distinguish "not yet
 *     locked" from "not enabled" from "never will be" before the switch is requested
 *     (DS70005399D Register 9-1, and Register 28-5 note 1), so lock is waited for
 *     AFTER the request, and the backend does not invent a readiness predicate.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Backend detail code behind nora_clock_last_diag().  Print it; do not branch on it.
 *
 * The contract deliberately has no per-phase timeout status, so these values are
 * where "which phase of the sequence stalled" survives.  Separating the OSWEN wait
 * from the LOCK wait is the minimum that makes a NORA_CLOCK_ERR_TIMEOUT actionable:
 * a source that never starts and a PLL that never locks need different answers from
 * the person reading the console.
 */
typedef enum {
    NORA_CLOCK_DSPIC33CK_DIAG_NONE                 = 0,

    /* OSCCON.OSWEN never cleared: the requested oscillator did not take over.  The
     * usual cause is that it is not running, or that FCKSM disabled clock switching
     * so the request was ignored by the silicon. */
    NORA_CLOCK_DSPIC33CK_DIAG_OSWEN_TIMEOUT        = 1,

    /* The switch completed but OSCCON.LOCK never set: the part is on the PLL and the
     * PLL is not locked.  Its output frequency is undefined -- every divisor derived
     * from it is wrong, the console's first. */
    NORA_CLOCK_DSPIC33CK_DIAG_LOCK_TIMEOUT         = 2,

    /* nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_1, ...) with no successful
     * nora_clock_pll_configure() behind it.  This silicon folds the PLL's INPUT into
     * the same NOSC selection that names the PLL, so there is no NOSC value to write
     * for "the PLL" until something has said which input it runs from. */
    NORA_CLOCK_DSPIC33CK_DIAG_PLL_NOT_CONFIGURED   = 3,

    /* nora_clock_pll_configure() for the PLL that is currently driving the system
     * clock.  The contract refuses this; on this silicon the divider fields also
     * cannot be changed while the PLL is running, so the refusal is required rather
     * than merely tidy. */
    NORA_CLOCK_DSPIC33CK_DIAG_PLL_IN_USE           = 4,

    /* A caller-declared source's frequency was needed and has never been declared.
     * nora_clock_pll_configure() must divide it to reach the target, so it refuses;
     * nora_clock_switch_source() does not need it and does not. */
    NORA_CLOCK_DSPIC33CK_DIAG_INPUT_HZ_UNKNOWN     = 5,

    /* A nonzero input_hz contradicted a frequency this contract determines itself. */
    NORA_CLOCK_DSPIC33CK_DIAG_INPUT_HZ_CONFLICT    = 6,

    /* Preflight, frequency arm: the operating point the request would produce is
     * outside this part's limits.  Refused before the first register write. */
    NORA_CLOCK_DSPIC33CK_DIAG_FOSC_OUT_OF_RANGE    = 7,

    /* The switch sequence ran to completion and COSC did not move: the silicon DISCARDED
     * the request.  Two causes, and neither is visible in any status register -- FCKSM in
     * the configuration fuses disabled clock switching, or OSCCON.CLKLOCK is set.  Worth
     * its own code because the sequence itself reported success: without this check the
     * HAL would return NORA_CLOCK_OK for a switch that never happened, which is the
     * failure this backend was rewritten to stop being able to hide. */
    NORA_CLOCK_DSPIC33CK_DIAG_SWITCH_IGNORED       = 8
} nora_clock_dspic33ck_diag_t;

/*
 * The oscillator control words, as read in one pass.
 *
 * Raw on purpose: this is post-mortem and bring-up evidence, and a decoded struct
 * would drop whatever the decoder did not think to keep.  Everything that is a
 * QUESTION about the clock -- which source, running, locked, at what frequency --
 * is nora_clock_get_state() and needs none of this.
 */
typedef struct {
    uint16_t osccon;   /* COSC / NOSC / LOCK / OSWEN / CF / CLKLOCK */
    uint16_t clkdiv;   /* PLLPRE, FRCDIV, DOZE / DOZEN / ROI       */
    uint16_t pllfbd;   /* PLLFBDIV                                 */
    uint16_t plldiv;   /* POST1DIV / POST2DIV / VCODIV             */
} nora_clock_dspic33ck_capture_t;

/* One pass over the four words above.  Reads only; changes nothing. */
void nora_clock_dspic33ck_raw_capture(nora_clock_dspic33ck_capture_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CLOCK_DSPIC33CK_H */
