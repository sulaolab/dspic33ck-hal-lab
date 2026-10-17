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

/* Rev4/Rev5 XTAL_OUT -> RB0/OSCI and MPQ6612A in mikroBUS socket 3. The normal
 * FRC/WM8904-only image stays the default. Enable with -Define
 * EV08P02A_PWM_AUDIO=1 (ECPLL, X1->OSCI jumper) or add
 * EV08P02A_PWM_AUDIO_HAL_FRC=1 (FRCPLL, no jumper). */
#ifndef EV08P02A_PWM_AUDIO
#define EV08P02A_PWM_AUDIO 0
#endif
#if (EV08P02A_PWM_AUDIO != 0) && (EV08P02A_PWM_AUDIO != 1)
#error "EV08P02A_PWM_AUDIO must be 0 or 1"
#endif
/* Scope-only PWM: drive the socket 3 logic pins while nSLEEP stays low.
 * This is a separate test ROM image; the normal MPQ build still checks nFAULT. */
#ifndef EV08P02A_PWM_SCOPE_ONLY
#define EV08P02A_PWM_SCOPE_ONLY 0
#endif
#if (EV08P02A_PWM_SCOPE_ONLY != 0) && (EV08P02A_PWM_SCOPE_ONLY != 1)
#error "EV08P02A_PWM_SCOPE_ONLY must be 0 or 1"
#endif
#if EV08P02A_PWM_SCOPE_ONLY && !EV08P02A_PWM_AUDIO
#error "EV08P02A_PWM_SCOPE_ONLY requires EV08P02A_PWM_AUDIO=1"
#endif
/* PWM implementation: always the hal_pwm_audio core + ev08p02a_pwm_audio_hal.c
 * backend. The first-light ev08p02a_pwm_audio.c path and its switches
 * (EV08P02A_PWM_AUDIO_HAL, _SCOPE_FRC, _ELASTIC_FIFO) were removed on
 * 2026-10-15; EV08P02A_PWM_AUDIO_HAL_FRC covers the jumper-free FRCPLL case.
 * A leftover EV08P02A_PWM_AUDIO_HAL=1 on an old command line is harmless and
 * accepted; asking for the removed path (=0) or its switches is an error. */
#if defined(EV08P02A_PWM_AUDIO_HAL) && (EV08P02A_PWM_AUDIO_HAL != 1)
#error "EV08P02A_PWM_AUDIO_HAL=0 (ev08p02a_pwm_audio.c) was removed; the HAL path is the only PWM path"
#endif
#if defined(EV08P02A_PWM_SCOPE_FRC) || defined(EV08P02A_PWM_ELASTIC_FIFO)
#error "EV08P02A_PWM_SCOPE_FRC/_ELASTIC_FIFO were removed; use EV08P02A_PWM_AUDIO_HAL_FRC=1 (FRCPLL, elastic FIFO)"
#endif
/* Independent FRCPLL clock for the MPQ board's HAL path. The backend keeps
 * its duty FIFO elastic because the WM8904 TDM clock is crystal-derived. */
#ifndef EV08P02A_PWM_AUDIO_HAL_FRC
#define EV08P02A_PWM_AUDIO_HAL_FRC 0
#endif
#if (EV08P02A_PWM_AUDIO_HAL_FRC != 0) && (EV08P02A_PWM_AUDIO_HAL_FRC != 1)
#error "EV08P02A_PWM_AUDIO_HAL_FRC must be 0 or 1"
#endif
#if EV08P02A_PWM_AUDIO_HAL_FRC && !EV08P02A_PWM_AUDIO
#error "EV08P02A_PWM_AUDIO_HAL_FRC requires EV08P02A_PWM_AUDIO=1"
#endif
#define EV08P02A_PWM_CLOCK_FRC (EV08P02A_PWM_AUDIO_HAL_FRC)
/* Repeat each 48 kHz duty sample for 1/2/4 carrier periods. */
#ifndef EV08P02A_PWM_CARRIER_FACTOR
#define EV08P02A_PWM_CARRIER_FACTOR 1
#endif
#if (EV08P02A_PWM_CARRIER_FACTOR != 1) && (EV08P02A_PWM_CARRIER_FACTOR != 2) && (EV08P02A_PWM_CARRIER_FACTOR != 4)
#error "EV08P02A_PWM_CARRIER_FACTOR must be 1, 2, or 4"
#endif
/* Error-feedback quantizer order for the HAL PWM path: 0 = round, 1, 2.
 * Default 0: the audible noise was lost PG4DC updates, not resolution, and
 * order 2 costs ~70 us per DMA2 refill, which with Type_LB AVAS left the
 * console unresponsive (2026-10-14). *cp5602 still selects it at run time. */
#ifndef EV08P02A_PWM_NS_ORDER
#define EV08P02A_PWM_NS_ORDER 0
#endif
#if (EV08P02A_PWM_NS_ORDER < 0) || (EV08P02A_PWM_NS_ORDER > 2)
#error "EV08P02A_PWM_NS_ORDER must be 0, 1, or 2"
#endif
/* PWM-only audio gain is applied before PCM enters the shared HAL. */
#ifndef EV08P02A_PWM_GAIN_DB_X10
#define EV08P02A_PWM_GAIN_DB_X10 0
#endif
#if (EV08P02A_PWM_GAIN_DB_X10 < -240) || (EV08P02A_PWM_GAIN_DB_X10 > 240) || ((EV08P02A_PWM_GAIN_DB_X10 % 5) != 0)
#error "EV08P02A_PWM_GAIN_DB_X10 requires -240..240 in 0.5 dB steps"
#endif
#if EV08P02A_PWM_AUDIO && !DEMO_ENABLE_WM8904_AUDIO
#error "PWM audio needs the WM8904 final-output block"
#endif
#if EV08P02A_PWM_AUDIO && defined(EV08P02A_WM8904_DSPIC_IS_MASTER) && EV08P02A_WM8904_DSPIC_IS_MASTER
#error "PWM audio first-light requires WM8904 master and 48 kHz from Rev4/Rev5 X1"
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

/*
 * POT Click on the Curiosity Nano Base for Click boards -- ON by default.
 *
 * NOT called DEMO_ENABLE_*: that prefix is reserved above for the SHARED app/ modules,
 * every board has to answer for each of them, and app/app_config.h #errors when one is
 * unresolved. This module is board-owned (boards/ev08p02a/ev08p02a_pot.c), so making
 * DM330030 and EV88G73A declare a position on a Click socket they do not have would be
 * a question with no meaning. Board-local switch, board-local prefix -- the same shape
 * EV08P02A_WM8904_DSPIC_IS_MASTER already uses.
 *
 * NOT mutually exclusive with anything above, and that is a fact about the pins rather
 * than a hope: the pot uses one analog pin (RA2/RA3/RA4 depending on the socket, see
 * ev08p02a_pins.h) and no bus at all, while the three demos above fight over SPI1 and
 * I2C1. So a codec image can read the knob and app_config.h has no combination to refuse.
 *
 * WHY IT IS ON BY DEFAULT even though the Click is a board a user has to fit. The cost of
 * being wrong in each direction is not symmetric. With no Click present the pin floats and
 * ?av reports a drifting number beside the socket it came from -- visibly nothing. With
 * the Click present and this off, there is no command at all and the answer to "does the
 * ADC work on this part" is silence, which is the state this whole module exists to end.
 *
 * Set it to 0 for an image that must leave the pin alone -- it then stays at its POR
 * analog state and nothing reads it.
 */
#ifndef EV08P02A_ENABLE_ADC_POT
#define EV08P02A_ENABLE_ADC_POT 1
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
 * The AVAS synth's own level is NOT one of these: it is WM8904_AUDIO_AVAS_SHIFT (default
 * in wm8904_audio.c, overridden below because POST moves it), because it calibrates a
 * synthesiser against full scale rather than against a circuit.
 *
 * Must be a multiple of 5 (the table's grid is 0.5 dB) and within +-240 (+-24.0 dB);
 * app_config.h refuses anything else by name rather than snapping it silently. A run-time
 * request in between IS snapped, and the console prints what it realised.
 */

/*
 * PRE: applied to the codec's input samples, ahead of everything. 0.0 dB ships -- the
 * identity, bit for bit (gain_db.h).
 *
 * WHY THE COMPENSATION MOVED TO POST (2026-10-08, aligned with the AK Nano image): this
 * board's WM8904 init is the AK one (-1.5 dB analog, -10.125 dB ADC digital), so the ADC
 * clips at ~0.31 FS of the 32-bit word (-10.1 dBFS). Lifting at PRE saturates the INPUT before the AVAS
 * mix, the mute ramp and any later DSP see it; lifting at POST keeps the chain's interior
 * ~10 dB below full scale and clips once, at the wire. This part has no FPU and the chain
 * is saturating int32, so "low inside, lift at the end" matters more here than on AK (which
 * does it in float and still puts it on POST). The 24-bit ADC leaves 8 spare LSBs in the
 * word, so carrying it 12 dB lower inside costs no SNR.
 *
 * `?tp`'s meter reads the RX buffer, which neither stage writes, so it still answers
 * "is the input small".
 */
#ifndef PRE_GAIN_CODEC_DB_X10
#define PRE_GAIN_CODEC_DB_X10   0      /* +0.0 dB -- identity */
#endif

/*
 * POST: applied on the way out, after the mix -- so it scales the codec input AND the AVAS
 * synth together, which is what a master output gain means.
 *
 * +12.0 dB ships: the same total codec-path compensation as AK Nano (PRE 0 / POST +12),
 * so a full-level line input reaches Q31 full scale and the scope-only PWM spans its whole
 * duty range, exactly as on AK. Before 2026-10-08 this board shipped PRE +6 / POST 0, and
 * ADC clip point lifted 6 dB (0.62 FS) then capped the PWM at ~19..81 % duty (measured).
 *
 * WM8904_AUDIO_AVAS_SHIFT below compensates POST and includes the AK Nano
 * follow-up's relative +6 dB AVAS source increase (14 -> 13).
 */
#ifndef POST_GAIN_CODEC_DB_X10
#define POST_GAIN_CODEC_DB_X10  120    /* +12.0 dB */
#endif

/*
 * AVAS synth level on the wire, as a left shift of its Q15 output (wm8904_audio.c). The
 * Original level was 14 with POST 0 dB; 12 compensated POST +12 dB. The
 * 2026-10-14 setting 13 raises only the AVAS source by approximately 6 dB.
 */
/*
 * AVAS parts mask at boot (*tb): all parts, including the Type_LB noise bank.
 * The PWM HAL build booted with the noise off (0x07) until the bank moved onto
 * the DSP accumulators; with it on, Type_LB measures a 622.5 us block ISR of
 * 667 us (miss=0, dma_errors=0) and the owner confirmed SW0 responds
 * (2026-10-14). *tb0007 turns the noise off at run time if margin is needed.
 */
#ifndef WM8904_AUDIO_AVAS_BOOT_PARTS
#define WM8904_AUDIO_AVAS_BOOT_PARTS 0x0Fu
#endif

#ifndef WM8904_AUDIO_AVAS_SHIFT
#define WM8904_AUDIO_AVAS_SHIFT 13 /* relative +6 dB AVAS source, as AK Nano */
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
