#ifndef DM330030_PINS_H
#define DM330030_PINS_H

/*
 * Board pin assignment for the DM330030 dsPIC33CK256MP508 Curiosity board.
 *
 * This follows the AK starter convention: board code owns pins/PPS, peripheral
 * HALs own peripheral registers. CK RP numbers use the device's real RP map,
 * not AK's packed-pin+1 convention.
 */

#include "nora_gpio.h"

/* ---- UART1 console: the board's USB-UART, BOTH directions ----
 *
 * The bridge is an **MCP2221A**, which is a SEPARATE DEVICE from the PKOB4
 * programmer/debugger on this board -- the console does not pass through PKOB4 (that is
 * what buildtools/flashauto.ps1 drives). DM330030 User's Guide DS50002859A shows the
 * MCP2221A on p.14 and both pins on p.19:
 *
 *   MCU U1TX -> RP68 / RD4 -> MCP2221A RX
 *   MCU U1RX <- RP67 / RD3 <- MCP2221A TX
 *
 * THE RX PIN WAS MISSING FROM THIS TREE UNTIL 2026-08-05, and its absence had been read
 * as a hardware unknown ("which pin is the console RX?") when it was only a gap in the
 * record: the inherited Microchip demo printed and never read, so nobody needed RX and
 * nobody wrote it down. It routes like any other input, collides with nothing this board
 * uses (TDM sits on RD2/RD6/RD7/RD8, TX on RD4), and it is what makes
 * BOARD_CONSOLE_HAS_RX=1 true in board_profile.h -- i.e. what unblocks the whole
 * interactive-console half of the parity contract.
 *
 * Untested on hardware, like everything else on this board: no DM330030 exists here.
 */
#define DM330030_UART1_TX_RP         ((nora_gpio_rp_t)68u)
#define DM330030_UART1_RX_RP         ((nora_gpio_rp_t)67u)

/* ---- Existing demo LEDs and switches ---- */
#define DM330030_LED1_PIN            NORA_GPIO_PIN(NORA_GPIO_PORT_E, 6)
#define DM330030_LED2_PIN            NORA_GPIO_PIN(NORA_GPIO_PORT_E, 5)

#define DM330030_RGB_RED_PIN         NORA_GPIO_PIN(NORA_GPIO_PORT_E, 15)
#define DM330030_RGB_GREEN_PIN       NORA_GPIO_PIN(NORA_GPIO_PORT_E, 14)
#define DM330030_RGB_BLUE_PIN        NORA_GPIO_PIN(NORA_GPIO_PORT_E, 13)

#define DM330030_SW1_PIN             NORA_GPIO_PIN(NORA_GPIO_PORT_E, 7)
#define DM330030_SW2_PIN             NORA_GPIO_PIN(NORA_GPIO_PORT_E, 8)
#define DM330030_SW3_PIN             NORA_GPIO_PIN(NORA_GPIO_PORT_E, 9)

/* ---- Potentiometer used by the inherited demo ADC helper ---- */
#define DM330030_POT_PIN             NORA_GPIO_PIN(NORA_GPIO_PORT_E, 3)

/* ---- SPI1/TDM to the WM8904 ----
 *
 * NAMES ARE ROLE + RP, THE CONNECTOR IS THIS COMMENT. Every one of these signals
 * reaches the codec through mikroBUS-A, and that is a fact about the wiring, not
 * about the signal: the peripheral, the HAL and the board bring-up all care that a
 * pin is BCLK, and none of them cares which connector carries it. Putting the
 * connector in the identifier also made this board read differently from
 * EV88G73A, where the same four signals on the same peripheral are
 * EV88G73A_TDM_RP_{BCLK,FS,SDO,SDI} and reach the codec by direct jumper. Same
 * signals, same roles, two spellings -- so the spelling is now shared and the
 * difference lives here, where a reader looking at a suspicious jumper will find it.
 *
 * WHICH mikroBUS-A PIN EACH ONE IS, and what the WM8904 board calls it:
 *
 *   role   RP    port  mikroBUS-A pin   WM8904 board
 *   ----   ----  ----  --------------   -----------------------------
 *   FS     66    RD2   CS               LRCLK / frame sync
 *   SDO    70    RD6   MOSI             DACDAT  (dsPIC -> codec)
 *   SDI    71    RD7   MISO             ADCDAT  (codec -> dsPIC)
 *   BCLK   72    RD8   SCK              BCLK
 *
 * DIRECTION IS NOT A FACT ABOUT THE PIN and is deliberately not stated here: it
 * follows from which side is the audio-clock master, which is a build-time choice
 * (see dm330030_tdm_pins_init(), which takes the role). The first hardware target
 * has the WM8904 as master, so FS/BCLK/SDI would be inputs and SDO the only output.
 */
#define DM330030_TDM_RP_FS      ((nora_gpio_rp_t)66u)  /* RD2 / mikroBUS-A CS   */
#define DM330030_TDM_RP_SDO     ((nora_gpio_rp_t)70u)  /* RD6 / mikroBUS-A MOSI */
#define DM330030_TDM_RP_SDI     ((nora_gpio_rp_t)71u)  /* RD7 / mikroBUS-A MISO */
#define DM330030_TDM_RP_BCLK    ((nora_gpio_rp_t)72u)  /* RD8 / mikroBUS-A SCK  */

/* ---- THERE IS NO MCLK PIN ON THIS BOARD, and there was not one before either ----
 *
 * DM330030_AUDIO_RP_MCLK (RP69/RD5) was here until 2026-08-04, feeding a REFO1 stage in
 * dm330030_board.c that generated ~12.5 MHz for the codec's SYSCLK. Both are deleted.
 *
 * The reason it is not needed is about THIS FIRMWARE, not about the codec board: there is
 * no code here that generates or outputs an MCLK, in any build or either TDM master/slave
 * direction, so there is nothing for an MCLK pin to carry. Where the codec's SYSCLK
 * physically comes from is a board fact this tree does not set, read or measure -- and this
 * board cannot be measured at all (nobody has one). The WM8904 board is the SAME one both
 * boards use and it does carry a 12.288 MHz XTAL, which is what made the deleted stage's
 * premise ("this codec has no crystal") false; that the XTAL is FITTED is a fact, that it
 * reaches the codec's MCLK net in every jumper arrangement is not one we have checked.
 * (Narrowed 2026-08-09: this read "so the codec's SYSCLK comes from the codec board ...
 * true in either direction" -- see plan doc 20.4/20.6.) The pin itself was never more than
 * a placeholder: a free mikroBUS-A pin, chosen without evidence that the codec's MCLK
 * input lands there.
 *
 * The reasons in full, including why 12.5 MHz was the wrong figure regardless, are in the
 * note where the stage was (dm330030_board.c) and in docs/ck_source_layout.md.
 */

/* ---- I2C1 control bus to the codec, on the ALTERNATE pair ASDA1/ASCL1 ----
 *
 * Reaches the codec through mikroBUS-A's SDA/SCL pins. Selected by the ALTI2C1
 * config fuse (config_bits.c) and NOT by PPS -- I2C1 has no PPS entries on this
 * device, the fuse simply chooses between two fixed pairs, so there is no routing
 * call to make.
 *
 * IN RP FORM, matching EV88G73A_I2C1_RP_{ASDA,ASCL} and the four TDM macros above.
 * These were the only pins on this board expressed as port/bit, which forced
 * dm330030_i2c1_pins_init() to call nora_gpio_set_analog() where every other
 * stage on both boards calls the _rp_ form: the same act in two vocabularies, one
 * of which had to be translated by hand every time the two boards were compared.
 *
 * NEITHER PIN IS ANALOG-CAPABLE, on this part or on MC105. This comment used to say the
 * opposite -- "these pins ARE analog-capable on MP508 (unlike MC105 ...), so forcing them
 * digital is load-bearing here" -- and it was wrong: MP508 implements ANSELC bits 0-3, 6
 * and 7, exactly like MC105, so RC8/RC9 have no ANSEL bit on either. Measured against both
 * DFP headers on 2026-08-03, while auditing what the deleted boot-time ANSEL sweep had
 * been covering up. dm330030_i2c1_pins_init() still states the analog bit, and why it does
 * so despite the no-op is recorded there.
 */
#define DM330030_I2C1_RP_ASCL   ((nora_gpio_rp_t)57u)  /* RC9 / mikroBUS-A SCL */
#define DM330030_I2C1_RP_ASDA   ((nora_gpio_rp_t)56u)  /* RC8 / mikroBUS-A SDA */

/*
 * ---- The same wiring as text, for the two exercisers that report it ----
 *
 * Here rather than at the use site so the text and the RP macros above change together.
 * They were hand-typed in main.c, twice, with the numbers spelled out a second time --
 * which is precisely the duplication EV88G73A_AUDIO_WIRING_STR exists to avoid on the
 * other board. A report that misdescribes the wiring is worse than no report.
 *
 * THE CONNECTOR NAME LIVES HERE -- in these strings and in the comments on the macros
 * above -- and not in the function names, and no longer in the macro names either. The
 * macros used to be DM330030_MIKROBUS_A_*, which meant "mikroBUS" had been driven out of
 * the function names while staying in the identifiers those functions passed around.
 * These strings are where the connector belongs: they are the report a human reads while
 * looking at the physical board, which is the one place the connector is the point.
 */
/*
 * The clock clause says what THIS FIRMWARE does -- nothing -- and not where the codec's
 * SYSCLK comes from. It read "codec MCLK from its own XTAL" until 2026-08-09: a physical
 * claim, printed as a finding, on the one board in this tree that NOBODY CAN EVER RUN to
 * check it (section 20.2). That is the same defect EV88G73A's banner had (section 19.3,
 * corrected again in 20.4), and a compile-only board is exactly where it would have survived
 * longest.
 */
#define DM330030_AUDIO_WIRING_STR                                              \
    "DM330030 mikroBUS-A: BCLK=RP72(RD8/SCK) FS=RP66(RD2/CS)"                  \
    " SDO=RP70(RD6/MOSI) SDI=RP71(RD7/MISO),"                                  \
    " I2C1 ASDA1=RP56 ASCL1=RP57 @400k; no MCLK supplied by the dsPIC"

#define DM330030_TDM_LOOPBACK_WIRING_STR                                       \
    "DM330030 mikroBUS-A: BCLK=RP72(RD8/SCK) FS=RP66(RD2/CS)"                  \
    " SDO=RP70(RD6/MOSI) SDI=RP71(RD7/MISO); jumper SDO->SDI to loop back"

#define DM330030_I2C1_WHERE_STR                                                \
    "mikroBUS-A ASDA1=RP56(RC8) ASCL1=RP57(RC9), 400 kHz"

#endif /* DM330030_PINS_H */
