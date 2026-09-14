/*
 * uart_platform_stdio.c
 * ---------------------
 * Sends everything printf() produces to the board console UART.
 *
 * Was app/console_stdio.c; renamed to the fleet's name for this exact file when it
 * moved here. See the header.
 *
 * This is the ONLY reason the MCC-generated mcc_generated_files/uart1.* pair
 * survived as long as it did. That file had already been gutted -- every
 * UART1_* function was a one-line forward to nora_uart_* -- and of the
 * eleven functions in its 674-line header, exactly one (UART1_Initialize) was
 * still called anywhere. What kept it in the build was this write() hook buried
 * at the bottom, which is what makes printf work at all: delete the file without
 * noticing it and the console goes silent with no compile error.
 *
 * So it lives here, in the console's platform layer, deliberately:
 *
 *   - It is not HAL material. Which UART is "the console" is an application
 *     decision; hal_uart is instance-agnostic on purpose and must stay that way.
 *   - It is not board material either. The board layer owns pins and routing
 *     (the board bring-up file), not the question of where stdout goes.
 *
 * The .libc.write section attribute is what makes the linker prefer this over
 * the C library's stub, and it is required -- a plain `write()` does not
 * necessarily win. Keep it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "uart_platform_stdio.h"
#include "nora_clock.h"
#include "nora_uart.h"

/* The console instance is UART_PLATFORM_CONSOLE_UART_INST in the header -- exposed
 * there, not private here, so EV88G73A can assert its own console constant against it.
 *
 * The RATE is deliberately NOT here: it is the caller's argument. A board whose PLL did
 * not lock has to drop to a rate its fallback clock can actually resolve, and that
 * decision needs the clock's outcome -- which the board knows and this file does not.
 * Mechanism here, policy at the board (the shape EV88G73A's own UART bring-up already
 * had, before that bring-up became this function's second caller). */
#define CONSOLE_UART_INST   UART_PLATFORM_CONSOLE_UART_INST

/*
 * Bring-up observability: stdio retargets to this UART, so if its init fails
 * there is no console left to report the failure on. Keep the result in a
 * volatile for the debugger rather than discarding it. (The pin/PPS stage is
 * captured separately by the caller -- a mis-routed PPS leaves peripheral init
 * reporting OK while producing no output, so the two stages fail independently
 * and need separate evidence.)
 */
volatile nora_uart_status_t g_console_init_status =
    NORA_UART_ERR_NOT_INITIALIZED;

void uart_platform_stdio_init(uint32_t baudrate)
{
    /*
     * Clocked from FOSC/2 (BCLKSEL = FOSC_DIV2), i.e. the system Fcy, taken from
     * the Clock HAL so the baud divisor tracks the actual operating point rather
     * than a duplicated literal. The caller must have brought the clock up first.
     */
    const nora_uart_config_t config = {
        .uart_clk_hz  = nora_clock_get_fcy_hz(),
        .baudrate     = baudrate,
        .timeout_ms   = 0u,
        .get_ms       = 0,
        .data_bits    = 8u,
        .stop_bits    = 1u,
        .parity       = NORA_UART_PARITY_NONE,
        .high_speed   = true,
        .clock_source = NORA_UART_BCLKSEL_FOSC_DIV2,
        .enable_tx    = true,
        .enable_rx    = true,
    };

    g_console_init_status = nora_uart_init(CONSOLE_UART_INST, &config);
}

int __attribute__((__section__(".libc.write"))) write(
    int handle,
    void *buffer,
    unsigned int len)
{
    const uint8_t *p = (const uint8_t *)buffer;
    unsigned int i;

    /* stdout and stderr both go to the console; there is nowhere else to send
     * them on this board, so the handle is deliberately ignored. */
    (void)handle;

    /*
     * LF -> CR LF, and this is the printf boards' single place for it -- the same rule
     * ev88g73a_console_out.c's console_put() applies to its own transport, argued once
     * in uart_platform/console_out.h: every string in this firmware carries a bare
     * '\n'. This path is printf/fputs/putchar, which is DM330030's entire console
     * (its console_out_* are thin wrappers over stdio) and EV88G73A's only under
     * -Define ENA_WM8904_TRACE=1.
     *
     * A byte at a time rather than one nora_uart_write() of the whole buffer,
     * because the CR being inserted is not IN the buffer -- and stdio hands this hook
     * whole lines, so the loop runs where a memcpy-into-a-scratch-buffer version would
     * need a buffer sized for the longest line anyone ever prints.
     *
     * THE RETURN VALUE IS INPUT BYTES CONSUMED, never the number sent: stdio reads
     * "returned less than len" as a short write and re-sends the tail, so counting the
     * inserted CRs would make every line ending duplicate part of its own line.
     */
    for (i = 0u; i < len; i++) {
        if (p[i] == (uint8_t)'\n') {
            if (nora_uart_write_byte(CONSOLE_UART_INST, (uint8_t)'\r') !=
                NORA_UART_OK) {
                return (int)i;
            }
        }

        if (nora_uart_write_byte(CONSOLE_UART_INST, p[i]) !=
            NORA_UART_OK) {
            return (int)i;
        }
    }

    return (int)len;
}
