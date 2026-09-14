#ifndef EV08P02A_BOARD_H
#define EV08P02A_BOARD_H

/*
 * ev08p02a_board.{c,h} -- BRING-UP. Everything here runs once, in order, before the
 * profile starts: reset-cause latch, clock policy, console UART, pin routing, the two
 * peripheral bring-ups the audio path needs, and this board's half of the BOARD SEAM (app/app_traps.h).
 *
 * Ported 2026-08-26 from EV88G73A's board.{c,h} (same MCU family, same Nano-standard
 * bring-up sequence: reset-cause latch, clock, console UART, user I/O, I2C1, TDM
 * pins). This board never had a monolithic pre-split file of its own -- it is born
 * already in the three-role shape EV88G73A's own board_ev88g73a.{c,h} was split
 * into, because that is a line a later reader can re-derive:
 *
 *   ev08p02a_pins.h    facts      compile time only, no code
 *   ev08p02a_board.*   bring-up   once, in order   <- this file
 *   ev08p02a_io.*      runtime    every iteration, owns no init
 *
 * TWO THINGS ARE NOT IN THIS HEADER, by the same convention:
 *   - LED0 and SW0 accessors      -> ev08p02a_io.h  (runtime, not bring-up)
 *   - the exerciser switches      -> board_profile.h. They select which SHARED
 *     app/ modules this PROFILE runs; nothing about the board changes with them.
 *     Each board directory has a board_profile.h with one normalized name per
 *     decision, app/app_config.h refuses anything unresolved, and main.c only
 *     READS them -- see that file's own history note for why the convention
 *     exists (it predates this board).
 *
 * The pin configuration this file performs is still what owns direction on those pins:
 * ev08p02a_io.c only reads and writes levels.
 *
 * A NOTE ON EVERY DATED COMMENT BELOW: same as ev08p02a_board.c's -- they are EV88G73A's
 * own development history, kept for the technical reasoning, not a claim about this
 * file's own (nonexistent) history.
 */

#include <stdbool.h>
#include <stdint.h>

/* For NORA_UART_INST_1, named by EV08P02A_CONSOLE_UART_INST below. Included
 * here rather than left to each caller so the constant is usable on its own. */
#include "nora_uart.h"

/* For nora_spi_i2s_tdm_clock_role_t in ev08p02a_tdm_pins_init()'s signature. */
#include "nora_spi_i2s_tdm.h"

/* For nora_clock_status_t on g_ev08p02a_clock_init_status below, and because
 * anything reading this board's clock verdict wants get_fosc_hz()/get_fcy_hz() from the
 * same place -- they are the frequency half of that verdict. */
#include "nora_clock.h"

/* The board's pin facts, as one file with no code in it. Included here so that anything
 * describing this board gets the pinout with it -- main.c's audio port needs the RP
 * numbers for its wiring report, and used to carry its own copy of them. */
#include "ev08p02a_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EV08P02A_FRC_HZ         (8000000UL)     /* internal FRC, PLL input */
#define EV08P02A_TARGET_FOSC_HZ (200000000UL)   /* device max: 200 MHz Fosc / 100 MIPS */

/*
 * Console baud, chosen from the operating point actually reached.
 *
 * 230400 is the goal but is only representable once Fcy is high: at the 4 MHz
 * FRC fallback the divisor rounds to 250000 baud (+8.5%), which garbles every
 * frame. 9600 resolves to within 0.2% at both 4 MHz and 100 MHz, so the fallback
 * console stays readable and can report why the PLL was not used.
 */
#define EV08P02A_UART_BAUD_FAST (230400UL)
#define EV08P02A_UART_BAUD_SAFE (9600UL)

/*
 * WHICH UART is the console, as a value rather than a set of wrapper functions.
 *
 * This board used to export seven ev08p02a_uart_*() functions. Four of them had
 * exactly one caller each -- ev08p02a_console_out.c -- and the other three were
 * called 45 times from this board's own main.c and audio module, i.e. the console's
 * output had two names for one channel. The functions are gone: the console
 * transport now lives entirely in ev08p02a_console_out.c on top of hal_uart, and
 * everything that prints calls uart_platform/console_out.h.
 *
 * What genuinely was a board fact is this constant -- the console is on UART1
 * because the Nano's on-board debugger CDC is wired to RC10/RC11 -- so it is stated
 * once, here, and used by the two files that need it: ev08p02a_console_out.c (the
 * transport) and ev08p02a_board.c (the peripheral bring-up). main.c reads the baud
 * it actually got from the HAL with it.
 */
#define EV08P02A_CONSOLE_UART_INST (NORA_UART_INST_1)

/*
 * Bring the board up, in the order the dependencies require: reset-cause latch (before
 * anything can touch RCON), clock, console baud chosen from the clock actually reached,
 * user I/O pins, UART1, DMA bus priority.
 *
 * The profile switches that used to sit here live in board_profile.h now (they passed
 * through main.c as EV08P02A_ENABLE_* in between) -- see the note at the top of this file.
 */
void ev08p02a_board_init(void);

/*
 * THE CLOCK RESULT IS NOT BEHIND ACCESSORS. It is read exactly as DM330030's is, which
 * is the whole reason the five functions that used to be declared here are gone
 * (2026-08-03) -- two boards answering "what clock did we get" should not need two
 * shapes. Ask, in both cases:
 *
 *     nora_clock_get_fosc_hz() / _get_fcy_hz()   the frequencies. The HAL DERIVES
 *         these from the oscillator registers on every call and is authoritative;
 *         ev08p02a_fosc_hz()/_fcy_hz() only forwarded it, which made a board look like
 *         a second source of truth. (Until 2026-08-10 the HAL RECORDED what it had been
 *         asked for instead, which is a different and much weaker claim: it made
 *         "achieved == requested" compare a request against itself. 0 now means the
 *         frequency is not derivable, never a guess.)
 *     g_ev08p02a_clock_on_target                      false means the FRC->PLL bring-up
 *         did not reach EV08P02A_TARGET_FOSC_HZ and the board is still on the 8 MHz FRC.
 *         Every baud and delay figure derived below is then wrong, so main.c reports it
 *         on LED0 -- the console cannot, being the thing that breaks.
 *     g_ev08p02a_osccon_after_switch                  OSCCON as it read immediately
 *         after the switch attempt, i.e. BEFORE any fallback. Carries COSC (which source
 *         won) and LOCK (did it lock), the evidence that separates "never switched" from
 *         "switched but running unlocked at an unknown frequency".
 *     g_ev08p02a_clock_init_status                    the HAL status for the request.
 *     g_ev08p02a_clock_diag                           nora_clock_dspic33ck_diag_t: WHICH
 *         way it failed, which the status alone cannot say. Print it, never branch on it.
 *         It is the only route by which SWITCH_IGNORED -- the sequence ran and COSC never
 *         moved, because FCKSM disabled clock switching or CLKLOCK is set -- becomes
 *         visible to a human, and that is the failure this board shipped with.
 *
 * All four volatiles are defined in ev08p02a_board.c beside the stage that sets them,
 * and are volatile precisely so a debugger can read them on a board whose console never
 * came up.
 */

/*
 * There is deliberately no ev08p02a_uart_baud(). It returned a static this file's .c
 * had set from EV08P02A_UART_BAUD_FAST/SAFE, i.e. what the board ASKED for; the UART
 * HAL records what it actually applied. Ask the HAL:
 *
 *     nora_uart_get_baudrate(EV08P02A_CONSOLE_UART_INST)
 *
 * The distinction is not academic on this board -- the banner prints this figure
 * precisely so a garbled console can be diagnosed, and a wish is not evidence.
 */

/* The clock verdict, described above. Same shape as DM330030's g_dm330030_* set. */
extern volatile nora_clock_status_t g_ev08p02a_clock_init_status;
extern volatile bool                     g_ev08p02a_clock_on_target;
extern volatile uint16_t                 g_ev08p02a_osccon_after_switch;
extern volatile uint16_t                 g_ev08p02a_clock_diag;

/*
 * LED0 AND SW0 ARE NOT HERE, they are in ev08p02a_io.h. Their PIN CONFIGURATION still
 * happens in this file's ev08p02a_board_init(), and stays its exclusive business -- the
 * io file writes levels and never touches a direction register, which is the same
 * one-owner-per-pin rule DM330030's led_sw/board split was created to enforce.
 */

/*
 * CONSOLE TEXT AND INPUT ARE NOT HERE. They were: write_string/write_u32/write_hex16,
 * rx_ready/read_byte/tx_done. Everything that prints on this board -- main.c's banner,
 * the audio module's status lines, and the shared console/trap/probe modules -- now
 * calls uart_platform/console_out.h, whose EV08P02A implementation is
 * ev08p02a_console_out.c talking to hal_uart directly.
 *
 * The formatting was the give-away that these were in the wrong file: decimal digit
 * generation and fixed-width hex are not board facts. The one board fact is
 * EV08P02A_CONSOLE_UART_INST above.
 */

/*
 * THE RESET CAUSE IS NOT DECLARED HERE. Ask app/app_traps.h's BOARD SEAM:
 *
 *     board_reset_cause_str()   the cause as a short string
 *     board_reset_cause_raw()   the latched word behind it
 *
 * ev08p02a_reset_cause_str()/_raw() used to be declared here and the seam functions
 * forwarded to them -- two public names per question, where DM330030 has one, and its
 * seam functions read RCON directly. Deleted 2026-08-04; main.c's banner (the only
 * caller outside the seam) now asks the same name the shared *rc console command does.
 *
 * WHAT THIS BOARD STILL DOES DIFFERENTLY, and it is one line rather than two functions:
 * ev08p02a_board_init() LATCHES RCON before anything else can touch it, and clears it, so
 * each boot reports its own cause instead of an accumulation. That is what tells a POR
 * ("you power cycled it") from a SWR ("the *sr console command worked") -- on a Curiosity
 * Nano with no reset button, the only evidence a software reset actually happened.
 * DM330030 deliberately does not latch.
 */

/*
 * I2C1 bring-up on the board's ASDA1/ASCL1 pair: analog off, weak pull-ups, HAL init at
 * 400 kHz with the divisor derived from the Clock HAL's recorded Fcy.
 *
 * Was ev08p02a_i2c1_init() in ev08p02a_i2c1_probe.c. It stayed on the board when the
 * probe moved to app/i2c_probe.c, because pins are the one part of that module which
 * really is board knowledge. Two callers: the probe (through i2c_probe_t.bus_init) and
 * the WM8904 path -- same bus, same pins.
 */
bool ev08p02a_i2c1_init(void);

/*
 * SPI1/TDM pin routing for the WM8904 path: RP50 BCLK / RP51 FS / RP48 SDO / RP49 SDI,
 * by direct jumper from the Nano's edge header.
 *
 * This is the whole board content of app/wm8904_audio.c on this board, reached through
 * wm8904_audio_port_t.configure_pins. It takes the ROLE rather than being compiled for
 * one: the wiring drives either direction, so which side generates BCLK/FS is the
 * profile's decision (main.c) and not this file's.
 */
bool ev08p02a_tdm_pins_init(nora_spi_i2s_tdm_clock_role_t role);

/*
 * THERE IS NO MCLK STAGE ON EITHER BOARD, and until 2026-08-04 this note claimed the
 * opposite -- that DM330030's dm330030_mclk_init() (REFO1 at 12.5 MHz to the codec's MCLK
 * pin) was needed there and was "the ONE genuine asymmetry in the audio path", because
 * only THIS board's codec self-clocks from its own X1.
 *
 * WRONG, AND WORTH KEEPING THE CORRECTION VISIBLE: the same WM8904 board is used on both,
 * and its XTAL is on both. The dsPIC has no master clock to supply on either board -- there
 * is no MCLK pin and no code path that could drive one. (Narrowed 2026-08-09: this also said
 * "the codec self-clocks in every configuration", which holds only at jumper A-XTAL. The A
 * jumper selects the codec's MCLK INPUT -- A-XTAL: the CODEC-A board's XTAL, codec is TDM
 * master; A-extMCLK: a BCLK arriving from outside, codec is TDM slave. Board fact, from the
 * person who wired the board, 2026-08-09. So at A-extMCLK the codec runs on a clock WE
 * generate -- just not from an MCLK stage, which is why the claim made here is still about
 * OUR side only -- see wm8904_audio.h and plan doc 20.4 + 21.)
 * That stage, its pin, and the
 * wm8904_audio_port_t.mclk_init hook it filled are all deleted; the audio seam is now
 * symmetric (see the table in dm330030_board.h).
 *
 * THE ROLE SWAP DOES NOT CHANGE IT. WM8904 and the dsPIC can trade TDM master/slave on
 * either board -- that is what the role argument to ev08p02a_tdm_pins_init() is for, and
 * CK has run the master direction on hardware. Which side drives BCLK/FS says nothing
 * about where the codec's SYSCLK comes from; MCLK source is a board/compile fact,
 * not a transport leg's master/slave role.
 *
 * The reasoning that stood here is still right about one thing and it is worth keeping: an
 * empty stage returning true would claim a capability, and the next person to debug a
 * clockless codec would find a step that "succeeded". The answer is no stage on either
 * side, not a stub on this one.
 */

#ifdef __cplusplus
}
#endif

#endif /* EV08P02A_BOARD_H */
