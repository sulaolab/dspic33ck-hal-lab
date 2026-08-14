#ifndef NORA_RESET_H
#define NORA_RESET_H

/*
 * Why this boot happened: RCON, decoded once for the whole family.
 *
 * WHAT THIS IS, AND WHAT IT IS NOT
 * --------------------------------
 * RCON's bits and their priority are a FAMILY fact -- a POR also sets BOR on this
 * family, a trap conflict is more specific than the watchdog that may also be set --
 * so deciding which one to name is not board knowledge and should not be written per
 * board. EV88G73A had the seven-way decoder; DM330030 had none and returned a
 * disclaimer string instead, so the shared console and the shared trap handler
 * (uart_app/system_console.c, app/app_traps.c) got a real answer on one board and an
 * apology on the other, through the same BOARD SEAM call (app/app_traps.h).
 *
 * WHAT STAYS WITH THE BOARD is the LATCH POLICY, and it stays because the two boards
 * genuinely differ:
 *
 *   EV88G73A latches RCON at the very top of its board_init() and clears the bits
 *       afterwards, so exactly one boot's worth of causes is present. It has to: with
 *       no reset button on a Curiosity Nano, telling a POR ("you power cycled it")
 *       from an SWR ("the *sr command worked") is the only evidence a software reset
 *       happened at all.
 *   DM330030 does not latch, and deliberately: nothing there clears RCON either, so
 *       the register still holds what the last reset set, and adding a latch-and-clear
 *       would change what a debugger sees on a board whose hardware verification is
 *       still deferred.
 *
 * That difference is exactly why the decoder takes `latched` rather than reading RCON
 * itself. Without a latch the bits ACCUMULATE across resets, and naming "the" cause
 * from an accumulation is a plausible-looking wrong answer -- the failure mode this
 * repo keeps finding in its own history. So a non-latching board says so, and gets a
 * decode that admits it rather than one that guesses.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * Portable reset snapshot API -------------------------------------------------
 *
 * The CK/AK-compatible family below makes the board's RCON policy explicit.
 * Call capture once, before clock/port/peripheral setup.  It returns true only
 * for the first valid capture after reset; later or invalid calls return false
 * without reading or clearing RCON.
 *
 * PRESERVE_RCON captures a raw diagnostic word but never names a cause: sticky
 * bits may be from an earlier reset when the preceding boot did not clear them.
 * AND_CLEAR_RCON captures, classifies, and clears the family reset-cause bits,
 * so its cause is authoritative for this boot.
 */
typedef enum {
    NORA_RESET_LATCH_PRESERVE_RCON   = 0,
    NORA_RESET_LATCH_AND_CLEAR_RCON  = 1,
} nora_reset_latch_policy_t;

typedef enum {
    NORA_RESET_CAUSE_UNKNOWN = 0,
    NORA_RESET_CAUSE_POWER_ON,
    NORA_RESET_CAUSE_BROWNOUT,
    NORA_RESET_CAUSE_EXTERNAL,
    NORA_RESET_CAUSE_SOFTWARE,
    NORA_RESET_CAUSE_WATCHDOG,
    NORA_RESET_CAUSE_OTHER,
} nora_reset_cause_t;

bool nora_reset_snapshot_capture(nora_reset_latch_policy_t policy);
bool nora_reset_snapshot_is_captured(void);
nora_reset_cause_t nora_reset_snapshot_cause(void);
uint32_t nora_reset_snapshot_raw(void);
const char *nora_reset_snapshot_cause_str(void);
bool nora_reset_snapshot_is_power_on_class(void);
bool nora_reset_snapshot_is_warm(void);

/*
 * Name the most specific cause in `latched`.
 *
 * `latched` must be a reset-cause word, either captured before anything could clear it
 * or read live. `is_latched` says which: pass false when the bits may have accumulated
 * over more than one reset, and the return value refuses to name a single cause when
 * more than one candidate is present rather than reporting the highest-priority one as
 * though it were the whole story.
 *
 * The parameter is uint32_t because that is the family-wide width -- the same width
 * nora_reset_snapshot_raw() returns, and the width AK's RCON actually has. On CK the
 * register is 16-bit, so only the low half is meaningful; the backend narrows it
 * itself rather than making every caller pass a cast. A caller may hand this the
 * 16-bit SFR directly and let it widen.
 *
 * Returns a pointer to a string literal -- never NULL, never a buffer, so it is safe to
 * hand straight to a console writer from a trap handler.
 */
const char *nora_reset_cause_str(uint32_t latched, bool is_latched);

/*
 * Clear every cause bit this family defines. Call immediately after capturing the word,
 * so the NEXT boot reports its own cause instead of an accumulation. Boards that do not
 * latch must not call this: leaving RCON alone is what makes a live read meaningful.
 */
void nora_reset_cause_clear(void);

#endif /* NORA_RESET_H */
