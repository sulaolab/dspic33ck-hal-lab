/*
 * general_console.c -- console module 'g': general / basic info.
 *
 *   ?gv  build ID of the running image
 *   ?gh  the help text
 *
 * Split out of console_dispatch.c when the console moved to uart_app/, and named
 * after dspic33ak-audio-dsp-sonora's uart_app/general_console.c, which owns the
 * same letter for the same two things (?gv version, ?gh help). Same reasoning as the
 * other module files: the dispatcher should answer "which letters exist", not
 * implement one of them.
 */

#include "app_console.h"

#include "console_out.h"
#include "console_task.h"

/* Generated per build by buildtools/build.ps1, which also defines this macro.
 * Absent for builds made any other way -- ?gv then says so rather than lying. */
#if defined(EV88G73A_HAVE_BUILD_ID_H)
#include "ev88g73a_build_id.h"
#endif

void general_console_onmsg(app_console_msg_t *msg)
{
    switch (msg->name) {
    case 'v':
        /* Which IMAGE is running, not merely that the board restarted. build.ps1
         * stamps this into a generated header; a build made any other way says so
         * rather than printing a misleading blank. */
        console_out_str(" build ID = ");
#if defined(EV88G73A_HAVE_BUILD_ID_H)
        console_out_str(EV88G73A_BUILD_ID);
#else
        console_out_str("(not stamped)");
#endif
        console_out_str("\n");
        msg->status = APP_CONSOLE_OK;
        break;

    case 'h':
        /* The same text the boot banner prints, from one place: they were two texts
         * in two files before and had already drifted. See console_task_print_help(). */
        console_task_print_help();
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
