/*
 * system_console.c -- console module 's': system.
 *
 * System console module. `?sr` reports reset information for this profile.
 *
 *   *sr  software reset
 *   ?sr  why this boot happened (reset cause + raw RCON)
 *
 * WHY THIS IS NOT IN A BOARD DIRECTORY
 * ------------------------------------
 * It was, in boards/ev88g73a/ev88g73a_console.c, and its board knowledge was one
 * thing: the latched reset cause. That is now the BOARD SEAM section of
 * app/app_traps.h, and what remains
 * here is a RESET instruction (a device instruction, on both parts) and two lines of
 * text.
 *
 * The command matters most on a board that cannot be reset any other way -- the
 * EV88G73A Curiosity Nano has no reset button -- but nothing about it is specific to
 * that board, and a board that gains a console gets this for free.
 */

#include "app_console.h"

#include <stdint.h>

#include "app_traps.h"      /* BOARD SEAM: board_reset_cause_raw()/_str() */
#include "console_out.h"
#include "console_task.h"
/*
 * Reached directly, not through the board seam, and deliberately: the PORTABLE cause
 * is a family fact with the same meaning on every board (a board that never captured
 * reports not-captured, which is why the field below is conditional). Only the LATCH
 * POLICY is a board decision, and that stays where it was. diag_console.c already
 * reaches a HAL header the same way.
 */
#include "nora_reset.h"

/*
 * Wait for the UART to physically finish shifting out what has been queued, then
 * reset. The wait is not cosmetic -- see console_task_wait_tx_drain().
 */
static void console_system_reset_now(void)
{
    console_out_str(" \"*sr\" software reset now (was: ");
    console_out_str(board_reset_cause_str());
    console_out_str("). Rebooting...\n");

    (void)console_task_wait_tx_drain();

    /*
     * dsPIC software reset. Sets RCON.SWR, which is what board_reset_cause_str()
     * reports on the next boot -- the evidence that this command worked rather than
     * someone having power-cycled the board. Same instruction the trap handlers use,
     * and the same one sonora uses.
     */
    __asm__ volatile ("reset");

    /* Not reached. */
    for (;;) {
    }
}

void system_console_onmsg(app_console_msg_t *msg)
{
    switch (msg->name) {
    case 'r':
        if (msg->kind == '*') {
            console_system_reset_now();            /* does not return */
            return;
        }
        console_out_str("reset cause this boot: ");
        console_out_str(board_reset_cause_str());
        console_out_str("  RCON=0x");
        console_out_hex16(board_reset_cause_raw());
        /*
         * BOTH decodes, on purpose -- they answer different questions and can differ.
         * The board-seam string above is CK's most-specific-first diagnostic decode and
         * is the only one that can say TRAPR or IOPUWR (a trap conflict, i.e. the stack
         * overflow app_traps.c reports). The portable one below is the CK/AK common
         * cause enum, which has no member for either and would call them SWR/OTHER; it
         * is what portable application logic branches on, so print what that logic will
         * actually see rather than assuming it agrees with the diagnostic string.
         *
         * Omitted entirely on a board that never captured a snapshot: the API reports
         * UNKNOWN/warm there by contract, and printing that next to a real decoded
         * cause would read as a disagreement rather than as an absence.
         */
        if (nora_reset_snapshot_is_captured()) {
            console_out_str("  portable=");
            console_out_str(nora_reset_snapshot_cause_str());
        }
        console_out_str("\n");
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
