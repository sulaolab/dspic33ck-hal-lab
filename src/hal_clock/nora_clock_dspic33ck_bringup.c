/*
 * nora_clock_dspic33ck_bringup.c -- the boot clock stage, once, with its policy.
 *
 * See the header for the rules and where they were learned. This file is the merge of
 * ev88g73a_clock_init() and dm330030_clock_init(), which differed in their two
 * frequencies and in DM330030 having a compile-time no-PLL arm.
 */

#include "nora_clock_dspic33ck_bringup.h"

#include <stddef.h>

#include "nora_clock_dspic33ck.h"

/*
 * The verdict, from what the hardware says rather than from what any call returned.
 *
 * This is the part that used to be unable to fail. The pre-NORA HAL reported the Fosc it
 * had been ASKED for, so "achieved Fosc == target" compared a request against itself and
 * was true no matter what the silicon did -- which is how a board with clock switching
 * disabled in its fuses reported a successful 200 MHz bring-up while running on an 8 MHz
 * oscillator. Fosc is now derived from the oscillator registers, so the comparison is a
 * real one.
 */
static bool on_target(const nora_clock_dspic33ck_bringup_t *req,
                      nora_clock_source_t expected_source)
{
    nora_clock_state_t state;

    if (nora_clock_get_state(&state) != NORA_CLOCK_OK) {
        return false;
    }

    return (state.source == expected_source) &&
           state.locked &&
           (state.fosc_hz == req->target_fosc_hz);
}

void nora_clock_dspic33ck_bringup(const nora_clock_dspic33ck_bringup_t  *req,
                                  nora_clock_dspic33ck_bringup_result_t *out)
{
    nora_clock_dspic33ck_capture_t capture;

    if ((req == NULL) || (out == NULL)) {
        return;
    }

    /*
     * The source direct, no PLL. A run-time comparison, not a build-time one: see the
     * header.
     */
    if (req->target_fosc_hz == req->input_hz) {
        out->status = nora_clock_switch_source(req->source, req->input_hz);
        out->diag   = nora_clock_last_diag();

        nora_clock_dspic33ck_raw_capture(&capture);
        out->osccon_after_switch = capture.osccon;

        out->on_target = (out->status == NORA_CLOCK_OK) && on_target(req, req->source);
        return;
    }

    {
        const nora_clock_pll_config_t pll_cfg = {
            .source    = req->source,
            .input_hz  = req->input_hz,
            .target_hz = req->target_fosc_hz,
        };

        out->status = nora_clock_pll_configure(NORA_CLOCK_PLL_1, &pll_cfg, NULL);
    }

    if (out->status == NORA_CLOCK_OK) {
        /*
         * And now run on it -- the second half of the sequence the contract separates.
         * input_hz is 0 because the PLL's frequency is not the caller's to declare: it is
         * whatever the dividers just programmed produce, and this backend derives it.
         */
        out->status = nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_1, 0u);
    }

    /* Whichever of the two calls failed is the one `status` describes, and its diag with
     * it -- captured before the fallback, which would otherwise overwrite both. */
    out->diag = nora_clock_last_diag();

    /* Snapshot BEFORE any fallback: taking it afterwards would overwrite the evidence
     * with the fallback's own state. */
    nora_clock_dspic33ck_raw_capture(&capture);
    out->osccon_after_switch = capture.osccon;

    out->on_target = (out->status == NORA_CLOCK_OK) &&
                     on_target(req, NORA_CLOCK_SOURCE_PLL_1);

    if (!out->on_target) {
        /* Never stay on a PLL that did not lock. This is a BEST-EFFORT recovery: its result
         * is deliberately not reported, because `status` above must keep describing the
         * request that was made, and `on_target` already says we are not where we asked to
         * be. So the fallback itself is not evidenced here -- osccon_after_switch is the
         * state BEFORE it, and nora_clock_get_state() is the only authority on the clock
         * actually running afterwards. */
        (void)nora_clock_switch_source(req->source, req->input_hz);
    }
}
