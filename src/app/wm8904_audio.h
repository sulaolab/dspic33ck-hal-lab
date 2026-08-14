#ifndef WM8904_AUDIO_H
#define WM8904_AUDIO_H

/*
 * wm8904_audio.{c,h} -- WM8904 passthrough audio over TDM8/32-bit, for any board.
 *
 * line-in -> WM8904 ADC -> dsPIC (optional mute/gain ramp) -> WM8904 DAC -> headphone
 *
 * WHAT THIS REPLACED
 * ------------------
 * Two files doing the same six-step bring-up:
 *
 *   boards/ev88g73a/ev88g73a_wm8904_audio.c   411 lines
 *   app/demo_wm8904_audio.c                   213 lines, but NOT board-independent
 *                                             despite living in app/: it included
 *                                             <board>_board.h as <board>_mikrobus_a_*,
 *                                             so it only ever compiled for DM330030
 *
 * Measured before merging: of the EV88G73A file's 411 lines, about 45 were board
 * facts -- four RP numbers with their PPS routing, plus one call each to the board's
 * I2C bring-up and its SW0 read. Everything else was application: the transport
 * geometry, which side drives BCLK/FS, the gain-ramp ownership model, what the button
 * means, and the reporting. The two files' bring-up order, TDM8/32-bit settings and
 * SPI polarity trio were identical, and their error messages nearly word for word.
 *
 * So: one implementation here, and a per-board port of at most four function pointers
 * (see wm8904_audio_port_t) whose entire job is pins and electricity.
 *
 * THE SPLIT, STATED AS A RULE
 * ---------------------------
 * The board answers "which pins" -- routing and electricity, nothing else. (This read
 * "which pins, and is there a clock to supply" while wm8904_audio_port_t still had an
 * mclk_init slot to ask that second question with; the slot went on 2026-08-04 and the
 * sentence outlived it. Corrected 2026-08-09.) It is NOT asked
 * which side of the TDM pair is master: that is a design choice this module takes from
 * its config, and it hands the chosen role to the board so the same board port serves
 * either topology. That is the difference from the code this replaced, where the
 * master/slave switch was a #if inside the board's own file and the board rejected the
 * role it had not been compiled for.
 *
 * SINGLE-CODEC, DELIBERATELY
 * --------------------------
 * One codec, one transport, static state. THE REASON CHANGED, and the old reason is
 * worth stating because it is no longer true (corrected 2026-08-09). This used to read:
 * "because the transport HAL underneath is itself a singleton
 * (nora_spi_i2s_tdm_spi1(), one global port and one global block callback). A second
 * instance here would be a lie about what the layer below can do. If SPI2/a second codec
 * ever arrives, the HAL is what has to change first."
 *
 * The HAL has since changed (phases 1-5 of the canonical NORA alignment): it has instance
 * handles, spi1()/spi2()/spi3(), per-instance block callbacks, ownership modes, and a
 * SYSTEM/sync-domain API for co-clocked multi-leg topologies. So the layer below would
 * NOT have to change first, and a second instance here would no longer be a lie about it.
 *
 * The reason now is simply that THIS MODULE IS SCOPED TO ONE CODEC: every board in this
 * tree carries exactly one WM8904, and a single-codec module with static state is the
 * honest shape for that. A second codec is a change HERE (or a second module), not a
 * prerequisite in the HAL -- and per the plan doc's section 15, a CK board with two codecs
 * does not exist, so this is not a deferred plan either.
 */

#include <stdbool.h>
#include <stdint.h>

#include "nora_spi_i2s_tdm.h"   /* nora_spi_i2s_tdm_clock_role_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /*
     * Pin directions and PPS routing for the role this build runs the dsPIC in.
     * REQUIRED. Called by the transport HAL through its own port struct, i.e. at
     * open() time, not at start() time.
     *
     * It is handed the role rather than compiled for one: as slave, BCLK/FS/SDI are
     * inputs and SDO is the only output; as master, BCLK/FS/SDO are outputs and SDI is
     * the only input. Same pins either way on every board so far. Return false for a
     * role the wiring genuinely cannot do (e.g. a board where FS is hard-wired from
     * elsewhere) and the bring-up stops with a message.
     */
    bool (*configure_pins)(nora_spi_i2s_tdm_clock_role_t role);

    /*
     * Bring up the I2C control bus the codec is on: pins, pull-ups, HAL init, at
     * whatever rate this board's wiring is known to work at. REQUIRED.
     *
     * Rate is deliberately the board's business and not a field here. It is the one
     * number in this whole path with contradictory measured evidence across the fleet
     * (400 kHz working where 100 kHz does not), so it belongs where the wiring is
     * described, not where the codec protocol is.
     */
    bool (*i2c_init)(void);

    /*
     * THERE IS NO mclk_init HOOK (removed 2026-08-04). It was
     * `bool (*mclk_init)(void)`, called first because a WM8904 with no SYSCLK does not
     * answer on I2C, and documented as "NULL when the codec self-clocks from its own
     * crystal -- which is EV88G73A's case ... DM330030 must divide one out of REFO1".
     *
     * That distinction does not exist. The WM8904 board is the SAME on both boards and
     * carries its own 12.288 MHz XTAL, and the role decides who drives BCLK/FS, not where
     * SYSCLK comes from. With both boards passing NULL the hook was dead code that also
     * stated a false premise, so it is gone rather than merely unused.
     *
     * TWO CLAIMS, AND ONLY ONE OF THEM IS OURS TO MAKE (separated 2026-08-09, second review
     * pass -- this note used to assert the second as fact):
     *   1. THIS FIRMWARE SUPPLIES NO MCLK, in any build, on either board. Certain, and
     *      provable from the tree: no board defines an MCLK pin, `tcfg.mclk_enable` is false
     *      in both clock roles, and that field is a BRG source select rather than an output
     *      anyway (see the note at the tcfg.mclk_enable assignment in the .c).
     *   2. WHERE THE CODEC'S SYSCLK PHYSICALLY COMES FROM in each jumper position. A BOARD
     *      fact, not ours -- this repo does not set the A jumper and cannot read it. It is
     *      now known, from the person who wired the board (2026-08-09): the jumper selects
     *      the WM8904's MCLK INPUT, between (A-XTAL) the CODEC-A board's own XTAL, which is
     *      the codec-TDM-master position, and (A-extMCLK) a BCLK arriving from outside, which
     *      is the codec-TDM-slave position. So at A-extMCLK the codec's SYSCLK is derived
     *      from OUR SPI BRG's BCLK -- see the consequence note below.
     * So: this module needs nothing from claim 2 to do its job. What it needs is only that IT
     * HAS NO CLOCK-GENERATION STAGE TO RUN -- there is no MCLK for it to set up, in any build.
     *
     * WHY THE WORDING IS STILL "NO DEDICATED MCLK STAGE" AND NOT "NOT US" (2026-08-09, third
     * review pass, and the answer above is why it was right to refuse). This note once read
     * "arrives ALREADY CLOCKED, by something that is not us". At A-extMCLK that is false:
     * the clock the codec runs on is one this firmware generates. Claim 1 survives intact --
     * we output no MCLK -- but only because the codec, not us, is the thing routing a BCLK
     * into its own MCLK input. "Already clocked" was wrong for a second reason, now positively
     * disproved rather than merely unprovable: at A-extMCLK there is no MCLK at all while
     * wm8904_init() runs (step 2, before step 3 opens the transport and BCLK starts), yet the
     * codec read its device ID back and verified every register write (plan doc section 19).
     * So this tree's old premise "a WM8904 with no SYSCLK does not answer on I2C" -- the reason
     * the deleted MCLK step had to come first -- does not hold for register I/O.
     *
     * CONSEQUENCE FOR RATE SELECTION AT A-extMCLK, reasoned from the driver and NOT measured:
     * there SYSCLK = MCLK = BCLK = 256 x fs by construction (TDM8 x 32 bit), so only wm8904.c
     * rate rows whose CLK_SYS_RATE is 256 are ratio-correct -- which the default 48 000 row is,
     * and it is the row section 19 ran. Calling wm8904_set_rate_hz() for another rate at that
     * jumper position would program a SYSCLK/fs the hardware does not have (16 000 asks for
     * 768 while the true ratio stays 256). Nothing calls it in this profile today.
     *
     * IF A BOARD EVER NEEDS THE MCU TO SOURCE ONE, do not restore this shape. The fleet
     * already has the right one: dspic33ak-audio-dsp-sonora's board/audio/audio.c
     * board_route_mclk() ROUTES an existing clock (CLC passthrough of a BCLK or of a
     * dedicated MCLK net) and deliberately routes NOTHING in the codec-master case, and
     * it is keyed on board/compile facts rather than on any leg's role. A generator hook
     * invites exactly the defect deleted here: a second clock, at a frequency the
     * driver's rate tables do not use, contending with the codec's own crystal.
     */

    /*
     * Momentary input that toggles the digital mute ramp, sampled once per poll.
     * NULL when the board has no button, which also switches the block callback to a
     * plain copy (see gain_ramp_ms).
     *
     * Active level is already resolved: true means pressed. Debouncing is not needed
     * at the rates this is polled (see the note in the .c file).
     */
    bool (*mute_button_pressed)(void);

    /*
     * One line naming the wiring, printed at bring-up. A report that does not say
     * which pins and which clock arrangement it drove is a report nobody can act on --
     * the same reason i2c_probe_t carries `where`.
     */
    const char *wiring;
} wm8904_audio_port_t;

typedef struct {
    const wm8904_audio_port_t *port;

    /* The 1-based instance number chip_drivers/wm8904.c maps to an I2C HAL instance.
     * 1 on both current boards; a field because it is wm8904.c's convention, not a
     * fact about this module. */
    uint8_t i2c_inst_legacy;

    /*
     * Which side drives BCLK/FS/LRCLK. false = the WM8904 does (dsPIC transport is
     * SLAVE); true = the dsPIC does (codec is SLAVE). THIS FIELD IS ABOUT CLOCK DRIVING
     * ONLY; it says nothing about where the codec's SYSCLK comes from, and nothing here
     * needs to. (This said "still using its own crystal", then "still SYSCLK'd by something
     * other than us" -- both are board claims this repo does not set, read or measure, and
     * the second contradicted the mclk_init note above, which is where the reasoning is.)
     *
     * The codec is configured with the opposite of this, in one place, so the two
     * cannot disagree.
     */
    bool dspic_is_master;

    /* Baud-rate generator divisor for the self-clocked master case; ignored as slave.
     * BCLK = Fp / (2 * (brg + 1)). */
    uint16_t brg;

    /* Sample rate the gain ramp converts milliseconds with. Must match what the codec
     * was configured for (wm8904.c's own default is 48 kHz). */
    uint32_t sample_rate_hz;

    /*
     * Mute/unmute ramp length. 0 -- or a port with no mute_button_pressed -- selects a
     * plain copy block callback with no gain stage at all, which is what a board
     * without a button had before this merge. Non-zero costs one Q31 multiply per
     * sample in the block ISR.
     */
    uint16_t gain_ramp_ms;

    /*
     * Status-line throttle, in MILLISECONDS of running time (app/timer_app.h's
     * GetTicks()). 0 = print on every report the caller allows. The first report after
     * wm8904_audio_start() always prints, whatever this says -- see due_ms() in the .c.
     *
     * THESE TWO USED TO BE COUNTS OF `report` CALLS, named *_every_n_reports, and that
     * is why they needed the paragraph that used to be here explaining that the two
     * boards' main loops differ by three orders of magnitude: the same number meant ~2 s
     * on EV88G73A and a few screen repaints on DM330030, so neither value could be read
     * without knowing which loop it was going to be counted in. In milliseconds the
     * number means what it says on both, and it stays right when a loop changes pace.
     */
    uint32_t status_period_ms;

    /*
     * How often to explain that nothing started, in milliseconds. 0 = stay silent (the
     * one thing a period of 0 does NOT mean here is "as often as possible" -- silence is
     * the useful answer for a board whose console is a fixed ANSI screen region).
     *
     * Worth a knob rather than always-on: with no codec attached the bring-up aborts,
     * and a poll that then prints nothing leaves a reader unable to tell "the codec is
     * not wired" from "the load display is broken". That ambiguity has cost real time
     * on this repo.
     */
    uint32_t idle_report_period_ms;
} wm8904_audio_config_t;

/*
 * Bring up I2C -> codec -> transport, verify it is running, then
 * unmute. Every stage reports its own failure through console_out and returns without
 * unmuting; nothing here continues past a codec that did not confirm its ID.
 *
 * There is no clock-generation stage in front of the I2C one, because THIS FIRMWARE HAS NO
 * MCLK TO GENERATE -- not on either board, not in either TDM role. How the codec's SYSCLK is
 * physically supplied is a board/jumper fact this module neither sets nor reads. (This said
 * "the codec arrives already clocked ... and never by us" until 2026-08-09, and before that
 * "clocked from the WM8904 board's own crystal"; both claimed more than the tree knows -- see
 * the note above where the mclk_init hook used to be, and plan doc section 20.4.)
 *
 * `cfg` is stored by pointer and must outlive the call sequence, so a static const in
 * the board's profile costs no RAM.
 */
void wm8904_audio_start(const wm8904_audio_config_t *cfg);

/*
 * Make the running audio path quiet for an intentional runtime stop.  It first applies the
 * codec's analog headphone mute, then disables the TDM instance and its DMA interrupt, and
 * finally closes the transport's shared port.  That order is for the flashing workflow --
 * UART traffic while a drag-and-drop programmer resets the target must not be decoded as
 * audible TDM samples.
 *
 * The call is idempotent, and it is a STOP: it does not resume, and nothing here brings the
 * path back.  What comes back is a FULL bring-up -- a boot, or wm8904_audio_restart() below,
 * which runs the same complete sequence rather than resuming a half-torn-down path.  (Before
 * *tr existed this comment said callers "must not attempt to restart this path in place",
 * which is still true of doing it by hand and is exactly why the restart is one function.)
 */
void wm8904_audio_stop(void);

/*
 * `*tr` -- stop, then run the complete bring-up again, with no reset.
 *
 * Uses the config the board passed to wm8904_audio_start(), so it needs at least one prior
 * start (returns false, with a line, otherwise). Every profile default is re-established:
 * PRE/POST gain, path, mute request and the AVAS gate, which is what makes *ts -> *tr the
 * documented way to undo a session of tuning by ear.
 *
 * It is also the only way to walk the transport's close() -> open() -> inst_start() lifecycle
 * on hardware without a reset -- see wm8904_audio_lifecycle_probe().
 *
 * Returns whether audio is running afterwards; each failing stage prints its own reason.
 */
bool wm8904_audio_restart(void);

/*
 * WM8904_AUDIO_ENABLE_TDM_DIAG -- does this image carry the transport's DIAGNOSTIC commands?
 * DEFAULT 1: it does. It is turned off to buy program memory, and nothing else.
 *
 * WHAT IS GATED, and why these three and not others:
 *
 *   ?ta   wm8904_audio_avas_bench() -- times the AVAS engine in the foreground. A BENCH: it
 *         answers "what would this cost in the block ISR", which is a question asked while
 *         choosing a voice, not while running one. The periodic TDM1 load line (*tq/?tq)
 *         answers the operational version of the same question and is NOT gated -- see below.
 *   *tb   the AVAS parts mask, all four entry points. A listening tool ("switch the envelope
 *         off and hear what the 185 lines contribute"); the boot default is set through the
 *         engine's own set_parts(), not through this, so removing it changes no sound.
 *   *tl   wm8904_audio_lifecycle_probe() AND wm8904_audio_lifecycle_probe_unconfigured(),
 *         with the probe_*() helpers they share. These certify the transport's lifecycle
 *         GATES, which is a property of the HAL under test rather than of the audio: once
 *         a build has been through them on hardware, carrying them costs ROM to re-prove
 *         what the previous image already proved about the same HAL.
 *
 * WHAT IS DELIBERATELY NOT GATED, because an image without it cannot be judged:
 *
 *   *tq/?tq  the periodic TDM1 load line. It is the ONLY evidence of headroom and of
 *            miss == 0, and the voices in this tree are chosen against exactly those two
 *            numbers -- an image that makes a sound nobody can measure is not cheaper, it
 *            is unverifiable. Cutting it would also be self-defeating in the one build that
 *            needs the space: two voices at run time are what most need watching.
 *   *ts/?ts  the pre-flash analog-mute gate, and its `analog mute verified` phrase, which
 *            buildtools/README.md's flashing procedure gates on. A safety interlock.
 *   ?tp      the current path and what CODEC-IN carries -- the one report that says whether
 *            samples are arriving at all, i.e. the first thing consulted when there is no
 *            sound. Silence with no ?tp is indistinguishable from a dead codec.
 *
 * ONE MACRO for code, declarations and strings together, for the reason the SYSTEM_PROBE
 * macro below records: --gc-sections runs with -ffunction-sections but NOT -fdata-sections,
 * so removing only the dispatch would discard the functions and leave every string they
 * print linked. The letters become ERR_NOT_FOUND like any unknown letter rather than
 * printing a "not built" reply, since that reply is a string and a string is the thing this
 * gate exists to buy back.
 *
 * DEFAULTED FROM AVAS_CK_VOICE_BOTH, which is the honest statement of why it exists: both
 * voices in one image do not fit a 64 KB part alongside these commands (measured: the
 * BOTH=1 link was 930 B short with them in), so `-Define AVAS_CK_VOICE_BOTH=1` alone
 * selects a diag-less image rather than failing to link. Only the command line / xml form
 * is seen here on purpose -- this header must not include the DSP engine's header to read a
 * default, and a hand-edited default inside avas_synth_line_ck.h would therefore be invisible
 * to it. wm8904_audio.c sees BOTH headers and #errors on that combination, so the hole is
 * closed where it can be seen rather than left to the linker to report as 82 unplaced
 * sections. Pass -Define WM8904_AUDIO_ENABLE_TDM_DIAG=1 with BOTH=1 and it refuses there too.
 */
#ifndef WM8904_AUDIO_ENABLE_TDM_DIAG
#if defined(AVAS_CK_VOICE_BOTH) && (AVAS_CK_VOICE_BOTH)
#define WM8904_AUDIO_ENABLE_TDM_DIAG 0
#else
#define WM8904_AUDIO_ENABLE_TDM_DIAG 1
#endif
#endif
#if (WM8904_AUDIO_ENABLE_TDM_DIAG != 0) && (WM8904_AUDIO_ENABLE_TDM_DIAG != 1)
#error "WM8904_AUDIO_ENABLE_TDM_DIAG must be 0 or 1"
#endif

#if WM8904_AUDIO_ENABLE_TDM_DIAG

/*
 * `*tl` -- attempt the four lifecycle calls that must be REFUSED in the current state, and
 * report each verdict (returns true when all four behaved as specified).
 *
 * RUN IT TWICE -- once while streaming, once after *ts -- because the expectations are
 * state-dependent and each state includes a call that must SUCCEED. That is the point: a
 * bool that is false in every state is no more a gate than one that is always true, and
 * "always true" was exactly what phase 1 of the NORA alignment could not rule out on
 * hardware. Chosen so that a missing gate is non-destructive; the one exception (inst_start
 * while stopped, which would really arm the stream behind the codec mute) is reported.
 */
bool wm8904_audio_lifecycle_probe(void);

/*
 * The third entry point, and NOT a console command: the board calls it once during init,
 * immediately before wm8904_audio_start().
 *
 * It probes the only state in which phase 3's configure-ownership gate is observable --
 * mode == NONE, i.e. before the FIRST inst_configure(). That state cannot be reached again
 * afterwards, because close() deliberately does not reset the mode, so there is nothing for
 * a `*t?` letter to attach to. Three calls: inst_start() and inst_stop() must be refused
 * with ERR_CONFIG_MODE, and inst_configure(NULL) must be refused with ERR_BAD_ARGUMENT --
 * the negative control, since NONE is the legal way INTO SINGLE.
 *
 * The inst_start() line is the one that pays: *tl answers ERR_NOT_OPEN to that same call in
 * its stopped state, so the pair is direct evidence that the mode check precedes the opened
 * check -- the deliberate asymmetry with inst_configure(), which keeps ERR_ALREADY_OPEN
 * first.
 *
 * Non-destructive with a missing gate (it would answer NOT_OPEN and arm nothing), and it
 * leaves the transport untouched for the start() that follows.
 */
bool wm8904_audio_lifecycle_probe_unconfigured(void);

#endif /* WM8904_AUDIO_ENABLE_TDM_DIAG */

/*
 * WM8904_AUDIO_USE_SYSTEM_START -- which configure-ownership mode this board's shipping
 * bring-up uses. DEFAULT 0: the shipping consumer stays on SINGLE, with the same calls in the
 * same order it has always used. It is a MEASUREMENT build, not a feature:
 *
 *   0  inst_configure()   -> open() -> inst_start()          stop: inst_stop() -> close()
 *   1  configure_system() -> open() -> start_all_domains()   stop: stop_all_domains() -> close()
 *
 * Set 1 to measure phase 4's ONE-LEG EQUIVALENCE -- the SYSTEM route must reproduce the SINGLE
 * route's ?ta / ?tq load, ?tp slot mask and miss count on the same hardware from the same
 * transport config, which is built once and shared by both routes so a difference in the
 * numbers cannot be a difference in the framing.
 *
 * The stop side switches WITH the start side and must: a SYSTEM-committed stream refuses
 * inst_stop() with ERR_CONFIG_MODE, so a half-switched build would have a *ts that silently
 * stops nothing. What this does NOT measure is two legs latching one FS edge -- only one leg
 * is built on either board (USE_SPI2 == 0).
 */
#ifndef WM8904_AUDIO_USE_SYSTEM_START
#define WM8904_AUDIO_USE_SYSTEM_START 0
#endif
#if (WM8904_AUDIO_USE_SYSTEM_START != 0) && (WM8904_AUDIO_USE_SYSTEM_START != 1)
#error "WM8904_AUDIO_USE_SYSTEM_START must be 0 or 1"
#endif

/*
 * WM8904_AUDIO_ENABLE_SYSTEM_PROBE -- does this image carry `*tm`? DEFAULT 0: it does NOT.
 *
 * `*tm` is a LAB command, and the argument is not primarily its size. It is that the thing it
 * measures IS NOT IN A SHIPPING IMAGE EITHER WAY. Both configs link with
 * remove-unused-sections and no shipping consumer calls the domain API, so in a =0 build the
 * linker discards configure_system(), start_domain(), stop_domain(), start_all_domains() and
 * stop_all_domains() entirely -- measured:
 * the phase-4 HAL with no caller cost +126 B, and +4,413 B once `*tm` gave it one. Gating the
 * probe out therefore does not ship untested code; it ships an image in which the code under
 * test is absent. A diagnostic for functions that are not linked in diagnoses nothing.
 *
 * The second reason is that in a shipping image `*tm` is a HAZARD rather than an asset. Run
 * from the stopped state it commits SYSTEM ownership, which is a ONE-WAY COMMIT (see the
 * comment on the probe below): after it, *tr answers CFG_MODE and *tl refuses, so audio cannot
 * be restarted without a reset. On a board in someone's hands that is a mistyped letter away.
 *
 * Set 1 for the lab image, and BUILD IT WITH THE SAME OPTIMISATION AND VOICE SETTINGS AS THE
 * SHIPPING IMAGE -- lab = production + this one macro. Otherwise the 5/5 and 14/14 verdicts
 * certify a different code generation than the one that ships, and the probe's whole value is
 * that it measures the real transport rather than a plausible one.
 *
 * What is gated: the `m` console dispatch in the board file, the declarations here, the probe
 * body, the SYSTEM-held flag *tl consults, and every string only they print. All under this
 * one macro on purpose -- deleting just the dispatch would leave the probe's .const alive,
 * because --gc-sections runs with -ffunction-sections but NOT -fdata-sections, so unreferenced
 * const inside a translation unit that is still linked is not reclaimed.
 *
 * NOT gated BY THIS MACRO, and must not be: `*tl` and `*tl(virgin)`, whose subject is the
 * inst_* family that the shipping consumer really does call, and the shared probe_*() output
 * helpers they use. That reasoning stands and is unchanged -- what it argues is that *tl does
 * not follow *tm, because the two measure different things. It is NOT a claim that *tl can
 * never be removed: WM8904_AUDIO_ENABLE_TDM_DIAG=0 does remove it, for a different reason
 * (program memory, when two voices must share a 64 KB part) and by a knob of its own. The
 * probe_*() helpers are shared, so they are compiled when EITHER macro wants them.
 */
#ifndef WM8904_AUDIO_ENABLE_SYSTEM_PROBE
#define WM8904_AUDIO_ENABLE_SYSTEM_PROBE 0
#endif
#if (WM8904_AUDIO_ENABLE_SYSTEM_PROBE != 0) && (WM8904_AUDIO_ENABLE_SYSTEM_PROBE != 1)
#error "WM8904_AUDIO_ENABLE_SYSTEM_PROBE must be 0 or 1"
#endif

#if WM8904_AUDIO_ENABLE_SYSTEM_PROBE

/* Upper bound on legs *tm builds a setup array for. This build has one (USE_SPI2 == 0); the
 * probe refuses rather than overrun if a future build exceeds it. */
#define WM8904_AUDIO_TM_MAX_LEGS 4u

/*
 * `*tm` -- exercise phase 4's SYSTEM / sync-domain API, and the reason that API is not dead
 * code: both build configs carry remove-unused-sections, no shipping consumer calls the domain
 * functions, so with no caller the linker discards all five and the phase's ROM cost reads as
 * about zero while the firmware ships code that has never executed once.
 *
 * DEFAULT BUILD ONLY: with WM8904_AUDIO_USE_SYSTEM_START=1 it refuses at compile time. That
 * build already ships SYSTEM, so the domain calls below are legal in it -- measured on hardware
 * reporting four false "gate defects" while genuinely stopping the live transport under a line
 * claiming the audio was untouched. The =1 config exists to measure NUMBERS; the gates are
 * measured in the default config.
 *
 * State-dependent like *tl, and it refuses any state but these two:
 *   RUNNING          -- 5 rejections, all returning before any register write, so the audio
 *                       is untouched. configure_system() must answer ALREADY_OPEN and NOT
 *                       CFG_MODE: it deliberately does not look at the mode, because it is
 *                       the way OUT of SINGLE ownership.
 *   STOPPED (closed) -- the 14-step sequence, which really commits SYSTEM, opens, and runs
 *                       start_all_domains()'s arm-all-then-go startup. Silent, because *ts
 *                       left the codec analog-muted.
 *
 * A STOPPED-STATE *tm ENDS THE SESSION: SYSTEM ownership is a ONE-WAY COMMIT. inst_configure()
 * is the only call that commits SINGLE and it refuses under SYSTEM (one of the 14 checks proves
 * it), configure_system() only ever commits SYSTEM, and close() deliberately does not touch the
 * mode. So the probe closes the port, says in those words that *sr is required, and leaves the
 * mode held. Until the reset *tr answers CFG_MODE (its bring-up calls inst_configure) and *tl
 * refuses, so AUDIO CANNOT BE RESTARTED; *ts still answers OK because it only re-asserts the
 * codec mute over an already-stopped transport. Run it last, or from the running state where it
 * changes nothing.

 */
bool wm8904_audio_system_probe(void);

/* True once *tm has committed SYSTEM ownership; never cleared, because nothing undoes it. Used by
 * *tl, whose expectations all assume SINGLE, to refuse in one line instead of growing a second
 * set of expected codes for a mode it was never about. */
bool wm8904_audio_system_mode_held(void);

#endif /* WM8904_AUDIO_ENABLE_SYSTEM_PROBE */

/* Print the state used by `?ts`, without changing the audio or transport state. */
void wm8904_audio_stop_report(void);

/*
 * Once per main-loop iteration. The mute button is sampled on EVERY call regardless of
 * `report` -- button responsiveness must not follow the print cadence -- while the
 * status line and the not-started line are gated by `report` and then by their
 * counters above.
 */
void wm8904_audio_poll(bool report);

/*
 * Consume the latest raw RX observation in MAIN context and update the CODEC-IN meter
 * shown by ?tp. The DMA callback publishes only one rotating frame's top words; this
 * function does the peak and slot-mask arithmetic after the callback has returned.
 *
 * Call it about once per millisecond from a board's wait loop when one exists. It is
 * idempotent between new observations, and wm8904_audio_poll() calls it as a slower
 * fallback for boards without a fast wait loop. Never call it from the DMA callback.
 */
void wm8904_audio_rx_observe_poll(void);

/*
 * The mute button alone, so its sampling rate is the BOARD's choice and not a consequence
 * of how long a main-loop iteration happens to take.
 *
 * WHY THIS EXISTS AS A SEPARATE CALL. wm8904_audio_poll() above already samples the button
 * on every call, and that was believed to be enough. It is not: EV88G73A's main loop spends
 * its whole 500 ms LED state inside one wait, so poll() ran at 2 Hz and a deliberate
 * 100-300 ms tap was simply not sampled -- the button answered only when held past half a
 * second, which reads as a broken or laggy switch rather than as a cadence problem.
 *
 * Call this from wherever the board waits, at 100 ms or so: fast enough that a real tap
 * cannot fall between two samples, slow enough that release bounce cannot read as a second
 * press. It is one GPIO read plus an edge test and prints only on a press, so the extra
 * calls cost nothing measurable and nothing on the console.
 *
 * SAFE TO CALL FROM BOTH PLACES, and poll() still does: a board that has no fast wait loop
 * to hang this on keeps exactly the behaviour it had, instead of losing the button to a
 * refactor it did not participate in.
 */
void wm8904_audio_button_poll(void);

/* True once the transport is running and the codec is unmuted. */
bool wm8904_audio_started(void);

/*
 * WHAT THE BLOCK CALLBACK DOES, AT RUNTIME. A bring-up starts in `chain`, which is what this
 * board does in normal operation; the four single-purpose positions are one *tp away and are
 * kept unchanged, because they are the documented cost baselines
 * (docs/ck_silicon_findings.md Part 5) and a baseline that changes stops being one.
 *
 * wm8904_audio_path_next() advances one position in a FOUR-position cycle and names where
 * it landed:
 *
 *   chain -> gain -> copy -> mute -> chain ...
 *
 *   chain TDMin -> AVAS -> Gain -> TDMout over one int32 buffer: the two slots the WM8904
 *         converts are decoded, the synth is MIXED in when started, the SW0 mute ramp is
 *         applied, and the result is encoded back. The rest of the frame passes through
 *         verbatim. With the synth stopped the AVAS stage is skipped entirely, so this
 *         position costs the copy baseline plus two slots of arithmetic.
 *   gain  CODEC-IN to CODEC-OUT through the SW0 mute ramp -- one Q15 scale per slot, the
 *         same cost on every slot of every block. Offered only on a board that has a button
 *         and a ramp time, and it says so when it does not.
 *   copy  CODEC-IN copied to CODEC-OUT verbatim, nothing of ours in between: the baseline
 *         the gain path's cost is measured against
 *   mute  digital zeros out (does the output survive us sending nothing?)
 *
 * wm8904_audio_path_set() selects a position by INDEX -- 0 copy, 1 mute, 2 tone, 3 gain,
 * 4 chain -- and returns false, having said why, for an index that does not exist or a gain
 * position this build cannot offer.
 *
 *   tone  a dsPIC-generated 1 kHz sine out, CODEC-IN ignored (is the dsPIC->codec
 *         direction -- slot alignment, encode, DAC path -- sound?). REACHABLE BY INDEX
 *         ONLY, deliberately: it is the one position that drives the analog output with
 *         something loud and synthetic, and arriving there by walking a cycle one keystroke
 *         too far, into headphones, is a different class of surprise from arriving at a copy
 *         or a silence. A bare wm8904_audio_path_next() from tone returns to the cycle's
 *         first position.
 *
 * These positions exist so that "the analog output carries something that is not the input"
 * can be split into a direction WITHOUT one flash per hypothesis.
 *
 * wm8904_audio_path_report() prints the current position plus what CODEC-IN carries: a
 * sampled peak level for slots 0/1 (0..32767, peak-hold, cleared by the call) and a
 * sampled per-slot activity mask. Silence and arriving-in-the-wrong-slot look identical
 * from the analog side and need different fixes; the mask is what tells them apart.
 *
 * All three print through console_out and are safe before the transport is running.
 */
void wm8904_audio_path_next(void);
bool wm8904_audio_path_set(uint16_t index);


/*
 * The AVAS synth, which is a STAGE OF THE CHAIN and not a path of its own.
 *
 * It used to be a path, and the two things that cost were both consequences of that.
 * Stopping it switched the path away in the same call, so the block ISR stopped calling
 * the engine and its ~3 s release fade never reached the speaker (an abrupt mute); and the
 * gate then froze part-way, which skipped both the phase reset and the 4 s attack on the
 * next start. As a stage, "stopped" is just the chain without its contribution, so the
 * fade renders because nothing stops calling it -- which is how sonora has always been
 * shaped (fx_domain_48k.c calls its AVAS source unconditionally) and why the same time
 * constants sound right there.
 *
 *   wm8904_audio_avas_bench()       ?ta -- time the engine in the FOREGROUND, where it
 *                                   cannot overrun anything and emits no audio. This is
 *                                   the only safe way to learn the cost: the load monitor
 *                                   cannot report an overrun, because the code that prints
 *                                   it is what stops running. Refuses while the synth is
 *                                   sounding -- fade included -- since both would then be
 *                                   advancing the same oscillators.
 *   wm8904_audio_avas_enable_set()  `a` / *ta0001 / *ta0000 -- START and STOP the synth.
 *                                   Start selects the chain path if we are not on it and
 *                                   prints the overrun warning first; stop leaves the path
 *                                   alone so the fade can be heard.
 *
 * ?ta is gated by WM8904_AUDIO_ENABLE_TDM_DIAG; the start/stop entry point is not.
 */
#if WM8904_AUDIO_ENABLE_TDM_DIAG
void wm8904_audio_avas_bench(void);
#endif
void wm8904_audio_avas_enable_set(bool on);

/*
 * Has the synth been STARTED -- the latch, not the gate.
 *
 * Exists for the 'a' hotkey, so that a toggle has ONE source for "is it on" instead of a
 * flag kept beside the key -- two states that can disagree, and the disagreement shows up
 * as a keypress that appears to do nothing.
 *
 * Deliberately false during the release fade, even though the engine is still rendering:
 * pressing `a` there means "start again", which is a supported path (it resumes without a
 * phase reset, because a phase jump mid-fade is a click). ?tp reports the fade as its own
 * state; sonora keeps the same split for the same reason (classic_controls.c).
 */
bool wm8904_audio_avas_enable_on(void);

/*
 * WHICH VOICE -- Type_TY L1 or Type_LB L3, run-time exclusive.
 *
 * One engine renders both: what differs is a coefficient-set descriptor
 * (avas_line_ck_set_t), which is why a second voice costs flash and no per-sample code.
 * An image built with -DAVAS_CK_VOICE_BOTH=1 holds both and switches between them;
 * without it there is one voice and asking for the other prints why it cannot.
 *
 *   wm8904_audio_avas_voice_key()   `a` = type_ty, `A` = type_lb -- SELECT AND START, or
 *                                   stop if it is the one already sounding, or refuse
 *                                   if the other one is. sonora's rule and sonora's
 *                                   keys, deliberately: the two boards are listened to
 *                                   in one session and a different key on each is a
 *                                   mistake waiting for the moment your attention is on
 *                                   the sound. One entry point for both keys so they
 *                                   cannot drift apart.
 *   wm8904_audio_avas_voice_set()   *tv0000 / *tv0001 -- the same selection without the
 *                                   start, for anything scripted. Refuses while the
 *                                   engine still owns its oscillators, fade included:
 *                                   a switch re-seeds all of them from the new voice's
 *                                   measured t = 0 phases, and mid-sound that is a click.
 *   wm8904_audio_avas_voice()       Which voice is loaded NOW, read from the descriptor
 *                                   the engine is using rather than a remembered index.
 *   wm8904_audio_avas_voice_report() ?tv -- current voice, whether it is sounding, and
 *                                   which voices this image actually contains.
 */
/* Restated here rather than pulled from the engine's header: a board file binding a key
 * has no business including the DSP module, and this is the whole of what it needs to
 * know. The two definitions are checked against each other where they meet
 * (wm8904_audio.c), so a rename cannot leave them silently swapped. */
#define WM8904_AUDIO_AVAS_VOICE_TYPE_TY    (0u)
#define WM8904_AUDIO_AVAS_VOICE_TYPE_LB   (1u)

void    wm8904_audio_avas_voice_key(uint8_t voice);
bool    wm8904_audio_avas_voice_set(uint8_t voice);
uint8_t wm8904_audio_avas_voice(void);
void    wm8904_audio_avas_voice_report(void);

/*
 * *tb<mask> / ?tb -- which parts of the engine run (AVAS_TYPE_TY_CK_PART_*).
 *
 * The whole engine ONCE measured 115 % of the block period and could not be run at
 * all. It now fits with margin and every part is on by default; the 115 % was
 * generated code rather than the algorithm. What the mask is still for: dropping the
 * envelope rebuild leaves the clusters as pure tones at their centroid frequencies
 * -- thin, which is exactly what the AK work found when it tried a harmonic grid --
 * so switching it is how the 185 lines' contribution is heard, and the per-part split
 * is what ?ta attributes the cost with.
 *
 * Clearing the GATE bit pins the gate open, so a stop is immediate rather than a
 * ~3 s fade. See avas_line_ck_is_active(), which has to special-case it.
 *
 * All four entry points are gated by WM8904_AUDIO_ENABLE_TDM_DIAG. The BOOT mask is not
 * set through them -- the engine's own init/set_parts does that -- so an image without
 * them sounds identical, it just cannot be re-masked from the console.
 */
#if WM8904_AUDIO_ENABLE_TDM_DIAG
void wm8904_audio_avas_parts_set(uint8_t parts);
void wm8904_audio_avas_parts_report(void);

/*
 * The console's usage line and bound for *tb, owned HERE rather than at the call site.
 *
 * The board profile deliberately does not include the engine header (a DSP dependency
 * in a board profile), so it used to spell the mask out itself -- and that copy went
 * stale the moment NOISE was added: it listed "1=carriers 2=envelope 4=gate", offered
 * "*tb0007 = all", and REJECTED 000f as out of range. The help lying was cosmetic; the
 * bound rejecting a legal mask was not. Both now come from the module that knows the
 * mask, so adding a bit cannot leave a stale copy behind.
 */
void wm8904_audio_avas_parts_usage(void);
bool wm8904_audio_avas_parts_valid(uint16_t value);

#endif /* WM8904_AUDIO_ENABLE_TDM_DIAG */

void wm8904_audio_path_report(void);

/*
 * PRE / POST GAIN -- the chain's dB calibration, in TENTHS OF A DECIBEL, signed.
 *
 * PRE scales CODEC-IN before the AVAS stage sees it; POST scales the block on its way out,
 * after the mix. The boot values come from the profile (PRE_/POST_GAIN_CODEC_DB_X10) and are
 * re-read by every wm8904_audio_start(), so a session of tuning by ear is undone by *ts then *tr
 * rather than by remembering what it started at. WHY dB at all, and why a table rather than
 * a powf: see src/app/dsp/gain_db.h -- including the caveat that this restores LEVEL and not
 * signal-to-noise ratio.
 *
 * ONLY THE CHAIN PATH APPLIES THEM. copy/mute/tone/gain are the diagnostic baselines the
 * chain's cost and behaviour are measured against, and a calibration quietly inside `copy`
 * would move the baseline itself. The report says so when a non-chain path is selected.
 *
 * SET, not stepped, for the reason *tq is: an explicit value is idempotent, so a repeated
 * or half-echoed command cannot leave the level somewhere neither side intended. A request
 * off the table's 0.5 dB grid is SNAPPED and the realised value is printed; a request
 * outside +-24.0 dB is REFUSED and returns false, printing the range. Both functions do
 * their own printing, so a caller adds no message of its own.
 */
bool wm8904_audio_pre_gain_set(int16_t db_x10);
bool wm8904_audio_post_gain_set(int16_t db_x10);

/*
 * The value in force plus the number of samples that CLAMPED since the last read, which is
 * the one thing a boost can do wrong: an overflow here is a sign flip, not a loud sample,
 * so it is counted in the block ISR and reported rather than left to be heard. Cleared on
 * read, so a count always belongs to a known interval.
 */
void wm8904_audio_pre_gain_report(void);
void wm8904_audio_post_gain_report(void);

/*
 * The periodic TDM1 load line, on or off. ON after a bring-up: it is the only continuous
 * evidence that blocks are still arriving and that none were missed.
 *
 * SET, not toggled, because the console form is `*tq0000` / `*tq0001` (fleet grammar:
 * kind + module + name + hex payload, as in sonora's `*ar0001`). An explicit value is
 * idempotent -- a repeated command, or one whose echo was missed, cannot leave the board
 * in the opposite state to the one asked for, which a toggle can.
 *
 * What this does NOT switch off is the load figures themselves: the periodic line keeps
 * `max=/(%)/margin=` and omits `last=/deadline=`. Those two are the confusing part in a
 * repeating log and are available one line at a time from the report below.
 */
void wm8904_audio_load_line_set(bool on);

/* Whether that line is currently on, so a '?' can report it without changing it. */
bool wm8904_audio_load_line_on(void);

/*
 * `?tq`: says whether the periodic line is on, then prints exactly ONE full line
 * INCLUDING last=/deadline=. The deadline is the foreground-window average; per-block
 * jitter is intentionally not collected in the audio callback. Works with the periodic
 * line off -- that is what makes silencing it cheap rather than blinding.
 */
void wm8904_audio_load_line_report(void);

#ifdef __cplusplus
}
#endif

#endif /* WM8904_AUDIO_H */
