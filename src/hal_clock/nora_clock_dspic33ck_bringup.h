#ifndef NORA_CLOCK_DSPIC33CK_BRINGUP_H
#define NORA_CLOCK_DSPIC33CK_BRINGUP_H

/*
 * The boot-time clock stage: ask for an operating point, get told what you got.
 *
 * WHAT THIS IS, AND WHAT IT IS NOT
 * --------------------------------
 * nora_clock.h already offers the mechanisms -- configure a PLL, switch to a source,
 * observe what the hardware is doing. What every board then wrote for itself was the
 * POLICY around them, and both boards in this repo had written the same one:
 *
 *   - snapshot the oscillator registers as soon as the requested sequence has run and
 *     before any fallback, so the evidence is the state that the request produced and not
 *     the state the fallback left behind;
 *   - decide "on target" from the OBSERVED clock and not from a returned status -- a
 *     status of OK says the sequence ran, while the observed source, its lock and the
 *     achieved Fosc say the part is actually running where the board claims. Checking
 *     the status alone is precisely how a half-configured PLL passes for success;
 *   - never stay on a PLL that did not lock. Its output is undefined, so every derived
 *     divisor is wrong -- the console's above all -- and the board goes dark with no way
 *     to say why. Return to the known input source instead, whose frequency the HAL can
 *     derive, so the fallback console is at least coherent.
 *
 * That last rule was learned the hard way on EV88G73A (a silent half-speed clock
 * presenting as a garbled console) and then copied to DM330030 by hand. Copied policy is
 * policy that can drift, so it lives here now.
 *
 * WHY THE SEQUENCE IS TWO CALLS AND NOT ONE
 *   Configuring a PLL and selecting it are separate operations in the contract, and this
 *   is the file that joins them, once, in the one place where "and then run on it" is
 *   actually the intent. Before that separation the backend's configure call switched the
 *   clock as a side effect, so no caller could bring up a PLL without also being
 *   re-clocked, and a live clock event was hidden from the only party that knows what
 *   else is timed off it.
 *
 * It is in hal_clock/ and not in app/ because it speaks only the clock contract and this
 * family's own oscillator registers; no application concept appears in it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "nora_clock.h"

/* What the board asks for. This is the whole of a board's clock policy as data. */
typedef struct {
    nora_clock_source_t source;          /* what feeds the PLL, and the fallback  */
    uint32_t            input_hz;        /* that source's frequency               */
    uint32_t            target_fosc_hz;  /* wanted Fosc; == input_hz means no PLL */
} nora_clock_dspic33ck_bringup_t;

/*
 * What it got. Every field is evidence a debugger or a console command can read, and
 * boards publish them under their own names (g_<board>_clock_init_status and so on).
 */
typedef struct {
    nora_clock_status_t status;               /* what the failing call returned      */
    uint16_t            diag;                 /* nora_clock_dspic33ck_diag_t detail  */
    uint16_t            osccon_after_switch;  /* COSC = who won, LOCK = did it lock  */
    bool                on_target;            /* the observed-state verdict          */
} nora_clock_dspic33ck_bringup_result_t;

/*
 * Run the stage. Never fails in a way the caller must handle: every outcome is reported
 * through `out` and there is no return value to check. On any refusal it makes a
 * BEST-EFFORT return to `source` at `input_hz` -- best-effort because that recovery is a
 * clock switch too, and its result is not reported (see the .c for why). So
 * `on_target == false` means "not where we asked to be", not "now on `source`":
 * nora_clock_get_state() is the authority on what the part is actually running on.
 *
 * `target_fosc_hz == input_hz` asks for the source direct with no PLL. That used to be a
 * compile-time `#if` on DM330030 and a capability EV88G73A did not have at all; as a
 * run-time comparison it costs one branch and both boards gain it.
 */
void nora_clock_dspic33ck_bringup(const nora_clock_dspic33ck_bringup_t  *req,
                                  nora_clock_dspic33ck_bringup_result_t *out);

#endif /* NORA_CLOCK_DSPIC33CK_BRINGUP_H */
