#ifndef BOARD_PROFILE_H
#define BOARD_PROFILE_H

/*
 * boards/ev08p02a/board_profile.h -- THIS PROFILE'S ANSWERS, and nothing else.
 *
 * WHY THE FILENAME IS NOT BOARD-PREFIXED, when every other file here is: this is a
 * SEAM name, like uart_platform/console_out.h. Each board directory holds a file called
 * board_profile.h, each configuration puts only its own board directory on the
 * include path (see extra-include-directories in firmware.X/nbproject/configurations.xml),
 * so `#include "board_profile.h"` from app/app_config.h resolves to the selected
 * board's copy. Shared code therefore reads ONE name and never learns a board's.
 *
 * WHAT BELONGS HERE
 * -----------------
 * The questions no hardware answers -- which shared demos this image runs, what the
 * console is capable of, and which policy this profile picked. One value each, with the
 * reason attached.
 *
 * WHAT DOES NOT
 * -------------
 * Pins (ev08p02a_pins.h), rates and clock policy (ev08p02a_board.h), and anything
 * about the PART (that is the HAL's device tables). If a value here would change when
 * the part changed, it is in the wrong file.
 *
 * Ported 2026-08-26 from EV88G73A's board_profile.h, defaults included, for parity
 * on the same MCU family. The seam itself exists (2026-08-05) because EV88G73A used
 * to gate the shared demos with EV88G73A_ENABLE_* in its own main.c while DM330030
 * gated THE SAME shared modules with ENA_* in app/app_config.h -- two vocabularies
 * for one decision, and they collided: a TDM loopback build needed BOTH
 * `-DEV88G73A_ENABLE_TDM_LOOPBACK=1` (so that profile built its port struct and
 * calls) AND `-DENA_TDM_MASTER_LOOPBACK=1` (so the shared module's body was not
 * #if'd away), because that module reads app_config.h directly and cannot see a
 * #define made in main.c. Half-right meant a silent empty object and a link error.
 * Since then there is one name per decision, resolved in each board's own copy of
 * this file, and app_config.h refuses anything it cannot resolve -- which is why
 * this board gets the seam for free rather than needing to invent it.
 */

/*
 * Wrong-include-path guard. If this file is reached while the compile line says a
 * different board, the include directories and the board define disagree -- which would
 * otherwise silently hand one board's profile to another board's code.
 */
#if !defined(DSPIC33CK_BOARD_EV08P02A)
#error "boards/ev08p02a/board_profile.h was included without DSPIC33CK_BOARD_EV08P02A -- the configuration's extra-include-directories and its preprocessor-macros disagree (firmware.X/nbproject/configurations.xml)."
#endif

/* ---- Which shared app/ modules this image RUNS (each -D-overridable) ---- */

/*
 * WM8904 passthrough over TDM8/32-bit -- ON by default on this board, which is the
 * profile it is normally flashed with. Mutually exclusive with the other two below:
 * all three drive SPI1/I2C1 on the same four-plus-two pins, and app_config.h enforces it.
 */
#ifndef DEMO_ENABLE_WM8904_AUDIO
#define DEMO_ENABLE_WM8904_AUDIO 1
#endif

/*
 * Codec-less TDM master exerciser (the FS/alignment and DMA-throughput harness).
 * This is the one that also carries DEMO_TDM_TX_MODE (fixed / ramp / sine) -- that
 * switch stays in the demo, being a property of the demo rather than of this board.
 */
#ifndef DEMO_ENABLE_TDM_MASTER_LOOPBACK
#define DEMO_ENABLE_TDM_MASTER_LOOPBACK 0
#endif

/*
 * Standalone I2C1 probe: with no codec attached a clean NACK proves the module works.
 * Off by default because the codec path above owns the same bus and is on by default.
 */
#ifndef DEMO_ENABLE_I2C_PROBE
#define DEMO_ENABLE_I2C_PROBE 0
#endif

/* ---- Audio level calibration: dB into the chain, dB out of it ---- */

/*
 * PRE / POST gain on the codec path, in TENTHS OF A DECIBEL, signed. Same two names
 * sonora exposes (PRE_GAIN_CODEC_DB / POST_GAIN_CODEC_DB), same meaning, same two apply
 * points -- the _X10 suffix is only because this part has no FPU and the config value has
 * to be an integer the preprocessor can compare.
 *
 * These are the values the image BOOTS with. The console can change either at run time
 * (*ti / *to, read back with ?ti / ?to), so a listening session does not need a rebuild;
 * what is written here is what survives a reset.
 *
 * WHY THEY LIVE IN THE BOARD PROFILE AND NOT IN wm8904_audio.c
 * -----------------------------------------------------------
 * They calibrate THIS board's analog stage, not the audio algorithm. The WM8904's analog
 * input gain here is set low to keep the input circuit's noise out of the converter, so
 * the digital signal arrives 18.7 dB below full scale (measured, section 3 of that doc) --
 * a fact about this board's wiring and its codec register settings. A different board with
 * a different front end wants different numbers, which is the test for belonging here.
 * The AVAS synth's own level is NOT one of these: it is WM8904_AUDIO_AVAS_SHIFT inside
 * wm8904_audio.c, because it calibrates a synthesiser against full scale rather than
 * against a circuit.
 *
 * Must be a multiple of 5 (the table's grid is 0.5 dB) and within +-240 (+-24.0 dB);
 * app_config.h refuses anything else by name rather than snapping it silently. A run-time
 * request in between IS snapped, and the console prints what it realised.
 */

/*
 * PRE: applied to the codec's input samples, ahead of everything. +6.0 dB ships, and THIS
 * is the stage that carries the compensation for the analog attenuation described above --
 * the thing that is too quiet is the INPUT, so the input is where it is lifted.
 *
 * WHY THE COMPENSATION IS HERE AND NOT ON POST (corrected 2026-08-08; it shipped on POST
 * for one commit): PRE is before the mix, so it lifts the codec input and NOTHING else. The
 * AVAS synthesiser is added after it and therefore keeps the level
 * WM8904_AUDIO_AVAS_SHIFT (14) was set to by ear -- the calibration that has passed a
 * listening test twice is untouched, and there is no re-listen owed for it. POST would have
 * lifted both by the same dB, which is a different and larger change than the one asked for.
 *
 * What PRE does move, on purpose: `?tp`'s meter reads the RX buffer, which this stage does
 * not write (it scales into s_chain[]), so the meter keeps answering "is the input small"
 * rather than reporting this gain back. The AVAS gate is SW0, not a level, so it is not
 * affected either.
 */
#ifndef PRE_GAIN_CODEC_DB_X10
#define PRE_GAIN_CODEC_DB_X10   60     /* +6.0 dB */
#endif

/*
 * POST: applied on the way out, after the mix -- so it scales the codec input AND the AVAS
 * synth together, which is what a master output gain means.
 *
 * 0.0 dB ships: exactly the identity, bit for bit (gain_db.h), so the output stage is a
 * no-op today and the only level change in this image is PRE's. Raising this is the knob
 * for "everything is too quiet", and note that it moves the AVAS level too -- attenuating
 * the synth back is then a change to WM8904_AUDIO_AVAS_SHIFT (sonora's separate
 * PRE_GAIN_AVAS_SYNTH_DB), not to this value. Design doc section 5 and section 10.
 */
#ifndef POST_GAIN_CODEC_DB_X10
#define POST_GAIN_CODEC_DB_X10  0      /* +0.0 dB -- identity */
#endif

/* ---- Console: capability first, then what this profile does with it ---- */

/*
 * The on-board debugger's CDC gives this board a BIDIRECTIONAL console: TX on RP58
 * and RX on RP59 (ev08p02a_pins.h). Capability, not choice -- so it is stated as a
 * board fact even though it lives in the profile header, and the pins that make it
 * true are in pins.h.
 */
#define BOARD_CONSOLE_HAS_RX 1

/*
 * POLICY: RX is POLLED, not interrupt-driven. It is polled inside the LED-edge wait,
 * which is what makes the console answer while a status line is throttled or the blink
 * is paused -- see the parity contract, item 2.4. Interrupt RX would buy latency this
 * console does not need, and would add a vector to a build whose ISR budget is measured.
 *
 * Setting this to 1 also requires hal_uart/nora_uart_dspic33ck_isr.c to be IN this
 * configuration's source list; app_config.h refuses the combination rather than letting
 * a "capability" be enabled that nothing implements.
 */
#define BOARD_CONSOLE_USE_RX_ISR 0

/*
 * The interactive command set (uart_app/console_task.*): ?gv ?gh *sr ?sr ?dt ?xl and
 * the forced-trap commands. On, and it is the reason this board can be debugged at all
 * without a debugger attached -- the Nano has no reset button, so *sr is the only
 * software reset and ?sr the only evidence it happened.
 */
#define DEMO_ENABLE_CONSOLE_COMMANDS 1

#endif /* BOARD_PROFILE_H */
