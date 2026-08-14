/*
 * nora_reset_dspic33ck.c -- RCON decoded once, with the latch policy left to the board.
 *
 * See the header for why the decode is shared and the latch is not. This file is
 * ev88g73a_reset_cause_str()'s seven-way ladder, made to state whether it is allowed to
 * name a single cause.
 */

#include "nora_reset.h"

#include <xc.h>

/*
 * Priority order, most specific first, and it matters: several bits can be set at once
 * even within one boot (a POR also sets BOR on this family). SWR is checked ahead of the
 * power-on causes because that is the bit a console reset command produces, and
 * mistaking it for a POR would defeat the point of decoding this at all.
 *
 * The table exists so the ladder and the "is more than one candidate set" test below
 * cannot disagree about what the candidates are -- the bug that a second hand-written
 * list invites.
 */
typedef struct {
    uint16_t    mask;
    const char *name;
} reset_cause_desc_t;

static const reset_cause_desc_t reset_causes[] = {
    { (uint16_t)(1u << _RCON_TRAPR_POSITION),  "TRAPR(trap conflict)" },
    { (uint16_t)(1u << _RCON_IOPUWR_POSITION), "IOPUWR(illegal opcode/uninit W)" },
    { (uint16_t)(1u << _RCON_WDTO_POSITION),   "WDTO(watchdog)" },
    { (uint16_t)(1u << _RCON_SWR_POSITION),    "SWR(software reset)" },
    { (uint16_t)(1u << _RCON_EXTR_POSITION),   "EXTR(MCLR)" },
    { (uint16_t)(1u << _RCON_POR_POSITION),    "POR(power-on)" },
    { (uint16_t)(1u << _RCON_BOR_POSITION),    "BOR(brown-out)" },
};

#define RESET_CAUSE_COUNT (sizeof reset_causes / sizeof reset_causes[0])

/* Snapshot state is deliberately separate from the CK legacy raw decoder. */
static bool                     s_snapshot_captured;
static uint32_t                 s_snapshot_raw;
static nora_reset_cause_t  s_snapshot_cause;

/*
 * Clear-policy decode.  Its power-event-first precedence is the CK/AK common
 * contract; nora_reset_cause_str() below retains the older CK diagnostic
 * priority for existing callers.
 *
 * THE TWO PRECEDENCES DIFFER, THEY DISAGREE IN PRACTICE, AND BOTH ARE RIGHT.
 * This one is power-event-first (POR, BOR, EXTR, SWR, WDTO); the legacy ladder
 * is most-specific-first (TRAPR, IOPUWR, WDTO, SWR, EXTR, POR, BOR).
 *
 * MEASURED, not hypothetical: a Curiosity Nano cold start reads RCON=0x0083 --
 * POR and BOR and EXTR all set, because MCLR is held while the supply comes up.
 * The legacy ladder names that EXTR(MCLR); this one names it POWER_ON.
 *
 * That is the intended answer in each case, because they answer different
 * questions.  The legacy string is a diagnostic: "which is the most specific
 * thing RCON can tell me", and it is the only decode that can say TRAPR, the
 * sole evidence of a stack overflow (see app_traps.c).  This one is a
 * classification an application BRANCHES on -- cold or warm -- and a supply that
 * has just come up is cold no matter what else was asserted during it.  Treating
 * that boot as warm is the error that matters: it would skip a codec
 * pre-shutdown on a genuinely cold start.
 *
 * So do not "fix" either order to match the other, and do not assume the two
 * agree.  The console prints both for exactly this reason.
 *
 * What WOULD be wrong is classifying under PRESERVE, where bits may have
 * accumulated across several resets and no precedence can be justified.  That
 * is why this function is only ever called on the AND_CLEAR path.
 */
static nora_reset_cause_t reset_snapshot_decode(uint16_t raw)
{
    if ((raw & (uint16_t)(1u << _RCON_POR_POSITION)) != 0u) {
        return NORA_RESET_CAUSE_POWER_ON;
    }
    if ((raw & (uint16_t)(1u << _RCON_BOR_POSITION)) != 0u) {
        return NORA_RESET_CAUSE_BROWNOUT;
    }
    if ((raw & (uint16_t)(1u << _RCON_EXTR_POSITION)) != 0u) {
        return NORA_RESET_CAUSE_EXTERNAL;
    }
    if ((raw & (uint16_t)(1u << _RCON_SWR_POSITION)) != 0u) {
        return NORA_RESET_CAUSE_SOFTWARE;
    }
    if ((raw & (uint16_t)(1u << _RCON_WDTO_POSITION)) != 0u) {
        return NORA_RESET_CAUSE_WATCHDOG;
    }
    return NORA_RESET_CAUSE_OTHER;
}

bool nora_reset_snapshot_capture(nora_reset_latch_policy_t policy)
{
    if ((s_snapshot_captured) ||
        ((policy != NORA_RESET_LATCH_PRESERVE_RCON) &&
         (policy != NORA_RESET_LATCH_AND_CLEAR_RCON))) {
        return false;
    }

    /* Read RCON once before any optional clear. */
    s_snapshot_raw = (uint32_t)RCON;
    s_snapshot_captured = true;
    s_snapshot_cause = NORA_RESET_CAUSE_UNKNOWN;

    if (policy == NORA_RESET_LATCH_AND_CLEAR_RCON) {
        s_snapshot_cause = reset_snapshot_decode((uint16_t)s_snapshot_raw);
        nora_reset_cause_clear();
    }

    return true;
}

bool nora_reset_snapshot_is_captured(void)
{
    return s_snapshot_captured;
}

nora_reset_cause_t nora_reset_snapshot_cause(void)
{
    return s_snapshot_cause;
}

uint32_t nora_reset_snapshot_raw(void)
{
    return s_snapshot_raw;
}

bool nora_reset_snapshot_is_power_on_class(void)
{
    return (s_snapshot_cause == NORA_RESET_CAUSE_POWER_ON) ||
           (s_snapshot_cause == NORA_RESET_CAUSE_BROWNOUT);
}

bool nora_reset_snapshot_is_warm(void)
{
    return !nora_reset_snapshot_is_power_on_class();
}

const char *nora_reset_snapshot_cause_str(void)
{
    switch (s_snapshot_cause) {
        case NORA_RESET_CAUSE_POWER_ON:  return "POR (power-on, cold)";
        case NORA_RESET_CAUSE_BROWNOUT:  return "BOR (brown-out, cold)";
        case NORA_RESET_CAUSE_EXTERNAL:  return "MCLR (external, warm)";
        case NORA_RESET_CAUSE_SOFTWARE:  return "SWR (software, warm)";
        case NORA_RESET_CAUSE_WATCHDOG:  return "WDT (watchdog, warm)";
        case NORA_RESET_CAUSE_OTHER:     return "OTHER (warm)";
        case NORA_RESET_CAUSE_UNKNOWN:
        default:                              return "UNKNOWN (warm)";
    }
}

const char *nora_reset_cause_str(uint32_t latched, bool is_latched)
{
    uint8_t i;
    uint8_t set_count = 0u;

    /*
     * The public contract carries the family-wide 32-bit width; this backend's RCON
     * is 16 bits. Narrowing here is what keeps those two facts from being mixed: the
     * contract does not shrink to CK's register, and no caller pays a cast to reach a
     * CK-width parameter. It also keeps the loop below in 16-bit arithmetic, which is
     * what this core is.
     */
    const uint16_t rcon = (uint16_t)latched;

    for (i = 0u; i < (uint8_t)RESET_CAUSE_COUNT; i++) {
        if ((rcon & reset_causes[i].mask) != 0u) {
            set_count++;
        }
    }

    if (set_count == 0u) {
        return "unknown";
    }

    /*
     * On a board that does not latch, two or more candidates means the bits have
     * accumulated across resets and the most specific one is NOT necessarily this
     * boot's cause. Say that instead of guessing; the raw word is still available to the
     * caller and is the honest evidence.
     *
     * A POR/BOR pair is the one case where two bits are normal even on a latching board,
     * which is why this test only applies when the caller admits it did not latch.
     */
    if (!is_latched && (set_count > 1u)) {
        return "(multiple bits set, not latched -- read the raw word)";
    }

    for (i = 0u; i < (uint8_t)RESET_CAUSE_COUNT; i++) {
        if ((rcon & reset_causes[i].mask) != 0u) {
            return reset_causes[i].name;
        }
    }

    return "unknown"; /* unreachable: set_count > 0 guarantees a hit above */
}

void nora_reset_cause_clear(void)
{
    RCONbits.POR    = 0;
    RCONbits.BOR    = 0;
    RCONbits.EXTR   = 0;
    RCONbits.SWR    = 0;
    RCONbits.WDTO   = 0;
    RCONbits.IOPUWR = 0;
    RCONbits.TRAPR  = 0;
}
