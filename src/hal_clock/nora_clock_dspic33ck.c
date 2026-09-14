#include "nora_clock.h"
#include "nora_clock_dspic33ck.h"
#include "nora_clock_dspic33ck_reg.h"
#include "nora_clock_device_dspic33ck.h"

#include <stddef.h>

/*
 * Device PLL limits, verified against the dsPIC33/PIC24 Family Reference Manual
 * "Oscillator Module with High-Speed PLL", DS70005255B section 8.0 (Master PLL).
 * Every value below is a datasheet figure, not an estimate.
 *
 * FRM naming, mapped to this file: FPLLI is the raw PLL input, FPFD = FPLLI / N1
 * is the phase-detector input, N1 = PLLPRE, M = PLLFBDIV, N2 = POST1DIV and
 * N3 = POST2DIV. All of N1 (1-8), N2 (1-7), N3 (1-7) and M (16-200) hold the
 * divider value directly.
 */
#define PLLI_MIN_HZ         (8000000UL)     /* FPLLI / FPFD minimum  */
#define PLLI_MAX_HZ         (64000000UL)    /* FPLLI maximum         */
#define VCO_MIN_HZ          (400000000UL)   /* Fvco minimum          */
#define VCO_MAX_HZ          (1600000000UL)  /* Fvco maximum          */
/* FPFD has no fixed upper bound: the FRM caps it at Fvco/16, checked per
 * candidate in solve_pll() once that candidate's Fvco is known. */
#define PFD_MAX_VCO_RATIO   (16U)
/* FPLLO must not exceed 400 MHz (100 MIPS) as the Master core system clock, and
 * Fosc is FPLLO/2 (below), so Fosc caps at 200 MHz. */
#define FOSC_MAX_HZ         (200000000UL)   /* system Fosc maximum   */
#define PLLFBDIV_MIN        (16U)
#define PLLFBDIV_MAX        (200U)
#define PLLPRE_MIN          (1U)
#define PLLPRE_MAX          (8U)
#define POSTDIV_MIN         (1U)
#define POSTDIV_MAX         (7U)

/*
 * Fixed divide-by-2 between the PLL output (FPLLO) and the system clock (Fosc).
 *
 * The PLL postscalers produce FPLLO = FPFD * M / (N2 * N3), but FPLLO is NOT the
 * system clock: DS70005255B Figure 1-2 routes it through a fixed "/ 2" stage,
 * shown as FPLLO/2, before it reaches the S1/S3 inputs of the clock-switch mux.
 * So Fosc = FPLLO / 2, and since Fcy = Fosc / 2, Fcy = FPLLO / 4.
 *
 * The FRM's own numbers confirm the chain: it caps FPLLO at 400 MHz and calls
 * that "100 MIPS" (400/4), and its 8 MHz FRC example reaches 50 MIPS with
 * M 125 / N1 1 / N2 5 / N3 1, i.e. FPLLO = 200 MHz = 50 MIPS * 4.
 *
 * Omitting this stage is what a first bring-up gets wrong, and the failure is
 * quiet rather than loud: OSCCON.LOCK still reads 1 and COSC still shows the PLL,
 * because the PLL itself is fine -- only the part's speed is half of what the
 * solver recorded. First seen on dsPIC33CK64MC105/EV88G73A (2026-07-28) as a
 * console garbled at every baud, the recorded Fcy 100 MHz being twice the real
 * 50 MHz; confirmed by host baud sweep (clean only near 115200, half of the
 * intended 230400) before the FRM settled it.
 */
#define FPLLO_TO_FOSC_DIV   (2U)

typedef struct {
    uint16_t feedback_div;
    uint16_t pre_div;
    uint16_t post_div1;
    uint16_t post_div2;
} pll_solution_t;

/* ==========================================================================
 * State
 *
 * There is deliberately NO cached Fosc and no cached PLL frequency here. Every
 * frequency this file reports is computed from a register capture taken during the
 * call, because a fail-safe clock monitor can move the system clock with this HAL
 * uninvolved, and a cached answer is confidently wrong exactly then. What remains is
 * the two things the hardware genuinely does not hold.
 * ========================================================================== */

/*
 * 1. The caller-declared frequencies -- one store per logical source, shared by every
 *    API that names that source, as the contract requires. The silicon cannot measure
 *    an external oscillator, so if nobody declared it, it is unknown (0), and 0 stays
 *    unknown rather than becoming a guess.
 *
 *    BFRC and LPRC are here alongside PRIMARY even though they are on-chip: their data
 *    sheet figures are nominal, and the contract does not let a nominal number be
 *    promoted to exact HAL knowledge just because it is printed.
 */
static uint32_t g_declared_primary_hz;
static uint32_t g_declared_bfrc_hz;
static uint32_t g_declared_lprc_hz;

/*
 * 2. Which input the PLL was last successfully configured to run from.
 *
 *    THIS EXISTS FOR EXACTLY ONE PURPOSE: choosing the NOSC encoding of a later
 *    nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_1, ...). On this silicon the PLL's
 *    input is not a separate field -- FRCPLL and PRIPLL are different NOSC values -- so
 *    "switch to the PLL" is not a writable request until something says which input.
 *
 *    IT MUST NEVER REACH nora_clock_source_hz(). That function is a register readback,
 *    and the contract is explicit that a backend must not close the "0 for a PLL that
 *    was just configured" window by remembering the request: a function that is
 *    sometimes a readback and sometimes a record of intent gives the caller no way to
 *    tell which one it got, and the intent is the value that stays confidently wrong
 *    after the hardware moves. Selecting a mux encoding is not reporting a frequency,
 *    which is why this use is legitimate and that one is not.
 *
 *    Cleared before programming begins, so a FAILED configure cannot leave a stale
 *    input behind for a switch to act on.
 */
static nora_clock_source_t g_pll_input = NORA_CLOCK_SOURCE_UNKNOWN;

/* Backend detail behind the last non-OK status. See nora_clock_dspic33ck_diag_t. */
static uint16_t g_diag;

static nora_clock_status_t solve_pll(
    uint32_t input_hz,
    uint32_t target_hz,
    pll_solution_t *solution);

/* ==========================================================================
 * Frequency derivation -- all of it from a capture, none of it from memory
 * ========================================================================== */

/*
 * Fosc that this divider chain produces from a given PLL input, or 0 if that cannot be
 * computed. The divider fields come from a capture; the input frequency does not, because
 * this silicon does not always hold it (see the two callers).
 *
 * 0 whenever the input is unknown or a divider field reads 0 -- the latter is not a legal
 * programming, so the operating point it describes does not exist. Unknown, not a
 * division trap.
 *
 * 64-bit intermediate: a legal Fvco tops out at 1.6 GHz, but these are register contents
 * and nothing here has promised they are legal. Multiplying before dividing also keeps the
 * result exact for chains this HAL did not program.
 */
static uint32_t pll_output_from(uint32_t input_hz,
                               const nora_clock_dspic33ck_fields_t *f)
{
    if (input_hz == 0u || f->pllpre == 0u || f->post1div == 0u || f->post2div == 0u) {
        return 0u;
    }

    return (uint32_t)(((uint64_t)input_hz * f->pllfbdiv / f->pllpre) /
                      ((uint32_t)f->post1div * f->post2div * FPLLO_TO_FOSC_DIV));
}

/*
 * The PLL's output as its own registers presently describe it, or 0.
 *
 * The extra 0 case here is the one the contract calls out for this family: while the
 * system is NOT running from the PLL, this silicon exposes no PLL input select to read --
 * the selection IS the system-clock selection -- so the PLL's input, and therefore its
 * output, is unknown. That is the readback window, and it is not closed with remembered
 * intent.
 */
static uint32_t pll_output_hz(const nora_clock_dspic33ck_fields_t *f)
{
    uint32_t input_hz;

    switch (nora_clock_device_dspic33ck_cosc_pll_input(f->cosc)) {
    case NORA_CLOCK_SOURCE_FRC:
        input_hz = NORA_CLOCK_FRC_HZ;
        break;
    case NORA_CLOCK_SOURCE_PRIMARY:
        input_hz = g_declared_primary_hz;
        break;
    default:
        return 0u;
    }

    return pll_output_from(input_hz, f);
}

/*
 * The frequency of an arbitrary logical source, given a capture. Shared by
 * nora_clock_source_hz() and by the Fosc computation, so that "what is FRC" cannot have
 * two answers depending on which one you asked.
 */
static uint32_t source_hz_from(const nora_clock_dspic33ck_fields_t *f,
                               nora_clock_source_t source)
{
    switch (source) {
    case NORA_CLOCK_SOURCE_FRC:
        return NORA_CLOCK_FRC_HZ;

    /* FRCDIV is readable whether or not this selection is the one running, so its
     * frequency is derivable either way: it is what that selection WOULD deliver, and
     * that is a property of the divider, not of being selected. */
    case NORA_CLOCK_SOURCE_FRC_DIVIDED:
        return NORA_CLOCK_FRC_HZ /
               nora_clock_device_dspic33ck_frcdiv_divisor(f->frcdiv);

    case NORA_CLOCK_SOURCE_PRIMARY:
        return g_declared_primary_hz;
    case NORA_CLOCK_SOURCE_BFRC:
        return g_declared_bfrc_hz;
    case NORA_CLOCK_SOURCE_LPRC:
        return g_declared_lprc_hz;

    case NORA_CLOCK_SOURCE_PLL_1:
        return pll_output_hz(f);

    default:
        /* Including NORA_CLOCK_SOURCE_UNKNOWN: a source this backend cannot name has
         * no frequency it can report. */
        return 0u;
    }
}

/*
 * Apply the input_hz declaration rules for `source`, and hand back the frequency the
 * contract then knows for it (0 = unknown).
 *
 * Both APIs share this because the contract shares it: the CLASSIFICATION of input_hz
 * is common, and only the consequence of "unknown" differs -- which is why that
 * consequence is the caller's decision here and not this function's.
 */
static nora_clock_status_t declare_and_resolve(
    nora_clock_source_t source,
    uint32_t input_hz,
    const nora_clock_dspic33ck_fields_t *f,
    uint32_t *resolved_hz)
{
    uint32_t *slot;
    uint32_t known;

    switch (source) {
    case NORA_CLOCK_SOURCE_PRIMARY:
        slot = &g_declared_primary_hz;
        break;
    case NORA_CLOCK_SOURCE_BFRC:
        slot = &g_declared_bfrc_hz;
        break;
    case NORA_CLOCK_SOURCE_LPRC:
        slot = &g_declared_lprc_hz;
        break;
    default:
        /*
         * Contract-known, or not a source that can carry a declaration at all. A
         * nonzero input_hz is then a RESTATEMENT to be checked, never a value to store:
         * one of the two numbers would be wrong and this HAL must not silently pick.
         *
         * A PLL output whose frequency cannot presently be derived (0) is the one case
         * where a nonzero value is neither confirmed nor contradicted. It is accepted
         * and dropped -- not stored -- because storing it is precisely the "report
         * intent as a readback" that nora_clock_source_hz() forbids.
         */
        known = source_hz_from(f, source);
        if (input_hz != 0u && known != 0u && input_hz != known) {
            g_diag = (uint16_t)NORA_CLOCK_DSPIC33CK_DIAG_INPUT_HZ_CONFLICT;
            return NORA_CLOCK_ERR_INVALID_ARG;
        }
        *resolved_hz = known;
        return NORA_CLOCK_OK;
    }

    /* Caller-declared: a nonzero value REPLACES any earlier declaration -- re-declaring
     * is how a board says the hardware changed. A zero does not clear one: "I am not
     * restating it" is not "forget it". */
    if (input_hz != 0u) {
        *slot = input_hz;
    }
    *resolved_hz = *slot;
    return NORA_CLOCK_OK;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

nora_clock_status_t nora_clock_pll_configure(
    nora_clock_pll_t pll,
    const nora_clock_pll_config_t *config,
    uint32_t *resolved_hz)
{
    nora_clock_dspic33ck_fields_t f;
    nora_clock_dspic33ck_reg_pll_config_t reg_config;
    pll_solution_t solution;
    nora_clock_status_t status;
    uint32_t input_hz;
    uint16_t pll_nosc;

    g_diag = (uint16_t)NORA_CLOCK_DSPIC33CK_DIAG_NONE;

    if (pll == NORA_CLOCK_PLL_2) {
        /* The Auxiliary PLL is not a system-clock selection on this family, so PLL2 is
         * absent from the model rather than present-but-unsupported. */
        return NORA_CLOCK_ERR_NOT_PRESENT;
    }
    if (pll != NORA_CLOCK_PLL_1 || config == NULL) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    /* A PLL output is never a legal PLL input on any backend. Caught before the input
     * table so that the status is the contract's INVALID_ARG and not "unsupported
     * routing", which would suggest another part might allow it. */
    if (config->source == NORA_CLOCK_SOURCE_PLL_1 ||
        config->source == NORA_CLOCK_SOURCE_PLL_2) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    status = nora_clock_device_dspic33ck_pll_input_nosc(config->source, &pll_nosc);
    if (status != NORA_CLOCK_OK) {
        return status;
    }

    nora_clock_dspic33ck_reg_capture(NULL, &f);

    /*
     * The PLL driving the system clock may not be reconfigured. The contract refuses
     * this so the caller keeps the decision to re-source its own clock; on this silicon
     * the divider fields additionally must not be written while the PLL is running, so
     * the refusal is a hardware requirement and not only a policy.
     */
    if (nora_clock_device_dspic33ck_decode_cosc(f.cosc) == NORA_CLOCK_SOURCE_PLL_1) {
        g_diag = (uint16_t)NORA_CLOCK_DSPIC33CK_DIAG_PLL_IN_USE;
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    status = declare_and_resolve(config->source, config->input_hz, &f, &input_hz);
    if (status != NORA_CLOCK_OK) {
        return status;
    }

    /* Unlike a source switch, this call must DIVIDE the input to reach the target, so an
     * unknown input makes the request unanswerable. */
    if (input_hz == 0u) {
        g_diag = (uint16_t)NORA_CLOCK_DSPIC33CK_DIAG_INPUT_HZ_UNKNOWN;
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    status = solve_pll(input_hz, config->target_hz, &solution);
    if (status != NORA_CLOCK_OK) {
        return status;
    }

    reg_config.feedback_div = solution.feedback_div;
    reg_config.pre_div = solution.pre_div;
    reg_config.post_div1 = solution.post_div1;
    reg_config.post_div2 = solution.post_div2;

    /* Cleared BEFORE the first write: from here on the PLL is neither what it was nor
     * yet what was asked for, and a switch must not be able to select it on the strength
     * of an input this call may fail to finish establishing. */
    g_pll_input = NORA_CLOCK_SOURCE_UNKNOWN;

    status = nora_clock_dspic33ck_reg_pll_program(&reg_config);
    if (status != NORA_CLOCK_OK) {
        return status;
    }

    g_pll_input = config->source;

    /*
     * NO CLOCK SWITCH HERE. The contract separates programming a PLL from selecting it,
     * and this backend used to do both in this function -- which meant a caller could
     * not bring up a PLL without also being re-clocked, and the "configure" name hid a
     * live clock event from the one party that knows what else is timed off it.
     */

    if (resolved_hz != NULL) {
        /* Written only after programming succeeded. The solver accepts exact solutions
         * only, so this is config->target_hz -- an output for the caller's record, not a
         * value to test for inequality. */
        *resolved_hz = config->target_hz;
    }

    return NORA_CLOCK_OK;
}

nora_clock_status_t nora_clock_switch_source(
    nora_clock_source_t source,
    uint32_t input_hz)
{
    nora_clock_dspic33ck_fields_t f;
    nora_clock_source_t observed;
    nora_clock_status_t status;
    uint32_t resolved_hz;
    uint16_t nosc;
    bool is_pll = (source == NORA_CLOCK_SOURCE_PLL_1);

    g_diag = (uint16_t)NORA_CLOCK_DSPIC33CK_DIAG_NONE;

    /* Is this a destination at all? PLL_1 is one, but not a plain mux selection, so it
     * does not go through the encoder. */
    if (!is_pll) {
        status = nora_clock_device_dspic33ck_encode_source(source, &nosc);
        if (status != NORA_CLOCK_OK) {
            return status;
        }
    }

    nora_clock_dspic33ck_reg_capture(NULL, &f);

    status = declare_and_resolve(source, input_hz, &f, &resolved_hz);
    if (status != NORA_CLOCK_OK) {
        return status;
    }

    /*
     * SAME SOURCE = A DECLARATION UPDATE, NOT A CLOCK SWITCH.
     *
     * Judged from what the hardware is OBSERVED to report, never from what this HAL last
     * programmed: a fail-safe monitor that moved the part elsewhere must not be able to
     * turn a genuine recovery switch into a silent no-op. UNKNOWN is never "the same
     * source" -- not being able to name the current source is not having established
     * that no transition is needed.
     *
     * For PLL_1 this also covers the case where the part reports the PLL while
     * g_pll_input names a different input, or none: it is still the same SOURCE, so it
     * is still a no-op. Re-issuing the switch to make the record agree would be exactly
     * the PLL-mode-to-PLL-mode transition this silicon prohibits.
     */
    observed = nora_clock_device_dspic33ck_decode_cosc(f.cosc);
    if (observed == source && source != NORA_CLOCK_SOURCE_UNKNOWN) {
        return NORA_CLOCK_OK;
    }

    if (is_pll) {
        /*
         * "The PLL" is not writable as a destination until something has said which
         * input it runs from, because that choice IS the NOSC value. Only a successful
         * nora_clock_pll_configure() sets it.
         *
         * Note what is NOT checked here: whether the PLL's dividers are sensible. This
         * part's reset values (M=150, N1=1, POST1=4, POST2=1) are a perfectly ordinary
         * 150 MHz configuration, so "never configured" is indistinguishable from
         * "configured" in the registers, and inventing a check would only produce a
         * confident answer to a question the silicon does not answer.
         */
        if (g_pll_input == NORA_CLOCK_SOURCE_UNKNOWN) {
            g_diag = (uint16_t)NORA_CLOCK_DSPIC33CK_DIAG_PLL_NOT_CONFIGURED;
            return NORA_CLOCK_ERR_INVALID_ARG;
        }
        status = nora_clock_device_dspic33ck_pll_input_nosc(g_pll_input, &nosc);
        if (status != NORA_CLOCK_OK) {
            return status;
        }
        /* The frequency the request would land on is the PLL's, which the capture cannot
         * yet derive (its input select is the selection being requested), so it is
         * computed from the input this HAL just programmed. Intent is allowed to pick a
         * mux encoding and to be checked against a limit; it is never REPORTED. */
        resolved_hz = pll_output_from(source_hz_from(&f, g_pll_input), &f);
    }

    /*
     * PREFLIGHT, FREQUENCY ARM: do not begin a transition this silicon cannot legally
     * complete. Refused before the first register write, so the clock is left exactly as
     * it was.
     *
     * An unknown frequency removes only this arm -- an operating point that cannot be
     * computed cannot be checked -- and is NOT a reason to refuse the switch. A caller
     * forced to invent a plausible number to get a legal operation performed would have
     * every derived frequency wrong and authoritative-looking.
     */
    if (resolved_hz > FOSC_MAX_HZ) {
        g_diag = (uint16_t)NORA_CLOCK_DSPIC33CK_DIAG_FOSC_OUT_OF_RANGE;
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    /*
     * PREFLIGHT, TRANSITION-LEGALITY ARM: this silicon prohibits a direct PLL mode ->
     * PLL mode transition, and there is deliberately no check for it here. It is
     * unreachable through this contract rather than merely unlikely: the only requestable
     * PLL destination is PLL_1, and if the part is already on a PLL then COSC decodes to
     * PLL_1 and the same-source no-op above has already returned OK. Writing the check
     * would mean writing code that cannot execute, and a reader would have to work out
     * why it never fires. If a second PLL destination is ever added to this family's
     * backend, this is where the arm goes.
     */

    status = nora_clock_dspic33ck_reg_switch(nosc, is_pll, &g_diag);
    if (status != NORA_CLOCK_OK) {
        return status;
    }

    /*
     * Confirm from the hardware that the selection actually moved.
     *
     * The sequence completing is not the same as the switch happening: if FCKSM disabled
     * clock switching in the configuration fuses, or CLKLOCK is set, the silicon
     * DISCARDS the OSWEN request and OSWEN reads back 0 at once -- so every wait above
     * is satisfied while COSC never moves. That is not a hypothetical: it is what
     * boards/dm330030/config_bits.c did until 2026-08-10, and the pre-NORA version of
     * this HAL returned OK and cached the frequency it had been ASKED for, so the board
     * would have run on the wrong clock with every consumer believing otherwise.
     *
     * Reported as ERR_TIMEOUT because the requested transition did not complete, with
     * the diag distinguishing it from a source that never started.
     */
    nora_clock_dspic33ck_reg_capture(NULL, &f);
    if (nora_clock_device_dspic33ck_decode_cosc(f.cosc) != source) {
        g_diag = (uint16_t)NORA_CLOCK_DSPIC33CK_DIAG_SWITCH_IGNORED;
        return NORA_CLOCK_ERR_TIMEOUT;
    }

    return NORA_CLOCK_OK;
}

nora_clock_status_t nora_clock_get_state(nora_clock_state_t *out)
{
    nora_clock_dspic33ck_fields_t f;

    if (out == NULL) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    /* SINGLE PASS: every field below comes from one capture, so a caller cannot be
     * handed a combination the hardware was never in. Not atomic, and the contract does
     * not claim it is -- no interrupt is disabled. */
    nora_clock_dspic33ck_reg_capture(NULL, &f);

    out->source = nora_clock_device_dspic33ck_decode_cosc(f.cosc);

    /* COSC names the source that is driving the CPU this instruction, so naming it is
     * the hardware reporting it as running. */
    out->ready = (out->source != NORA_CLOCK_SOURCE_UNKNOWN);

    if (out->source == NORA_CLOCK_SOURCE_UNKNOWN) {
        out->locked = false;      /* cannot establish lock for a source we cannot name */
    } else if (out->source == NORA_CLOCK_SOURCE_PLL_1) {
        out->locked = f.lock;
    } else {
        out->locked = true;       /* nothing to lock */
    }

    out->fosc_hz = source_hz_from(&f, out->source);
    return NORA_CLOCK_OK;
}

uint32_t nora_clock_get_fosc_hz(void)
{
    nora_clock_state_t state;

    /* Literally the same computation as nora_clock_get_state().fosc_hz -- one code path,
     * so the two cannot drift into two truths. */
    if (nora_clock_get_state(&state) != NORA_CLOCK_OK) {
        return 0u;
    }
    return state.fosc_hz;
}

uint32_t nora_clock_get_fcy_hz(void)
{
    /* Fcy = Fosc / 2 on this family, unconditionally. 0 stays 0: unknown Fosc is
     * unknown Fcy, not 0 Hz. */
    return nora_clock_get_fosc_hz() / 2u;
}

uint32_t nora_clock_source_hz(nora_clock_source_t source)
{
    nora_clock_dspic33ck_fields_t f;

    nora_clock_dspic33ck_reg_capture(NULL, &f);
    return source_hz_from(&f, source);
}

uint16_t nora_clock_last_diag(void)
{
    return g_diag;
}

/* ==========================================================================
 * Local helpers
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * Solve PLLPRE / PLLFBDIV / POST1DIV / POST2DIV for an exact Fosc target.
 *
 * Fvco = (Fin / PLLPRE) * PLLFBDIV,  Fosc = Fvco / (POST1DIV * POST2DIV * 2).
 *
 * A given Fosc target usually has several exact solutions that differ only in
 * where they place the VCO. This picks the one with the HIGHEST Fvco rather than
 * the first one found, for two reasons:
 *
 *   - Jitter. A higher VCO frequency divided down gives better phase-noise
 *     margin, which is why Microchip's own configurations run the VCO near the
 *     top of its range.
 *   - Margin against the limits above. First-found tends to land on the lowest
 *     legal Fvco, i.e. hard against VCO_MIN_HZ. Sitting exactly on a limit is the
 *     worst place to be: measured on dsPIC33CK64MC105, a target of 200 MHz solved
 *     first-found to Fvco 400 MHz and the part ran at roughly 55 MHz Fcy instead
 *     of 100 MHz, i.e. the PLL never locked properly.
 *
 * What max-Fvco actually selects here, read back from the registers on
 * EV88G73A (2026-08-10): 8 MHz FRC to 200 MHz Fosc solves to PLLPRE 1 /
 * PLLFBDIV 200 / POST1DIV 2 / POST2DIV 2 -- Fvco 1600 MHz at the top of its
 * range, FPLLO 400 MHz, Fosc 200 MHz, Fcy 100 MHz.
 *
 * BEWARE OF QUOTED DIVIDER RECIPES: whether a source counts the fixed FPLLO/2
 * changes every POST value it lists. The comment this replaces cited a vendor
 * "100 MIPS from FRC" recipe as 200 / POST1 4 / POST2 2, which under THIS file's
 * convention (see FPLLO_TO_FOSC_DIV) is Fcy 50 MHz, not 100 -- so it was either
 * quoting the other convention or misremembered. It is dropped rather than
 * corrected because this bench cannot check it; the measured solution above is
 * what this solver produces and can be re-derived from the two frequencies.
 * -------------------------------------------------------------------------- */
static nora_clock_status_t solve_pll(
    uint32_t input_hz,
    uint32_t target_hz,
    pll_solution_t *solution)
{
    uint16_t pre_div;
    uint16_t post_div1;
    uint16_t post_div2;
    uint64_t best_vco = 0u;

    if (target_hz == 0u) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    if ((uint64_t)target_hz > FOSC_MAX_HZ) {
        return NORA_CLOCK_ERR_UNREPRESENTABLE;
    }

    /* FPLLI is the raw PLL input, before N1. Bounded independently of FPFD. */
    if (input_hz < PLLI_MIN_HZ || input_hz > PLLI_MAX_HZ) {
        return NORA_CLOCK_ERR_UNREPRESENTABLE;
    }

    for (pre_div = PLLPRE_MIN; pre_div <= PLLPRE_MAX; pre_div++) {
        /* FPFD, the phase-detector input: FPLLI scaled down by N1. */
        const uint64_t pfd_hz = (uint64_t)input_hz / pre_div;

        if (((uint64_t)input_hz % pre_div) != 0u) {
            continue;
        }
        if (pfd_hz < PLLI_MIN_HZ) {
            continue;
        }

        for (post_div1 = POSTDIV_MIN; post_div1 <= POSTDIV_MAX; post_div1++) {
            for (post_div2 = POSTDIV_MIN; post_div2 <= POSTDIV_MAX; post_div2++) {
                const uint64_t post_product = (uint64_t)post_div1 * post_div2 *
                                              FPLLO_TO_FOSC_DIV;
                const uint64_t numerator = (uint64_t)target_hz * post_product;
                uint64_t feedback_div;
                uint64_t vco;

                /* Keep POST1DIV >= POST2DIV per the recommended ordering. */
                if (post_div1 < post_div2) {
                    continue;
                }
                if ((numerator % pfd_hz) != 0u) {
                    continue;
                }

                feedback_div = numerator / pfd_hz;
                if (feedback_div < PLLFBDIV_MIN || feedback_div > PLLFBDIV_MAX) {
                    continue;
                }

                vco = pfd_hz * feedback_div;
                if (vco < VCO_MIN_HZ || vco > VCO_MAX_HZ) {
                    continue;
                }

                /* FPFD's upper bound is Fvco/16, so it can only be applied once
                 * this candidate's Fvco is known -- hence here and not with the
                 * FPFD minimum above. */
                if ((vco / PFD_MAX_VCO_RATIO) < pfd_hz) {
                    continue;
                }

                /* Keep the highest-Fvco candidate, not the first one found. */
                if (vco <= best_vco) {
                    continue;
                }

                best_vco = vco;
                solution->feedback_div = (uint16_t)feedback_div;
                solution->pre_div = pre_div;
                solution->post_div1 = post_div1;
                solution->post_div2 = post_div2;
            }
        }
    }

    if (best_vco == 0u) {
        return NORA_CLOCK_ERR_UNREPRESENTABLE;
    }

    return NORA_CLOCK_OK;
}
