/*
 * general_console.c -- console module 'g': general / basic info.
 *
 *   ?gv  protocol/version line of the running image
 *   ?gh  liveness hello followed by the CK help text
 *
 * General console module for `?gv` version and `?gh` liveness. It retains the
 * profile-specific help text after the hello line, so local interactive use does
 * not lose the only command inventory built into small images. Same reasoning as the
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
    if (!msg) {
        return;
    }

    /* Keep module 'g' read-only, matching Sonora.  Previously a '*' request was
     * accepted accidentally because this handler inspected only msg->name. */
    if (msg->kind != '?') {
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    switch (msg->name) {
    case 'v':
        /* The four tokens intentionally parallel Sonora's
         * "SONORA console-v2 <role> <revision>" contract.  The product token is
         * CK, not SONORA: a version query must identify the image actually running.
         * build.ps1 stamps the final token; a build made any other way says so
         * rather than printing a misleading blank. */
        console_out_str(" CK console-v2 HAL-Lab ");
#if defined(EV88G73A_HAVE_BUILD_ID_H)
        console_out_str(EV88G73A_BUILD_ID);
#else
        console_out_str("(not stamped)");
#endif
        console_out_str("\n");
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_OK;
        break;

    case 'h':
        /* Sonora owns ?gh as a deliberately tiny liveness response.  Put the
         * corresponding marker first, then retain CK's established help text.  A
         * cross-board liveness check can therefore key on "console hello", while
         * existing CK operators still receive the command inventory they expect. */
        console_out_str(" CK console hello\n");
        console_task_print_help();
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
