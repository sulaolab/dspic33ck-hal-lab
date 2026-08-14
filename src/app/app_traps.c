/*
 * app_traps.c -- the eight trap vectors, the persistent latch and the boot report.
 * See app_traps.h for the design, the two policies and why a trap does not print
 * from trap context.
 *
 * Replaces boards/ev88g73a/ev88g73a_traps.c and boards/dm330030/traps.c, which had
 * the same vectors clearing the same flags and differed only in what they did next.
 */

#include "app_traps.h"

#include <xc.h>

#include "console_out.h"

/*
 * The latch, in persistent RAM.
 *
 * __attribute__((persistent)) puts these in a section the C runtime startup does
 * NOT clear -- verified with objdump: the section carries the PERSIST flag, unlike
 * .nbss. That is the whole trick: the handler writes here and resets, and the
 * values are still readable after the reset, when there is a working console to
 * print them with.
 *
 * A power-on leaves this RAM undefined, so a magic word guards it. 0x54A9 is
 * arbitrary but fixed; combined with the range check on the id and the POR test in
 * report_previous(), random RAM being mistaken for a trap record is not a practical
 * concern on a lab board.
 */
#define APP_TRAP_MAGIC ((uint16_t)0x54A9u)

/*
 * A SECOND magic word, guarding the counter alone, because the counter has a
 * different lifetime from the record.
 *
 * The record is consumed: report_previous() prints it and calls app_traps_clear(),
 * which invalidates APP_TRAP_MAGIC so the next boot cannot reprint a stale one. The
 * counter must NOT be consumed -- it counts traps since power-on, and a rising
 * count is what distinguishes a deterministic bug from a one-off glitch.
 *
 * Sharing one magic between the two made that impossible, and it was a real defect
 * rather than a theoretical one: MEASURED as reading "traps since power-on: 1" on
 * three successive forced math traps, where the whole point of the field is to read
 * 1, 2, 3. The clear zeroed the shared magic, so the next handler saw "no valid
 * record" and reset the count to 0 -- destroying the very evidence the counter
 * exists to carry, while its own comment claimed it survived.
 */
#define APP_TRAP_COUNT_MAGIC ((uint16_t)0x1C63u)

__attribute__((persistent)) static uint16_t s_trap_magic;
__attribute__((persistent)) static uint16_t s_trap_id;
__attribute__((persistent)) static uint16_t s_trap_intcon1;
__attribute__((persistent)) static uint16_t s_trap_intcon3;
__attribute__((persistent)) static uint16_t s_trap_intcon4;
__attribute__((persistent)) static uint16_t s_trap_count;
__attribute__((persistent)) static uint16_t s_trap_count_magic;

/*
 * Common tail for every handler.
 *
 * Deliberately tiny and calling nothing that touches a peripheral. The machine
 * state in trap context is already suspect -- that is what a trap means -- so the
 * only job here is to get the evidence into RAM that survives, then apply the
 * policy.
 *
 * INTCON1/3/4 are snapshotted BEFORE the flag is cleared, because the flag is part
 * of the evidence. s_trap_count is not reset by a trap, so a trap that repeats every
 * boot (the interesting case -- a deterministic bug rather than a glitch) is visible
 * as a rising count instead of looking like a fresh one-off.
 */
static void app_trap_record_and_apply_policy(app_trap_id_t id)
{
    s_trap_intcon1 = INTCON1;
    s_trap_intcon3 = INTCON3;
    s_trap_intcon4 = INTCON4;

    /* Guarded by its OWN magic, which app_traps_clear() leaves alone -- so the
     * count is reset only when this really is the first trap since power-on, not
     * merely the first since the last report. See the comment on the two magics. */
    if (s_trap_count_magic != APP_TRAP_COUNT_MAGIC) {
        s_trap_count       = 0u;        /* first trap since power-on */
        s_trap_count_magic = APP_TRAP_COUNT_MAGIC;
    }
    s_trap_count++;
    s_trap_id    = (uint16_t)id;
    s_trap_magic = APP_TRAP_MAGIC;

#if (APP_TRAPS_POLICY == APP_TRAPS_POLICY_SPIN)
    /*
     * Halt with the live state intact, for a board driven with a debugger attached.
     * The latch above still happened, which is the one thing this consolidation adds
     * to the previous spin-only handlers: a debugger reads the record without having
     * to reconstruct which vector it came from, and a subsequent TRAPR reset can
     * still be explained.
     */
    for (;;) {
    }
#else
    /*
     * Same instruction the *sr console command uses, so the next boot's reset cause
     * reads SWR. That is intentional: SWR plus a trap record says "the firmware
     * trapped and restarted", which a bare SWR alone would not.
     */
    __asm__ volatile ("reset");
#endif
}

/* -------------------------------------------------------------------------- */
/* The vectors.                                                               */
/*                                                                            */
/* All of the ones this family actually has, not just the four MCC generates.  */
/* Checked against both linker scripts' symbol lists (p33CK64MC105.gld,        */
/* p33CK256MP508.gld): _HardTrapError, _SoftTrapError, _ReservedTrap5 and      */
/* _ReservedTrap7 exist on both parts and were once falling through to the     */
/* default vector, which is exactly the silent failure this file removes.      */
/*                                                                            */
/* no_auto_psv on all of them. DM330030's handlers were auto_psv (MCC's        */
/* default) and EV88G73A's were not; unified deliberately rather than split by  */
/* board, because nothing here reads constant data -- only SFRs and the        */
/* persistent latch -- so the PSV setup auto_psv emits is pure cost in the one */
/* context where the fewest instructions is the whole design goal.             */
/* -------------------------------------------------------------------------- */

void __attribute__((__interrupt__, no_auto_psv)) _OscillatorFail(void)
{
    INTCON1bits.OSCFAIL = 0;
    app_trap_record_and_apply_policy(APP_TRAP_OSC_FAIL);
}

void __attribute__((__interrupt__, no_auto_psv)) _AddressError(void)
{
    INTCON1bits.ADDRERR = 0;
    app_trap_record_and_apply_policy(APP_TRAP_ADDRESS_ERROR);
}

void __attribute__((__interrupt__, no_auto_psv)) _StackError(void)
{
    INTCON1bits.STKERR = 0;
    app_trap_record_and_apply_policy(APP_TRAP_STACK_ERROR);
}

void __attribute__((__interrupt__, no_auto_psv)) _MathError(void)
{
    INTCON1bits.MATHERR = 0;
    app_trap_record_and_apply_policy(APP_TRAP_MATH_ERROR);
}

void __attribute__((__interrupt__, no_auto_psv)) _HardTrapError(void)
{
    /* A trap taken while already in a trap. No flag of its own to clear -- the
     * originating trap's flag is in the INTCON snapshot. Its overwhelmingly common
     * cause is a stack overflow: W15 past SPLIM means the handler's own context push
     * overflows too. */
    app_trap_record_and_apply_policy(APP_TRAP_HARD_ERROR);
}

void __attribute__((__interrupt__, no_auto_psv)) _SoftTrapError(void)
{
    /* DMA/DAE/NAE/DOOVR class, reported through INTCON3. Cleared here so a repeat is
     * distinguishable from a stuck flag. */
    INTCON3bits.DAE   = 0;
    INTCON3bits.NAE   = 0;
    INTCON3bits.DOOVR = 0;
    app_trap_record_and_apply_policy(APP_TRAP_SOFT_ERROR);
}

/*
 * Reserved vectors. Nothing in the datasheet is documented as reaching these, so
 * arriving here means the device did something unlisted -- which is precisely why
 * they should not fall through to the default vector unnoticed.
 */
void __attribute__((__interrupt__, no_auto_psv)) _ReservedTrap5(void)
{
    app_trap_record_and_apply_policy(APP_TRAP_RESERVED_5);
}

void __attribute__((__interrupt__, no_auto_psv)) _ReservedTrap7(void)
{
    app_trap_record_and_apply_policy(APP_TRAP_RESERVED_7);
}

/* -------------------------------------------------------------------------- */
/* Foreground side: report the record from the previous boot.                  */
/* -------------------------------------------------------------------------- */

const char *app_traps_id_str(app_trap_id_t id)
{
    switch (id) {
    case APP_TRAP_NONE:          return "none";
    case APP_TRAP_OSC_FAIL:      return "OSC FAIL (clock failed / FSCM)";
    case APP_TRAP_ADDRESS_ERROR: return "ADDRESS ERROR (unimplemented address)";
    case APP_TRAP_STACK_ERROR:   return "STACK ERROR (W15 out of bounds)";
    case APP_TRAP_MATH_ERROR:    return "MATH ERROR (div-by-0 / acc overflow)";
    case APP_TRAP_HARD_ERROR:    return "HARD TRAP (trap within a trap)";
    case APP_TRAP_SOFT_ERROR:    return "SOFT TRAP (DMA/DAE/NAE/DOOVR)";
    case APP_TRAP_RESERVED_5:    return "reserved trap 5";
    case APP_TRAP_RESERVED_7:    return "reserved trap 7";
    default:                     return "unknown";
    }
}

uint16_t app_traps_count(void)
{
    if (s_trap_count_magic != APP_TRAP_COUNT_MAGIC) {
        return 0u;
    }

    return s_trap_count;
}

app_trap_id_t app_traps_last_id(void)
{
    if (s_trap_magic != APP_TRAP_MAGIC) {
        return APP_TRAP_NONE;
    }
    if (s_trap_id >= (uint16_t)APP_TRAP_COUNT) {
        return APP_TRAP_NONE;
    }

    return (app_trap_id_t)s_trap_id;
}

void app_traps_clear(void)
{
    s_trap_magic   = 0u;
    s_trap_id      = 0u;
    s_trap_intcon1 = 0u;
    s_trap_intcon3 = 0u;
    s_trap_intcon4 = 0u;
    /*
     * s_trap_count and s_trap_count_magic deliberately survive: the count is per
     * power-on, not per report, and a rising count is what distinguishes a
     * deterministic bug from a glitch.
     *
     * s_trap_count_magic is the reason this works. An earlier version made this same
     * claim while sharing one magic word with the record -- so zeroing the magic here
     * silently reset the count on the next trap, and the field read 1 forever.
     * Leaving the count's own guard alone is what actually implements the comment
     * above; keep the two magics separate.
     */
}

void app_traps_report_previous(void)
{
    app_trap_id_t id   = app_traps_last_id();
    uint16_t      rcon = board_reset_cause_raw();

    /*
     * A power-on leaves ALL of this RAM undefined, the counter included, so deal
     * with POR before reading anything else. This runs unconditionally rather than
     * only when a record looks valid: the counter has its own magic, and if random
     * RAM happened to match it the count would otherwise carry a garbage value
     * across a genuine power-on and be reported as "repeating" on the first real
     * trap. Note a trap resets via the RESET instruction, which reads as SWR, and a
     * trap-within-a-trap as TRAPR -- never as POR. So there is nothing to report on a
     * POR by construction.
     */
    if ((rcon & (1u << _RCON_POR_POSITION)) != 0u) {
        app_traps_clear();
        s_trap_count       = 0u;
        s_trap_count_magic = APP_TRAP_COUNT_MAGIC;
        return;
    }

    /*
     * TRAPR with no software record: a trap so severe that the handler could not
     * run. Measured, not theorised -- forcing a stack overflow (*xs) produces exactly
     * this, and the reason is structural rather than a defect here: overflow leaves
     * W15 past SPLIM, so the trap handler's own context push overflows again, the
     * hardware sees a trap within a trap and resets on the spot. No software handler
     * can report a stack overflow, because a handler needs stack to run.
     *
     * So this branch is the ONLY evidence for that whole class of fault, which makes
     * the RCON.TRAPR reporting genuinely load-bearing rather than a duplicate of the
     * latch below. Called out explicitly instead of leaving a bare "Reset = TRAPR" in
     * the banner for someone to interpret.
     */
    if (((rcon & (1u << _RCON_TRAPR_POSITION)) != 0u) && (id == APP_TRAP_NONE)) {
        console_out_str(
            "\n*** TRAP CONFLICT on the previous run (RCON.TRAPR): a trap fired "
            "inside\n"
            "    a trap, so the hardware reset before any handler could record it.\n"
            "    Classic cause: STACK OVERFLOW -- W15 past SPLIM means the handler's\n"
            "    own context push overflows too. Look for runaway recursion or an\n"
            "    oversized local before anything else.\n"
            "    (No handler ran, so the trap counter did not advance for this one.)\n");
        return;
    }

    if (id == APP_TRAP_NONE) {
        return;                 /* clean boot: stay quiet */
    }

    console_out_str("\n*** TRAP on the previous run: ");
    console_out_str(app_traps_id_str(id));
    console_out_str("\n    INTCON1=0x");
    console_out_hex16(s_trap_intcon1);
    console_out_str(" INTCON3=0x");
    console_out_hex16(s_trap_intcon3);
    console_out_str(" INTCON4=0x");
    console_out_hex16(s_trap_intcon4);
    console_out_str("\n    traps since power-on: ");
    console_out_u32((uint32_t)s_trap_count);
    if (s_trap_count > 1u) {
        console_out_str("  <- repeating, so this is deterministic");
    }
    console_out_str("\n");

    app_traps_clear();
}
