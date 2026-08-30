#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*
 * app_config.h -- the ONE vocabulary shared code is allowed to read.
 *
 * This file used to DEFINE the switches (`ENA_WM8904_AUDIO`, `ENA_TDM_MASTER_LOOPBACK`,
 * `ENA_I2C_WM8904_PROBE`) with defaults of 0, while boards/ev88g73a/main.c defined its own
 * `EV88G73A_ENABLE_*` set for THE SAME shared modules. Two names per decision, and they
 * collided in a way that could only be discovered at link time: a TDM loopback build had
 * to pass both `-DEV88G73A_ENABLE_TDM_LOOPBACK=1` and `-DENA_TDM_MASTER_LOOPBACK=1`, because
 * the profile read one and the shared module the other, and the shared module cannot see a
 * #define made in a board's main.c. Getting it half right produced an empty object file.
 *
 * NOW: each board directory owns a board_profile.h that answers the same questions under
 * NORMALIZED names, the configuration's include path selects which one is found, and this
 * file's whole job is to (1) pull it in, (2) derive what is derivable, and (3) REFUSE
 * anything unresolved or self-contradictory. A setting that is missing is an #error, never
 * a silent 0 -- the failure mode being designed out is a -Define that looks right and does
 * nothing.
 *
 * THE NAMES, and who may write them:
 *
 *   DEMO_ENABLE_WM8904_AUDIO         board_profile.h (or -D)   which shared app/ modules
 *   DEMO_ENABLE_TDM_MASTER_LOOPBACK  board_profile.h (or -D)   this image runs
 *   DEMO_ENABLE_I2C_PROBE            board_profile.h (or -D)
 *   DEMO_ENABLE_CONSOLE_COMMANDS     board_profile.h (or -D)
 *   PRE_GAIN_CODEC_DB_X10            board_profile.h (or -D)   audio level calibration,
 *   POST_GAIN_CODEC_DB_X10           board_profile.h (or -D)   tenths of a dB, signed
 *   BOARD_CONSOLE_HAS_RX             board_profile.h           capability, not choice
 *   BOARD_CONSOLE_USE_RX_ISR         board_profile.h           policy
 *   DEMO_ENABLE_DMA_SELFTEST         DERIVED here -- do not define it anywhere
 *
 * Board pin/peripheral facts stay in <board>_pins.h / <board>_board.h; the clock
 * operating point stays in <board>_board.c. Nothing in this file is a board fact.
 */

/*
 * The selected board's answers. Resolves to boards/<board>/board_profile.h through the
 * configuration's extra-include-directories -- see either copy's header for why the
 * filename carries no board prefix.
 */
#include "board_profile.h"

/* ---------------------------------------------------------------------------- */
/* 0. The old vocabulary is gone, and saying so is part of the job.               */
/*                                                                              */
/* A leftover -DENA_TDM_MASTER_LOOPBACK=1 in a script, a note or a shell history  */
/* would otherwise be accepted by the compiler and do NOTHING -- which is the     */
/* exact failure this rename exists to remove, arriving through the back door.    */
/* So the retired names are refused by name, each pointing at its replacement.    */
/* Delete these six once no reachable script or document still shows them.        */
/* ---------------------------------------------------------------------------- */

#if defined(ENA_TDM_MASTER_LOOPBACK)
#error "ENA_TDM_MASTER_LOOPBACK is retired -- use DEMO_ENABLE_TDM_MASTER_LOOPBACK (one name now; it used to need EV88G73A_ENABLE_TDM_LOOPBACK as well)."
#endif
#if defined(ENA_WM8904_AUDIO)
#error "ENA_WM8904_AUDIO is retired -- use DEMO_ENABLE_WM8904_AUDIO."
#endif
#if defined(ENA_I2C_WM8904_PROBE)
#error "ENA_I2C_WM8904_PROBE is retired -- use DEMO_ENABLE_I2C_PROBE."
#endif
#if defined(EV88G73A_ENABLE_WM8904_AUDIO)
#error "EV88G73A_ENABLE_WM8904_AUDIO is retired -- use DEMO_ENABLE_WM8904_AUDIO."
#endif
#if defined(EV88G73A_ENABLE_TDM_LOOPBACK)
#error "EV88G73A_ENABLE_TDM_LOOPBACK is retired -- use DEMO_ENABLE_TDM_MASTER_LOOPBACK."
#endif
#if defined(EV88G73A_ENABLE_I2C1_PROBE)
#error "EV88G73A_ENABLE_I2C1_PROBE is retired -- use DEMO_ENABLE_I2C_PROBE."
#endif
#if defined(EV88G73A_ENABLE_SPI_EXERCISER)
#error "EV88G73A_ENABLE_SPI_EXERCISER is retired -- DEMO_ENABLE_DMA_SELFTEST is derived below and must not be set by hand."
#endif

/* ---------------------------------------------------------------------------- */
/* 1. Nothing unresolved.                                                        */
/*                                                                              */
/* A profile that forgot one of these would otherwise get the C preprocessor's    */
/* answer -- undefined evaluates to 0 in #if -- i.e. the feature would silently   */
/* not be built. That is exactly the failure this vocabulary exists to remove, so */
/* the check is here rather than trusted to review.                              */
/* ---------------------------------------------------------------------------- */

#if !defined(DEMO_ENABLE_WM8904_AUDIO)
#error "DEMO_ENABLE_WM8904_AUDIO must be resolved by the selected board_profile.h (or -D)."
#endif
#if !defined(DEMO_ENABLE_TDM_MASTER_LOOPBACK)
#error "DEMO_ENABLE_TDM_MASTER_LOOPBACK must be resolved by the selected board_profile.h (or -D)."
#endif
#if !defined(DEMO_ENABLE_I2C_PROBE)
#error "DEMO_ENABLE_I2C_PROBE must be resolved by the selected board_profile.h (or -D)."
#endif
#if !defined(DEMO_ENABLE_CONSOLE_COMMANDS)
#error "DEMO_ENABLE_CONSOLE_COMMANDS must be resolved by the selected board_profile.h (or -D)."
#endif
#if !defined(PRE_GAIN_CODEC_DB_X10)
#error "PRE_GAIN_CODEC_DB_X10 must be resolved by the selected board_profile.h (or -D) -- an unstated audio gain would silently be 0 dB, which is a level nobody chose."
#endif
#if !defined(POST_GAIN_CODEC_DB_X10)
#error "POST_GAIN_CODEC_DB_X10 must be resolved by the selected board_profile.h (or -D)."
#endif

#if !defined(BOARD_CONSOLE_HAS_RX)
#error "BOARD_CONSOLE_HAS_RX must be stated by the selected board_profile.h -- it is a board capability, so there is no default."
#endif
#if !defined(BOARD_CONSOLE_USE_RX_ISR)
#error "BOARD_CONSOLE_USE_RX_ISR must be stated by the selected board_profile.h -- polled or interrupt-driven is a policy, so there is no default."
#endif

/*
 * DEMO_ENABLE_DMA_SELFTEST is derived, and defining it by hand is refused: it must
 * follow "is an SPI1/DMA path built", which is what makes the self-test load-bearing.
 * It was `EV88G73A_ENABLE_SPI_EXERCISER`, computed the same way in that board's main.c.
 */
#if defined(DEMO_ENABLE_DMA_SELFTEST)
#error "DEMO_ENABLE_DMA_SELFTEST is derived from the enabled demos -- do not define it."
#endif
#define DEMO_ENABLE_DMA_SELFTEST \
    (DEMO_ENABLE_WM8904_AUDIO || DEMO_ENABLE_TDM_MASTER_LOOPBACK)

/* ---------------------------------------------------------------------------- */
/* 2. Nothing self-contradictory.                                                */
/* ---------------------------------------------------------------------------- */

/*
 * All three demos drive the same SPI1 (four TDM pins) and/or the same I2C1 pair, on both
 * boards. The pins are the reason, not flash: the 64 KB part once forced exercisers out
 * of the link as well, and that constraint is gone (the source list comes from
 * configurations.xml now). Do not re-derive a size argument from these #errors.
 */
#if DEMO_ENABLE_WM8904_AUDIO && DEMO_ENABLE_TDM_MASTER_LOOPBACK
#error "DEMO_ENABLE_WM8904_AUDIO and DEMO_ENABLE_TDM_MASTER_LOOPBACK both drive SPI1 and its four TDM pins; enable only one."
#endif
#if DEMO_ENABLE_WM8904_AUDIO && DEMO_ENABLE_I2C_PROBE
#error "DEMO_ENABLE_WM8904_AUDIO and DEMO_ENABLE_I2C_PROBE both drive I2C1; the codec path already probes the device ID, so enable only one."
#endif

/*
 * The two audio gains have to be ON the generated table's grid, and the table is the
 * authority: src/app/dsp/gain_db_tables.h ships +-24.0 dB in 0.5 dB steps
 * (tools/gen_gain_db_tables.py). Checked here in the preprocessor, with the literals
 * restated, because app_config.h is included by translation units that have no business
 * pulling in a DSP table -- and because the message a wrong value should produce names the
 * value and the grid, which is more use than a failed array bound. wm8904_audio.c ties the
 * same numbers back to GAIN_DB_HALF_MIN/MAX, so a regenerated table with a wider range
 * cannot leave these two silently stricter than the code.
 *
 * OFF-GRID IS REFUSED HERE BUT SNAPPED AT RUN TIME, on purpose: a config is written once
 * and read for months, so 3 tenths of a dB that quietly became 5 is worth stopping the
 * build over, whereas a *to typed at a console gets its realised value printed straight
 * back and the operator sees the snap happen.
 */
#if (PRE_GAIN_CODEC_DB_X10 > 240) || (PRE_GAIN_CODEC_DB_X10 < -240)
#error "PRE_GAIN_CODEC_DB_X10 is outside +-240 (+-24.0 dB), which is the range src/app/dsp/gain_db_tables.h covers."
#endif
#if (POST_GAIN_CODEC_DB_X10 > 240) || (POST_GAIN_CODEC_DB_X10 < -240)
#error "POST_GAIN_CODEC_DB_X10 is outside +-240 (+-24.0 dB), which is the range src/app/dsp/gain_db_tables.h covers."
#endif
#if ((PRE_GAIN_CODEC_DB_X10) % 5) != 0
#error "PRE_GAIN_CODEC_DB_X10 must be a multiple of 5 -- the table's grid is 0.5 dB, so 5 tenths per step (e.g. 60 = +6.0 dB, 65 = +6.5 dB)."
#endif
#if ((POST_GAIN_CODEC_DB_X10) % 5) != 0
#error "POST_GAIN_CODEC_DB_X10 must be a multiple of 5 -- the table's grid is 0.5 dB, so 5 tenths per step (e.g. 60 = +6.0 dB, 65 = +6.5 dB)."
#endif

/* A command set with no way to receive a command is not a configuration, it is a typo. */
#if DEMO_ENABLE_CONSOLE_COMMANDS && !BOARD_CONSOLE_HAS_RX
#error "DEMO_ENABLE_CONSOLE_COMMANDS=1 needs BOARD_CONSOLE_HAS_RX=1 -- this board's console has no receive path."
#endif

/* Likewise, ISR RX is only a capability if some configuration actually links the
 * vectors. Both configurations exclude hal_uart/nora_uart_dspic33ck_isr.c today (the gate is
 * capability now, not board name -- it used to be excluded on the board that HAS console
 * input and linked on the board that had no RX pin routed). Flipping this to 1 therefore
 * has to be done together with the source list, and this is where that is said. */
#if BOARD_CONSOLE_USE_RX_ISR
#error "BOARD_CONSOLE_USE_RX_ISR=1 also needs hal_uart/nora_uart_dspic33ck_isr.c in the configuration's source list (it is excluded in both today) -- add it there, then remove this guard."
#endif
#if BOARD_CONSOLE_USE_RX_ISR && !BOARD_CONSOLE_HAS_RX
#error "BOARD_CONSOLE_USE_RX_ISR=1 with BOARD_CONSOLE_HAS_RX=0 -- an interrupt on a receive path that does not exist."
#endif

#endif /* APP_CONFIG_H */
