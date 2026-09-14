/*
 * diag_console.c -- console module 'd': diagnostics.
 *
 * Diagnostic console module. The `d` commands in this profile exercise the timer.
 *
 * WHY THIS IS NOT IN A BOARD DIRECTORY
 * ------------------------------------
 * ?dh tests the high-resolution timer, and that timer is the HAL's
 * (nora_high_res_timer, SCCP1 on this family) -- shared by both boards. The
 * test lived in boards/ev88g73a/ev88g73a_console.c purely because that is where
 * it was written, and its dependencies say so plainly: nora_high_res_timer_*
 * and text output, and not one board-specific pin, register or constant.
 *
 * A shared timer whose self-test only one board can run is a test that will rot
 * the moment the other board needs it.
 */

#include "app_console.h"

#include <stddef.h>
#include <stdint.h>

#include "console_out.h"
#include "nora_high_res_timer.h"

/*
 * "?dh" -- prove (or disprove) the high-res timer's 32-bit read.
 *
 * Why this exists: the measured audio block period reads 655.3-655.4 us almost
 * always, but occasionally comes out short by exactly 1024 + N*2048 counts (half
 * a frame plus N whole frames, N up to 10) while the ISR time is unchanged. Two
 * candidate causes -- a glitchy counter read, or genuinely irregular ISR entry --
 * and they need different fixes, so measure rather than argue.
 *
 * This samples get_count() in a tight loop and checks the sequence itself:
 * anything other than a small positive step is a read defect, independent of the
 * audio path. A backward step or a jump near 1024 / 65536 counts names the bug.
 *
 * MEASURED 2026-08-08, because the reply as first written accused the wrong thing.
 * The loop runs in the foreground with interrupts on, so the audio block ISR lands
 * inside it perhaps half the time, and that shows up as ONE big step whose size is
 * the ISR's own duration -- not as a read defect. Verified by prediction rather than
 * by argument: with the chain idle (ISR 38.3 us) the big steps were 3890-3894 counts
 * at Fcy 100 MHz = 38.9 us; with the AVAS synth running (ISR 321 us) the same test
 * gave 32196-32208 = 322 us. Twelve runs, `backward` and `worst` zero in every one.
 *
 * So `backward` is the defect indicator. `steps>=1024` is only suspicious when the
 * size does NOT match the block ISR, or when there is more than one of them.
 */
static void console_diag_timer_selftest(void)
{
    enum { SAMPLES = 256u };
    uint32_t prev;
    uint32_t min_step = 0xFFFFFFFFu;
    uint32_t max_step = 0u;
    uint32_t backward = 0u;
    uint32_t big      = 0u;      /* steps >= 1024 counts, i.e. the suspect size */
    uint32_t worst_backward = 0u;
    uint16_t i;

    if (!nora_high_res_timer_is_initialized()) {
        console_out_str("?dh: high-res timer not initialized\n");
        return;
    }

    prev = nora_high_res_timer_get_count();
    for (i = 0u; i < (uint16_t)SAMPLES; i++) {
        uint32_t now  = nora_high_res_timer_get_count();
        uint32_t step = now - prev;    /* unsigned: a backward step shows up huge */

        if (step > 0x80000000u) {
            backward++;
            if ((0u - step) > worst_backward) {
                worst_backward = 0u - step;
            }
        } else {
            if (step < min_step) { min_step = step; }
            if (step > max_step) { max_step = step; }
            if (step >= 1024u)   { big++; }
        }
        prev = now;
    }

    console_out_str("?dh: SCCP1 read test, ");
    console_out_u32((uint32_t)SAMPLES);
    console_out_str(" samples: step min=");
    console_out_u32(min_step);
    console_out_str(" max=");
    console_out_u32(max_step);
    console_out_str(" backward=");
    console_out_u32(backward);
    console_out_str(" (worst=");
    console_out_u32(worst_backward);
    console_out_str(") steps>=1024=");
    console_out_u32(big);
    console_out_str("\n");
    console_out_str(
        "     backward>0 = read defect. One step>=1024 with backward=0 is the block\n"
        "     ISR preempting this loop; its size is the ISR time (~3900 idle, ~32200\n"
        "     with AVAS).\n");
}

/*
 * "?du" -- how many times the console's own RX path had to be rescued.
 *
 * Why this exists: this console went DEAF while it kept printing.  Characters were
 * put on the wire (the host's bridge writes and flushes), nothing was echoed, the
 * audio ISR meanwhile ran 885 000 blocks with miss = 0, a USB re-enumeration changed
 * nothing because the firmware never restarted, and only a reset ended it.  The
 * mechanism is a latched OERR that halts the receiver, on a path where the only code
 * that cleared OERR sat behind a gate that answers false once the FIFO is empty --
 * so the reader could not reach its own way out.  nora_uart_rx_ready() now
 * clears it, and this is the count of how often it mattered.
 *
 * 0 means the failure has not happened since boot.  Non-zero means it HAS and was
 * survived -- which is the number to watch, because a rescue is not a fix for
 * whatever is overrunning the FIFO in the first place.
 *
 * The instance is the console's own, from the board's console_out layer: asking any
 * other instance would report a UART nobody is typing at.
 */
static void console_diag_uart_rx_status(void)
{
    console_out_str("?du: console UART RX overruns recovered = ");
    console_out_u32(console_in_overrun_recovered());
    console_out_str("\n");
    console_out_str(
        "     0 = the receiver has not latched an overrun since boot;\n"
        "     non-zero = reception WOULD have stopped for good that many times\n");
}

/* Module entry point, called from uart_app/console_dispatch.c. */
void diag_console_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

    if (msg->kind != '?') {
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    switch (msg->name) {
    case 'h':
        if (msg->data_len != 0u) {
            console_out_str("?dh: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        console_diag_timer_selftest();
        msg->status = APP_CONSOLE_OK;
        break;

    case 't':
        /* `?dt` was the old CK-only spelling.  Do not retain it as an alias: it is a
         * one-character transposition of Sonora's `?td` declick report, so an alias
         * would preserve exactly the cross-board typo this move removes. */
        console_out_str("?dt: retired to avoid the ?td declick command; use ?dh for "
                        "the high-res timer self-test\n");
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        break;

    case 'u':
        if (msg->data_len != 0u) {
            console_out_str("?du: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        console_diag_uart_rx_status();
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
