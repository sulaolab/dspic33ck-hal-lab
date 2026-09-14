/*
 * ev88g73a_board.c -- this board's bring-up: the ORDER and the STEPS, in one file.
 *
 * THE SECTION ORDER HERE IS THE SAME AS dm330030_board.c's, deliberately (2026-08-03):
 * board-local policy defines, bring-up observability volatiles, the clock stage,
 * <board>_board_init() (the order itself), the individual pin/peripheral stages,
 * the clock accessors, TDM pins, I2C1, and the BOARD SEAM (app/app_traps.h) at the
 * bottom. The two
 * files are being reduced to "same calls, different parameter context" (see
 * docs/ck_source_layout.md), and that progress is only legible at a glance if the two
 * files are read in the same sequence. A section this board does not have is simply
 * absent rather than moved. (Until 2026-08-04 that read "DM330030's REFO1 MCLK"; there
 * is no such section on either side any more -- NEITHER board generates an MCLK, because
 * neither defines an MCLK pin and no code path here can drive one. The premise the deleted
 * stage rested on was false too: the WM8904 board is the same on both and its 12.288 MHz
 * XTAL is fitted on both.
 * See app/wm8904_audio.h's note where the mclk_init hook used to be before restoring
 * anything of that shape.)
 *
 * One asymmetry CLOSED on 2026-08-03: that board's boot-time ANSEL port sweep, which this
 * one never had, was deleted rather than copied here. Both boards now own ANSEL per pin,
 * in the stage that configures the pin -- which is what this file already did, and is why
 * it was the shape the other board was moved to and not the reverse.
 *
 * TWO MORE CLOSED ON 2026-08-04, both of the "one board has a function the other does not,
 * for no reason in the hardware" kind:
 *   - ev88g73a_uart1_init() is gone; the console PERIPHERAL stage is the shared
 *     uart_platform_stdio_init() call both boards now make. See the note where it was.
 *   - ev88g73a_reset_cause_str()/_raw() are gone; the BOARD SEAM functions at the
 *     bottom answer directly, as DM330030's always did.
 * Neither touched what the board DOES -- the register writes and the strings are the same
 * -- so both are shape, which is the only thing a diff of these two files can show.
 */

#ifndef DSPIC33CK_BOARD_EV88G73A
#error "boards/ev88g73a/ev88g73a_board.c is EV88G73A-owned. Build it only in the CK64MC105_EV88G73A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "ev88g73a_board.h"

#include <stddef.h>
#include <stdint.h>
#include <xc.h>

/* This board's half of the app-layer BOARD SEAM (declared at the bottom of app/app_traps.h);
 * implemented at the bottom of this file, next to the reset-cause latch it mostly exposes. */
#include "app_traps.h"

#include "nora_clock.h"
#include "nora_dma.h"
#include "nora_gpio.h"
#include "nora_i2c_master.h"
#include "nora_pps.h"
#include "nora_gpio_table.h"         /* this board's pins as data, applied in one call */
#include "nora_clock_dspic33ck_bringup.h" /* the shared boot clock stage; this board states 2 frequencies */
#include "nora_reset.h"         /* the shared RCON decode; this board supplies the latch */
#include "nora_spi_i2s_tdm_dspic33ck_pins.h"   /* the shared TDM routing; this board supplies 4 RP numbers */
#include "uart_platform_stdio.h"    /* the shared console peripheral stage, as on DM330030 */

/*
 * The console instance is stated twice on this board and must not drift: once for the
 * TRANSPORT (EV88G73A_CONSOLE_UART_INST, used by ev88g73a_console_out.c, which addresses
 * the UART directly because this part has no room for printf) and once for the BRING-UP,
 * inside uart_platform_stdio.c. It is the same peripheral, so say so to the compiler.
 *
 * DM330030 needs no such assertion: it names no instance at all, because everything it
 * prints goes through printf and therefore through that file's write() hook.
 */
_Static_assert((int)EV88G73A_CONSOLE_UART_INST == (int)UART_PLATFORM_CONSOLE_UART_INST,
               "EV88G73A's console transport and the shared console bring-up must use "
               "the same UART instance");

/* Every pin number this file uses is in ev88g73a_pins.h (included via the board header),
 * so a wiring change is one file and this one keeps only what it DOES with the pins. */

/* Fast-mode, matching the WM8904 documentation's own I2C1 setup. A rate is policy, not a
 * pin, so it stays here with its reason rather than in the pins header. */
#define EV88G73A_I2C1_BUS_HZ  (400000UL)

/*
 * Bring-up observability. The console is UART1 itself, so the stages below cannot
 * print their own failure. Keep the results in volatiles so a debugger can read them
 * even when nothing comes out of the port.
 *
 * THE CONSOLE PERIPHERAL'S OWN STATUS IS NOT HERE any more (2026-08-04). It was
 * g_ev88g73a_uart1_init_status, set by an ev88g73a_uart1_init() that built the same
 * eleven-field nora_uart_config_t DM330030 reaches through
 * uart_platform_stdio_init(). Both boards now call that one function, so the status is
 * its g_console_init_status -- one symbol, one name, both boards, and the name a
 * debugger is told to look for in the bring-up docs. Nothing in this tree ever read the
 * board-prefixed one.
 */
volatile nora_clock_status_t g_ev88g73a_clock_init_status =
    NORA_CLOCK_ERR_NOT_SUPPORTED;

/*
 * The PIN stages, which this file used to discard with a bare (void) on every call.
 *
 * Kept separate from the console peripheral's status (g_console_init_status, in the shared
 * uart_platform_stdio.c) because they fail independently: a mis-routed PPS leaves
 * nora_uart_init() reporting OK while producing no output, so one result cannot stand
 * in for the other. bool rather than a status enum because that is all the GPIO/PPS HAL
 * returns.
 *
 * Naming matches DM330030's g_dm330030_uart1_pins_init_ok / _user_io_pins_init_ok:
 * two boards answering the same question should not need two vocabularies. That board
 * had the checks and no fallback policy; this one had the policy and no checks.
 */
volatile bool g_ev88g73a_uart1_pins_init_ok = false;
volatile bool g_ev88g73a_user_io_pins_init_ok = false;

/*
 * The rest of the bring-up evidence. `g_<board>_` and volatile for the same reason as
 * the two above and as DM330030's g_dm330030_clock_on_target / _osccon_after_switch:
 * these are read by a debugger on a board whose console may never come up, and a
 * board-prefixed name is what you can actually find in a symbol table. They were file
 * statics under bare names until 2026-08-03; nothing outside this file ever reached them
 * directly then either, so the change is a rename plus volatile.
 *
 * The latched RCON word used to be a file static here (bare-named `reset_cause_raw`,
 * because DM330030 had no counterpart to it). It now lives inside the reset HAL, held by
 * nora_reset_snapshot_capture() -- see ev88g73a_board_init(). What this board still
 * owns, and the only thing it ever owned, is WHICH POLICY it captures with.
 *
 * uart_baud_applied was a file static beside it and is now a LOCAL in
 * ev88g73a_board_init() (2026-08-04). It only ever existed at file scope because
 * ev88g73a_uart1_init() was a second function that had to read it; with the peripheral
 * stage shared, the rate is decided and passed in the same three lines. It is not kept as
 * debugger evidence the way DM330030 keeps g_dm330030_console_baud, because on this board
 * the question is already better answered elsewhere: nora_uart_get_baudrate() reports
 * what the HAL APPLIED rather than what the board asked for, which is the distinction
 * ev88g73a_board.h refuses to blur (see the note where ev88g73a_uart_baud() used to be).
 */
volatile bool     g_ev88g73a_clock_on_target = false;
volatile uint16_t g_ev88g73a_osccon_after_switch = 0u;

/* Which way it failed, as nora_clock_dspic33ck_diag_t -- see the header. Separate from
 * the status because the status is the CONTRACT's answer and must stay portable, while
 * this is this silicon's detail; a consumer that branched on it would stop compiling on
 * the other family, which is exactly what the split is for. */
volatile uint16_t g_ev88g73a_clock_diag = 0u;

/*
 * Run the system clock from the internal FRC through the PLL to the device
 * maximum: Fosc 200 MHz, Fcy 100 MHz (100 MIPS). This is what makes 230400 baud
 * reachable at all.
 *
 * At the 8 MHz FRC boot clock (Fcy = 4 MHz) the 16x baud divisor for 230400
 * rounds to 1, giving 250000 baud -- +8.5%, far outside the ~2% a UART frame
 * tolerates, so every character arrives corrupted. No divisor lands near 230400
 * at 4 MHz (the next one down is 200000, -13%), so raising Fcy is the only fix.
 * Solved by the Clock HAL as PLLPRE 1 / PLLFBDIV 200 / POST1DIV 2 / POST2DIV 2:
 * Fvco 1600 MHz, FPLLO 400 MHz, and the silicon's fixed FPLLO/2 gives Fosc
 * 200 MHz. Read back from the oscillator registers on this board (2026-08-10),
 * which is where the previous version of this comment -- PLLFBDIV 50 / POST2DIV 1,
 * Fvco 400 MHz -- was found to be wrong. It had never matched the hardware: the
 * console log shows 1/200/2/2 on every boot before this HAL was replaced too.
 *
 * The stage's POLICY -- snapshot OSCCON before anything else, judge "on target" from
 * three conditions rather than the HAL status alone, and never stay on a PLL that did
 * not lock -- was written out here and copied by hand into dm330030_board.c. It is now
 * nora_clock_dspic33ck_bringup() (2026-08-02), so this board states its two frequencies and
 * nothing else. See nora_clock_dspic33ck_bringup.h; the rules are unchanged and this board
 * is where two of them were learned.
 */
static void ev88g73a_clock_init(void)
{
    static const nora_clock_dspic33ck_bringup_t req = {
        .source         = NORA_CLOCK_SOURCE_FRC,
        .input_hz       = EV88G73A_FRC_HZ,
        .target_fosc_hz = EV88G73A_TARGET_FOSC_HZ,
    };
    nora_clock_dspic33ck_bringup_result_t res;

    nora_clock_dspic33ck_bringup(&req, &res);

    /* Republished under this board's own names, which is what the console and a
     * debugger look for. */
    g_ev88g73a_clock_init_status   = res.status;
    g_ev88g73a_osccon_after_switch = res.osccon_after_switch;
    g_ev88g73a_clock_on_target     = res.on_target;
    g_ev88g73a_clock_diag          = res.diag;
}

/* ========================================================================== */
/* Bring-up order -- the one thing this half of the file is for.               */
/* ========================================================================== */

/* Static, and their order is the point. Declared here so ev88g73a_board_init() can be
 * read as the order without the stages in the way, matching dm330030_board.c. */
static bool ev88g73a_uart1_pins_init(void);
static bool ev88g73a_user_io_pins_init(void);

void ev88g73a_board_init(void)
{
    /*
     * Latch the reset cause FIRST, before anything else can touch RCON. AND_CLEAR
     * clears the cause bits immediately after capturing them, so the next boot
     * reports its own cause instead of an accumulation of every reset since
     * power-on. See the header for why that matters on a board with no reset button.
     *
     * This was an open-coded `RCON` read plus nora_reset_cause_clear() until
     * 2026-08-06. The pair is the same two operations in the same order -- the
     * capture owns them jointly now, so a board cannot accidentally clear before it
     * has saved, and the policy is stated by name rather than implied by which lines
     * are present. The return value is discarded on purpose: this is the first and
     * only capture by construction (it is the first statement of board_init), and
     * there is no console to report a failure to yet.
     */
    (void)nora_reset_snapshot_capture(NORA_RESET_LATCH_AND_CLEAR_RCON);

    /* Clock first: the UART divisor is derived from the resulting Fcy. */
    ev88g73a_clock_init();

    /* Only the fast console is worth having, but it is unrepresentable at the
     * FRC fallback -- so the rate follows the clock we actually got. A local since
     * 2026-08-04: the console stage it feeds is now a call, not a second function that
     * had to reach a file static. */
    const uint32_t uart_baud_applied = g_ev88g73a_clock_on_target
                                           ? EV88G73A_UART_BAUD_FAST
                                           : EV88G73A_UART_BAUD_SAFE;

    /* LED0 and SW0. Ordered before the console on this board and after it on
     * DM330030, which is a real difference and not a formatting one: there the
     * console is what would report a user-I/O failure, here the same information
     * is read from g_ev88g73a_user_io_pins_init_ok by a debugger either way. */
    g_ev88g73a_user_io_pins_init_ok = ev88g73a_user_io_pins_init();

    /* Pins first, then the peripheral -- two stages with two results, and now the same
     * two CALLS DM330030 makes: this board's pin stage, then the shared console
     * bring-up. The rate is the argument, which is the whole reason that function takes
     * one; the result lands in g_console_init_status. */
    g_ev88g73a_uart1_pins_init_ok = ev88g73a_uart1_pins_init();
    uart_platform_stdio_init(uart_baud_applied);

    /* DMA SRAM-bus priority (MSTRPR.DMAPR), needed before any DMA channel is
     * armed -- the SPI/TDM transport behind app/wm8904_audio.c is the consumer
     * on this profile. */
    nora_dma_global_init();
}

/* ========================================================================== */
/* The stages themselves                                                       */
/* ========================================================================== */

/*
 * Every stage checked, not cast away: this is the console, so a routing failure here is
 * precisely the case that cannot report itself. RX as well as TX, unlike DM330030 --
 * that board's console is output-only, which is a wiring fact and not a shape one.
 *
 * Its own function since 2026-08-03, named to match dm330030_uart1_pins_init(): the pin
 * stage and the peripheral stage are two stages with two results, and this board used to
 * run them as one. Splitting it is what made the NEXT step possible -- the peripheral
 * half is now the shared uart_platform_stdio_init() call in board_init() above (see the
 * note where ev88g73a_uart1_init() used to be, further down).
 */
static bool ev88g73a_uart1_pins_init(void)
{
    return nora_pinmux_route_output(NORA_PPS_OUTPUT_U1TX,
                                         EV88G73A_UART1_TX_RP,
                                         true) &&
           nora_pinmux_route_input(NORA_PPS_INPUT_U1RX,
                                        EV88G73A_UART1_RX_RP);
}

/*
 * ev88g73a_uart1_init() USED TO SIT HERE and left on 2026-08-04. It was a local
 * nora_uart_config_t and one nora_uart_init() call -- and every one of its
 * eleven fields already held the value uart_platform_stdio.c sets: Fcy from the Clock
 * HAL, the caller's baud, no timeout and no time source, 8N1, high_speed, BCLKSEL =
 * FOSC_DIV2, TX and RX both enabled. Not "similar": the same, field for field. So
 * ev88g73a_board_init() now makes the same uart_platform_stdio_init(baud) call
 * DM330030's does, and the difference between the two boards' consoles is down to what
 * it always really was -- how they WRITE (printf there, console_out.h here), not how the
 * peripheral is brought up.
 *
 * The reasoning that was written out here is kept where it now applies, in
 * uart_platform_stdio.{h,c}, because it is not board-specific:
 *
 *   - FOSC/2 = 100 MHz is the peripheral clock this project's BRG math is built on, and
 *     it resolves 230400 to within half a percent (4x oversampling, BRG = 108 ->
 *     229357 baud, -0.45%). The FRC's own tolerance dominates that error, not the
 *     divisor. Do not hand the baud generator undivided FOSC.
 *   - The peripheral is initialised even when the pin stage failed, so that a debugger
 *     can read BRG and the enables and tell "wrong rate" from "wrong pin".
 *
 * This was recorded as OUT OF SCOPE for the convergence series (docs/ck_source_layout.md,
 * "a different path, not a different parameter") on the grounds that DM330030's console
 * is printf and this board's is not. That held for the WRITE side and was over-applied:
 * uart_platform_stdio.c was already compiled into this configuration -- ex="false" in
 * both configs -- and only the init was being duplicated.
 *
 * IT COSTS EV88G73A +18 BYTES OF FLASH (and gives back 4 of RAM), measured, and the reason
 * is worth knowing before assuming de-duplication is free here: the map shows that with
 * --gc-sections on (since 2026-08-03) the shared init's whole 54-byte .text was DISCARDED
 * from this configuration, because nothing called it. Calling it links those 54 bytes;
 * deleting the local config, the two reset-cause forwarders and the uart_baud_applied
 * static gives back about 36. Reported rather than absorbed, per the standing rule that EV
 * growth is the user's call; adopted for the de-duplication, with the number in hand.
 */

/*
 * This board's two user-I/O pins, as data (2026-08-02), and as its own stage rather
 * than a block inside board_init (2026-08-03) so it sits where DM330030's nine-entry
 * equivalent sits. What differs between the two boards here is only the table.
 *
 * SW0 is described by one nora_gpio_config_t rather than config_digital_input()
 * followed by set_pull(). One struct is one description: the pull cannot be forgotten
 * or lost between two calls, and forgetting it here is not theoretical -- SW0 has no
 * external pull-up (DS70005517B Sec.4.2.2, "Mechanical Switch": RD13 ties to GND
 * through the button and nothing else), so without it RD13 floats when the button is
 * released and ev88g73a_sw0_pressed() is meaningless. Active-low, same source. That
 * struct was a local here and is now nora_gpio_cfg_input_pullup, shared with the
 * board that had its own copy.
 *
 * NOTHING PRECEDES THIS THAT TOUCHES ANSEL, and both entries need that not to matter --
 * which is the case, because each config states .analog itself
 * (hal_gpio/nora_gpio_table_dspic33ck.c). Worth being explicit about, because the note that
 * used to sit here claimed neither pin is analog-capable on MC105 and that was WRONG:
 * LED0 is RD10 and SW0 is RD13, and ANSELD implements exactly bits 10 and 13 on this
 * part (DFP header, checked 2026-08-03). Both pins come out of reset ANALOG. An LED that
 * cannot be driven and a button that always reads pressed are what an unstated analog
 * bit would look like here, so the shared configs' .analog = false is load-bearing on
 * this board -- it is not defensive boilerplate.
 *
 * DM330030 reached the same place from the other direction: it had a boot-time sweep that
 * cleared ANSEL across all five ports, deleted 2026-08-03, which is what its table's old
 * "pot entry must stay last" ordering rule existed to work around. Neither table has an
 * ordering dependency now.
 */
static const nora_gpio_table_entry_t ev88g73a_user_io_pins[] = {
    { EV88G73A_LED0_PIN, &nora_gpio_cfg_output_high },  /* LED0 starts lit */
    { EV88G73A_SW0_PIN,  &nora_gpio_cfg_input_pullup },
};

static bool ev88g73a_user_io_pins_init(void)
{
    return nora_gpio_table_apply(
        ev88g73a_user_io_pins,
        (uint8_t)(sizeof ev88g73a_user_io_pins / sizeof ev88g73a_user_io_pins[0]));
}

/*
 * FIVE CLOCK ACCESSORS USED TO SIT HERE and left on 2026-08-03, because this board was
 * the only one in the tree that had them:
 *
 *   ev88g73a_fosc_hz() / _fcy_hz() were one-line pass-throughs to
 *       nora_clock_get_fosc_hz() / _get_fcy_hz(). Every other consumer already
 *       called the HAL directly -- uart_platform_stdio.c, app/wm8904_audio.c,
 *       app/demo_tdm_master_loopback.c, and both of DM330030's own rate-derived stages.
 *       A board cannot make the HAL's record of Fosc any more authoritative by
 *       forwarding it, so it should not appear to.
 *   ev88g73a_clock_at_target() / _clock_status_code() / _osccon_after_switch() returned
 *       the three variables above. Those are now g_ev88g73a_* and volatile, so main.c
 *       reads them the same way dm330030's report reads g_dm330030_*: the value, not a
 *       function that returns the value.
 *
 * The point is not the twelve lines. It is that "what clock did we get" now has ONE
 * answer shape across both boards -- HAL for the frequencies, g_<board>_ volatiles for
 * the verdict -- instead of two.
 */

/*
 * TWO SETS OF FUNCTIONS USED TO CLOSE THIS FILE AND BOTH LEFT, for the same reason:
 * this file is bring-up, and neither was.
 *
 *   ev88g73a_led0_set/_toggle, ev88g73a_sw0_pressed -> ev88g73a_io.c
 *       Runtime accessors. Their pins are still CONFIGURED above, which is the half
 *       that genuinely is bring-up and stays the only owner of direction.
 *   rx_ready, read_byte, tx_done and the three text writers -> ev88g73a_console_out.c
 *       The console transport, next to the uart_platform/console_out.h contract it
 *       implements. What this file keeps of the console is the part that IS board
 *       knowledge: the pins and their PPS routing, and the rate the clock outcome
 *       allows. The peripheral configuration itself is shared (see above).
 */

/*
 * THE RESET CAUSE IS ANSWERED ONCE, at the BOARD SEAM names (app/app_traps.h), at the
 * bottom of this file -- and it used to be answered twice (deleted 2026-08-04).
 *
 * ev88g73a_reset_cause_raw() and ev88g73a_reset_cause_str() sat here, and
 * board_reset_cause_raw()/_str() at the bottom were one-line forwards to them: two public
 * names per question on a board that has one answer. DM330030 never had the pair -- its
 * seam functions read RCON directly -- so this was pure shape difference, which is
 * exactly what makes a diff of the two files hard to read.
 *
 * The one caller outside the seam was main.c's banner, which asked
 * ev88g73a_reset_cause_str() while the shared console's *rc command asked
 * board_reset_cause_str() for the same string. It now asks the same one, and this board
 * exports no reset-cause function of its own.
 *
 * WHAT IS NOT SHARED, and must not be: the LATCH. See ev88g73a_board_init() -- RCON is
 * captured and cleared before anything else runs, because a Curiosity Nano has no reset
 * button and telling a POR from the *sr command's SWR is the only evidence a software
 * reset happened. DM330030 deliberately reads RCON live instead. That difference is real,
 * it is one line in board_init() plus the `true`/`false` argument to the shared decoder,
 * and it stays visible.
 */

/* -------------------------------------------------------------------------- */
/* SPI1/TDM pin routing for the WM8904 audio path.                             */
/*                                                                            */
/* Was ev88g73a_wm8904_configure_pins() inside ev88g73a_wm8904_audio.c, which   */
/* is now app/wm8904_audio.c. This is the whole board content of that path:     */
/* four RP numbers and their directions.                                        */
/*                                                                            */
/* Switched on the ROLE it is handed, not on a build-time #if as before. The    */
/* board has no opinion on which side of the pair drives BCLK/FS -- that is the */
/* profile's choice (see the config in main.c) -- and the wiring supports both   */
/* directions, so refusing one of them here was the board claiming a decision   */
/* it does not own. Same four pins either way; only direction and PPS differ.    */
/*                                                                            */
/* THE FORTY LINES OF DIRECTIONS AND PPS TOKENS THAT USED TO BE HERE ARE GONE    */
/* (2026-08-02): they were identical to DM330030's, so they now live once in     */
/* hal_spi_i2s_tdm/nora_spi_i2s_tdm_dspic33ck_pins.c and this board passes its four   */
/* numbers. The order that file uses is the one that was verified here, so       */
/* nothing about what this board does changed -- see that file's header.         */
/*                                                                            */
/* Placed before I2C1 to match dm330030_board.c's section order (2026-08-03).    */
/* Nothing sits between the two on either board, because this firmware has no    */
/* MCLK stage on either -- so there is nothing to order against (2026-08-04).    */
/* (Said "both codecs run from the WM8904 board's own X1" until 2026-08-09; the  */
/* ordering argument only needs the absence of OUR stage. See plan doc 20.4.)    */
/* -------------------------------------------------------------------------- */

static const nora_spi_i2s_tdm_pinmap_t ev88g73a_tdm_pinmap = {
    .bclk = EV88G73A_TDM_RP_BCLK,
    .fs   = EV88G73A_TDM_RP_FS,
    .sdo  = EV88G73A_TDM_RP_SDO,
    .sdi  = EV88G73A_TDM_RP_SDI,
};

bool ev88g73a_tdm_pins_init(nora_spi_i2s_tdm_clock_role_t role)
{
    return nora_spi_i2s_tdm_pins_configure(&ev88g73a_tdm_pinmap, role);
}

/* -------------------------------------------------------------------------- */
/* I2C1 bring-up: pins, pull-ups, HAL init.                                    */
/*                                                                            */
/* Was inside ev88g73a_i2c1_probe.c, which is now app/i2c_probe.c. This half    */
/* stayed behind because it is the only half that names a pin -- the probe      */
/* reaches it through the bus_init hook in i2c_probe_t, and the WM8904 path      */
/* calls it directly (same bus, same pins).                                     */
/*                                                                            */
/* Same physical pins as DM330030's (RC8/RC9, the alternate I2C1 pair) and the   */
/* same 400 kHz. The two boards still differ in three small ways, which is       */
/* step 4 of the convergence work: this one asks in RP form, enables the         */
/* internal pull-ups, and passes timeout_ms = 10.                               */
/* -------------------------------------------------------------------------- */

/*
 * static, like every other pin stage in this file: its only caller is
 * ev88g73a_i2c1_init() below, and DM330030's dm330030_i2c1_pins_init() is static for the
 * same reason -- exporting it would let a caller clear ANSEL and set the pulls without
 * ever initialising the I2C module, i.e. a bus that looks configured and answers nothing.
 * Its own function since 2026-08-03, to match that board.
 */
static bool ev88g73a_i2c1_pins_init(void)
{
    /* Force digital. This is a NO-OP on this part and on MP508 alike: neither implements
     * an ANSELC bit for RC8/RC9 (checked against both DFP headers on 2026-08-03 -- only
     * ANSELC0-3/6/7 exist on either). The comment that used to sit here said MP508's
     * equivalent pins ARE analog-capable and the call was load-bearing there; that was
     * wrong, and DM330030's copy of the same claim was corrected with it.
     *
     * Kept for the reason the SPI/DMA audit favoured explicit-and-verified over
     * implicit-and-assumed, and more concretely because ANSEL is this stage's to own now
     * that no sweep states it elsewhere: writing a bit the silicon does not implement is
     * harmless, and the default SDA1/SCL1 pair either board could be retargeted to is
     * RB8/RB9, where ANSELB8/9 are real. */
    if (!nora_gpio_rp_set_analog(EV88G73A_I2C1_RP_ASDA, false)) return false;
    if (!nora_gpio_rp_set_analog(EV88G73A_I2C1_RP_ASCL, false)) return false;

    /* No PPS routing call: see the note on the RP defines at the top of this file. */

    /* Weak internal pull-ups: with nothing on the bus, SDA/SCL would otherwise float
     * with no external pull-up to hold them high. This does not make the bus equivalent
     * to a real one (no device, no real bus capacitance), but it is what turns
     * "undefined floating logic" into "a clean idle bus a START can be driven onto". */
    if (!nora_gpio_rp_set_pull(EV88G73A_I2C1_RP_ASDA, NORA_GPIO_PULL_UP)) return false;
    if (!nora_gpio_rp_set_pull(EV88G73A_I2C1_RP_ASCL, NORA_GPIO_PULL_UP)) return false;

    return true;
}

/*
 * The single public I2C1 bring-up: both the probe (i2c_probe_t.bus_init) and the audio
 * path (wm8904_audio_port_t.i2c_init) call THIS, exactly as DM330030's single
 * dm330030_i2c1_init() serves both of its callers. That board kept a static
 * _init_at(bus_hz) underneath because it once had two rates; it is gone as of 2026-08-04,
 * so both boards now have exactly one function here and the rate is a define on each.
 */
bool ev88g73a_i2c1_init(void)
{
    nora_i2c_config_t cfg;

    /* Pins first, and checked -- see DM330030's note: a probe that skips this drives the
     * bus through pins still configured as analog inputs on a part where those bits are
     * real. */
    if (!ev88g73a_i2c1_pins_init()) {
        return false;
    }

    cfg.fcy_hz             = nora_clock_get_fcy_hz();
    cfg.bus_hz             = EV88G73A_I2C1_BUS_HZ;
    /*
     * timeout_ms = 10 is INERT while get_ms is NULL -- the HAL disables timeout handling
     * entirely without a time source (nora_i2c_master.h), so what actually bounds a
     * stuck bus here is its pending/bus-idle guards, exactly as on DM330030 where the
     * timeout reads 0. Left as it is rather than quietly wired to GetTicks(): a running
     * 1 ms time now exists in both profiles (app/timer_app.h), so this CAN be honoured,
     * but turning a dead timeout live changes I2C failure behaviour and wants its own
     * hardware pass. Recorded in docs/ck_source_layout.md.
     */
    cfg.timeout_ms         = 10u;
    cfg.get_ms             = NULL;
    cfg.pending_timeout_ms = 0u;

    return nora_i2c_init(NORA_I2C_INST_1, &cfg) == NORA_I2C_OK;
}

/* -------------------------------------------------------------------------- */
/* BOARD SEAM (app/app_traps.h) -- the non-text facts the app layer needs here. */
/*                                                                            */
/* Four functions, and they are the entire board content of the console's       */
/* system and exception commands. That is the measurement that let those        */
/* commands move to uart_app/system_console.c and uart_app/traps_console.c: what they     */
/* needed from EV88G73A was the reset cause and two addresses, not a pin.       */
/* -------------------------------------------------------------------------- */

/*
 * The LATCHED word, not RCON -- which is the whole difference from DM330030's identically
 * named function, and the reason this board can name one cause. See ev88g73a_board_init().
 */
uint16_t board_reset_cause_raw(void)
{
    /* The snapshot is 32-bit for AK parity; RCON is 16-bit here and app_traps.c
     * tests it bit by bit, so the seam keeps the register's own width. */
    return (uint16_t)nora_reset_snapshot_raw();
}

const char *board_reset_cause_str(void)
{
    /*
     * The seven-way ladder that used to be here is nora_reset_cause_str()
     * (2026-08-03): RCON's bits and their priority are a family fact, not a board one,
     * and DM330030 was answering this same question with a disclaimer string for want of
     * a copy. What stayed on this board is the LATCH, which is why `true` is honest here:
     * the AND_CLEAR snapshot holds exactly one boot's worth of causes, so naming the most
     * specific one is sound. DM330030 passes `false` to the same decoder and gets a
     * refusal whenever more than one bit is set.
     *
     * Deliberately still the LEGACY decoder rather than
     * nora_reset_snapshot_cause_str(): the portable cause enum has no TRAPR or
     * IOPUWR member (they are CK-specific), and TRAPR is the only evidence a stack
     * overflow ever produces -- see the trap-conflict report in app_traps.c. The
     * portable classification is reported alongside this string by the console, not
     * instead of it.
     */
    return nora_reset_cause_str(board_reset_cause_raw(), true);
}

/*
 * The first data address with nothing behind it. RAM on CK64MC105 is
 * 0x1000..0x2FFF (linker script p33CK64MC105.gld: data ORIGIN 0x1000,
 * LENGTH 0x2000), so 0x3000 is the first address that does not exist.
 */
volatile uint16_t *board_trap_bad_addr(void)
{
    return (volatile uint16_t *)0x3000u;
}

/*
 * A stack pointer past SPLIM: the end of RAM. The linker does set SPLIM -- the map
 * shows --stackguard=16 with the stack at 0x2172 length 0xe8e, i.e. topping out at
 * 0x3000 -- so 0x2FF0 leaves just enough room for the move itself to complete
 * before the next push crosses the limit.
 */
uint16_t board_trap_stack_beyond_limit(void)
{
    return 0x2FF0u;
}

/* The three text writers that used to close this file (string, hex16, u32) are now
 * ev88g73a_console_out.c's console_out_str/hex16/u32 -- see the "TWO SETS OF FUNCTIONS
 * USED TO CLOSE THIS FILE" note above. */
