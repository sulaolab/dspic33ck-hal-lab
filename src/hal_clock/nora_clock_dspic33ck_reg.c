#include "nora_clock_dspic33ck_reg.h"

#include <stddef.h>

#include <xc.h>

/*
 * Bounded polling budget for switch / lock completion. Large enough to cover a
 * worst-case PLL lock at low input frequencies, small enough to fail fast if a source
 * never starts. Counted in iterations rather than time on purpose: the clock the loop
 * runs on is the thing being changed, so there is no stable unit of time to wait in.
 */
#define NORA_CLOCK_DSPIC33CK_POLL_LIMIT (1000000UL)

#define WAIT_CLEAR(EXPR, DIAG_CODE)                              \
    do {                                                         \
        uint32_t poll_count = NORA_CLOCK_DSPIC33CK_POLL_LIMIT;   \
        while ((EXPR) != 0u) {                                    \
            if (--poll_count == 0u) {                             \
                *diag = (uint16_t)(DIAG_CODE);                    \
                return NORA_CLOCK_ERR_TIMEOUT;                    \
            }                                                     \
        }                                                         \
    } while (0)

#define WAIT_SET(EXPR, DIAG_CODE)                                \
    do {                                                         \
        uint32_t poll_count = NORA_CLOCK_DSPIC33CK_POLL_LIMIT;   \
        while ((EXPR) == 0u) {                                    \
            if (--poll_count == 0u) {                             \
                *diag = (uint16_t)(DIAG_CODE);                    \
                return NORA_CLOCK_ERR_TIMEOUT;                    \
            }                                                     \
        }                                                         \
    } while (0)

/* --------------------------------------------------------------------------
 * Capture
 *
 * Four SFR reads, then all decoding from the copies. Decoded through the DFP's own
 * bitfield types rather than local shift/mask constants: the layout is then the DFP's
 * statement about this part, not a second copy of it in this file that a different
 * dsPIC33CK variant could quietly contradict.
 * -------------------------------------------------------------------------- */
void nora_clock_dspic33ck_reg_capture(
    nora_clock_dspic33ck_capture_t *raw,
    nora_clock_dspic33ck_fields_t *fields)
{
    union { uint16_t w; OSCCONBITS b; } osccon;
    union { uint16_t w; CLKDIVBITS b; } clkdiv;
    union { uint16_t w; PLLFBDBITS b; } pllfbd;
    union { uint16_t w; PLLDIVBITS b; } plldiv;

    osccon.w = OSCCON;
    clkdiv.w = CLKDIV;
    pllfbd.w = PLLFBD;
    plldiv.w = PLLDIV;

    if (raw != NULL) {
        raw->osccon = osccon.w;
        raw->clkdiv = clkdiv.w;
        raw->pllfbd = pllfbd.w;
        raw->plldiv = plldiv.w;
    }

    if (fields != NULL) {
        fields->cosc     = (uint16_t)osccon.b.COSC;
        fields->nosc     = (uint16_t)osccon.b.NOSC;
        fields->oswen    = (osccon.b.OSWEN != 0u);
        fields->lock     = (osccon.b.LOCK != 0u);
        fields->cf       = (osccon.b.CF != 0u);
        fields->clklock  = (osccon.b.CLKLOCK != 0u);
        fields->pllpre   = (uint16_t)clkdiv.b.PLLPRE;
        fields->frcdiv   = (uint16_t)clkdiv.b.FRCDIV;
        fields->pllfbdiv = (uint16_t)pllfbd.b.PLLFBDIV;
        fields->post1div = (uint16_t)plldiv.b.POST1DIV;
        fields->post2div = (uint16_t)plldiv.b.POST2DIV;
    }
}

/*
 * The public raw capture (nora_clock_dspic33ck.h). Same pass, without the decode: it is
 * this layer's job because it is this layer's registers.
 */
void nora_clock_dspic33ck_raw_capture(nora_clock_dspic33ck_capture_t *out)
{
    if (out != NULL) {
        nora_clock_dspic33ck_reg_capture(out, NULL);
    }
}

/* --------------------------------------------------------------------------
 * Program the PLL divider chain
 *
 * The CK PLLPRE / PLLFBDIV / POST1DIV / POST2DIV bitfields hold the divider and
 * multiplier values directly, so solved values are written as-is.
 *
 * No ordering constraint between the four writes: this is only ever reached while the
 * PLL is not driving the system clock, so no intermediate combination is ever a clock
 * the part is running on. That is established by the caller and is the reason this
 * function does not switch.
 * -------------------------------------------------------------------------- */
nora_clock_status_t nora_clock_dspic33ck_reg_pll_program(
    const nora_clock_dspic33ck_reg_pll_config_t *config)
{
    if (config == NULL) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    CLKDIVbits.PLLPRE = config->pre_div;
    PLLFBDbits.PLLFBDIV = config->feedback_div;
    PLLDIVbits.POST1DIV = config->post_div1;
    PLLDIVbits.POST2DIV = config->post_div2;

    return NORA_CLOCK_OK;
}

/* --------------------------------------------------------------------------
 * Request a clock switch and wait for completion
 *
 * OSCCONH<2:0> holds NOSC and OSCCONL<0> holds OSWEN; both are written through the
 * __builtin_write_OSCCONx unlock helpers, which emit the byte-write unlock sequence the
 * silicon requires. The hardware clears OSWEN when the switch completes, and sets LOCK
 * when a selected PLL has locked.
 *
 * If FCKSM disabled clock switching in the configuration fuses, the OSWEN write is
 * discarded by the silicon and OSWEN reads back 0 immediately -- so the switch "succeeds"
 * here while COSC never moves. That is why the caller's verdict comes from a fresh
 * capture and not from this return value: this function reports that the sequence ran,
 * not that the clock is what was asked for.
 * -------------------------------------------------------------------------- */
nora_clock_status_t nora_clock_dspic33ck_reg_switch(
    uint16_t nosc,
    bool wait_lock,
    uint16_t *diag)
{
    if (diag == NULL) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }
    *diag = (uint16_t)NORA_CLOCK_DSPIC33CK_DIAG_NONE;

    __builtin_write_OSCCONH((uint8_t)(nosc & 0x07u));
    __builtin_write_OSCCONL((uint8_t)0x01u);   /* OSWEN = 1 : request switch */

    WAIT_CLEAR(OSCCONbits.OSWEN, NORA_CLOCK_DSPIC33CK_DIAG_OSWEN_TIMEOUT);

    if (wait_lock) {
        WAIT_SET(OSCCONbits.LOCK, NORA_CLOCK_DSPIC33CK_DIAG_LOCK_TIMEOUT);
    }

    return NORA_CLOCK_OK;
}
