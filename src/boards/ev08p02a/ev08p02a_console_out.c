/*
 * ev08p02a_console_out.c -- EV08P02A's implementation of uart_platform/console_out.h,
 * and this board's ONLY console transport.
 *
 * Ported 2026-08-26 from EV88G73A's file of the same role (same MCU family, same
 * Nano-standard UART1 console pins). The paragraph below describing "five
 * delegations onto a parallel board API" is EV88G73A's own history, kept because
 * the shape it explains -- one file, no separate board UART API -- is exactly what
 * this file was born as; it is not a claim that this file had that history itself.
 *
 * That measurement: four of the old API's seven functions had exactly one caller --
 * this file -- while the other three were called 45 times from EV88G73A's own
 * main.c and audio module, which is to say the console's output had two names for one
 * UART and the shared modules' seam was being bypassed by the board's own code.
 *
 * What was board-specific about any of it was WHICH UART: EV08P02A_CONSOLE_UART_INST
 * in ev08p02a_board.h, because the Nano's debugger CDC is wired to UART1's RC10/RC11.
 * Decimal digit generation and fixed-width hex never were. Bring-up (pins, PPS,
 * clock-derived baud) stays in ev08p02a_board.c -- this file only moves bytes.
 *
 * Note what is NOT here: printf. This is a 256 KB part and its own code avoids stdio.
 * See uart_platform/console_out.h.
 */

#ifndef DSPIC33CK_BOARD_EV08P02A
#error "boards/ev08p02a/ev08p02a_console_out.c is EV08P02A-owned. Build it only in the CK256MC005_EV08P02A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "console_out.h"

#include <stddef.h>
#include <xc.h>               /* SRbits.IPL, SET_CPU_IPL() */

#include "app_config.h"       /* DEMO_* switches, resolved and checked */
#include "app_console.h"
#include "ev08p02a_board.h"   /* EV08P02A_CONSOLE_UART_INST */
#include "ev08p02a_pot.h"     /* module 'a' -- the POT Click on a mikroBUS socket */
#include "nora_uart.h"

/*
 * Interrupt-driven TX: a RAM ring, drained by the HAL's async TX engine from
 * _U1TXInterrupt.
 *
 * Why not nora_uart_write_byte(): it spins on UxSTAH.UTXBF, and the CPU spinning on a
 * UART status SFR for the ~33 ms a periodic report takes at 230400 put an audible
 * ~2 s "zat" on the MPQ6612A PWM output. Bench A/B, owner listening, 2026-10-14/15
 * (record: commit 88e4eda's message, and the AI memory note
 * ck-pwm-dma-eoc-trigger-drops-duty-updates; the bench code was not kept): the noise
 * stayed with the UART moved off its pad, off USB-CDC, and on UART2; it went away
 * with the same bytes shifted by UART1 but paced by a timer instead of the status
 * poll. So the poll is the cause, not the shifter, the pad or the baud -- the
 * mechanism (SFR-bus contention with DMA2's PG4DC write) is a hypothesis.
 *
 * Here the foreground only touches RAM. The TX ISR still reads UTXBF while it fills
 * the FIFO, but that is ~9 reads per 8 bytes instead of a continuous spin.
 *
 * Single producer (foreground; no ISR prints), single consumer (the TX ISR). head is
 * written only by the producer, tail and inflight only with the TX interrupt masked or
 * from inside it; both are 16-bit, so their loads and stores are atomic here.
 * Contiguous chunks of at most CONSOLE_TX_CHUNK_MAX are handed to nora_uart_tx_start(),
 * so a wrap costs one extra interrupt and no copy, and a writer waiting on a full ring
 * gets space back every ~5 ms instead of after the whole ring has drained.
 */
#define CONSOLE_TX_RING_SIZE  1024u   /* power of two; one ~750 B report fits whole */
#define CONSOLE_TX_RING_MASK  (CONSOLE_TX_RING_SIZE - 1u)
#define CONSOLE_TX_CHUNK_MAX  128u

_Static_assert((CONSOLE_TX_RING_SIZE & CONSOLE_TX_RING_MASK) == 0u,
               "CONSOLE_TX_RING_SIZE must be a power of two");

static uint8_t s_tx_ring[CONSOLE_TX_RING_SIZE];
static volatile uint16_t s_tx_head;
static volatile uint16_t s_tx_tail;
static volatile uint16_t s_tx_inflight;
static bool s_tx_async;

/* Bytes dropped because the ring was full with no way to drain it (TX interrupt
 * masked by the caller's IPL, or no transfer running). Read it from a debugger;
 * the console cannot be trusted to report its own loss. */
volatile uint16_t g_ev08p02a_console_tx_dropped;

/* With the TX interrupt masked or from inside it: hand the next contiguous run to the
 * engine. Called only when no transfer is in flight. */
static void console_tx_start_locked(void)
{
    uint16_t tail = s_tx_tail;
    uint16_t head = s_tx_head;
    uint16_t len;

    if (head == tail) {
        return;
    }
    len = (head > tail) ? (uint16_t)(head - tail)
                        : (uint16_t)(CONSOLE_TX_RING_SIZE - tail);
    if (len > CONSOLE_TX_CHUNK_MAX) {
        len = CONSOLE_TX_CHUNK_MAX;
    }
    s_tx_inflight = len;
    if (nora_uart_tx_start(EV08P02A_CONSOLE_UART_INST, &s_tx_ring[tail], len) !=
        NORA_UART_OK) {
        s_tx_inflight = 0u;
    }
}

/* TX ISR context (via nora_uart_tx_irq_handler): retire the chunk, chain the next. */
static void console_tx_event(nora_uart_instance_t inst, uint32_t events, void *user_data)
{
    (void)inst;
    (void)user_data;

    if ((events & NORA_UART_EVENT_SEND_COMPLETE) != 0u) {
        s_tx_tail = (uint16_t)((s_tx_tail + s_tx_inflight) & CONSOLE_TX_RING_MASK);
        s_tx_inflight = 0u;
        console_tx_start_locked();
    }
}

/* Start a transfer if none is running. The TX interrupt is masked by raising the CPU
 * IPL to its priority -- never lowering an IPL the caller already had. */
static void console_tx_kick(void)
{
    uint16_t saved_ipl = SRbits.IPL;

    if (saved_ipl < EV08P02A_CONSOLE_TX_IRQ_PRIORITY) {
        SET_CPU_IPL(EV08P02A_CONSOLE_TX_IRQ_PRIORITY);
    }
    if (!nora_uart_tx_is_busy(EV08P02A_CONSOLE_UART_INST)) {
        console_tx_start_locked();
    }
    if (saved_ipl < EV08P02A_CONSOLE_TX_IRQ_PRIORITY) {
        SET_CPU_IPL(saved_ipl);
    }
}

static void console_tx_push(uint8_t b)
{
    uint16_t head = s_tx_head;
    uint16_t next = (uint16_t)((head + 1u) & CONSOLE_TX_RING_MASK);

    /* Full: wait for the ISR to retire a chunk. This spins on RAM, not on a UART
     * SFR. It cannot wait for an ISR the caller's IPL masks, or for a transfer that
     * is not running, so those drop the byte instead of hanging. */
    while (next == s_tx_tail) {
        console_tx_kick();
        if (SRbits.IPL >= EV08P02A_CONSOLE_TX_IRQ_PRIORITY ||
            !nora_uart_tx_is_busy(EV08P02A_CONSOLE_UART_INST)) {
            g_ev08p02a_console_tx_dropped++;
            return;
        }
    }
    s_tx_ring[head] = b;
    s_tx_head = next;
}

void ev08p02a_console_out_tx_init(void)
{
    s_tx_head = 0u;
    s_tx_tail = 0u;
    s_tx_inflight = 0u;
    s_tx_async = nora_uart_is_initialized(EV08P02A_CONSOLE_UART_INST) &&
                 nora_uart_set_callback(EV08P02A_CONSOLE_UART_INST,
                                        console_tx_event, NULL) == NORA_UART_OK;
}

/* The HAL does not own the TX vectors by default (nora_uart_dspic33ck_isr.c,
 * NORA_UART_HAL_OWNS_TX_VECTORS); a board that uses async TX forwards its own. */
void __attribute__((interrupt, no_auto_psv)) _U1TXInterrupt(void)
{
    nora_uart_tx_irq_handler(EV08P02A_CONSOLE_UART_INST);
}

/*
 * This board's ONE output byte, and the one place a line ending is made.
 *
 * Sources carry a bare '\n' and the CR is added here, so the wire still carries CR LF
 * -- the whole argument is in uart_platform/console_out.h. Everything below routes
 * through this, including the digit and hex-nibble writers that can never emit a '\n':
 * one call site for the transport is smaller than four, and it leaves nowhere for a
 * second, untranslated path to appear later.
 *
 * Queues only; each public writer below kicks the transfer once when it is done, so a
 * line costs one masked section rather than one per byte. Before
 * ev08p02a_console_out_tx_init() (or if it failed) this is the old blocking write.
 */
static void console_put(char c)
{
    if (!s_tx_async) {
        if (c == '\n') {
            (void)nora_uart_write_byte(EV08P02A_CONSOLE_UART_INST, (uint8_t)'\r');
        }
        (void)nora_uart_write_byte(EV08P02A_CONSOLE_UART_INST, (uint8_t)c);
        return;
    }

    if (c == '\n') {
        console_tx_push((uint8_t)'\r');
    }
    console_tx_push((uint8_t)c);
}

static void console_flush_kick(void)
{
    if (s_tx_async) {
        console_tx_kick();
    }
}

void console_out_char(char c)
{
    console_put(c);
    console_flush_kick();
}

void console_out_str(const char *s)
{
    if (s == NULL) {
        return;
    }

    while (*s != '\0') {
        console_put(*s);
        s++;
    }
    console_flush_kick();
}

void console_out_u32(uint32_t value)
{
    char digits[10];
    uint8_t count = 0u;

    if (value == 0u) {
        console_put('0');
        console_flush_kick();
        return;
    }

    /* Generated least-significant first, so it is emitted in reverse below. The
     * bound on `count` is the array, not a digit count: a 32-bit value cannot exceed
     * ten decimal digits, and if that ever changed the loop would truncate rather
     * than write past the buffer. */
    while ((value != 0u) && (count < (uint8_t)sizeof(digits))) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (count-- != 0u) {
        console_put(digits[count]);
    }
    console_flush_kick();
}

void console_out_hex16(uint16_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    int8_t nibble;

    for (nibble = 3; nibble >= 0; nibble--) {
        console_put(hex[(value >> (nibble * 4)) & 0xFu]);
    }
    console_flush_kick();
}

bool console_out_idle(void)
{
    /* Ring drained and no chunk in flight, then the shifter itself: the reset
     * acknowledgement has to be on the wire, not just queued. */
    if (s_tx_async && (s_tx_head != s_tx_tail ||
                       nora_uart_tx_is_busy(EV08P02A_CONSOLE_UART_INST))) {
        return false;
    }
    return nora_uart_tx_done(EV08P02A_CONSOLE_UART_INST);
}

bool console_in_ready(void)
{
    return nora_uart_rx_ready(EV08P02A_CONSOLE_UART_INST);
}

bool console_in_read(uint8_t *out)
{
    if (out == NULL) {
        return false;
    }

    return nora_uart_read_byte(EV08P02A_CONSOLE_UART_INST, out) ==
           NORA_UART_OK;
}

uint32_t console_in_overrun_recovered(void)
{
    nora_uart_rx_status_t status;

    if (nora_uart_rx_status_get(EV08P02A_CONSOLE_UART_INST, &status) !=
        NORA_UART_OK) {
        return 0u;
    }

    return status.rx_overrun_recovered_count;
}

bool console_in_health(console_in_health_t *out)
{
    nora_uart_rx_status_t status;
    nora_uart_rx_regs_t regs;

    if (out == NULL) {
        return false;
    }

    /* No partial fill on failure: the caller is told "cannot answer" and prints
     * that, because an all-zero tuple is a legitimate healthy reading too. */
    if (nora_uart_rx_status_get(EV08P02A_CONSOLE_UART_INST, &status) !=
        NORA_UART_OK) {
        return false;
    }
    if (nora_uart_rx_regs_get(EV08P02A_CONSOLE_UART_INST, &regs) !=
        NORA_UART_OK) {
        return false;
    }

    out->bytes   = status.rx_byte_count;
    out->overrun = status.rx_overrun_recovered_count;
    out->framing = status.framing_error_count + status.parity_error_count;
    out->mode    = regs.mode;
    out->sta     = regs.sta;
    out->stah    = regs.stah;
    return true;
}

/*
 * Board hook for uart_app/console_dispatch.c: a module letter that needs board knowledge.
 *
 * 's' (reset, reset cause) and 'x' (traps) were both here, in ev08p02a_console.c, and
 * both moved to app/ once the four values they actually needed became the BOARD SEAM in app/app_traps.h.
 * For a long time this board owned NO letter as a result, and answering "not found" was
 * the accurate answer.
 *
 * It now owns TWO when WM8904 audio is enabled: 't' (audio transport path and load
 * controls) and the narrow Classic compatibility form `*cy00` (Type_TY AVAS toggle).
 * This is the hook working as intended -- the command attaches to a board file instead
 * of teaching the dispatcher about a board, and the handlers live next to the config
 * they control (boards/ev08p02a/main.c) rather than here.
 *
 * Declared at the point of use, which is console_dispatch.c's stated convention for a
 * module handler with exactly one caller.
 */
#if DEMO_ENABLE_WM8904_AUDIO
void ev08p02a_transport_console_onmsg(app_console_msg_t *msg);   /* main.c */
void ev08p02a_classic_console_onmsg(app_console_msg_t *msg);     /* main.c */
bool ev08p02a_transport_console_hotkey(uint8_t ch);              /* main.c */
#endif

void console_board_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

#if EV08P02A_ENABLE_ADC_POT
    /*
     * 'a' -- ADC. Board-owned because what it reads is a Click board in one of this
     * base board's mikroBUS sockets, which is the case console_dispatch.c keeps this
     * hook for. First letter to claim it on this board.
     *
     * The single-key AVAS hotkeys 'a' / 'A' are not in the way: a hotkey is only taken
     * while no command line is being built, and '?' or '*' has already started one by
     * the time this 'a' arrives (app_console_is_idle(), app_console.h).
     */
    if (msg->module == 'a') {
        ev08p02a_pot_console_onmsg(msg);
        return;
    }
#endif

#if DEMO_ENABLE_WM8904_AUDIO
    if (msg->module == 't') {
        ev08p02a_transport_console_onmsg(msg);
        return;
    }

    if (msg->module == 'c') {
        ev08p02a_classic_console_onmsg(msg);
        return;
    }
#endif

    msg->status = APP_CONSOLE_ERR_NOT_FOUND;
}

/*
 * Board hook for uart_app/console_task.c: single-key hotkeys, routed the same way and
 * for the same reason as the 't' module above. The keys live next to the commands they
 * duplicate (boards/ev08p02a/main.c) so that a key and its command cannot drift apart;
 * this file still knows about the console and nothing about TDM.
 */
bool console_board_hotkey(uint8_t ch)
{
#if DEMO_ENABLE_WM8904_AUDIO
    return ev08p02a_transport_console_hotkey(ch);
#else
    (void)ch;

    return false;
#endif
}
