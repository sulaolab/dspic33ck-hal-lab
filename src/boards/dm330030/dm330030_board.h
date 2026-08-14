#ifndef DM330030_BOARD_H
#define DM330030_BOARD_H

/*
 * dm330030_board.{c,h} -- BRING-UP. Everything here runs once, in order, before the
 * profile starts: clock policy, console pins and rate, user I/O pins, DMA bus priority,
 * the mikroBUS-A peripheral bring-ups, and this board's half of the BOARD SEAM (app/app_traps.h).
 *
 * THIS FILE IS board.c AND system.c MERGED. They were split by MCC's habit (a generated
 * "system" file plus a hand-written board file), not by anything a reader could
 * re-derive: system.c held the ORDER and board.c held the STEPS, so following the
 * bring-up meant reading two files alternately -- and at the time, the ordering
 * dependency between the boot-time ANSEL sweep (deleted 2026-08-03) and the pot's
 * analog pin spanned that boundary. One role, one file.
 *
 * The three roles are now the same on both boards, divided by WHEN each matters:
 *
 *   dm330030_pins.h     facts      compile time only, no code
 *   dm330030_board.*    bring-up   once, in order   <- this file
 *   dm330030_io.*       runtime    every iteration, owns no init
 *
 * plus dm330030_led3_rgb.* which stays its OWN file: it drives a soft PWM off the 1 ms
 * tick, i.e. it holds a state machine, and a device with state is not an accessor.
 *
 * SYSTEM_Initialize() / CLOCK_Initialize() went with the merge. They were MCC names for
 * functions that had nothing generated left inside them, and "system" said less than the
 * board's own name: on a two-board tree, dm330030_board_init() is the one that answers
 * "whose bring-up is this" -- which is also why EV88G73A's has always been called
 * ev88g73a_board_init().
 */

#include <stdbool.h>
#include <stdint.h>

/* For nora_spi_i2s_tdm_clock_role_t in dm330030_tdm_pins_init()'s signature -- the same
 * include ev88g73a_board.h carries, for the same reason. */
#include "nora_spi_i2s_tdm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up clock, console pins, the console, user I/O pins and the DMA bus priority --
 * in that order, and the clock has to be first: the console's baud divisor is derived
 * from the Fcy actually achieved. Call once from profile_bring_up(), before anything
 * prints.
 *
 * Was SYSTEM_Initialize().
 */
void dm330030_board_init(void);

/* ---------------------------------------------------------------------------- */
/* The stages BELOW are public because dm330030_board_init() is not their only   */
/* caller: they are reached through app-layer hooks (i2c_probe_t.bus_init,       */
/* wm8904_audio_port_t) from main.c's port structs, on profiles that enable them. */
/*                                                                              */
/* TWO STAGES ARE DELIBERATELY NOT HERE and are static in the .c:                 */
/*   dm330030_uart1_pins_init()   dm330030_user_io_pins_init()                    */
/* Each has exactly one caller -- dm330030_board_init() -- and the console's PPS   */
/* routing must precede the peripheral enable, so exporting them would let a       */
/* caller produce a silent console; the ordering is enforced by linkage rather     */
/* than by a comment (raised in review, 2026-08-02). Their results are still       */
/* observable, as the g_dm330030_*_init_ok volatiles.                             */
/*                                                                              */
/* A THIRD, dm330030_ports_digital_default(), was listed here until 2026-08-03    */
/* and is now DELETED rather than hidden -- with it went the ordering rule that   */
/* the pot's analog pin had to be configured after it. See the note in the .c.     */
/* ---------------------------------------------------------------------------- */

/*
 * SPI1/TDM pin routing on mikroBUS-A, for the role it is handed.
 *
 * Was dm330030_mikrobus_a_spi1_tdm_client_pins_init() -- the client half only, named
 * after a decision the wiring does not make. Both roles are supported now, because the
 * four pins physically carry either direction and the transport HAL asks for exactly one
 * configure_pins(role) hook. Which side drives BCLK/FS is the PROFILE's choice; see the
 * reason recorded beside .dspic_is_master in main.c.
 *
 * Same shape as EV88G73A's ev88g73a_tdm_pins_init(). Deliberately says nothing about
 * CLC1: the HAL repoints the FS pin to CLC1OUT by itself when a 50%-duty FS is asked
 * for, by finding whichever pin this function routed FRMSYNC to.
 */
bool dm330030_tdm_pins_init(nora_spi_i2s_tdm_clock_role_t role);

/*
 * WM8904 audio support (pins: dm330030_pins.h).
 *
 * FACTS KEEP THE CONNECTOR, FUNCTIONS TAKE THE ROLE -- the rule the TDM routing above
 * already followed, applied to the rest of the audio path. These three were
 * dm330030_mikrobus_a_mclk_init() / _i2c1_init() (100 kHz) / _i2c1_audio_init()
 * (400 kHz), plus a public _i2c1_pins_init(). The same WM8904 board hangs off both
 * boards in this repo, and it fills the same wm8904_audio_port_t slots on each -- so the
 * names that fill those slots differ only in the board prefix:
 *
 *      EV88G73A                      DM330030
 *      ev88g73a_tdm_pins_init(role)  dm330030_tdm_pins_init(role)
 *      ev88g73a_i2c1_init()          dm330030_i2c1_init()
 *
 * THAT TABLE HAD A THIRD ROW UNTIL 2026-08-04, reading "(none -- codec has X1)" against
 * dm330030_mclk_init(), which contradicted the sentence directly above it: the same WM8904
 * board is on both, so its XTAL is on both. The MCLK stage is deleted, not converged -- see
 * the note in dm330030_board.c where it was, and docs/ck_src_layout.md. The table is now
 * the whole of the audio seam, and it is symmetric.
 *
 * "mikroBUS-A" is still everywhere it belongs -- the comments on the pin macros, the
 * wiring strings the boot report prints, config_bits.c's ALTI2C1 note -- because WHICH
 * CONNECTOR the codec is plugged into is a fact about this board. It just is not a
 * property of the operation, and putting it in the function name made a shared
 * operation look board-specific.
 *
 * The pin MACROS followed the function names on 2026-08-03: DM330030_MIKROBUS_A_*_RP
 * became DM330030_TDM_RP_* / _I2C1_RP_* (and _AUDIO_RP_MCLK, deleted with the MCLK stage
 * on 2026-08-04), matching EV88G73A's spelling for
 * the same roles on the same peripheral. Same reasoning one level down -- the identifier
 * these functions pass around names a ROLE, and the connector that carries it is written
 * out in full where a reader with the board in front of them will look (dm330030_pins.h).
 *
 *  - dm330030_i2c1_init(): force the alternate I2C1 pins digital (selection itself is
 *    the ALTI2C1 config fuse, config_bits.c) and init the I2C HAL at 400 kHz. Serves
 *    both app/i2c_probe.h's `bus_init` and app/wm8904_audio.h's `i2c_init`; the pins-only
 *    stage is static now, so the two cannot be run out of order.
 */
bool dm330030_i2c1_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DM330030_BOARD_H */
