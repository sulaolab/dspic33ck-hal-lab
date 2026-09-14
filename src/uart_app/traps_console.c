/*
 * traps_console.c -- console module 'x': exceptions (traps).
 *
 * Was app/console_traps.c. sonora has no 'x' module, so this name follows its
 * PATTERN (<module>_console.c) rather than an existing file -- the shape of the file
 * is the same as system_console.c's, so it reads the same way.
 *
 *   ?xl  last trap and the count since power-on
 *   *xa  force an address error    *xm  force a math error
 *   *xs  force a stack overflow
 *
 * WHY 'x' AND NOT 't'
 * -------------------
 * These were *ta / *tm / *ts before the grammar changed. sonora's *tr means "restart the
 * audio stream" and CK has a transport too (hal_spi_i2s_tdm), so 't' is reserved to
 * match. One letter meaning two things across two repos is how muscle memory turns
 * into a wrong command on hardware.
 *
 * WHY THIS IS NOT IN A BOARD DIRECTORY
 * ------------------------------------
 * It was, and the reason given was real but narrow: forcing an address error needs to
 * know where RAM ends, and forcing a stack overflow needs a value past SPLIM. Those
 * two numbers are now the BOARD SEAM section of app/app_traps.h, which is all the board
 * ever contributed -- the trap machinery itself is app/app_traps.c and shared.
 *
 * WHY THE FORCED TRAPS EXIST AT ALL
 * ---------------------------------
 * Trap handlers are the one piece of code that only runs when something has already
 * gone wrong, which makes them exactly the kind of code that rots unnoticed. Each of
 * these is genuinely undefined behaviour by design -- that is the point -- so they
 * are shaped to defeat the optimiser (volatile operands it cannot fold, values it
 * cannot prove) rather than relying on it being naive. Guarded by APP_TRAP_TEST_CMDS
 * so a shipping image need not carry them.
 */

#include "app_console.h"

#include <stdint.h>

#include "app_traps.h"      /* the trap ids, and the BOARD SEAM's two trap-test targets */
#include "console_out.h"
#include "console_task.h"

#if APP_TRAP_TEST_CMDS
/*
 * Stack error: push W15 past SPLIM.
 *
 * The first attempt at this recursed and did not trap. Rather than guess whether the
 * optimiser had folded the recursion or the frames were too small to reach the limit,
 * this drives W15 directly -- SPLIM is what the hardware compares against, so writing
 * a stack pointer beyond it is the definition of the fault rather than an attempt to
 * provoke it.
 *
 * noreturn + noinline so the compiler neither inlines this into the caller's frame nor
 * assumes execution continues afterwards.
 *
 * See board_trap_stack_beyond_limit() for the measured consequence: this exercises the
 * RCON.TRAPR reporting path, not the software latch, because no handler can run once
 * the stack is gone.
 */
static void __attribute__((noinline, noreturn)) console_traps_blow_stack(void)
{
    uint16_t beyond = board_trap_stack_beyond_limit();

    /* Move the stack pointer past the limit. The very next push -- the one below --
     * exceeds SPLIM and takes the trap. The operand is read into a W register before
     * W15 changes, so the value survives the move that invalidates the frame. */
    __asm__ volatile ("mov %0, w15" : : "r" (beyond));

    /* A push, to force the fault immediately rather than at some later call. */
    for (;;) {
        __asm__ volatile ("push w0");
    }
}

static void console_traps_force(uint8_t which)
{
    char one[2];

    console_out_str("console: forcing a trap on purpose (");
    one[0] = (char)which;
    one[1] = '\0';
    console_out_str(one);
    console_out_str(") -- expect a reset and a report\n");

    /* Let the line above leave the UART before the machine stops making sense. */
    (void)console_task_wait_tx_drain();

    switch (which) {
    case 'a': {
        /*
         * Address error: read an UNIMPLEMENTED data address. NOT a misaligned one --
         * the first attempt did that and the board carried straight on. See
         * board_trap_bad_addr().
         */
        volatile uint16_t *nowhere = board_trap_bad_addr();

        (void)*nowhere;
        break;
    }
    case 'm': {
        /* Math error: integer divide by zero. The divisor is volatile so the division
         * is emitted rather than folded into a compile-time diagnostic. */
        static volatile int denom;   /* zero-initialised */
        static volatile int result;

        result = 1000 / denom;
        (void)result;
        break;
    }
    case 's':
        console_traps_blow_stack();      /* does not return */
        break;
    default:
        console_out_str("console: no such trap test\n");
        break;
    }
}
#endif /* APP_TRAP_TEST_CMDS */

void traps_console_onmsg(app_console_msg_t *msg)
{
#if APP_TRAP_TEST_CMDS
    if (msg->kind == '*') {
        switch (msg->name) {
        case 'a':
        case 'm':
        case 's':
            console_traps_force(msg->name);   /* normally does not return */
            msg->status = APP_CONSOLE_OK;
            return;
        default:
            msg->status = APP_CONSOLE_ERR_NOT_FOUND;
            return;
        }
    }
#endif

    if (msg->name == 'l') {
        console_out_str("last trap: ");
        console_out_str(app_traps_id_str(app_traps_last_id()));
        console_out_str("  (cleared at boot after being reported)\n"
                        "traps since power-on: ");
        console_out_u32((uint32_t)app_traps_count());
        console_out_str("\n");
        msg->status = APP_CONSOLE_OK;
        return;
    }

    msg->status = APP_CONSOLE_ERR_NOT_FOUND;
}
