/*
 * dsPIC33CK256MC005 / EV08P02A first runnable profile.
 *
 * The EV08P02A board is the dsPIC33CK256MC005 Curiosity Nano. This profile uses
 * only board facts confirmed from the user guide: LED0 on RD10 (active-low) and
 * the on-board debugger CDC UART on U1 TX/RX through RC10/RP58 and RC11/RP59 --
 * confirmed pin-identical to EV88G73A's dsPIC33CK64MC105 Nano.
 *
 * Ported 2026-08-26 from EV88G73A's main.c for functional parity on the same MCU
 * family (256K flash / 16K RAM here vs 64K/8K there). Every dated comment below
 * ("used to", "is gone", "deleted 2026-...") describes EV88G73A's own development
 * history, kept because the console commands and design choices it explains are
 * unchanged on this board -- not a claim that this file has that history itself.
 */

#include <stdint.h>
#include <xc.h>

#include <stdbool.h>

#include "ev08p02a_board.h"   /* bring-up + the operating point it reached */
#include "ev08p02a_io.h"      /* LED0, SW0 */
#include "profile_main.h"

/* The running time. Every "every N ms" in this file is expressed against GetTicks(),
 * and this profile is what starts the tick -- from Fcy, deliberately, so that the LED
 * cycle stays a live check on the operating point. See app/timer_app.h. */
#include "timer_app.h"

/* The banner and every other print in this file go through the console seam. This
 * board no longer has a UART API of its own to call -- see ev08p02a_board.h. */
#include "console_out.h"
#include "nora_uart.h"   /* nora_uart_get_baudrate() for the banner */

/* Generated per build into build/<variant>/production/generated/ by
 * buildtools/build.ps1, which also defines EV08P02A_HAVE_BUILD_ID_H. Absent for
 * builds made any other way -- the banner then says so rather than lying. */
#if defined(EV08P02A_HAVE_BUILD_ID_H)
#include "ev08p02a_build_id.h"
#endif

#ifndef DSPIC33CK_BOARD_EV08P02A
#error "boards/ev08p02a/main.c is EV08P02A-owned. Build it only in the CK256MC005_EV08P02A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

/* THE PROFILE'S ANSWERS, FIRST: app_config.h pulls in this board's board_profile.h and
 * refuses anything unresolved, so every DEMO_ENABLE_ and BOARD_CONSOLE_ switch below is
 * decided before the first conditional include that reads one. See the block further down for what
 * this replaced (two vocabularies for one decision). */
#include "app_config.h"

/* DEMO_ENABLE_CONSOLE_COMMANDS is 1 in this board's board_profile.h and always has been in
 * effect: the console is what provides the software-reset command, and a board with no reset
 * button needs that in every build. It is a switch rather than an unconditional include
 * because the OTHER profile does not have it yet, and a capability that one board simply
 * lacks should be visible as a 0 somewhere rather than as an absence of code (see
 * boards/dm330030/board_profile.h and the parity contract).
 * Shared -- this was ev08p02a_console.h, whose contents needed no board knowledge at all. */
#if DEMO_ENABLE_CONSOLE_COMMANDS
#include "console_task.h"
#endif

/* Unconditional for the same reason as the console: a trap that goes unreported
 * is indistinguishable from a dead board, which is the failure mode this profile
 * has already been bitten by. See app/app_traps.h.
 *
 * Also the home of the BOARD SEAM, hence board_reset_cause_str() for the banner's Reset
 * line -- the same name the shared *rc console command uses, this board having no
 * reset-cause function of its own any more (see ev08p02a_board.h). */
#include "app_traps.h"

/* -------------------------------------------------------------------------- */
/* WHICH EXERCISERS THIS PROFILE RUNS -- ANSWERED IN board_profile.h.           */
/*                                                                            */
/* The values used to be defined here as EV08P02A_ENABLE_*, while DM330030      */
/* answered the same questions as ENA_* in app/app_config.h: two vocabularies    */
/* for one decision, and a TDM loopback build had to pass BOTH spellings         */
/* because the shared module reads app_config.h and cannot see a #define made    */
/* in this file. Since 2026-08-05 there is one name per decision, resolved in     */
/* boards/ev08p02a/board_profile.h, and app/app_config.h refuses anything        */
/* unresolved or contradictory rather than silently building a 0.                */
/*                                                                            */
/* So this file now READS the switches and never sets them. What stays here is   */
/* the reasoning that belongs next to the code each one selects; the defaults     */
/* and the board's own answers are one file away.                               */
/* -------------------------------------------------------------------------- */

/*
 * DEMO_ENABLE_WM8904_AUDIO -- the audio path: a single WM8904 codec, in the default build
 * the audio-clock master at 48 000 Hz (jumper A-XTAL) with dsPIC SPI1 as a TDM8/32-bit
 * SLAVE; EV08P02A_WM8904_DSPIC_IS_MASTER=1 swaps that (see the note beside the switch
 * below). This firmware supplies no MCLK in either direction. Passthrough audio (line-in -> WM8904 ADC -> dsPIC -> WM8904 DAC ->
 * headphone). Claims SPI1/RP48-51 and I2C1/RP56-57. The path itself is shared --
 * app/wm8904_audio.h -- and this board's half of it is the port struct below plus
 * ev08p02a_tdm_pins_init()/ev08p02a_i2c1_init() from ev08p02a_board.h.
 *
 * Default 1: the codec is what this board is for, and it is the only audio path left.
 * Set to 0 for the LED/UART/console baseline with no audio at all.
 *
 * TWO EXERCISERS USED TO BE SELECTED HERE AND BOTH ARE GONE. Recording them because the
 * flags they were selected with are the reason this block looks the way it does:
 *
 *   EV08P02A_ENABLE_SPI_DMA_MIN (deleted 2026-08-01) -- stage A of the SPI/DMA core
 *     rebuild: minimal 16-bit unframed SPI + DMA poking registers directly, to isolate
 *     faults before the transport core existed.
 *   EV08P02A_ENABLE_TDM_LOOPBACK (deleted 2026-08-02) -- TDM8/32-bit framed master
 *     loopback through the shared transport core, with CLC1 50%-duty FS. Streamed
 *     cleanly on hardware and was Phase A's proven-good exerciser.
 *
 * Both existed to debug a transport core that is now hardware-verified, so neither had
 * a remaining job. What is worth keeping from them is written down, not kept as code:
 * docs/ck_silicon_findings.md.
 *
 * Default (1) is in board_profile.h; override with -DDEMO_ENABLE_WM8904_AUDIO=0.
 */

/* DEMO_ENABLE_DMA_SELFTEST is DERIVED in app/app_config.h -- true when an SPI1/DMA0-1 path
 * is built, i.e. when the DMA controller and its self-test are needed. It was defined here
 * as EV08P02A_ENABLE_SPI_EXERCISER, by the same expression; it moved because the reason it is
 * derived rather than chosen (a self-test is load-bearing exactly when a DMA path exists) is
 * true of both boards, and because a profile should not be able to switch it off by hand.
 * The TDM master loopback below is the second path that note predicted would reuse the seam:
 * it streams through the same transport core on the same DMA channels. */

/*
 * DEMO_ENABLE_I2C_PROBE -- a standalone probe that reports what the I2C1 module
 * itself does with no codec attached (a clean NACK means the mechanism works). Written
 * as HW-mechanism prep before the codec was wired. See app/i2c_probe.h.
 *
 * Default 0, because it drives I2C1 and so does the WM8904 path, which is on by default
 * -- see the #error below. Enable it (and disable WM8904) to check the bus in isolation.
 *
 * The probe module itself (app/i2c_probe.c) is compiled either way, being shared; this
 * flag only decides whether this profile RUNS it. The bus bring-up it needs is
 * ev08p02a_i2c1_init(), which the WM8904 path also calls -- same bus, same pins.
 *
 * Default (0) is in board_profile.h, and the WM8904-vs-probe exclusivity that used to be
 * asserted here is one of app_config.h's checks now -- both boards need it.
 */

/*
 * DEMO_ENABLE_TDM_MASTER_LOOPBACK -- the codec-less TDM8/32-bit MASTER exerciser: dsPIC
 * SPI1 self-generates BCLK, a 50%-duty FS via CLC1, and a ramp on SDO. Claims
 * SPI1/RP48-51 and no I2C at all.
 *
 * This restores the capability the note above records as deleted on 2026-08-02
 * (DEMO_ENABLE_TDM_MASTER_LOOPBACK, "Phase A's proven-good exerciser"). It is NOT the
 * deleted code coming back: that was this board's private copy, and what runs now is the
 * shared app/demo_tdm_master_loopback.c that DM330030 already uses, with this board
 * supplying only its four pins through demo_tdm_master_loopback_port_t. The duplicate is
 * what was deleted; the exerciser is what was wanted back.
 *
 * Default 0 -- the codec path is what this board is normally for. Enable it (and the
 * WM8904 path goes off, see the #error below) when the point is to see TDM8 signals on a
 * scope with NO codec attached: nothing is required on the other end, because the master
 * is the clock source. That is also what makes it the exerciser that actually covers the
 * CLC1/fs_clc PPS path -- the codec build runs SPI1 as a SLAVE with FS_PULSE, so it never
 * engages fs_clc at all.
 *
 * Default (0) is in board_profile.h. The SPI1 exclusivity against the WM8904 path -- two
 * configured owners of one peripheral, in opposite roles, which is a build that cannot be
 * right rather than a runtime condition worth reporting -- is asserted in app_config.h.
 */

#if DEMO_ENABLE_DMA_SELFTEST
#include "dma_selftest.h"

/* Channel 3: the SPI/TDM transport owns 0 (RX) and 1 (TX) -- see
 * nora_spi_i2s_tdm_conf.h. The self-test takes the channel as an argument
 * precisely because that allocation is this profile's to know, not the test's. */
#define EV08P02A_SELFTEST_DMA_CH (NORA_DMA_CHANNEL_3)
#endif
#if DEMO_ENABLE_I2C_PROBE
#include "i2c_probe.h"

/*
 * What the shared probe needs to know about THIS board's bus. Everything here is a
 * profile or lab choice rather than probe logic, which is why the probe itself is
 * app/i2c_probe.c now:
 *
 *   - the WM8904's 7-bit address (0x34 >> 1) and its device-ID register (R0), used only
 *     as a concrete known target. This does not claim a codec is attached, and it does
 *     not depend on wm8904.c.
 *   - the bus bring-up, which is the board's (pins/pull-ups: see ev08p02a_board.c).
 *   - the pin description, because a report that does not say which wires it drove is a
 *     report nobody can act on.
 */
static const i2c_probe_t s_i2c1_probe = {
    .inst        = NORA_I2C_INST_1,
    .addr7       = 0x1Au,
    .reg_pointer = 0x00u,
    .bus_init    = ev08p02a_i2c1_init,
    .where       = "ASDA1=RP56(RC8) ASCL1=RP57(RC9), 400 kHz, no codec attached",
};
#endif
#if DEMO_ENABLE_TDM_MASTER_LOOPBACK
/*
 * ONE FLAG NOW, AND THIS IS WHERE THE TWO-FLAG TRAP USED TO BE. The shared demo's body sits
 * inside its own #if, so it has to be compiled with the same switch this profile reads --
 * and until 2026-08-05 that switch had two names: ENA_TDM_MASTER_LOOPBACK for the module
 * (app_config.h, a separate translation unit that cannot see a #define made here) and
 * EV08P02A_ENABLE_TDM_LOOPBACK for this file. A -D that set only one produced an empty
 * object and a link error, so this block carried an #error asserting the pair. There is one
 * name now, resolved in board_profile.h, so the mistake is impossible rather than diagnosed:
 *
 *     buildtools\build.ps1 -Configuration CK256MC005_EV08P02A -Full `
 *         -Define @('DEMO_ENABLE_TDM_MASTER_LOOPBACK=1','DEMO_ENABLE_WM8904_AUDIO=0')
 */

#include "demo_tdm_master_loopback.h"

/*
 * This board's half of the codec-less TDM master exerciser: the pins, and nothing else.
 * The same routing function the WM8904 path uses, in the other role -- ev08p02a_tdm_pins_init
 * takes the role precisely so one function serves both, which is what let this board's
 * private copy of the demo go (see demo_tdm_master_loopback.h).
 */
static const demo_tdm_master_loopback_port_t s_tdm_loopback_port = {
    .configure_pins = ev08p02a_tdm_pins_init,
    .wiring         = EV08P02A_TDM_LOOPBACK_WIRING_STR,
};
#endif

#if DEMO_ENABLE_WM8904_AUDIO
#include "wm8904_audio.h"
#include "app_console.h"   /* app_console_msg_t + the status codes, for *tp and *tq below */

/*
 * Phase B: a single WM8904 as passthrough audio over TDM8/32-bit. The path is shared
 * (app/wm8904_audio.c); what is below is this profile's half of it -- the wiring, and
 * the design choices the shared module cannot make for a board.
 *
 * WHICH SIDE DRIVES BCLK/FS/LRCLK IS A BUILD SWITCH, mirroring what the original sonora
 * project does: the WM8904 can be either half of the pair, and which one is chosen is a
 * build-time decision rather than something probed at runtime.
 *
 *   0 (default) -- WM8904 = master (drives BCLK/FS/LRCLK off its own X1 crystal),
 *     dsPIC SPI1 = SLAVE. Requires jumper A-XTAL. HARDWARE-TESTED including a listening
 *     pass on four paths (plan doc 18); this is the only position whose 48 000 Hz matches
 *     the AVAS coefficient tables.
 *   1 -- dsPIC SPI1 = MASTER (self-generated BCLK plus a 50%-duty FS via CLC1, the
 *     operating point the master path was scope-verified at), WM8904 = SLAVE. Requires
 *     jumper A-extMCLK. HARDWARE-TESTED 2026-08-09 (plan doc 19: passthrough ran, the
 *     codec returned ADC data locked to our FS, and the operator confirmed audio) --
 *     this line said "Desk design; not hardware-tested" until then. Transport fs is
 *     48 828.125 Hz here, so AVAS is refused by design, which is why the two jumper
 *     positions are complementary rather than alternatives.
 *
 * MASTER/SLAVE HERE IS ONLY ABOUT WHO DRIVES BCLK/FS -- it says nothing about where the
 * codec's SYSCLK comes from, and no build of either board supplies an MCLK (this firmware
 * has no MCLK output at all; see the note in ev08p02a_pins.h beside the wiring string).
 * What the A jumper physically switches is a board fact this firmware does not set and
 * has not measured: plan doc 20.4.
 */
#ifndef EV08P02A_WM8904_DSPIC_IS_MASTER
#define EV08P02A_WM8904_DSPIC_IS_MASTER 0
#endif

static const wm8904_audio_port_t s_wm8904_port = {
    .configure_pins      = ev08p02a_tdm_pins_init,
    .i2c_init            = ev08p02a_i2c1_init,      /* same bus and pins as the I2C1 probe */
    /* The .mclk_init slot is gone from wm8904_audio_port_t (2026-08-04), so there is no
     * NULL to write here any more. The note that stood in its place -- "the WM8904 board
     * here self-clocks from its own X1 ... DM330030 has to divide one out of REFO1
     * instead" -- was right at jumper A-XTAL on this board (measured: the codec masters
     * BCLK/FS at 48 000 Hz) and wrong about the other one: it is the same codec board, with
     * the same crystal. The A jumper picks which clock reaches the codec's MCLK input --
     * A-XTAL that crystal, A-extMCLK an incoming BCLK, i.e. ours (board fact, from the person
     * who wired it, 2026-08-09). So at A-extMCLK the codec is SYSCLK'd by our BRG, and what
     * this tree still lacks is only an MCLK STAGE, in either position. See ev08p02a_board.h
     * and plan doc 20.4 + 21. */
    .mute_button_pressed = ev08p02a_sw0_pressed,    /* SW0, active-low, read raw */
    /* From ev08p02a_pins.h, beside the RP numbers it describes. This line used to be a
     * hand-typed second copy of them. */
    .wiring              = EV08P02A_AUDIO_WIRING_STR,
};

static const wm8904_audio_config_t s_wm8904_audio = {
    .port            = &s_wm8904_port,
    .i2c_inst_legacy = 1u,     /* wm8904.c maps this to NORA_I2C_INST_1 */
    .dspic_is_master = (EV08P02A_WM8904_DSPIC_IS_MASTER != 0),
    /* Self-clocked BCLK = Fp/(2*(brg+1)); brg=3 at Fcy=100 MHz -> 12.5 MHz BCLK ->
     * TDM8/32-bit fs ~= 48.8 kHz, the scope-verified master operating point. Ignored in
     * the default slave build. */
    .brg             = 3u,
    /* Matches wm8904.c's own default (WM8904_SAMPLE_RATE_REG_48K at init). Revisit if
     * wm8904_set_rate_hz() is ever called for this profile. */
    .sample_rate_hz  = 48000u,
    /* Deliberately long, for demonstrating the SW0 mute ramp -- the fade is meant to be
     * obvious rather than transparent. Snapped to the nearest curve in gain_ctrl.c's table,
     * which is 791 ms at this rate; the boot banner prints both numbers so the snap is
     * visible. 300 ms (curve (6,7)) is what this board asked for before, if a ramp that
     * merely avoids the click is wanted again. */
    .gain_ramp_ms    = 800u,
    /*
     * 0 = no throttle of its own: profile_wait_next_tick() below already gates `report`
     * to EV08P02A_STATUS_PRINT_PERIOD_MS, and two periods stacked on one line means the
     * observed cadence is their product, which is the kind of arithmetic that produced
     * the 4x error recorded just below.
     */
    .status_period_ms      = 0u,
    /*
     * One "not started" reminder per 40 s -- the cadence the predecessor actually had,
     * kept as a measured number. It expressed this as "20 reports", and its comment
     * claimed that was ~10 s, having assumed the countdown ticked on every ~500 ms poll
     * when it only ticked on the ~2 s reports: wrong by 4x, and wrong in the way a count
     * of calls invites. Measured on hardware at 40 s before that merge and after it; the
     * number is what survives, now stated in the unit it was measured in.
     */
    .idle_report_period_ms = 40000u,
};

/*
 * Console module 't' (audio transport) -- runtime path and load controls, reached through
 * console_board_onmsg() in ev08p02a_console_out.c.
 *
 * WHY IT IS IN THIS FILE. The command needs s_wm8904_audio, which is the profile's
 * config and belongs beside the port struct above rather than in a console file that
 * would then have to know this board's TDM geometry. So the handler is here, next to
 * what it controls, and the board's console file only routes the letter -- the same
 * division uart_app/console_dispatch.c uses for the shared modules.
 *
 * WHY 't'. console_dispatch.c reserves it for audio transport, to keep sonora's *tr
 * meaning the same thing across the fleet. This is the first command to claim it.
 *
 *   *tp  advance the block path: gain -> copy -> mute -> gain (gain is the boot default)
 *   *tp0000..*tp0003  select one: 0 copy, 1 mute, 2 tone, 3 gain. Tone is NOT in the
 *        cycle -- it drives the output with a synthetic 1 kHz, so it is asked for by
 *        index rather than arrived at by one keystroke too many (see wm8904_audio.h)
 *   ?tp  the current path, plus what CODEC-IN carries (peak level + active slot mask)
 *   *tq0000  stop the periodic TDM1 load line;  *tq0001  resume it (on after a bring-up)
 *   ?tq  whether that line is on, plus ONE full line including last/deadline
 *   *ts  analog-mute WM8904, then halt TDM/DMA (use before flashing); ?ts reports state
 *   *tb000f which AVAS parts run: 1=carriers 2=envelope 4=gate 8=noise; ?tb reports
 *   *ti<NNNN> PRE gain, tenths of a dB, SIGNED (two's complement): *ti0000 = 0.0 dB,
 *        *ti003C = +6.0 dB, *tiFFF6 = -1.0 dB. ?ti reports it and the clamp count
 *   *to<NNNN> POST gain, same units and same grammar. ?to likewise
 *   *tc<NNNN> AVAS pitch trim, cent (not tenths), SIGNED, same two's-complement
 *        grammar as *ti and *to: *tc0000 = 0 cent, *tc00C8 = +200 cent, *tcFF38 =
 *        -200 cent. Out-of-range clamps rather than refuses (matches *ti and *to's
 *        own gain-table clamp), and the result also snaps to the nearest 5 cent
 *        -- the ratio table is only that fine (see avas_pitch_ck_table.h; it
 *        used to be 1 cent and that alone did not fit the 64K EV88G73A image
 *        this profile was ported from). ?tc reports the committed and, if
 *        different, the still-pending value, both already snapped. The up/down
 *        arrow keys move in the same 5 cent steps -- see
 *        ev08p02a_transport_console_hotkey() below
 *
 * TWO OF THESE ARE OPTIONAL: *tb/?tb and *tl are gated by
 * WM8904_AUDIO_ENABLE_TDM_DIAG (wm8904_audio.h), which a two-voice image turns off to buy
 * the program memory the second voice needs. In such an image *tb and *tl are ABSENT --
 * ERR_NOT_FOUND like any unknown letter, the same treatment *tm gets and for the same
 * reason (a "not built" reply is a string). Everything else in the list above is present in
 * every image, deliberately: see the macro's comment for which and why.
 */
void ev08p02a_transport_console_onmsg(app_console_msg_t *msg);

void ev08p02a_transport_console_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

    /* Sonora's one-shot declick-restart experiment.  CK uses the same WM8904 strategy
     * mask around one complete stop -> configure -> start sequence; a bare *td is the
     * baseline mask 00, hence exactly the protected counterpart of *tr. */
    if (msg->name == 'd') {
        if (msg->kind == '?') {
            console_out_str(" ?td: declick restart strategy bitmask (one-shot, *td<NN>):\n"
                            "   DEFAULT(0x00) shutdown = vendor WSEQ; startup = manual\n"
                            "   bit0 0x01 ordered HP disable; bit1 0x02 warm servo\n"
                            "   bit2 0x04 soft unmute; bit3 0x08 WSEQ shutdown\n"
                            "   bit4 0x10 soft shutdown; bit5 0x20 WSEQ startup\n"
                            "   bit6 0x40 legacy quench; e.g. *td00 *td20 *td40\n");
            wm8904_audio_declick_print_status();
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_OK;
            return;
        }
        if (msg->kind != '*') {
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            return;
        }
        {
            /* As on Sonora, use the first supplied byte and treat no payload as 00. */
            const uint8_t mask = (msg->data_len >= 1u) ? msg->data[0] : 0x00u;
            console_out_str(" *td: declick one-shot restart mask=0x");
            console_out_hex16((uint16_t)mask);
            console_out_str("\n");
            msg->data_len = 0u;
            msg->status = wm8904_audio_restart_declick(mask)
                            ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        }
        return;
    }

    if (msg->name == 'p') {
        if (msg->kind == '?') {
            wm8904_audio_path_report();
            msg->status = APP_CONSOLE_OK;
            return;
        }

        /*
         * Bare *tp cycles; *tp<NNNN> selects a path by index. The payload is optional here,
         * unlike *tq below, and that asymmetry is deliberate: a bare *tq would have to mean
         * "whichever state you are not in", while a bare *tp has an unambiguous meaning of
         * its own (advance, and say where it landed) that has been this board's habit since
         * the command existed. The indexed form exists for the one path kept out of the
         * cycle -- *tp0002, tone. Widths are a typing convention, so accept one or two
         * bytes exactly as *tq does.
         */
        if (msg->data_len == 0u) {
            wm8904_audio_path_next();
            msg->status = APP_CONSOLE_OK;
            return;
        }

        if (msg->data_len > 2u) {
            console_out_str(" *tp: value is 0000..0003, or omit it to cycle\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }

        {
            const uint16_t value =
                (msg->data_len == 2u)
                    ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                    : (uint16_t)msg->data[0];

            msg->status = wm8904_audio_path_set(value) ? APP_CONSOLE_OK
                                                       : APP_CONSOLE_ERR_BAD_DATA;
        }
        return;
    }

    /*
     * 'b' -- which parts of the AVAS engine run. *tb0007 = everything, which is the
     * boot default and now fits (67.8 % of the block period; the 115 % that once
     * made *tb0005 the only usable mode was generated code, not the algorithm).
     * *tb0005 = carriers + gate
     * with the envelope frozen, kept because switching between the two is how the
     * inter-line beating is heard rather than asserted. Explicit value for the same
     * reason as *tq.
     */
#if WM8904_AUDIO_ENABLE_TDM_DIAG
    if (msg->name == 'b') {
        if (msg->kind == '?') {
            wm8904_audio_avas_parts_report();
            msg->status = APP_CONSOLE_OK;
            return;
        }
        {
            uint16_t value;

            if ((msg->data_len == 0u) || (msg->data_len > 2u)) {
                wm8904_audio_avas_parts_usage();
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }
            value = (msg->data_len == 2u)
                        ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                        : (uint16_t)msg->data[0];
            /* Bound and usage both asked of the app module rather than spelled out
             * here: this file talks to the app module, not to the engine, and
             * including the engine header just for a bound would put a DSP dependency
             * in a board profile. Spelling it out instead is what left `> 7u` behind
             * when the NOISE bit arrived -- it rejected the legal mask 000f. */
            if (!wm8904_audio_avas_parts_valid(value)) {
                wm8904_audio_avas_parts_usage();
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                return;
            }
            wm8904_audio_avas_parts_set((uint8_t)value);
        }
        msg->status = APP_CONSOLE_OK;
        return;
    }
#endif /* WM8904_AUDIO_ENABLE_TDM_DIAG -- *tb / ?tb, absent like *tm rather than refused */

    /*
     * 'i' / 'o' -- PRE and POST gain, in tenths of a dB. One handler for the two, because
     * the only difference is which app function is called and duplicating the payload
     * decode and the help text would be two places for the units to drift.
     *
     * THE PAYLOAD IS SIGNED, two's complement in the same 16 bits every other command in
     * this module uses: *ti003C = +6.0 dB, *tiFFF6 = -1.0 dB. An offset encoding (value =
     * tenths + 240) would spare the operator the two's complement at the cost of a bias
     * nothing else in the fleet grammar has -- and hex is already the convention here, so
     * the cast is the smaller surprise. The app module prints the realised dB back either
     * way, which is what actually confirms the sign was read as intended.
     *
     * No bare form: the same explicit-value rule as *tq and *tb. A bare *ti has no
     * unambiguous meaning (a gain has no "other" state to toggle to), and this is a command
     * whose accidental form would be audible.
     */
    if ((msg->name == 'i') || (msg->name == 'o')) {
        const bool post = (msg->name == 'o');

        if (msg->kind == '?') {
            if (msg->data_len != 0u) {
                console_out_str(post ? " ?to: takes no value\n" : " ?ti: takes no value\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }
            if (post) {
                wm8904_audio_post_gain_report();
            } else {
                wm8904_audio_pre_gain_report();
            }
            msg->status = APP_CONSOLE_OK;
            return;
        }

        {
            uint16_t value;

            if ((msg->data_len == 0u) || (msg->data_len > 2u)) {
                console_out_str(post ? " *to" : " *ti");
                console_out_str(": needs tenths of a dB, signed hex -- 0000 = 0.0 dB,"
                                " 003C = +6.0 dB, FFF6 = -1.0 dB (0.5 dB grid)\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }

            /* A one-byte payload is sign-extended, so *tiF6 and *tiFFF6 both mean -1.0 dB
             * -- the width is a typing convention here exactly as it is for *tq, and a
             * short form that silently meant +246 tenths would be the one encoding mistake
             * loud enough to hurt. */
            value = (msg->data_len == 2u)
                        ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                        : (uint16_t)(int16_t)(int8_t)msg->data[0];

            msg->status = (post ? wm8904_audio_post_gain_set((int16_t)value)
                                : wm8904_audio_pre_gain_set((int16_t)value))
                              ? APP_CONSOLE_OK
                              : APP_CONSOLE_ERR_BAD_DATA;
        }
        return;
    }

    /*
     * 'c' -- AVAS pitch trim, cent rather than tenths of a dB, otherwise the same
     * signed two's-complement grammar as *ti and *to above (see the module comment).
     * Always OK: wm8904_audio_avas_pitch_set() clamps rather than refuses, and its
     * own printed reply is what says so when it did.
     */
    if (msg->name == 'c') {
        if (msg->kind == '?') {
            if (msg->data_len != 0u) {
                console_out_str(" ?tc: takes no value\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }
            wm8904_audio_avas_pitch_report();
            msg->status = APP_CONSOLE_OK;
            return;
        }

        {
            uint16_t value;

            if ((msg->data_len == 0u) || (msg->data_len > 2u)) {
                console_out_str(" *tc: needs cent, signed hex -- 0000 = 0, 00C8 = +200,"
                                " FF38 = -200 (the trim's full range)\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }

            value = (msg->data_len == 2u)
                        ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                        : (uint16_t)(int16_t)(int8_t)msg->data[0];

            (void)wm8904_audio_avas_pitch_set((int16_t)value);
        }
        msg->status = APP_CONSOLE_OK;
        return;
    }

    if (msg->name == 'q') {
        if (msg->kind == '?') {
            wm8904_audio_load_line_report();
            msg->status = APP_CONSOLE_OK;
            return;
        }

        /*
         * *tq / *tq0000 / *tq0001 -- the shared fleet on/off subset.  Sonora defines
         * a bare *tq as OFF, so CK does too: unlike a toggle, it has an unambiguous and
         * idempotent meaning.  The hex digits after the name are already decoded into
         * msg->data[] by app_console.c.
         *
         * Sonora additionally accepts *tq0002YYYY to select a telemetry period.  CK's
         * TDM1 load line has a board-profile-fixed period, so it must not accept that
         * spelling as though the requested period had taken effect.  Recognise its mode
         * word and reply UNSUPPORTED with an explanation instead of a generic length/data
         * error; automation can then distinguish "not implemented here" from bad input.
         */
        {
            uint16_t value;

            if (msg->kind != '*') {
                msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
                return;
            }

            if (msg->data_len == 0u) {
                wm8904_audio_load_line_set(false);
                msg->status = APP_CONSOLE_OK;
                return;
            }

            value = (msg->data_len >= 2u)
                        ? (uint16_t)(((uint16_t)msg->data[0] << 8) | (uint16_t)msg->data[1])
                        : (uint16_t)msg->data[0];

            if (value == 2u) {
                console_out_str(" *tq0002YYYY: telemetry-period selection is unsupported; "
                                "CK TDM1 uses its board-profile-fixed period\n");
                msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
                return;
            }

            if (msg->data_len > 2u) {
                console_out_str(" *tq: takes no period -- use *tq or *tq0000 = off, "
                                "*tq0001 = on\n");
                msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
                return;
            }

            if (value > 1u) {
                console_out_str(" *tq: value must be 0000 or 0001 (or omit it for off)\n");
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                return;
            }

            wm8904_audio_load_line_set(value != 0u);
        }
        msg->status = APP_CONSOLE_OK;
        return;
    }

    if (msg->name == 's') {
        /*
         * Keep the stop command argument-free.  It is used immediately before
         * flashing, where a typo must not be interpreted as a mode or a hidden
         * restart request.  `?ts` shares the same grammar and reports only.
         */
        if (msg->data_len != 0u) {
            console_out_str(" *ts/?ts: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }

        if (msg->kind == '?') {
            wm8904_audio_stop_report();
        } else {
            wm8904_audio_stop();
        }
        msg->status = APP_CONSOLE_OK;
        return;
    }

    /*
     * 'r' -- restart the stream with no reset, the same letter and meaning as sonora's *tr.
     * console_dispatch.c reserved 't' for exactly this and CK had nothing behind it, so *ts
     * was terminal until a reprogram; the transport's close() -> open() -> inst_start()
     * lifecycle was simply unreachable from this console.
     *
     * Argument-free for the same reason *ts is -- and unlike *ts it is not a pre-flash
     * command, so the risk it guards against is the opposite one: a stray payload must not
     * turn "restart" into a mode. OPERATION_FAILED rather than NOT_FOUND when the path does
     * not come back, because reading those two apart is what separates "this firmware lacks
     * the command" from "the transport failed to restart" -- the distinction that briefly
     * made an unimplemented *tr look like a phase-1 regression.
     */
    if (msg->name == 'r') {
        if (msg->data_len != 0u) {
            console_out_str(" *tr: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }
        if (msg->kind == '?') {
            console_out_str(" ?tr: no query form -- ?ts reports the state\n");
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            return;
        }

        msg->status = wm8904_audio_restart() ? APP_CONSOLE_OK
                                            : APP_CONSOLE_ERR_OPERATION_FAILED;
        return;
    }

    /*
     * 'l' -- the transport's lifecycle GATES, attempted where they must refuse. Not a load
     * command ('q' owns the load line): l is for lifecycle.
     *
     * RUN IT IN BOTH STATES -- *tl, then *ts, then *tl again, then *tr -- because the four
     * expectations are state-dependent and each state contains one call that must SUCCEED.
     * A single run in one state cannot tell a working gate from a bool that never varies,
     * which is the whole reason this command exists (wm8904_audio.h, and §12 of the
     * alignment plan). No query form: it performs HAL calls, so it is a '*' command even
     * though all it produces is a report.
     */
#if WM8904_AUDIO_ENABLE_TDM_DIAG
    if (msg->name == 'l') {
        if (msg->data_len != 0u) {
            console_out_str(" *tl: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }
        if (msg->kind == '?') {
            console_out_str(" ?tl: no query form -- *tl performs the calls it reports\n");
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            return;
        }

        msg->status = wm8904_audio_lifecycle_probe() ? APP_CONSOLE_OK
                                                    : APP_CONSOLE_ERR_OPERATION_FAILED;
        return;
    }
#endif /* WM8904_AUDIO_ENABLE_TDM_DIAG -- *tl, absent like *tm rather than refused */

#if WM8904_AUDIO_ENABLE_SYSTEM_PROBE
    /*
     * 'm' -- the SYSTEM / sync-domain API (phase 4). m is for mode: the ownership mode this
     * command is the only way to observe, since it is a file-static inside the HAL.
     *
     * ALSO RUN IT IN BOTH STATES (*tm, *ts, *tm, *tr): 5 rejections while running, then the
     * 14-step round trip while stopped, which is where arm-all-then-go actually executes.
     * Without this command the linker discards the whole domain API and phase 4 ships as code
     * that has never run (F3 in the alignment plan). It restores SINGLE before returning --
     * mandatory, because under SYSTEM *ts itself is gated off.
     *
     * LAB IMAGES ONLY, and the letter is not merely hidden -- it is ABSENT, falling through to
     * ERR_NOT_FOUND like any unknown letter. A production "not implemented" reply would cost a
     * string, which is the thing this gate exists to buy back. See the macro in wm8904_audio.h;
     * the same clause discards the domain API, so in a production image there is nothing under
     * this command to reach.
     */
    if (msg->name == 'm') {
        if (msg->data_len != 0u) {
            console_out_str(" *tm: takes no value\n");
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }
        if (msg->kind == '?') {
            console_out_str(" ?tm: no query form -- *tm performs the calls it reports\n");
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            return;
        }

        msg->status = wm8904_audio_system_probe() ? APP_CONSOLE_OK
                                                 : APP_CONSOLE_ERR_OPERATION_FAILED;
        return;
    }
#endif /* WM8904_AUDIO_ENABLE_SYSTEM_PROBE */

    msg->status = APP_CONSOLE_ERR_NOT_FOUND;
}

/*
 * Sonora Classic owns `*cy SS` as its synth-group command.  Its 00 subcode is
 * specifically the Type_TY AVAS hotkey -- not a generic "whichever AVAS voice is
 * selected" toggle.  CK has the same Type_TY action behind `a`, so accept the
 * structured spelling as well and send it through the same voice-key entry point.
 * That preserves the exclusive-voice and fade behaviour instead of manufacturing a
 * second enable flag beside the compatibility command.
 */
void ev08p02a_classic_console_onmsg(app_console_msg_t *msg)
{
    uint8_t subcode;

    if (msg == NULL) {
        return;
    }

    if (msg->name != 'y') {
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        return;
    }

    if (msg->kind != '*') {
        console_out_str(" ?cy: no query form -- *cy00 toggles the Type_TY AVAS voice\n");
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    if (msg->data_len != 1u) {
        console_out_str(" *cy: needs one synth subcode -- 00 = Type_TY AVAS toggle\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    subcode = msg->data[0];
    if (subcode == 0u) {
        console_out_str(" *cy00: Type_TY AVAS toggle\n");
        wm8904_audio_avas_voice_key((uint8_t)WM8904_AUDIO_AVAS_VOICE_TYPE_TY);
        msg->status = APP_CONSOLE_OK;
        return;
    }

    if (subcode <= 4u) {
        console_out_str(" *cy: Sonora synth subcode is unsupported on CK (only 00 = Type_TY AVAS)\n");
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    console_out_str(" *cy: unknown synth subcode\n");
    msg->status = APP_CONSOLE_ERR_BAD_DATA;
}

/*
 * Single-key hotkeys for this board, reached through console_board_hotkey() in
 * ev08p02a_console_out.c. Two keys, and they are ONE VOICE EACH rather than a key for
 * the engine plus a key for the selection: 'a' = Type_TY L1, 'A' = Type_LB L3,
 * fully exclusive at run time. Pressing the letter of the voice that is sounding stops
 * it; pressing the other one's letter refuses and says why, because a switch re-seeds
 * every oscillator and mid-sound that is a click. Both letters go through the one
 * entry point wm8904_audio_avas_voice_key() so they cannot come to mean different
 * shapes of thing -- see wm8904_audio.h, where the rule and sonora's precedent are.
 *
 * A ONE-VOICE IMAGE STILL BINDS BOTH, deliberately: 'A' there prints that the image
 * holds only the other voice and how to build one that holds both. A key that does
 * nothing is indistinguishable from a console that stopped listening, which on this
 * board is a real failure mode and not a hypothetical one.
 *
 * 'a' -- start/stop the AVAS synth, no Enter. sonora binds the same letter to the same
 * engine (apps/classic/classic_console.c: case 'a' -> UsrOperate_avas_synth()), and
 * matching it is the whole point: the two boards are listened to in one session, by ear,
 * switching back and forth, and a different key on each is a mistake waiting for the
 * moment your attention is on the sound instead of the keyboard. Since the synth became a
 * stage of the chain path, what this key does matches sonora too: a START from silence
 * begins at t = 0 of the reference and opens over 4 s, and a STOP fades out over ~3 s
 * instead of cutting -- pressed again during that fade it resumes without a phase reset,
 * which is sonora's rule and the reason a jump there would click.
 *
 * It is intentionally a bare toggle: measured cost is 289.2 us of a 667 us block
 * (43.0 %, miss = 0, 382.2 us margin), so the same direct interaction Sonora offers is
 * safe here. State remains in the audio module, rather than a duplicate flag beside the
 * key, so the two voice keys cannot drift apart.
 *
 * UP/DOWN ARROW -- AVAS pitch trim, +-5 cent per press (see wm8904_audio_avas_pitch_step()
 * and the module comment's *tc entry). A terminal sends an arrow as three bytes, the ANSI
 * CSI sequence ESC '[' 'A' (up) or ESC '[' 'B' (down), so this needs a tiny state machine
 * across calls rather than a single-byte match. app_console.c has no ESC handling of its
 * own (checked: nothing else on this board interprets 0x1B), so there is nothing to
 * collide with.
 *
 * 'A' NAMES TWO DIFFERENT THINGS -- Type_LB start/stop bare, and "pitch up" after
 * ESC '['. They cannot be confused because the state machine only reaches the arrow
 * branch after actually seeing ESC '[' first; a bare 'A' with the state machine idle
 * falls straight through to the existing case below, unchanged.
 *
 * UNRECOGNISED BYTES AFTER ESC ARE SWALLOWED, not handed to the line parser: this board
 * has no other use for ESC, and passing a half-consumed CSI sequence into a '*'/'?' line
 * would just start a line that can never parse, sitting there until the user notices
 * nothing is echoing.
 */
bool ev08p02a_transport_console_hotkey(uint8_t ch)
{
    static uint8_t s_esc_state; /* 0 = idle, 1 = saw ESC, 2 = saw ESC '[' */

    if (s_esc_state == 2u) {
        s_esc_state = 0u;
        if (ch == (uint8_t)'A') {
            wm8904_audio_avas_pitch_step(+5);
            return true;
        }
        if (ch == (uint8_t)'B') {
            wm8904_audio_avas_pitch_step(-5);
            return true;
        }
        return true;   /* some other CSI final byte -- swallow it, see above */
    }

    if (s_esc_state == 1u) {
        s_esc_state = (ch == (uint8_t)'[') ? 2u : 0u;
        return true;
    }

    if (ch == 0x1Bu) {
        s_esc_state = 1u;
        return true;
    }

    if (ch == (uint8_t)'a') {
        wm8904_audio_avas_voice_key((uint8_t)WM8904_AUDIO_AVAS_VOICE_TYPE_TY);
        return true;
    }

    if (ch == (uint8_t)'A') {
        wm8904_audio_avas_voice_key((uint8_t)WM8904_AUDIO_AVAS_VOICE_TYPE_LB);
        return true;
    }

    return false;
}
#endif

/*
 * LED0 blink, and a standing check on the clock.
 * ----------------------------------------------
 * Timing comes from Timer1, not a software delay loop, so the period is exact in Fcy
 * terms. A `while (count--) nop;` loop costs an unknown, compiler- and -O-dependent
 * number of cycles per iteration; that unknown multiplies into the elapsed time and is
 * indistinguishable from a clock error.
 *
 * SINCE 2026-08-03 THIS FILE NO LONGER OWNS TIMER1. It used to program T1CON/PR1 here
 * directly for a 100 ms period and poll IFS0bits.T1IF, with interrupts disabled -- so
 * this profile had a timer and no running time, and every cadence in it was expressed as
 * a COUNT OF 100 ms periods. app/timer_app.c now owns the timer and the vector, and the
 * cadences below are milliseconds against GetTicks().
 *
 * WHAT SURVIVES THE MOVE, AND HAD TO: the tick is clocked from the peripheral clock
 * (Fp = Fcy) and told the ASSUMED target Fcy as a compile-time constant, deliberately not
 * nora_clock_get_fcy_hz(). Using the HAL's own belief would self-correct and mask a
 * wrong operating point; against a fixed constant every GetTicks() millisecond is a
 * measurement of the real clock, and the blink is how you read it:
 *
 *     actual Fcy = EV08P02A_ASSUMED_FCY_HZ * 10 / (seconds for 10 cycles)
 *
 * One full ON->OFF->ON cycle is 1.000 s exactly when Fcy is on target. Verified on
 * hardware 2026-07-28: 10 cycles in 10 s, i.e. Fcy = 100 MHz. This is how the half-speed
 * clock behind the original garbled console was caught (it read 10 cycles in ~18-20 s),
 * and it is cheap to keep as a regression check -- the console cannot report a clock error
 * that breaks the console itself. A tick that fell back to the FRC instead would show up
 * as an obviously wrong blink (8 MHz vs 100 MHz), not as a silently correct one, which is
 * why timer_app offers the two clock sources as separate calls.
 *
 * The 1 ms period is exact at this Fcy: 100 MHz / 8 (prescale) / 1000 = 12500 counts,
 * PR1 = 12499. timer_app_start_from_fcy() refuses inexact divisors rather than rounding,
 * so a future operating point that cannot make a whole millisecond says so at bring-up.
 */
#define EV08P02A_ASSUMED_FCY_HZ (EV08P02A_TARGET_FOSC_HZ / 2UL)  /* 100 MHz */

/* 500 ms per LED state -> 1.000 s full cycle. */
#define EV08P02A_LED_STATE_PERIOD_MS (500u)

/*
 * SW0 sampling cadence, inside the wait rather than once per LED state.
 *
 * A deliberate human press is 100-300 ms, so sampling once per 500 ms iteration would
 * miss taps often enough to look like a broken button. Not sampled every millisecond
 * either, which the running time now makes possible: at 1 ms, contact bounce on RELEASE
 * can be seen as a second press and toggle the pause twice from one gesture. 100 ms is
 * comfortably above bounce and comfortably below the shortest real press -- it is the
 * cadence the polled-Timer1 version happened to have, kept on purpose.
 *
 * THIS APPLIES TO BOTH USES OF SW0, since 2026-08-05. It used to be compiled in only for
 * the blink-pause test, so the AUDIO build -- where SW0 is the mute button -- was left
 * sampling once per 500 ms iteration, and a normal tap was missed outright: the button
 * only answered when held past half a second. The reasoning above never depended on which
 * of the two the press means, so the gate in wait_next_led_edge() is now the union of the
 * two builds and only the action inside differs.
 */
#define EV08P02A_SW0_SAMPLE_PERIOD_MS (100u)

/* The RX DMA callback publishes raw CODEC-IN words once per completed TDM block; the
 * peak/mask calculation belongs in main context. One millisecond preserves a useful
 * sampled meter while keeping that work outside the 0.67 ms audio deadline. */
#define EV08P02A_AUDIO_OBSERVE_PERIOD_MS (1u)

/*
 * Console status/heartbeat cadence, deliberately INDEPENDENT of the LED blink.
 *
 * The 1.000 s LED cycle above is a live clock measurement (see the block comment at the
 * top of this file) and must not be retuned for console readability. So the profile polls
 * still run once per LED state -- SW0 sampling and the I2C1 scope-trigger transaction both
 * need that cadence -- and only the PRINTING is throttled to this period.
 *
 * INDEPENDENT IS NOW LITERALLY TRUE. This used to be
 * EV08P02A_STATUS_PRINT_PERIOD_MS / (periods_per_LED_state * 100), i.e. the print period
 * was a COUNT of LED states and therefore only expressible as a multiple of 500 ms -- a
 * value here that did not divide evenly was silently rounded, and an #error guarded the
 * case where it rounded to zero. Against GetTicks() the two cadences no longer share a
 * unit, so neither constrains the other and the guard is unnecessary. The gate is still
 * evaluated once per iteration, so the effective granularity is one LED state; that is a
 * property of where it is asked, not of how it is expressed.
 *
 * The per-iteration "tick LED0=on/off" line used to be the heartbeat. It is gone: at two
 * lines per second it buried the status lines it was printed alongside, and the LED itself
 * is the better liveness indicator (it is also the clock measurement, which a printf
 * never was).
 */
#define EV08P02A_STATUS_PRINT_PERIOD_MS (2000u)

/*
 * SW0 -> LED0 BLINK PAUSE, and why this exists at all.
 *
 * Everything else in this profile has hardware evidence; LED0 and SW0 did not. LED0 was
 * only ever *watched* (nobody had confirmed the pin drives the physical LED as opposed to
 * the loop merely running), and SW0 was never read in any build but the audio one, where it
 * is the mute button. One gesture closes both: press SW0 and the blink stops, press again
 * and it resumes. A pressed button that does nothing, or a blink that never pauses, points
 * at exactly one of the two pins.
 *
 * NOT ENABLED IN THE AUDIO BUILD. There SW0 is wm8904_audio's mute button
 * (see s_wm8904_audio_port), and one press meaning both "mute" and "stop blinking" makes
 * neither observation clean. The gate is the audio switch itself rather than a separate
 * define, so the two uses of SW0 cannot both be compiled in by accident.
 *
 * WHAT IS PAUSED IS THE LED, NOT THE CADENCE. The 1.000 s blink is also this profile's
 * live clock measurement (see the block comment at the top of this file), so pausing must
 * not touch the Timer1 wait, the console, the report gate or any exerciser -- they all keep
 * running while the LED sits still. That is deliberate: "I paused it" and "it hung" have to
 * stay distinguishable. What distinguishes them is the CONSOLE, which keeps answering
 * commands in every build (console_task_poll() runs inside the Timer1 wait, not between
 * iterations) -- not the periodic status line, which is the weaker signal: it is throttled
 * to a wall-clock period (DEMO_TDM_STATUS_PERIOD_MS, 5 s in the TDM loopback build), so it
 * only tells you something once every few seconds and only if that build is the one running.
 * When this test was written that line was throttled by a CALL COUNT instead, which on this
 * board worked out to one line per nine hours -- it proved nothing at all then. The console
 * is still the primary liveness evidence; the status line is now a slow second opinion.
 * The LED parks OFF so that paused is unambiguous at a glance.
 */
#if !DEMO_ENABLE_WM8904_AUDIO
#define EV08P02A_SW0_PAUSES_BLINK 1
#else
#define EV08P02A_SW0_PAUSES_BLINK 0
#endif

#if EV08P02A_SW0_PAUSES_BLINK
/*
 * Sampled at 100 ms inside the Timer1 wait, not once per 500 ms iteration, and latched.
 * A deliberate human press is 100-300 ms, so a once-per-iteration sample would miss taps
 * often enough to look like a broken button -- which is the exact ambiguity this test is
 * supposed to remove. The latch is consumed by profile_wait_next_tick(), so a press is
 * acted on once no matter how long it is held; releasing is not required to be seen.
 *
 * No debounce: contact bounce is sub-millisecond to a few ms, an order of magnitude below
 * this sampling interval, and the edge that matters is the first one either way.
 *
 * Not volatile: both the setter and the consumer run in MAIN context, so there is no
 * concurrency here to guard against. That reasoning used to be "this profile has no ISR at
 * all -- Timer1 is polled", which stopped being true on 2026-08-03 when the 1 ms tick
 * became interrupt-driven (app/timer_app.c). The conclusion survives for a better reason:
 * sw0_latch_poll() is called from the wait loop and sw0_take_press() from the iteration
 * body, both in main context, and nothing in the tick path touches this. Should SW0 ever
 * be sampled from the tick hook, this has to become volatile.
 */
static bool s_sw0_press_latched;

static void sw0_latch_poll(void)
{
    static bool was_pressed;
    bool        pressed = ev08p02a_sw0_pressed();

    if (pressed && !was_pressed) {
        s_sw0_press_latched = true;
    }
    was_pressed = pressed;
}

static bool sw0_take_press(void)
{
    bool pressed = s_sw0_press_latched;

    s_sw0_press_latched = false;

    return pressed;
}
#endif /* EV08P02A_SW0_PAUSES_BLINK */

/*
 * Is SW0 sampled inside the wait? Yes in either build that reads it at all -- the union of
 * the blink-pause test and the audio build's mute button. The two are mutually exclusive by
 * construction (EV08P02A_SW0_PAUSES_BLINK is !DEMO_ENABLE_WM8904_AUDIO), so this never
 * compiles both actions in; it exists so the CADENCE is stated once and cannot be given to
 * one use and forgotten for the other, which is exactly the defect it was added to fix.
 */
#if EV08P02A_SW0_PAUSES_BLINK || DEMO_ENABLE_WM8904_AUDIO
#define EV08P02A_SW0_SAMPLED_IN_WAIT 1
#else
#define EV08P02A_SW0_SAMPLED_IN_WAIT 0
#endif

/*
 * Wait out one LED state, servicing the console throughout.
 *
 * THE DEADLINE ADVANCES BY THE PERIOD, it is not resampled: `s_led_edge_ms +=` rather
 * than the fleet's usual `last = GetTicks()`. Both are wrap-safe; the difference is
 * drift. Resampling loses the fraction of a millisecond by which each wait overshoots, so
 * a 500 ms state becomes 500-501 ms and the LED cycle -- which is this profile's clock
 * MEASUREMENT -- would read up to 0.2% slow for reasons that have nothing to do with the
 * clock. Adding the period keeps the edges on an exact 500 ms grid and any single late
 * iteration is absorbed by the next. Print cadences resample, because they are cadences;
 * this one does not, because it is an instrument.
 *
 * Seeded on first call rather than at bring-up: the first state is then 500 ms measured
 * from when the loop actually starts, not from bring-up, so the banner and the codec's
 * own multi-hundred-millisecond init do not eat into it and show up as a short first
 * blink.
 */
static void wait_next_led_edge(void)
{
    static uint32_t s_led_edge_ms;
    static bool     seeded;
#if EV08P02A_SW0_SAMPLED_IN_WAIT
    uint32_t sw0_sample_ms;
#endif
#if DEMO_ENABLE_WM8904_AUDIO
    uint32_t audio_observe_ms;
#endif

    if (!seeded) {
        s_led_edge_ms = GetTicks();
        seeded = true;
    }
#if EV08P02A_SW0_SAMPLED_IN_WAIT
    sw0_sample_ms = s_led_edge_ms;
#endif
#if DEMO_ENABLE_WM8904_AUDIO
    audio_observe_ms = s_led_edge_ms;
#endif

    while ((uint32_t)(GetTicks() - s_led_edge_ms) < EV08P02A_LED_STATE_PERIOD_MS) {
        /*
         * Service the console here rather than once per main-loop iteration. The loop
         * spends essentially all of its ~500 ms inside this wait, and the UART RX path is
         * polled with no ring buffer -- draining only between iterations would let a typed
         * command overrun the hardware FIFO and arrive as fragments. This wait is
         * otherwise doing nothing.
         */
#if DEMO_ENABLE_CONSOLE_COMMANDS
        console_task_poll();
#endif

#if DEMO_ENABLE_WM8904_AUDIO
        if ((uint32_t)(GetTicks() - audio_observe_ms) >=
            EV08P02A_AUDIO_OBSERVE_PERIOD_MS) {
            /* Resample rather than catch up: this is intentionally a diagnostic meter,
             * not a real-time audio task, and it must not extend a late wait iteration. */
            audio_observe_ms = GetTicks();
            wm8904_audio_rx_observe_poll();
        }
#endif

#if EV08P02A_SW0_SAMPLED_IN_WAIT
        if ((uint32_t)(GetTicks() - sw0_sample_ms) >= EV08P02A_SW0_SAMPLE_PERIOD_MS) {
            sw0_sample_ms += EV08P02A_SW0_SAMPLE_PERIOD_MS;
#if EV08P02A_SW0_PAUSES_BLINK
            sw0_latch_poll();
#else
            /* Audio build: SW0 is the mute button. This does the edge detection and
             * publishes the request; it prints one line per press and nothing otherwise,
             * so calling it 5x per LED state costs nothing on the console. */
            wm8904_audio_button_poll();
#endif
        }
#endif
    }

    s_led_edge_ms += EV08P02A_LED_STATE_PERIOD_MS;
}

static void report_operating_point(void)
{
    console_out_str(
        "\nEV08P02A dsPIC33CK256MC005 bring-up: LED0/UART alive\n");

    /*
     * Build ID, so a console check can prove which IMAGE restarted rather than
     * merely that the board restarted. The banner line above is printed by every
     * build, old ones included -- so if a drag-and-drop programming attempt is
     * rejected and the board just resets, matching on the banner alone still
     * "passes". This line is what buildtools/flash-curiositynano.ps1's
     * -VerifyUartContains should be pointed at. build.ps1 supplies the value;
     * builds without it say so rather than printing a misleading blank.
     */
    console_out_str("Build ID = ");
#if defined(EV08P02A_HAVE_BUILD_ID_H)
    console_out_str(EV08P02A_BUILD_ID);
#else
    console_out_str("(not stamped)");
#endif
    console_out_str("\n");

    /* Why this boot happened. SWR here means the *sr console command worked;
     * POR means the board was power-cycled. With no reset button on the Nano,
     * that is the only way to tell those two apart. */
    console_out_str("Reset    = ");
    console_out_str(board_reset_cause_str());
    console_out_str("\n");

    /* Frequencies from the Clock HAL, verdict from the board's volatiles -- the same two
     * places DM330030 keeps them. The ev08p02a_fosc_hz()/_clock_at_target()/... wrappers
     * this used to call are gone; see ev08p02a_board.h. */
    console_out_str("Fosc Hz  = ");
    console_out_u32(nora_clock_get_fosc_hz());
    console_out_str("\nFcy Hz   = ");
    console_out_u32(nora_clock_get_fcy_hz());
    console_out_str("\nBaud     = ");
    console_out_u32(nora_uart_get_baudrate(EV08P02A_CONSOLE_UART_INST));
    console_out_str("\nClock    = ");
    console_out_str(
        g_ev08p02a_clock_on_target ? "PLL at target\n"
                                   : "NOT at target (fell back to FRC)\n");
    console_out_str("PLLstatus= ");
    console_out_u32((uint32_t)g_ev08p02a_clock_init_status);
    console_out_str("  (0 = OK, 4 = timeout)\nPLLdiag  = ");
    console_out_u32((uint32_t)g_ev08p02a_clock_diag);
    console_out_str("  (0 = none, 1 = no switch, 2 = no lock, 8 = switch ignored)\n");

    /* What actually landed in the PLL, read back from the registers rather than
     * recomputed, plus COSC/LOCK so a silent failure to switch or lock is
     * visible instead of being inferred from the frequency.
     *
     * DELIBERATELY READ DIRECTLY AND NOT THROUGH THE CLOCK HAL. The HAL offers
     * nora_clock_dspic33ck_raw_capture() for exactly these four words, and using it here
     * would be the tidier call -- but then the banner and the HAL would read the registers
     * through one shared path, so they could only ever agree. These four lines are the
     * INDEPENDENT reading that can contradict "Fosc Hz" above, and that is their whole
     * value: on 2026-08-10 they are what confirmed the new HAL's derived 200 MHz against
     * the actual divider chain, and what showed a comment in ev08p02a_board.c had been
     * quoting dividers this board never ran. Keep them independent. */
    console_out_str("PLLPRE   = ");
    console_out_u32((uint32_t)CLKDIVbits.PLLPRE);
    console_out_str("\nPLLFBDIV = ");
    console_out_u32((uint32_t)PLLFBDbits.PLLFBDIV);
    console_out_str("\nPOST1DIV = ");
    console_out_u32((uint32_t)PLLDIVbits.POST1DIV);
    console_out_str("\nPOST2DIV = ");
    console_out_u32((uint32_t)PLLDIVbits.POST2DIV);
    /* From the post-switch snapshot, not from OSCCON now: a fallback to the FRC
     * would otherwise overwrite the very evidence we want. */
    console_out_str("\nCOSC@sw  = ");
    console_out_u32(
        ((uint32_t)g_ev08p02a_osccon_after_switch >> 12) & 0x7u);
    /* The raw OSCCON.COSC code, not a yes/no. The legend names only the two values this
     * board can end on -- its target and its fallback -- because those are the only two
     * it asks for; a third value here would mean something wrote NOSC that this board
     * did not, and is worth seeing as the number it is rather than as "other". */
    console_out_str("  (COSC code: 1 = FRC+PLL, 0 = FRC)\nLOCK@sw  = ");
    console_out_u32(
        ((uint32_t)g_ev08p02a_osccon_after_switch >> 5) & 0x1u);
    console_out_str("\n");
}

/* -------------------------------------------------------------------------- */
/* Profile hooks -- see src/profile_main.h. The four-phase sequence these      */
/* implement used to be written out inline in this file, in its own main();    */
/* the sequence now lives once in src/main.c and is shared with dm330030.      */
/* -------------------------------------------------------------------------- */

/*
 * Recorded rather than acted on, like every other bring-up result on this board
 * (g_ev08p02a_*_init_ok): profile_report() runs after the console exists, so a tick that
 * did not start can SAY so. Failing hard here would be a silent hang before the first
 * character -- and the tick not starting does not stop the console, the trap report or the
 * operating-point dump, which are the things a person is reading at that moment.
 *
 * It does stop everything paced by GetTicks(), which is why it is worth a line: the
 * symptom, otherwise, is a dark LED and a loop that never advances, and that looks
 * exactly like a clock fault.
 */
static volatile bool g_ev08p02a_tick_start_ok;

void profile_bring_up(void)
{
    ev08p02a_board_init();

    /*
     * The running time, clocked from Fp = Fcy at the ASSUMED operating point -- see the
     * LED0-blink block comment for why the constant and not nora_clock_get_fcy_hz().
     * Before board_init would be wrong: Timer1 counts the peripheral clock, so the clock
     * stage has to have run.
     */
    g_ev08p02a_tick_start_ok = timer_app_start_from_fcy(EV08P02A_ASSUMED_FCY_HZ);
}

void profile_report(void)
{
    report_operating_point();

    if (!g_ev08p02a_tick_start_ok) {
        console_out_str(
            "1 ms tick FAILED to start -- LED0 will not blink and every timed poll is "
            "stopped (console still works)\n");
    }

    /* Before the help line, so a trap report is the first thing after the
     * operating point rather than being buried under it. Silent on a clean
     * boot. */
    app_traps_report_previous();

#if DEMO_ENABLE_CONSOLE_COMMANDS
    console_task_init();
    console_task_print_help();
#endif
}

void profile_start(void)
{
#if DEMO_ENABLE_DMA_SELFTEST
    /*
     * Prove the DMA controller in isolation BEFORE any SPI work, so a stream that
     * does not run can be attributed without a second flash cycle: PASS means the
     * controller moves data and a fault is on the trigger/peripheral side; FAIL
     * means stop investigating the SPI. This ordering is what found two of the
     * three DMA defects in docs/ck_silicon_findings.md.
     *
     * It REPORTS, it no longer gates. It used to skip the stage A/B/B2/C0 harness
     * on failure, and that gate was worth having because running those stages
     * against a controller known not to move data produces failures whose cause is
     * already known. Those stages were deleted with ev08p02a_spi_dma_min.c, so
     * there is nothing left to skip -- the exercisers that remain stream
     * continuously and are judged on `miss` and the load figures instead.
     */
    if (!dma_selftest_run(EV08P02A_SELFTEST_DMA_CH)) {
        console_out_str(
            "DMA selftest FAILED -- treat the exerciser output below as "
            "uninterpretable\n");
    }
#endif

#if DEMO_ENABLE_I2C_PROBE
    /* One-shot at boot. Mutually exclusive with the WM8904 path, which drives the
     * same bus -- see the #error above. */
    i2c_probe_run(&s_i2c1_probe);
#endif

#if DEMO_ENABLE_TDM_MASTER_LOOPBACK
    /* Codec-less TDM8/32-bit MASTER: self-clocked BCLK/FS/SDO. Needs nothing on the
     * other end -- see the port above and the note on the enable flag. */
    demo_tdm_master_loopback_start(&s_tdm_loopback_port);
#endif

#if DEMO_ENABLE_WM8904_AUDIO
    /*
     * MUST come immediately before the start below, and there is no console letter for it:
     * it probes the transport while NOTHING has been configured yet (mode == NONE), and
     * inst_configure() inside start() makes that state unreachable for the rest of the
     * session -- close() does not reset the mode. Guaranteed to be the first configure on
     * this board because app_config.h #errors if the loopback demo is enabled too.
     *
     * Gated with *tl: same subject, same probe_*() output, and the same argument for
     * dropping it (see WM8904_AUDIO_ENABLE_TDM_DIAG). It is the one item in the gate that
     * runs unasked at every boot, so a diag-less image is also three console lines quieter.
     */
#if WM8904_AUDIO_ENABLE_TDM_DIAG
    (void)wm8904_audio_lifecycle_probe_unconfigured();
#endif

    /* Phase B: real WM8904 codec over TDM8/32-bit. See the port/config above. */
    wm8904_audio_start(&s_wm8904_audio);
    /*
     * Said here rather than in console_task_print_help(), which is the shared help and
     * has no business naming one board's key. The hotkey is registered by this file, so
     * this is the one place where the key and its announcement cannot drift apart --
     * which is the same rule console_task.h states about the boot help and ?gh.
     */
    console_out_str("      'a' / 'A' alone (no Enter) start-stop the AVAS synth's"
                    " type_ty / type_lb voice, exclusive (stop fades out over ~3 s);"
                    " up/down arrow trim pitch +-5 cent;"
                    " Classic *cy00 is the Type_TY structured toggle\n");
#endif
}

/*
 * Every enabled exerciser is polled on EVERY call; `report` only decides whether
 * each one PRINTS. That split is why report is a parameter -- see the note in
 * src/profile_main.h. The I2C1 probe in particular must run its transaction each
 * time regardless, so a scope on ASDA1/ASCL1 keeps getting a fresh
 * START/ADDR/NACK to trigger on rather than one burst at boot.
 */
void profile_poll(bool report)
{
#if DEMO_ENABLE_I2C_PROBE
    i2c_probe_poll(&s_i2c1_probe, report);
#endif

#if DEMO_ENABLE_TDM_MASTER_LOOPBACK
    /* Takes no `report`: the shared demo throttles its own status line. */
    demo_tdm_master_loopback_poll();
#endif

#if DEMO_ENABLE_WM8904_AUDIO
    wm8904_audio_poll(report);
#endif

#if !DEMO_ENABLE_I2C_PROBE && !DEMO_ENABLE_WM8904_AUDIO && \
    !DEMO_ENABLE_TDM_MASTER_LOOPBACK
    /* A build with every exerciser disabled -- the LED/UART baseline. Still a
     * valid image: the LED keeps blinking and the console keeps answering. */
    (void)report;
#endif
}

/*
 * LED state, the print throttle and the wait all live here together: they are one cadence.
 * The LED half-period IS the loop period (and doubles as the live clock check described at
 * the top of this file), so splitting them across hooks would let one be retuned without
 * the other.
 *
 * The print throttle used to be a COUNT of LED states and is now a millisecond period
 * against GetTicks() (2026-08-03), which is what makes "independent of the blink" more
 * than an intention -- see EV08P02A_STATUS_PRINT_PERIOD_MS.
 */
bool profile_wait_next_tick(void)
{
    static bool     led_on;
    static uint32_t status_last_ms;
    bool            report;

#if EV08P02A_SW0_PAUSES_BLINK
    static bool blink_paused;

    if (sw0_take_press()) {
        blink_paused = !blink_paused;
        console_out_str(blink_paused ? "SW0: LED0 blink paused (loop still running)\n"
                                     : "SW0: LED0 blink resumed\n");
    }

    if (blink_paused) {
        /* Park OFF, once, then leave the pin alone -- see the note above. */
        if (led_on) {
            led_on = false;
            ev08p02a_led0_set(false);
        }
    } else {
        led_on = !led_on;
        ev08p02a_led0_set(led_on);
    }
#else
    led_on = !led_on;
    ev08p02a_led0_set(led_on);
#endif

    /*
     * Resampled (`= GetTicks()`), unlike the LED edge which advances by its period: this
     * is a print cadence, not a measurement, so absorbing the overshoot is right -- two
     * status lines must never come out closer together than the period, and being a
     * millisecond late is invisible. See wait_next_led_edge() for the other choice and
     * why it is the other choice.
     */
    report = ((uint32_t)(GetTicks() - status_last_ms) >= EV08P02A_STATUS_PRINT_PERIOD_MS);
    if (report) {
        status_last_ms = GetTicks();
    }

    wait_next_led_edge();

    return report;
}
