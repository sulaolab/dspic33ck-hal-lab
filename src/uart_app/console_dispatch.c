/*
 * console_dispatch.c -- app_onmsg(): the one symbol app_console.c requires.
 *
 * A parsed line arrives here as kind + module + name + decoded payload, and this
 * switches on the MODULE letter and does nothing else. Every letter has a file of
 * its own beside this one.
 *
 * NOT the same shape as dspic33ak-audio-dsp-sonora, on purpose. There the switch lives
 * inside uart_app/app_debug.c, together with that project's own debug modules
 * ('n', 'm', 'i'). CK has no app_debug and no such modules, so a routing-only file
 * is what is left -- and a router with nothing else in it is worth keeping separate,
 * because it is the file you read to answer "which letters exist".
 *
 * MODULE LETTERS -- aligned with the fleet on purpose, so the same command means the
 * same thing across repos:
 *
 *   'g'  general      ?gv version / build ID, ?gh help     general_console.c
 *   's'  system       *sr software reset, ?sr reset cause   system_console.c
 *   'd'  diagnostics  ?dh high-res timer read test          diag_console.c
 *   'x'  exceptions   ?xl last trap, *xa / *xm / *xs        traps_console.c
 *        (spaced out on purpose: an unspaced slash between two starred commands
 *         reads as a nested comment opener to the compiler -- -Wcomment says so,
 *         and it is right)
 *   't'  audio transport -- BOARD-OWNED (console_board_onmsg below), because what it
 *        controls is a board's codec. EV88G73A owns it when WM8904 audio is built:
 *        *tp path, *tb AVAS parts, *ti / *to gain, ?tq load, *td declick (UNSUPPORTED),
 *        *ts stop, *tr restart.  The lab-only *tl lifecycle gates and *tb parts controls
 *        are compiled only when WM8904_AUDIO_ENABLE_TDM_DIAG=1;
 *        *tm SYSTEM/sync-domain API is separately lab-only. DM330030 owns no letter.
 *        The letter is sonora's, and *tr means what it means there (restart the
 *        stream) -- which is why the trap commands are on 'x' here even though they
 *        were on 't' before: one letter meaning two things across two repos is how
 *        muscle memory turns into a wrong command on hardware. *tr was RESERVED and
 *        unimplemented until 2026-08-08 (it answered $01 = NOT_FOUND, which is not
 *        the same as $05 = OPERATION_FAILED and was briefly misread as one).
 *   'c'  Classic compatibility -- BOARD-OWNED: *cy00 Type_TY AVAS toggle. The CK
 *        board does not claim the rest of Sonora Classic; known synth subcodes that
 *        lack CK counterparts report UNSUPPORTED.
 *
 * STATUS: handlers set msg->status. Bit 7 set means OK, cleared means failure with
 * a reason in the low bits. Worth using -- the console this replaces had no way to
 * report that a command failed, it only echoed unrecognised input.
 *
 * console_board_onmsg() is the route for a letter that genuinely needs board
 * knowledge; nothing needs it today, so both boards implement it as "not found" and
 * it exists for the next one that does.
 */

#include "app_console.h"

#include <stddef.h>

/*
 * Module handlers. Declared here rather than in headers of their own because this is
 * the only caller of each and console_dispatch.c is the contract. (sonora gives each
 * module a small header of its own; with one caller that is a file per declaration,
 * so the declarations stay here until a second caller appears.)
 */
void general_console_onmsg(app_console_msg_t *msg);   /* general_console.c */
void diag_console_onmsg(app_console_msg_t *msg);      /* diag_console.c    */
void system_console_onmsg(app_console_msg_t *msg);    /* system_console.c  */
void traps_console_onmsg(app_console_msg_t *msg);     /* traps_console.c   */

/* Implemented by each board: a letter that genuinely needs board knowledge. */
void console_board_onmsg(app_console_msg_t *msg);

/* ------------------------------------------------------------------------- */

void app_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

    switch (msg->module) {
    case 'g':
        /* General: build ID and help. */
        general_console_onmsg(msg);
        break;

    case 'd':
        /* Diagnostics: board-independent, because what it tests is the HAL. */
        diag_console_onmsg(msg);
        break;

    case 's':
        /* System: software reset and reset cause. Board-independent since the
         * latched cause arrives through the BOARD SEAM in app/app_traps.h. */
        system_console_onmsg(msg);
        break;

    case 'x':
        /* Exceptions: the trap latch and the trap tests, on the shared handlers in
         * app/app_traps.c. */
        traps_console_onmsg(msg);
        break;

    /*
     * Anything else goes to the board. Nothing does today -- every letter in use is
     * board-independent -- so both boards answer "not found". The hook stays because
     * the next board-specific command should attach to a board file rather than
     * teaching this switch about a board.
     */
    default:
        console_board_onmsg(msg);
        break;
    }
}
