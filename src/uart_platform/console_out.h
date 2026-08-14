#ifndef CONSOLE_OUT_H
#define CONSOLE_OUT_H

/*
 * console_out.h -- the console's text output, as a four-function contract each
 * board implements.
 *
 * WHY THIS EXISTS
 * ---------------
 * Everything the console does is board-independent except getting characters out.
 * Measured on the EV88G73A modules before this existed: the command interpreter,
 * the trap reporter, the DSP-load display, the DMA self-test and the I2C1 probe
 * referenced ZERO board-specific pins between them -- their only tie to the board
 * was calling a board-named UART write. Board-independent logic wearing a
 * board-specific output channel, and the name of that channel was what pinned five
 * modules into boards/ev88g73a/.
 *
 * That board-named API is gone (2026-08-02): its bodies are now the EV88G73A
 * implementation of this contract, so on that board there is one output path rather
 * than two names for one UART. What remained board-specific was which UART -- a
 * constant, EV88G73A_CONSOLE_UART_INST.
 *
 * WHY NOT JUST printf
 * -------------------
 * DM330030 prints with printf(), retargeted through uart_platform/uart_platform_stdio.c's write()
 * hook. EV88G73A deliberately does not, because it is a 64 KB part.
 *
 * printf IS currently linked into the EV88G73A image -- 53 references in the map --
 * but not by EV88G73A's choice: chip_drivers/wm8904.c does `#define TRACE printf`
 * and uses it 50 times. So the WM8904 configuration already pays for stdio, while a
 * baseline configuration with the codec disabled does not. Making the console
 * require printf would take that choice away for good, so it does not.
 *
 * The other reason is duller and just as real: no variadic arguments means no
 * format string, so none of the format-vs-argument mismatches that a printf-based
 * console invites. u32 and hex16 cover what the commands actually print.
 *
 * WHAT IMPLEMENTS IT
 * ------------------
 * One small file per board:
 *   boards/ev88g73a/  -> ev88g73a_console_out.c, straight onto hal_uart. It IS the
 *                        board's console transport, not a delegation to one.
 *   boards/dm330030/  -> printf(), which is already that board's console
 *
 * Note the PARSER (app_console.c) needs none of this -- it prints nothing at all.
 * Only command handlers produce output. That is why app_console.c could be
 * vendored from sonora without modification.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE LINE-ENDING RULE, which applies to both output functions below (2026-08-07)
 * ------------------------------------------------------------------------------
 * Source strings carry a BARE '\n'. The implementation turns each one into CR LF on
 * the wire. No string literal in this firmware may contain '\r'.
 *
 * It used to be the opposite -- "sent as-is", line endings the caller's business --
 * and what that produced was 141 literals ending in CR LF and 65 ending in a bare
 * LF, in the same console, printing a staircase on any terminal not set to translate
 * incoming LF. Two conventions for one channel is the defect; the ROM the CRs cost on
 * the 64 KB part (-120 B measured) is the
 * smaller half of the reason.
 *
 * TRANSLATING here rather than dropping the CR and letting the terminal cope is what
 * keeps the BYTES ON THE WIRE IDENTICAL to before: no dependency on each person's
 * Tera Term line-ending setting, and no effect on the serial-monitor bridge (the
 * sibling serial-monitor/ repo; this repo no longer carries a copy) or on the
 * marker+CRLF protocol the AI-side bridge matches against.
 *
 * Each board pays for it in exactly one place -- console_put() in
 * ev88g73a_console_out.c, the write() hook in uart_platform_stdio.c for the printf
 * boards -- so there is no second path to forget.
 *
 * A leftover "\r\n" in a source string is not a syntax error: it emits CR CR LF,
 * which is two carriage returns and one line break, i.e. INVISIBLE in a terminal.
 * Grep for it, do not look for it.
 */

/*
 * One character. Exists for the parser, not for handlers: app_console.c echoes
 * each received character, prints the '$' prompt, and emits the "\b \b" erase
 * sequence for a backspace -- six single-character writes, all of which were
 * putchar() upstream. Routing them here is what keeps stdio out of the console.
 *
 * '\n' is translated here too, deliberately, so that the layer has ONE rule instead
 * of a per-function exception. The visible consequence is in the echo: a terminal
 * that ends its line with CR LF gets CR CR LF back. That is a terminal's normal
 * no-op, and it is a smaller price than a console_out_char('\n') that silently
 * disagrees with console_out_str("\n").
 */
void console_out_char(char c);

/* NUL-terminated. WHERE the lines break is still the caller's business -- it is
 * composing a console reply and it knows -- but the line ENDING is not; see the rule
 * above. */
void console_out_str(const char *s);

/* Unsigned decimal, no padding. */
void console_out_u32(uint32_t value);

/* Fixed-width four-digit hex, for register evidence. Fixed width is the point:
 * register values get read bit by bit, and a variable-width field moves the bit
 * positions between lines, which is where a misread becomes a wrong diagnosis. */
void console_out_hex16(uint16_t value);

/*
 * True once everything queued has physically left the transmitter.
 *
 * Needed because of one specific command: a software reset discards whatever is
 * still in the TX FIFO, so the acknowledgement never appears and the reset becomes
 * indistinguishable from a command that was silently ignored -- on a board with no
 * reset button, that difference is the only evidence the command worked.
 *
 * A board with no way to answer should return true rather than block forever;
 * losing the last line beats hanging.
 */
bool console_out_idle(void);

/*
 * INPUT. The other half of the seam, and the last thing tying the console poll
 * loop to a board: a poll loop that says "is a character ready, give me the
 * character" needs no board knowledge beyond these two answers.
 *
 * console_in_read() returns false when nothing is waiting -- non-blocking, because
 * the caller is a cooperative main-loop poll, not a thread that may sleep.
 */
bool console_in_ready(void);
bool console_in_read(uint8_t *out);

/*
 * How many times this board's console reception has been rescued from a latched
 * overrun -- the count `?du` prints, and the reason it is part of the seam rather
 * than a peek into the HAL: the diagnostics module must not know which UART
 * instance the console is on (that is the one fact this seam exists to hide), and it
 * needs a NUMBER, not a register.
 *
 * A board whose console cannot latch an overrun, or that has nothing to report,
 * returns 0 -- the same answer a healthy board gives, which is exactly right: the
 * command's promise is "non-zero means it happened", not "zero means supported".
 */
uint32_t console_in_overrun_recovered(void);

/*
 * RX HEALTH, for the failure that cannot be asked about.
 *
 * This console has gone deaf once (2026-08-10): every byte the host sent was
 * lost, echo included, while TX kept printing and the audio block counter kept
 * advancing. The only known recovery is a reprogram, which resets the part --
 * so `?du`, and any other query, arrives after the evidence has been destroyed.
 * Whatever we want to know has to be on the wire ALREADY, printed by the half
 * that still works. That is why these numbers are pushed into the periodic
 * line instead of being added as another command.
 *
 * WHAT IS IN HERE WAS DECIDED BY MEASUREMENT, not by what the HAL declares.
 * The first version of this struct carried the ISR-ring counters, and on hardware
 * they stayed at zero through a `?tq` that was received AND echoed: this console
 * runs the UART in POLLING mode -- nothing calls the ring's configure entry point
 * -- so those counters are structurally zero here and would have been a
 * diagnostic that always reads "nothing happened". They were replaced by the
 * numbers that are live and the three registers that answer the rest:
 *
 *   bytes stops advancing while the host types -> reception has stopped
 *   overrun advancing                          -> the OERR deadlock, already handled
 *   framing non-zero                           -> edges reached the pin but were not
 *                                                 characters. MEASURED 2026-08-11:
 *                                                 board powered before the Nano's USB,
 *                                                 FERR latched, and the reader's own
 *                                                 early return kept it latched
 *   mode  URXEN/UARTEN cleared                 -> the receiver was switched off
 *   sta   OERR standing set                    -> the rescue itself is not running
 *   stah  URXBE set while the host sends       -> no byte arrives at the pin
 *                                                 (nEDBG bridge or PPS mapping)
 *                                                 -- but URXBE CLEAR with bytes=0 is
 *                                                 the opposite finding: the FIFO is
 *                                                 holding bytes the reader cannot take
 *
 * Registers here even though the seam's rule is "numbers, not registers": that
 * rule exists so the caller never learns WHICH UART the console is on, and it
 * still does not. See console_out_hex16(), which exists for exactly this.
 *
 * False means the board cannot answer -- the caller is expected to say so rather
 * than print zeros, since all-zero is also what a board nobody has typed at
 * legitimately reports.
 */
typedef struct {
    uint32_t bytes;    /* bytes handed to the console reader since boot */
    uint32_t overrun;  /* latched-overrun rescues -- the `?du` number */
    uint32_t framing;  /* errored bytes discarded (framing + parity). Non-zero says
                        * the pin DID carry edges, and they were not characters --
                        * the RX-deafness signature of a board powered before its
                        * debugger's USB. See nora_uart_read_byte(). */
    uint16_t mode;     /* UxMODE: UARTEN, URXEN */
    uint16_t sta;      /* UxSTA:  OERR, FERR, PERR */
    uint16_t stah;     /* UxSTAH: URXBE */
} console_in_health_t;

bool console_in_health(console_in_health_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_OUT_H */
