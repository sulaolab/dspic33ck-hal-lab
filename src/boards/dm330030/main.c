/*******************************************************************************
Copyright 2019 Microchip Technology Inc. (www.microchip.com)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*******************************************************************************/

/*
 * Modified by SulaoLab, 2026 (Apache-2.0 section 4(b)). What changed is recorded
 * below in full.
 *
 * boards/dm330030/main.c -- this board's five profile hooks, and nothing else.
 *
 * The licence above is kept because this file descends from the Microchip demo's
 * main(); what is left of that demo here is the 1 ms timer configuration.
 *
 * WHAT LEFT THIS FILE (2026-08-02)
 * --------------------------------
 * It was 412 lines, of which about 380 were application code:
 *
 *   the pot/RGB/button demo   -> demo_rgb_pot_buttons.c (same directory: it speaks
 *                                three board APIs, so it is board-owned -- see that
 *                                file's header for why it did NOT go to app/)
 *   the button debounce       -> app/button_debounce.c (no pin, no port, no board
 *                                register: pure timing logic, one instance per button)
 *   I2cWm8904Probe()          -> DELETED, not moved. It was a second, weaker copy of
 *                                app/i2c_probe.c -- see the note at s_i2c1_probe below.
 *
 * Also gone: a "Getting Started" comment pointing at a readme.txt that does not
 * exist in this repo, and a "Required baud rate: 38400" note that stopped being true
 * when the console baud came from the Clock HAL (it is 230400 -- the surviving copy
 * of that note, corrected, is in demo_rgb_pot_buttons.c where the printing lives).
 *
 * boards/ev88g73a/main.c is the shape this now matches: hooks only, with the
 * per-build exercisers behind their ENA_* switches.
 */

#ifndef DSPIC33CK_BOARD_DM330030
#error "boards/dm330030/main.c is DM330030-owned. Build it only in the CK256MP508_DM330030 configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include <stdbool.h>
#include <stdio.h>       /* the tick-failure line in profile_report(); this board's
                          * console is printf through uart_platform_stdio.c */

#include "dm330030_board.h"
#include "profile_main.h"

/* The running time, and the vendor BSP tick registry that is layered on top of it.
 * timer_app.h is what starts Timer1 and owns its vector; timer_1ms.h is only the
 * TICK_HANDLER registry the inherited demo registers its RGB soft-PWM and button
 * sampler with. See app/timer_app.h. */
#include "timer_app.h"
#include "timer_1ms.h"

#include "nora_clock.h"   /* NORA_CLOCK_FRC_HZ -- what Timer1 counts here */

#include "dm330030_io.h"
#include "dm330030_pins.h"   /* the *_WIRING_STR / *_WHERE_STR text, beside the RP numbers */
#include "dm330030_pot.h"

#include "demo_rgb_pot_buttons.h"

/* The profile's switches, normalized: app_config.h pulls in this board's board_profile.h
 * (the include path selects which board's copy is found) and refuses anything unresolved or
 * contradictory. The values were ENA_* defined in app_config.h itself until 2026-08-05,
 * while EV88G73A answered the same questions under EV88G73A_ENABLE_* in its own main.c --
 * one decision, two names, and a build that had to set both. */
#include "app_config.h"

/*
 * The interactive command set. Since 2026-08-05 this profile has one -- see
 * board_profile.h for what had to be true first, and the parity contract
 * for what it has to answer. Shared with EV88G73A, not copied: what was missing
 * here was the input seam and a terminal the replies could be read on, not interpreter code.
 */
#if DEMO_ENABLE_CONSOLE_COMMANDS
#include "console_task.h"
#endif

/*
 * Optional WM8904 I2C bring-up probe (README step 5). Compile-time gated and
 * OFF by default so the baseline keeps building without the mikroBUS-A codec
 * board. Enable by defining DEMO_ENABLE_I2C_PROBE at build time.
 */
#if DEMO_ENABLE_I2C_PROBE
#include "i2c_probe.h"

/*
 * What the shared probe needs to know about THIS board's bus, mirroring
 * boards/ev88g73a/main.c's struct.
 *
 * This REPLACED a private I2cWm8904Probe() in this file that did the same
 * transaction less well. The three differences all favour the shared one:
 *
 *   - it classifies the result (OK / ERR_NACK, which is the module PASSING with no
 *     device attached / TIMEOUT-BUS-COLLISION, which is inconclusive) instead of
 *     printing "probe failed (status=%d)" for every one of them
 *   - it names the status through nora_i2c_status_str() instead of a raw int
 *   - it reports WHICH pins it drove, via `where` below
 *
 * and the bus_init hook closes a real shape defect on the way past: the private version
 * drove ASCL1/ASDA1 without going through the board's pin stage at all, so nothing
 * stated those pins' analog bit on that path (see dm330030_i2c1_init()). Harmless on
 * this particular pair -- RC8/RC9 have no ANSEL bit on MP508 -- but it meant two paths
 * to one bus, one of them incomplete.
 *
 * The rate this probes at CHANGED (2026-08-02): it was a 100 kHz wrapper of its own, and
 * the audio path used a 400 kHz one, so the same codec was reached at two rates depending
 * on the build. The fleet's measurement is that 400 kHz works where 100 kHz fails in
 * hardware, so there is now one dm330030_i2c1_init() at 400 kHz and both callers use it.
 * EV88G73A already worked this way.
 *
 * BOTH i2c_probe_run() (profile_start) AND i2c_probe_poll() (profile_poll) are wired now,
 * as EV88G73A does. The repeating transaction exists to give a scope something to trigger
 * on, so its absence here was a real capability difference -- and the reason given for it
 * was "the foreground iteration is a full ANSI screen repaint that this board's report
 * region does not have room to interleave with", which stopped being true on 2026-08-05
 * when the screen became a scrolling throttled line. Nothing else argued for one boot-time
 * probe, so the two profiles now answer this the same way
 * ("the difference today is a profile choice nobody stated").
 */
static const i2c_probe_t s_i2c1_probe = {
    .inst        = NORA_I2C_INST_1,
    .addr7       = 0x1Au,                     /* WM8904, 0x34 >> 1 */
    .reg_pointer = 0x00u,                     /* device-ID register (expect 0x8904) */
    .bus_init    = dm330030_i2c1_init,
    .where       = DM330030_I2C1_WHERE_STR,
};
#endif

#if DEMO_ENABLE_TDM_MASTER_LOOPBACK
#include "demo_tdm_master_loopback.h"

/*
 * This board's half of the codec-less TDM master smoke test. The demo used to carry
 * these four RP numbers itself -- see the note in demo_tdm_master_loopback.h -- which
 * meant a shared app/ module owned DM330030's mikroBUS-A pinout and could run nowhere
 * else. Same routing function the WM8904 path uses, in the other role.
 */
static const demo_tdm_master_loopback_port_t s_tdm_loopback_port = {
    .configure_pins = dm330030_tdm_pins_init,
    .wiring         = DM330030_TDM_LOOPBACK_WIRING_STR,
};
#endif

#if DEMO_ENABLE_WM8904_AUDIO
#include "wm8904_audio.h"

/*
 * WM8904 passthrough over TDM8/32-bit. The path is shared (app/wm8904_audio.c, which
 * this board's own demo_wm8904_audio.c was merged into); below is this board's half.
 *
 * THE ROLE-REJECTING WRAPPER THAT USED TO BE HERE IS GONE. It was a static
 * dm330030_tdm_pins_init(role) that returned false for anything but SLAVE and then
 * delegated to the board -- so "can this board be the clock master" was answered half in
 * this file and half in the board's function name. The board answers it now (both roles,
 * same four pins), and the profile states its CHOICE where the choice is made: beside
 * .dspic_is_master below.
 */
static const wm8904_audio_port_t s_wm8904_port = {
    .configure_pins      = dm330030_tdm_pins_init,
    .i2c_init            = dm330030_i2c1_init,      /* 400 kHz; same bus and pins as the probe */
    /* THERE IS NO .mclk_init SLOT ANY MORE (2026-08-04). It held dm330030_mclk_init(),
     * with a comment claiming "this codec has no crystal of its own, unlike EV88G73A" --
     * the same WM8904 board is on both and its XTAL is on both, so the claim was false, and
     * the stage drove ~12.5 MHz at a codec whose driver solves every rate row from
     * 12.288 MHz. Wrong frequency is reason enough; whether it was ALSO a second clock
     * competing with one the codec already had is a board fact this tree never checked
     * (that clause was dropped 2026-08-09 -- plan doc 20.4/20.6). Both the stage and the
     * hook are gone; see dm330030_board.c's note. */
    /* No mute button on this profile: S1/S2/S3 belong to the RGB/pot demo, and taking
     * one would change that demo's behaviour on a board whose hardware verification is
     * still deferred. NULL also selects the plain-copy block callback, which is exactly
     * what this board's own version did. */
    .mute_button_pressed = NULL,
    /* From dm330030_pins.h, beside the RP numbers it describes -- as EV88G73A does with
     * EV88G73A_AUDIO_WIRING_STR. This used to spell the pins out a second time here. */
    .wiring              = DM330030_AUDIO_WIRING_STR,
};

static const wm8904_audio_config_t s_wm8904_audio = {
    .port            = &s_wm8904_port,
    .i2c_inst_legacy = 1u,      /* wm8904.c maps this to NORA_I2C_INST_1 */
    /*
     * WM8904 drives BCLK/FS/LRCLK. Fixed here rather than a build switch like EV88G73A's,
     * and that is now the ONLY difference between the two profiles on this axis -- a
     * profile choice, not a limit. Set true and the board routes the master direction;
     * nothing in dm330030_board.c refuses it, and EV88G73A has run that direction on
     * hardware through the same shared code.
     *
     * THE REASON THIS COMMENT USED TO GIVE WAS FALSE: "on this board the codec's SYSCLK
     * comes from the dsPIC's REFO1 rather than a crystal, so having the dsPIC drive BCLK
     * as well was never the arrangement this wiring was designed for." The same WM8904
     * board with the same crystal is on both, the REFO1 stage is deleted (2026-08-04), and
     * the codec's SYSCLK has nothing to do with which side drives BCLK/FS.
     */
    .dspic_is_master = false,
    .brg             = 0u,      /* ignored as slave */
    .sample_rate_hz  = 48000u,  /* wm8904.c's own default; only used by the gain ramp */
    .gain_ramp_ms    = 0u,      /* no gain stage: plain passthrough copy */
    /*
     * 20 s, BY STATEMENT rather than by arithmetic. It was 1000 repaints, described as
     * "~20 s at the observed iteration rate", which was true only for as long as that rate
     * held; the value before that was a 65536-iteration bitmask, i.e. roughly 20 minutes,
     * which was less a choice than an artefact of masking. This is the same intent with
     * nothing left to re-derive.
     *
     * Since 2026-08-05 the profile HAS a throttle of its own too
     * (DM330030_STATUS_PRINT_PERIOD_MS, 1 s), so two periods are stacked here where the
     * note used to say there was only one. That is fine and not the arithmetic trap
     * EV88G73A avoids by setting this to 0: 20 s is a wall-clock check inside the module,
     * not a count of calls, so the observed cadence stays 20 s with a 1 s granularity
     * rather than becoming their product.
     */
    .status_period_ms      = 20000u,
    /*
     * 40 s, matching EV88G73A -- and this used to be 0, i.e. silent, for a reason that no
     * longer exists: "this console is a fixed ANSI screen region that the demo repaints, so
     * an unsolicited line lands in the middle of it". The console scrolls now, so the
     * reason to withhold the one line that distinguishes "no codec wired" from "the display
     * is broken" (parity contract 9.5) is gone with it. Never exercised on hardware either
     * way: this board's audio path has never run.
     */
    .idle_report_period_ms = 40000u,
};
#endif

/* -------------------------------------------------------------------------- */
/* THE TWO CADENCES, both in milliseconds of running time (GetTicks()).        */
/*                                                                            */
/* This board used to have neither: profile_wait_next_tick() returned true     */
/* immediately and the loop rate was whatever a full ANSI screen repaint took  */
/* at 230400 baud (~20 ms, measured, and the transmitter was busy for all of   */
/* it). A console cannot be interactive behind that, so the screen became one  */
/* throttled line and the loop got a stated period -- parity contract items    */
/* 3.1 and 3.4, and the same shape EV88G73A already has.                      */
/* -------------------------------------------------------------------------- */

/*
 * Loop period: how often the pot is sampled and pushed to the RGB channel, and how often
 * S3 is read. 20 ms is what the repaint accidentally gave this demo, kept deliberately --
 * 50 Hz is smooth for a knob driving a brightness, and it is the number the demo's own
 * comments already reasoned against (the LED1/LED2 mirror runs on the 1 ms tick precisely
 * because doing it at this rate would be visibly slower).
 *
 * S1/S2 do NOT depend on this: they are debounced on the 1 ms tick registry
 * (demo_rgb_pot_buttons.c), so no press is lost between iterations.
 */
#define DM330030_POLL_PERIOD_MS (20u)

/*
 * Status-line period. The line that replaced the eight-line region, and the number that
 * replaced "every iteration": at 50 Hz a scrolling line is unreadable and would bury the
 * console's own replies, which is the failure the ANSI screen was avoiding in the first
 * place. 1 s is fast enough to watch the pot move and slow enough to type under.
 */
#define DM330030_STATUS_PRINT_PERIOD_MS (1000u)

/* -------------------------------------------------------------------------- */
/* Profile hooks -- see src/profile_main.h. This board had its own main();     */
/* the four-phase sequence now lives once in src/main.c, shared with ev88g73a. */
/* -------------------------------------------------------------------------- */

/* Set by profile_bring_up(), read by profile_report() -- see both. */
static bool s_tick_start_ok;
static bool s_tick_clients_ok;

void profile_bring_up(void)
{
    dm330030_board_init();

    /*
     * The pot's PIN belongs to dm330030_board_init() above (dm330030_user_io_pins_init);
     * hal_adc owns ADC1/shared-core setup; dm330030_pot.c owns only the channel.
     *
     * The result is deliberately not checked here even though it used to be discarded
     * with a bare (void): a message printed now is erased by PrintHeader(), so the
     * failure is reported by the demo's pot line instead, via dm330030_pot_fault().
     */
    (void)dm330030_pot_init();

    /*
     * START THE RUNNING TIME, then hand the vendor registry a hook into it.
     *
     * FROM THE FRC, not from Fcy: Timer1 counts the 8 MHz internal oscillator directly,
     * so the tick keeps its 1 ms whatever the PLL did -- including a fallback to FRC,
     * which is the case where a wrong-rate tick would be least welcome. This is the
     * clock app/timer_1ms.c chose for itself before the timer moved to app/timer_app.c,
     * so nothing about this board's timing changes. (EV88G73A asks from Fcy instead, and
     * on purpose: its LED cycle is a live measurement of the operating point. The two
     * entry points and their reasons are documented in timer_app.h.)
     *
     * THE ORDER IS LOAD-BEARING. TIMER_SetConfiguration() no longer starts anything --
     * it checks that the tick IS running and registers the per-millisecond client walk
     * -- so calling it first would report failure and leave the RGB soft-PWM and the
     * button sampler in demo_rgb_pot_buttons_init() below with no ticks.
     *
     * Neither result is printed here: PrintHeader() in profile_report() erases whatever
     * is on the screen, so a failure has to be said afterwards, which is what
     * s_tick_start_ok/s_tick_clients_ok are for.
     */
    s_tick_start_ok   = timer_app_start_from_frc(NORA_CLOCK_FRC_HZ);
    s_tick_clients_ok = TIMER_SetConfiguration(TIMER_CONFIGURATION_1MS);

    demo_rgb_pot_buttons_init();
}

void profile_report(void)
{
    demo_rgb_pot_buttons_print_header();

    /*
     * NO CURSOR ADDRESSING ANY MORE. This used to be printed at ROW 17, because that was
     * the first row neither the header (rows 1..7) nor the repainted region (rows 8..15)
     * would erase within one poll -- the reason the bring-up could not simply say this.
     * The console scrolls now, so it is said here, in order, like anything else.
     *
     * Said only when something is wrong: with no tick the RGB soft-PWM and the button
     * debouncers have no clock at all, and their symptom (a frozen colour, a button that
     * never registers) reads exactly like a wiring fault. The console survives it -- see
     * profile_wait_next_tick(), which keeps polling -- so this line can be read and acted
     * on, which was not true when it landed in a region that scrolled past.
     */
    if (!s_tick_start_ok || !s_tick_clients_ok)
    {
        printf("1 ms tick FAILED (%s) -- RGB fade, buttons and the status line are dead "
               "(console still answers)\n",
               s_tick_start_ok ? "client registry" : "Timer1");
    }

    /*
     * Last, so the help is what the reader is left looking at -- and after the tick line,
     * so a failure is not buried under six lines of grammar. Same order as EV88G73A's
     * profile_report(), which additionally reports a previous trap here; this profile
     * cannot yet (APP_TRAPS_POLICY=2, the next parity item).
     */
#if DEMO_ENABLE_CONSOLE_COMMANDS
    console_task_init();
    console_task_print_help();
#endif
}

void profile_start(void)
{
#if DEMO_ENABLE_I2C_PROBE
    i2c_probe_run(&s_i2c1_probe);
#endif
#if DEMO_ENABLE_TDM_MASTER_LOOPBACK
    demo_tdm_master_loopback_start(&s_tdm_loopback_port);
#endif
#if DEMO_ENABLE_WM8904_AUDIO
    /* Before the first inst_configure(), which is the only state where the phase-3 mode
     * gate is observable -- see wm8904_audio_lifecycle_probe_unconfigured(). Kept in step
     * with ev88g73a/main.c on purpose: this board has no console letters, so an init-time
     * probe is the ONLY transport verification it can carry at all. The gate below is the
     * shared one and defaults to ON; this board has the room, so it never fires here. */
#if WM8904_AUDIO_ENABLE_TDM_DIAG
    (void)wm8904_audio_lifecycle_probe_unconfigured();
#endif

    wm8904_audio_start(&s_wm8904_audio);
#endif
}

/*
 * Pace the loop, and service the console throughout the wait.
 *
 * WHAT THIS USED TO BE: `return true;`, with a comment arguing that the demo rewrites a
 * fixed screen region rather than scrolling, so printing every iteration was intended and
 * there was nothing to block on. Both halves were true of that design and both are gone
 * with it -- there is now a period to wait out and a print to throttle.
 *
 * THE CONSOLE IS POLLED IN HERE, not once per iteration in profile_poll(), and that is the
 * point rather than a detail (parity contract 2.4/2.5). The RX path has no ring buffer --
 * hal_uart/nora_uart_dspic33ck_isr.c is excluded from this configuration, so BOARD_CONSOLE_USE_RX_ISR
 * is 0 -- so characters live in the hardware FIFO until something reads them. Draining only
 * between iterations would let a pasted command overrun that FIFO and arrive as fragments;
 * the wait is otherwise doing nothing. It also means input latency is bounded by this poll,
 * not by DM330030_STATUS_PRINT_PERIOD_MS.
 *
 * RESAMPLED, not advanced by the period: this is a cadence, not an instrument. EV88G73A's
 * equivalent wait does advance, because its LED edge IS its clock measurement and the drift
 * would read as a clock error; nothing here is measured against wall time, so absorbing the
 * overshoot is the simpler and safer choice (no catch-up burst if a print ever overruns).
 *
 * IF THE 1 ms TICK FAILED TO START, GetTicks() stands still and this loop does not exit --
 * deliberately not guarded. The console keeps being polled inside it, so the board still
 * answers ?sr / *sr and profile_report()'s tick-failure line is still on the wire above:
 * "the tick died" stays distinguishable from "the board died". Guarding it would instead
 * spin the demo against a frozen clock and print a status line that never changes.
 */
bool profile_wait_next_tick(void)
{
    static uint32_t status_last_ms;
    uint32_t        wait_start_ms = GetTicks();
    bool            report;

    while ((uint32_t)(GetTicks() - wait_start_ms) < DM330030_POLL_PERIOD_MS)
    {
#if DEMO_ENABLE_CONSOLE_COMMANDS
        console_task_poll();
#endif
    }

    report = ((uint32_t)(GetTicks() - status_last_ms) >= DM330030_STATUS_PRINT_PERIOD_MS);
    if (report)
    {
        status_last_ms = GetTicks();
    }

    return report;
}

void profile_poll(bool report)
{
    /* Actuation on every call, printing only when `report` -- see src/profile_main.h for
     * why that split is a parameter. This board used to discard `report` because it always
     * printed; it now honours it, and the demo is the module that changed shape. */
    demo_rgb_pot_buttons_poll(report);

#if DEMO_ENABLE_I2C_PROBE
    /* Every iteration, so a scope on the bus keeps getting a fresh START/ADDR/NACK to
     * trigger on rather than one burst at boot -- see the note on s_i2c1_probe. */
    i2c_probe_poll(&s_i2c1_probe, report);
#endif

#if DEMO_ENABLE_TDM_MASTER_LOOPBACK
    demo_tdm_master_loopback_poll();
#endif

#if DEMO_ENABLE_WM8904_AUDIO
    /* `report` now, not the hardcoded `true` this passed while the profile printed on every
     * iteration. The shared module's own status_period_ms above still governs -- it is much
     * the longer of the two -- so the observed cadence does not change. */
    wm8904_audio_poll(report);
#endif
}
