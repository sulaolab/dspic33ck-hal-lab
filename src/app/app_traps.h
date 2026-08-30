#ifndef APP_TRAPS_H
#define APP_TRAPS_H

/*
 * app_traps.h -- the device's hardware trap vectors, once, for every board this
 * repo builds, plus the boot-time report of a trap that happened BEFORE this boot.
 *
 * WAS TWO FILES
 * -------------
 * boards/ev88g73a/ev88g73a_traps.c and boards/dm330030/traps.c. Between them they
 * had the same eight vectors clearing the same flags on the same device family,
 * and differed in exactly one thing: what to do afterwards. Nothing else in either
 * file was board knowledge -- INTCON1/3/4, the trap flags and the RESET instruction
 * are dsPIC33CK facts, not EV88G73A or DM330030 facts. So the vectors live here and
 * the one real difference is a policy switch.
 *
 * THE TWO POLICIES, AND WHY BOTH ARE RIGHT SOMEWHERE
 * --------------------------------------------------
 *   APP_TRAPS_POLICY_LATCH_RESET  latch the cause to persistent RAM, then reset,
 *                                 so the next boot can print it.
 *   APP_TRAPS_POLICY_SPIN         latch the cause, then halt.
 *
 *   - EV88G73A is a Curiosity Nano with NO RESET BUTTON, usually driven over a
 *     console with no debugger attached. A spin there is an invisible hang, which
 *     was misread as a dead COM port, a hung firmware and a DSP overload in turn
 *     before the trap handlers existed. LATCH_RESET turns that into a printed line.
 *   - DM330030 is a full board normally used WITH a debugger, where halting
 *     preserves the live machine state for inspection -- strictly more information
 *     than a reset-and-report, provided someone is attached to look. Its hardware
 *     verification is deferred by agreement, so its behaviour is deliberately
 *     unchanged by this consolidation: SPIN is what MCC's original handlers did.
 *
 * There is NO DEFAULT. The policy is set per configuration in
 * firmware.X/nbproject/configurations.xml, next to DSPIC33CK_BOARD_*, and a
 * configuration that fails to set one does not compile. A default would have been
 * worse than an #error here: whichever value it took, one of the two boards would
 * have silently changed behaviour the day someone added a third configuration.
 *
 * TWO HALVES, BECAUSE A TRAP CANNOT ALWAYS PRINT
 * ----------------------------------------------
 * 1. The handlers run in trap context. The machine state there is already suspect
 *    -- that is what a trap means -- so they do the least they can: latch which
 *    trap fired and the raw INTCON1/3/4 words into persistent RAM, then apply the
 *    policy. They do NOT print. Printing from a trap means calling the UART with an
 *    unknown stack and a possibly-corrupt W-register set, and a printf that faults
 *    inside a trap handler loses the evidence entirely.
 *
 * 2. app_traps_report_previous() runs from normal code during boot, reads that
 *    latch, and prints it. By then the machine is sane and the console is up, so
 *    the report is trustworthy.
 *
 * This is the same division the reset-cause reporting already uses (RCON latched
 * early by board init, printed later by the banner), and it composes with it: a
 * trap-induced reset shows up as SWR in the reset cause AND as a trap record here,
 * which together say "the firmware trapped and restarted" rather than leaving a
 * bare SWR that looks like someone typed *sr.
 *
 * WHAT THIS NEEDS FROM THE BOARD
 * ------------------------------
 * Four functions, declared in the BOARD SEAM section at the bottom of this file and
 * implemented once per board. One is what these handlers themselves need (the latched
 * reset cause); the other three serve the console commands that report and provoke
 * traps. Everything else here is device-level.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Policy values. Compared, not tested for truth, so neither may be 0 -- an
 * undefined APP_TRAPS_POLICY would otherwise compare equal to whichever one was
 * zero and pick a behaviour by accident. */
#define APP_TRAPS_POLICY_LATCH_RESET 1
#define APP_TRAPS_POLICY_SPIN        2

#if !defined(APP_TRAPS_POLICY)
#error "APP_TRAPS_POLICY is not defined. Set it per configuration in firmware.X/nbproject/configurations.xml (preprocessor-macros, beside DSPIC33CK_BOARD_*): APP_TRAPS_POLICY=1 for latch-and-reset (a console board with no debugger), 2 for spin (a board used with a debugger attached). See app/app_traps.h for which board wants which and why there is no default."
#endif

#if (APP_TRAPS_POLICY != APP_TRAPS_POLICY_LATCH_RESET) && \
    (APP_TRAPS_POLICY != APP_TRAPS_POLICY_SPIN)
#error "APP_TRAPS_POLICY has an unrecognised value. Use 1 (latch-and-reset) or 2 (spin)."
#endif

/*
 * Trap-test console commands (*xa / *xm / *xs), implemented in uart_app/traps_console.c.
 *
 * On by default BECAUSE a trap handler is code that only runs when something has
 * already gone wrong -- the kind that rots unnoticed unless it can be fired on
 * demand. Without them, "the trap handler works" is an assumption; with them it is a
 * thirty-second console check. Set to 0 for a shipping image.
 *
 * Declared here rather than in console_traps.c because the help text in
 * console_task.c has to agree with what is actually compiled in.
 */
#ifndef APP_TRAP_TEST_CMDS
#define APP_TRAP_TEST_CMDS 1
#endif

/* Which trap fired. Ordered as the vector table is, so the value is also a hint
 * about where to look in the datasheet's trap-priority list. */
typedef enum {
    APP_TRAP_NONE = 0,
    APP_TRAP_OSC_FAIL,        /* clock failed -- FSCM caught a dead oscillator */
    APP_TRAP_ADDRESS_ERROR,   /* unimplemented address (NOT misalignment -- measured) */
    APP_TRAP_STACK_ERROR,     /* W15 outside the stack limits: overflow */
    APP_TRAP_MATH_ERROR,      /* divide-by-zero, or an accumulator overflow */
    APP_TRAP_HARD_ERROR,      /* hard trap: a trap taken while in a trap */
    APP_TRAP_SOFT_ERROR,      /* soft trap (DMA/DAE/NAE/DOOVR class) */
    APP_TRAP_RESERVED_5,
    APP_TRAP_RESERVED_7,
    APP_TRAP_COUNT
} app_trap_id_t;

/*
 * Clear the latch. Called by app_traps_report_previous() once the record has been
 * printed, so the next boot cannot reprint a stale one. Exposed because a caller
 * that reports the record some other way needs the same courtesy.
 *
 * The trap COUNTER deliberately survives this -- see app_traps_count().
 */
void app_traps_clear(void);

/*
 * Print what the last trap was, if there was one, and clear the record. Prints
 * nothing when the latch is empty, so a clean boot stays quiet. Output goes
 * through uart_platform/console_out.h, so it works on any board that implements the seam.
 *
 * Call after the console is up (i.e. from profile_report()).
 *
 * Compiled under both policies. Under SPIN the handler does not reset, so a
 * recorded trap normally waits for the debugger rather than a reboot -- but a hard
 * trap can still reset the part with RCON.TRAPR set, and that case is exactly what
 * the report below explains. There is nothing to gain from making it unavailable.
 */
void app_traps_report_previous(void);

/* Raw accessors, for a debugger or a console command (?xl). */
app_trap_id_t app_traps_last_id(void);
const char   *app_traps_id_str(app_trap_id_t id);

/*
 * Traps since power-on. Survives the per-report clear (unlike the id), so this is
 * what distinguishes a deterministic fault from a one-off glitch. 0 = none since
 * power-on.
 *
 * Readable on demand because the boot report is not always observable: a power
 * cycle can make the host re-enumerate the CDC, and the board prints its banner
 * before the port is back. Without this, "does the counter reset on POR?" is not
 * answerable from a console.
 */
uint16_t app_traps_count(void);

/* ========================================================================== */
/* BOARD SEAM -- the non-text facts this reporting path needs from whichever    */
/* board is linked, as a four-function contract each board implements.          */
/*                                                                             */
/* WAS A HEADER OF ITS OWN, app/board_seam.h, until 2026-08-06. Merged here     */
/* because all four functions exist for one purpose -- reporting and provoking  */
/* traps and resets -- which is this file's subject, and because three of the    */
/* consumers already included app_traps.h and had to include a second file to   */
/* say the same thing. What that header MEASURED is still the reason the seam    */
/* exists at all: the reset command, the reset-cause query and the trap-test     */
/* commands referenced no board pin and no board register between them, which is */
/* what let them move out of boards/<board>/ into uart_app/. Their entire board  */
/* knowledge was these four values, and two of those are really device values    */
/* (where RAM ends on this part).                                               */
/*                                                                             */
/* FUNCTIONS, NOT MACROS FROM A BOARD HEADER. Macros would mean shared files     */
/* doing `#if defined(DSPIC33CK_BOARD_...)` to pick which board header to        */
/* include, which is the coupling this seam exists to remove: every new board    */
/* would then edit files here. With functions, a new board adds one file of its  */
/* own and nothing above this line changes. Same shape, and same reasoning, as   */
/* uart_platform/console_out.h. The cost is that the two constants arrive at     */
/* runtime rather than as immediates -- affordable, because both are used once,  */
/* on a path that deliberately ends in a trap.                                   */
/*                                                                             */
/* IMPLEMENTED AT THE BOTTOM OF:                                                */
/*   boards/ev88g73a/ev88g73a_board.c   (the reset-cause latch is already there)*/
/*   boards/dm330030/dm330030_board.c                                            */
/* ========================================================================== */

/*
 * RESET CAUSE. RCON for this boot: a trap report needs to know whether it was a power-on
 * (no valid persistent RAM) or a reset (evidence worth printing).
 *
 * board_reset_cause_str() normally names the highest-priority bit set, e.g.
 * "SWR(software reset)". WHAT VARIES BETWEEN BOARDS IS THE LATCH, not the decode: a board
 * that captures RCON before anything can clear it has exactly one boot's worth of causes
 * and can always name one, while a board that reads it live may be looking at bits
 * accumulated over several resets and must decline instead of guessing. So this may also
 * return a string saying the word is ambiguous -- treat the return value as text to print,
 * never as a cause to branch on, and use the raw word above for that.
 *
 * The decode itself is NOT per board any more (2026-08-03): RCON's bits and their priority
 * are a family fact, so both implementations call hal_reset/nora_reset.h and pass
 * their own latch policy as a flag. Before that, EV88G73A had the seven-way ladder and
 * DM330030 answered this very call with a disclaimer for want of a copy.
 */
uint16_t    board_reset_cause_raw(void);
const char *board_reset_cause_str(void);

/*
 * TRAP-TEST TARGETS, for the *xa / *xs console commands (uart_app/traps_console.c).
 * Both are per-device RAM facts, and both are deliberately values rather than
 * actions: what the commands need is somewhere that is genuinely wrong, and only
 * the board's own linker script knows where that is.
 *
 * board_trap_bad_addr(): the first data address with no memory behind it, so a read
 * through it takes an address-error trap. NOTE, because the first attempt at this
 * got it wrong and looked like a broken handler: misalignment does NOT trap on this
 * core -- a misaligned word read inside valid RAM carries straight on. An
 * unimplemented address is what faults.
 *
 * board_trap_stack_beyond_limit(): a stack-pointer value past SPLIM, so the next
 * push takes a stack-error trap. MEASURED consequence, worth knowing before using
 * it: this does NOT produce a software _StackError report. W15 is past SPLIM, so
 * the handler's own context push overflows as well, the hardware sees a trap within
 * a trap and resets immediately with RCON.TRAPR set. That is inherent -- no
 * software handler can report a stack overflow, because a handler needs stack. The
 * command therefore exercises the RCON.TRAPR reporting path, not the latch path,
 * and app_traps_report_previous() says so when it fires.
 */
volatile uint16_t *board_trap_bad_addr(void);
uint16_t           board_trap_stack_beyond_limit(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_TRAPS_H */
