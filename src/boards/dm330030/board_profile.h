#ifndef BOARD_PROFILE_H
#define BOARD_PROFILE_H

/*
 * boards/dm330030/board_profile.h -- THIS PROFILE'S ANSWERS, and nothing else.
 *
 * The counterpart of boards/ev88g73a/board_profile.h, same filename on purpose: each
 * configuration puts only its own board directory on the include path, so
 * `#include "board_profile.h"` from app/app_config.h resolves to the selected board's
 * copy and shared code never learns a board's name. See that file's header for why the
 * two vocabularies (ENA_* here, EV88G73A_ENABLE_* there) had to become one.
 *
 * Pins live in dm330030_pins.h, rates and clock policy in dm330030_board.{c,h}, and
 * anything that would change with the PART belongs to the HAL's device tables.
 *
 * THE VALUES BELOW ARE THIS BOARD'S TODAY, not its target. Several are 0 because the
 * capability has not been ported yet rather than because this board cannot do it --
 * each such line says so, and the parity contract is the list being
 * worked towards. No line here should read as "this board is different" when the truth
 * is "this board is behind".
 */

#if !defined(DSPIC33CK_BOARD_DM330030)
#error "boards/dm330030/board_profile.h was included without DSPIC33CK_BOARD_DM330030 -- the configuration's extra-include-directories and its preprocessor-macros disagree (firmware.X/nbproject/configurations.xml)."
#endif

/* ---- Which shared app/ modules this image RUNS (each -D-overridable) ---- */

/*
 * WM8904 passthrough over TDM8/32-bit. OFF by default here, unlike EV88G73A: this
 * board's default image is still the inherited RGB/pot/button demo, and the audio path
 * repurposes the mikroBUS-A SPI + I2C1 pins it uses. Nothing on this board's audio path
 * has ever run on hardware.
 */
#ifndef DEMO_ENABLE_WM8904_AUDIO
#define DEMO_ENABLE_WM8904_AUDIO 0
#endif

/* Codec-less TDM master exerciser. Same pins, so same exclusivity (app_config.h). */
#ifndef DEMO_ENABLE_TDM_MASTER_LOOPBACK
#define DEMO_ENABLE_TDM_MASTER_LOOPBACK 0
#endif

/* Standalone I2C1 probe on the mikroBUS-A pair. */
#ifndef DEMO_ENABLE_I2C_PROBE
#define DEMO_ENABLE_I2C_PROBE 0
#endif

/* ---- Audio level calibration: dB into the chain, dB out of it ---- */

/*
 * PRE / POST gain on the codec path, in tenths of a dB -- see EV88G73A's board_profile.h
 * for what they mean, why they are here rather than in wm8904_audio.c, and the ranges
 * app_config.h enforces.
 *
 * BOTH 0.0 dB HERE, i.e. the identity, and that is a statement about evidence rather than
 * a preference: EV88G73A's +6.0 dB on PRE compensates a MEASURED 18.7 dB of analog attenuation
 * on that board's front end, and nothing on this board's audio path has ever run on hardware
 * (see DEMO_ENABLE_WM8904_AUDIO above). Copying a number calibrated against a circuit
 * nobody has listened to on this board would be inventing a measurement. 0.0 dB is bit-
 * exact, so this configuration behaves exactly as it did before the stage existed, and
 * whoever first brings this board's audio up sets these from what they measure.
 */
#ifndef PRE_GAIN_CODEC_DB_X10
#define PRE_GAIN_CODEC_DB_X10   0      /* +0.0 dB -- identity, unmeasured board */
#endif
#ifndef POST_GAIN_CODEC_DB_X10
#define POST_GAIN_CODEC_DB_X10  0      /* +0.0 dB -- identity, unmeasured board */
#endif

/* ---- Console: capability first, then what this profile does with it ---- */

/*
 * THIS BOARD'S CONSOLE IS BIDIRECTIONAL, and the tree simply did not say so until
 * 2026-08-05. The USB-UART is an MCP2221A -- a separate device from the PKOB4
 * programmer, which the console does not pass through -- with MCU TX on RP68/RD4 and
 * MCU RX on RP67/RD3 (DM330030 User's Guide DS50002859A, p.14 and p.19). The inherited
 * demo only ever printed, so only TX was recorded and RX looked like an unknown.
 *
 * Capability, not choice: the pin is real and dm330030_board.c now routes it.
 */
#define BOARD_CONSOLE_HAS_RX 1

/*
 * POLICY: polled, exactly as EV88G73A -- see that board_profile.h for the reasoning.
 * This board additionally links no RX vectors today (hal_uart/nora_uart_dspic33ck_isr.c is
 * excluded from both configurations now that the gate is capability rather than board
 * name), and app_config.h refuses a 1 here until a configuration provides them.
 */
#define BOARD_CONSOLE_USE_RX_ISR 0

/*
 * PORTED 2026-08-05 -- Phase 2, item 1 (parity contract section 2). It was 0 because
 * uart_app/console_task.* had never been wired into this profile's main loop, never
 * because this board could not answer; the RX path above was the missing piece, and
 * dm330030_console_out.c's input pair was two `return false`s.
 *
 * What made it work, none of it new code in uart_app/: the RX pin routed
 * (dm330030_board.c), the polled input seam onto hal_uart (dm330030_console_out.c),
 * and the ANSI fixed screen replaced by a scrolling console with a throttled status
 * line -- see demo_rgb_pot_buttons.h. That last one is the substantive change: an
 * interactive console and a region repainted ~60 times a second cannot share one
 * terminal, and the screen layout is explicitly outside the parity contract.
 *
 * NOT ported alongside it: trap reporting. This configuration is APP_TRAPS_POLICY=2 (spin),
 * so the forced-trap commands (*xa, *xm, *xs) halt the board rather than report, and ?xl
 * has nothing to recall. The
 * commands are listed by console_task_print_help() either way (APP_TRAP_TEST_CMDS
 * defaults to 1), which is honest about what is BUILT and silent about what the policy
 * does with it -- the policy is the next parity item.
 */
#define DEMO_ENABLE_CONSOLE_COMMANDS 1

#endif /* BOARD_PROFILE_H */
