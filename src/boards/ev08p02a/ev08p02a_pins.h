#ifndef EV08P02A_PINS_H
#define EV08P02A_PINS_H

/*
 * ev08p02a_pins.h -- every pin this board uses, and nothing else.
 *
 * Ported 2026-08-26 from EV88G73A's file of the same role: the dsPIC33CK256MC005
 * Curiosity Nano (EV08P02A) confirmed pin-identical to EV88G73A's MC105 Nano on
 * every signal this board uses (UART CDC RC10/RC11, ICSPDAT/CLK RB5/RB6, LED0
 * RD10, SW0/DBG2 RD13, I2C1 alternate pair RC8/RC9, MCLR) -- same RPn numbers,
 * confirmed against both parts' pin diagrams. The rationale below is EV88G73A's
 * own (this is why the "three-role" split exists at all); it applies here
 * unchanged because the pinout it explains is unchanged.
 *
 * WHY A SEPARATE HEADER (EV88G73A's original reasoning for the split)
 * --------------------
 * These macros used to sit inside one monolithic board file, where nothing outside
 * it could name them. The cost showed up immediately: the WM8904 audio port in
 * main.c describes its wiring for the boot report, and with the numbers unreachable
 * that description was typed out by hand -- two copies of the same four RP numbers,
 * one of which would eventually be updated alone. The pinout is also the first thing
 * a reader wants when a jumper looks wrong, and it should not require reading code.
 *
 * DM330030 has had this file (as board_pins.h) since its own reorganisation; EV88G73A
 * adopted the same idea under its own prefix, and this file is that idea again under
 * this board's.
 *
 * (Every dated note below -- "used to", "until 2026-...", "printed X unconditionally
 * until..." -- is EV88G73A's own history, kept for the reasoning it explains, not a
 * claim about this file's own history.)
 *
 * WHAT DOES NOT BELONG HERE
 * -------------------------
 * No policy, and no code. Bus rates, baud rates, clock targets and feature switches are
 * decisions with rationale attached, and they live where that rationale can be read next
 * to them (ev08p02a_board.{c,h}). This file answers exactly one question -- which pin --
 * so that a wiring change touches one place and a policy change never touches it.
 */

#include "nora_gpio.h"

/* ---- On-board LED and button (Curiosity Nano, DS70005656A) ---- */
#define EV08P02A_LED0_PIN     NORA_GPIO_PIN(NORA_GPIO_PORT_D, 10u)

/* Active-low: the button ties RD13 to GND and nothing else pulls it, so the internal
 * pull-up is load-bearing (DS70005656A Sec.4.2.2, "Mechanical Switch"). */
#define EV08P02A_SW0_PIN      NORA_GPIO_PIN(NORA_GPIO_PORT_D, 13u)

/* ---- Console UART1, through the Nano's on-board debugger CDC ---- */
#define EV08P02A_UART1_TX_RP  ((nora_gpio_rp_t)58u) /* RC10 */
#define EV08P02A_UART1_RX_RP  ((nora_gpio_rp_t)59u) /* RC11 */

/*
 * ---- I2C1 on the ALTERNATE pair, ASDA1/ASCL1 ----
 *
 * Selected by the ALTI2C1 fuse in config_bits.c -- NOT by PPS: I2C1 has no PPS entries
 * on this device, the fuse simply chooses between two fixed pairs. The default SDA1/SCL1
 * pins (RP40/41 = RB8/RB9) were rejected because they double as PGC1/PGD1, wired to the
 * Nano's on-board debugger.
 */
#define EV08P02A_I2C1_RP_ASDA ((nora_gpio_rp_t)56u) /* RC8 */
#define EV08P02A_I2C1_RP_ASCL ((nora_gpio_rp_t)57u) /* RC9 */

/*
 * ---- SPI1/TDM to the WM8904 ----
 *
 * By direct jumper from the Nano's edge header (no mikroBUS carrier). The same four pins
 * the master-loopback exerciser used and hardware-verified. Direction depends on which
 * side drives BCLK/FS and is decided per build, not here -- see ev08p02a_tdm_pins_init().
 */
#define EV08P02A_TDM_RP_BCLK  ((nora_gpio_rp_t)50u) /* SCK1 */
#define EV08P02A_TDM_RP_FS    ((nora_gpio_rp_t)51u) /* SS1, CLC1OUT when master */
#define EV08P02A_TDM_RP_SDO   ((nora_gpio_rp_t)48u) /* SDO1, dsPIC -> codec DAC */
#define EV08P02A_TDM_RP_SDI   ((nora_gpio_rp_t)49u) /* SDI1, codec ADC -> dsPIC */

/*
 * The same wiring as one line for the audio path's boot report (wm8904_audio_port_t's
 * `wiring`). Here rather than at the use site so the text and the numbers change
 * together -- it was the use site's own copy of these numbers that motivated this file.
 * Keep it in step with the six macros above; a report that misdescribes the wiring is
 * worse than none.
 *
 * The clock clause is NOT fixed text: the A jumper has two positions and they are not
 * interchangeable, so a dsPIC-master build and a codec-master build must not print the
 * same line. It printed "codec self-clocked (X1)" unconditionally until 2026-08-09, so a
 * dsPIC-master run reported the wrong jumper -- the same defect already recorded for
 * demo_tdm_master_loopback.c's banner.
 *
 * WHAT THE CLAUSE MAY SAY, AND WHY IT IS PHRASED THIS NARROWLY (revised 2026-08-09 after
 * review). The first fix said "MCLK from dsPIC (A-extMCLK)" -- naming the jumper's
 * apparent intent -- and that was WRONG AS A STATEMENT ABOUT THIS FIRMWARE, in a way that
 * contradicted five other files in the same HEAD (wm8904_audio.c/.h, ev08p02a_board.c/.h,
 * dm330030_*). This firmware sources NO MCLK, in any build, on either board:
 *   - there is no MCLK pin defined for either board (both _pins.h say so explicitly);
 *   - wm8904_audio.c sets `tcfg.mclk_enable = false` in BOTH clock roles; and
 *   - `mclk_enable` is not even an output -- it maps to SPI CON1L MCLKEN, which SELECTS
 *     THE SPI BAUD-RATE GENERATOR'S CLOCK SOURCE (the reference clock / REFO output vs
 *     FP). It does not enable an external MCLK output. There is no code path here that
 *     can emit an MCLK, so no banner of ours may claim one.
 *     (This bullet said "the SPI module's own reference INPUT (CLKGEN9)" until the second
 *     review pass. The conclusion was right and the register name was right; CLKGEN9 is AK
 *     vocabulary that does not apply to CK -- on CK256MC005/CK256MP508 the bit is a BRG
 *     source select. Corrected 2026-08-09.)
 * So the clause states only what the build DECIDES -- which side drives BCLK/FS -- plus
 * the jumper position that build REQUIRES. It deliberately does not describe what the
 * jumper physically connects: that is a board fact this repo does not set and has not
 * measured.
 *
 * The clause follows EV08P02A_WM8904_DSPIC_IS_MASTER, the same switch main.c uses to pick
 * the role.
 *
 * The default is repeated here rather than taken from main.c on purpose: this header is
 * included by translation units that main.c does not reach, and -Define applies to the
 * whole compile, so both guards see the same value.
 */
#ifndef EV08P02A_WM8904_DSPIC_IS_MASTER
#define EV08P02A_WM8904_DSPIC_IS_MASTER 0
#endif

#if ( EV08P02A_WM8904_DSPIC_IS_MASTER != 0 )
#define EV08P02A_AUDIO_WIRING_CLOCK_STR " dsPIC drives BCLK/FS (jumper A-extMCLK)"
#else
#define EV08P02A_AUDIO_WIRING_CLOCK_STR " codec drives BCLK/FS from X1 (jumper A-XTAL)"
#endif

#define EV08P02A_AUDIO_WIRING_STR                                              \
    "EV08P02A direct jumpers: BCLK=RP50 FS=RP51 SDO=RP48 SDI=RP49,"            \
    " I2C1 ASDA1=RP56 ASCL1=RP57," EV08P02A_AUDIO_WIRING_CLOCK_STR

/*
 * The same four pins for the codec-less master loopback exerciser
 * (demo_tdm_master_loopback_port_t's `wiring`). Separate from the string above
 * because the two describe different setups on the same pins: that one names the I2C
 * control bus and the A-jumper position its clock direction requires, and neither applies
 * here -- there is no codec to talk to, the dsPIC is always the clock master, and nothing
 * is on the other end unless you fit the SDO->SDI wire. ("the codec's own crystal" until
 * 2026-08-09: that described the clock clause as it read before it became conditional on
 * EV08P02A_WM8904_DSPIC_IS_MASTER, and it was a board claim besides -- plan doc 20.4.)
 *
 * Says what to probe and what is optional, because a demo that does not say what to
 * scope is a demo nobody can use. SDO->SDI is the only wire a user adds, and it is
 * not needed to see BCLK/FS/SDO: without it RX simply reads nothing, which is why
 * this runs with no codec attached.
 *
 * The FS pin is named without an "(RC3, CLC1OUT)" qualifier on purpose: whether that
 * pad carries CLC1OUT or the SPI's own FRMSYNC depends on the run-time fs_shape, which
 * this header cannot see. The demo prints the FS shape and the FRMSYPW/FRMCNT readback
 * on the following line; that is where to look for which source drives the pad.
 */
#define EV08P02A_TDM_LOOPBACK_WIRING_STR                                       \
    "EV08P02A SPI1 master: scope BCLK=RP50(RC2) FS=RP51(RC3)"                  \
    " SDO=RP48(RC0); optional jumper SDO=RP48 -> SDI=RP49(RC1) to echo"

#endif /* EV08P02A_PINS_H */
