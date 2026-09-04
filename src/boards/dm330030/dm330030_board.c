/*
 * dm330030_board.c -- this board's bring-up: the ORDER and the STEPS, in one file.
 *
 * See dm330030_board.h for why board.c and system.c were merged, and for the three-role
 * split this file is one third of. The file is laid out bring-up-order-first: clock
 * policy, then dm330030_board_init(), then the individual pin/peripheral stages it calls,
 * then the BOARD SEAM (app/app_traps.h) at the bottom.
 *
 * THE SECTION ORDER HERE IS THE SAME AS ev88g73a_board.c's, deliberately (2026-08-03):
 * policy defines, observability volatiles, clock stage, <board>_board_init(), the
 * stages, TDM pins, I2C1, BOARD SEAM. The two files are being reduced to "same
 * calls, different parameter context", and that progress is only legible at a glance
 * if both files are read in the same sequence.
 *
 * AS OF 2026-08-04 THERE IS NO SECTION HERE THAT THE OTHER FILE LACKS. The last one --
 * this board's REFO1 MCLK stage -- turned out not to be a capability the other board
 * lacked but a mistake this one carried; see the note where it was. What remains different
 * is parameter context (RP numbers, frequencies, RAM addresses) plus three deliberate
 * choices: the RCON latch, UART RX, and the I2C pull-ups.
 *
 * What used to be in system.c and did NOT survive the merge is recorded where it was
 * removed: MCC's raw clock block (see the long note above the clock section) and the
 * SYSTEM_Initialize()/CLOCK_Initialize() names.
 */

#ifndef DSPIC33CK_BOARD_DM330030
#error "boards/dm330030/dm330030_board.c is DM330030-owned. Build it only in the CK256MP508_DM330030 configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "dm330030_board.h"
#include "dm330030_pins.h"

#include "app_traps.h"              /* BOARD SEAM, implemented at the bottom of this file */
#include "uart_platform_stdio.h"    /* the console the clock policy chooses a rate for */

#include "nora_clock.h"
#include "nora_dma.h"          /* MSTRPR.DMAPR, once, before any channel is armed */
#include "nora_gpio.h"
#include "nora_i2c_master.h"   /* I2C1 bring-up behind app/i2c_probe.h's hook */
#include "nora_pps.h"
#include "nora_gpio_table.h"         /* this board's pins as data, applied in one call */
#include "nora_clock_dspic33ck_bringup.h" /* the shared boot clock stage; this board states 2 frequencies */
#include "nora_reset.h"         /* the shared RCON decode; this board does NOT latch (see below) */
#include "nora_spi_i2s_tdm_dspic33ck_pins.h"   /* the shared TDM routing; this board supplies 4 RP numbers */
#include <xc.h>   /* RCON, OSCCON -- REFOCONL/H left with the deleted MCLK stage below */

/* ========================================================================== */
/* Clock policy                                                                */
/* ========================================================================== */

/*
 * Board clock operating point (build-time menu).
 *
 * The Clock HAL sources the system PLL from the internal FRC (8 MHz, crystal-
 * independent) and records the result so nora_clock_get_fcy_hz() is the one
 * authoritative Fcy for the UART/I2C/TDM/SPI baud + BRG math.
 *
 *   DM330030_CLOCK_TARGET_FOSC_HZ = 200000000  -> Fosc 200 MHz / Fcy 100 MHz
 *                                              (dsPIC33CK256MP508 MAXIMUM, 100 MIPS)
 *                                              -- default. Enables accurate high
 *                                              baud (e.g. 230400) and MHz-range
 *                                              TDM BCLK.
 *   DM330030_CLOCK_TARGET_FOSC_HZ = 8000000    -> FRC direct, no PLL (Fcy 4 MHz).
 *                                              The original low-risk operating point.
 *
 * HW note: the PLL path is not yet hardware-verified (no board). A failure to lock is
 * handled rather than fatal -- see the fallback below -- so the first-flash check is
 * "which rate is the terminal on", not "is it bricked".
 */
#define DM330030_CLOCK_FRC_HZ          8000000UL     /* CK internal FRC (PLL input) */
#ifndef DM330030_CLOCK_TARGET_FOSC_HZ
#define DM330030_CLOCK_TARGET_FOSC_HZ  200000000UL   /* device maximum (100 MIPS)   */
#endif

/*
 * Console rate, chosen from the CLOCK RATE ACTUALLY RUNNING -- which is a different
 * question from "did we reach the operating point we asked for", and conflating the two
 * was a defect (found in review, 2026-08-02).
 *
 * 230400 is only representable once Fcy is high. At the 4 MHz FRC its divisor rounds to
 * 250000 baud, +8.5%, far outside the ~2% a UART frame tolerates, so every character
 * arrives corrupt -- i.e. exactly when the board most needs to explain itself, it would go
 * quiet. 9600 resolves to within 0.2% at both 4 MHz and 100 MHz.
 *
 * THE BUG THAT MADE THIS ITS OWN PREDICATE: the rate used to follow
 * g_dm330030_clock_on_target, and that flag is true whenever the REQUESTED point was
 * reached. Build with -DDM330030_CLOCK_TARGET_FOSC_HZ=8000000 -- the FRC-direct option
 * this file documents and offers -- and the request succeeds, so the flag is true, so the
 * console came up at 230400 on a 4 MHz Fcy and said nothing at all. The documented
 * low-risk operating point was the one that produced a dead terminal.
 */
#define DM330030_CONSOLE_BAUD_FAST     230400UL
#define DM330030_CONSOLE_BAUD_SAFE     9600UL

/*
 * Minimum 4x-oversampled divisor for DM330030_CONSOLE_BAUD_FAST to be worth using.
 *
 * The UART HAL rounds BRG+1 = round(Fcy / (4 * baud)) (hal_uart/nora_uart_dspic33ck.c,
 * uart_calc_brg), so the rate error from that rounding is at worst 0.5/(BRG+1). Staying
 * inside the ~2% a UART frame tolerates therefore needs BRG+1 >= 25.
 *
 * Checked against the achieved Fcy rather than against a list of known-good operating
 * points, so a future third entry in the clock menu cannot silently reintroduce the bug
 * above: 100 MHz gives 108 (0.5%, fine), 4 MHz gives 4 (12%, refused).
 */
#define DM330030_CONSOLE_FAST_MIN_DIV  25UL

/*
 * MCC's generated raw-register block used to sit in system.c and has been removed.
 * What it wrote, and why each line went:
 *
 *   CLKDIV / PLLFBD / OSCTUN / PLLDIV
 *       A complete second PLL configuration -- PLLFBDIV 150, POST1DIV 1:4 --
 *       DIFFERENT from the 200 MHz Fosc the Clock HAL sets up immediately below,
 *       and simply overwritten by it. Two sources of truth for the operating
 *       point, one of them dead. The HAL is authoritative: it records Fosc so
 *       nora_clock_get_fcy_hz() drives every baud/BRG calculation in the
 *       tree (UART, I2C, TDM). Keeping a stale duplicate here is how a clock
 *       change silently fails to take effect.
 *
 *   ACLKCON1 / APLLFBD1 / APLLDIV1
 *       Configured the auxiliary PLL and then left APLLEN disabled. Programming
 *       a divider chain into a PLL that is never enabled is a no-op; the reset
 *       state already has it off. Nothing in this repo uses the APLL.
 *
 *   CANCLKCON = 0
 *       CAN clock off. That is the reset state, and there is no CAN code here.
 *
 *   REFOCONL / REFOCONH = 0
 *       REFO off. Also the reset state. This was called "actively misleading" here
 *       because dm330030_mclk_init() further down the file owned REFO1 and generated
 *       the WM8904 MCLK from it -- but that stage is itself gone as of 2026-08-04 (see
 *       the note where it was), so REFO is now simply unused on this board and these
 *       two writes remain no-ops that say nothing.
 *
 *   RPCON = 0  ("IOLOCK disabled")
 *       Redundant AND wrong. On classic CK, IOLOCK cannot be changed by a plain
 *       write -- it needs the __builtin_write_RPCON() unlock sequence, which
 *       hal_gpio/nora_pps_dspic33ck.c performs around every PPS change (see its
 *       comment: AK's PAC allows a direct write, CK's does not). So this line
 *       never did what its comment claimed, and the HAL already handles the lock.
 *
 *   PMDCON = 0, PMD1..PMD8 = 0  ("all modules enabled")
 *       Reset state, hence no-ops. The evidence is on the bench: the EV88G73A
 *       profile writes no PMD register anywhere, and its CCP1 timer, SPI1, DMA0/1,
 *       I2C1, CLC1 and UART1 all work -- so peripherals are enabled out of reset
 *       on this family and this block was not load-bearing.
 *
 * Net effect: the clock is configured in exactly one place, by the Clock HAL.
 */

/*
 * Bring-up observability, one volatile per stage that can fail silently. The console is
 * derived from the clock, so a clock failure cannot be reported by printf -- a debugger
 * reads these instead.
 *
 * Status ENUMS, not bools, wherever the HAL returns a reason: "it failed" and "it failed
 * because the PLL never locked" are different amounts of help at 9 pm. (The two pin
 * stages below stay bool because the GPIO HAL only returns bool.) This is the pattern
 * ev88g73a_board.c already used and this board did not.
 */
volatile nora_clock_status_t g_dm330030_clock_init_status =
    NORA_CLOCK_ERR_NOT_SUPPORTED;
volatile bool     g_dm330030_clock_on_target = false;
volatile uint16_t g_dm330030_osccon_after_switch = 0u;

/* Which way it failed, as nora_clock_dspic33ck_diag_t -- the same name and shape as
 * EV88G73A's g_ev88g73a_clock_diag, because two boards answering "what went wrong with
 * the clock" should not need two vocabularies. Debugger evidence only on this board: it
 * is compile-only, so nothing here has been observed running. */
volatile uint16_t g_dm330030_clock_diag = 0u;

/* Rate the console actually got, so a garbled terminal can be told from a dead one. */
volatile uint32_t g_dm330030_console_baud = 0u;

/* Whether the achieved Fcy can represent DM330030_CONSOLE_BAUD_FAST. Published separately
 * from g_dm330030_clock_on_target because they answer different questions and used not to
 * -- see DM330030_CONSOLE_FAST_MIN_DIV. On the FRC this is false while on_target may be
 * true. */
volatile bool g_dm330030_console_fast_ok = false;

/* Bring-up observability: result of the console pin/PPS routing stage. Captured
 * separately from the peripheral-init result (g_console_init_status in
 * uart_platform/uart_platform_stdio.c) because a mis-routed PPS leaves peripheral init
 * reporting OK yet produces no console output -- the two stages fail independently.
 * volatile so it is readable from the debugger when the console never comes up. */
volatile bool g_dm330030_uart1_pins_init_ok = false;

/*
 * Same idea for the user I/O (LED1/LED2, the RGB channels, SW1..SW3, the pot's pin).
 *
 * THIS CALL WAS MISSING. dm330030_user_io_pins_init() had no caller anywhere in the
 * tree: LED1/LED2/RGB were never configured as outputs and SW1..SW3 never got their
 * pull-ups. It was masked for as long as the vendor headers existed, because
 * LED1_On() and BUTTON_S1_IsPressed() rewrote TRIS on every call -- the very defect
 * dm330030_io.h was written to remove. Removing them took away the only configurer, and
 * nothing failed loudly: the pins keep their reset state (inputs), so the LEDs stay
 * dark and the buttons read a floating pin.
 *
 * It belongs here, not in the demo: pin direction is a board fact, and the next
 * thing that uses these pins must not have to remember to configure them.
 */
volatile bool g_dm330030_user_io_pins_init_ok = false;

/*
 * Was CLOCK_Initialize(). Static now: dm330030_board_init() below is the only caller,
 * and the order it imposes is the whole reason this function is not called from anywhere
 * else -- everything downstream derives a divisor from the Fcy it establishes.
 */
/*
 * Bring up the system clock from the internal FRC. At the default target (200 MHz Fosc
 * / 100 MHz Fcy = device max) this runs FRC->PLL; set DM330030_CLOCK_TARGET_FOSC_HZ to
 * 8 MHz to run FRC direct with no PLL. The HAL records Fosc, so
 * nora_clock_get_fcy_hz() stays authoritative for all baud/BRG math
 * (UART/I2C/TDM). Timer1's FRC tick is independent and unchanged.
 *
 * TWO THINGS LEFT THIS FUNCTION (2026-08-02). The stage's policy -- OSCCON snapshotted
 * before anything else, "on target" judged from three conditions rather than the HAL
 * status alone, and never staying on a PLL that did not lock -- was copied here by hand
 * from ev88g73a_board.c, and is now nora_clock_dspic33ck_bringup(). And the FRC-direct arm
 * was a compile-time #if on the target frequency; the shared stage compares the two
 * frequencies at run time instead, which costs one branch, deletes the #if, and gives
 * EV88G73A the same option it never had.
 */
static void dm330030_clock_init(void)
{
    static const nora_clock_dspic33ck_bringup_t req = {
        .source         = NORA_CLOCK_SOURCE_FRC,
        .input_hz       = DM330030_CLOCK_FRC_HZ,
        .target_fosc_hz = DM330030_CLOCK_TARGET_FOSC_HZ,
    };
    nora_clock_dspic33ck_bringup_result_t res;

    nora_clock_dspic33ck_bringup(&req, &res);

    /* Republished under this board's own names, which is what the report and a debugger
     * look for -- see the note beside their definitions above. */
    g_dm330030_clock_init_status   = res.status;
    g_dm330030_osccon_after_switch = res.osccon_after_switch;
    g_dm330030_clock_on_target     = res.on_target;
    g_dm330030_clock_diag          = res.diag;
}

/* ========================================================================== */
/* Bring-up order -- the one thing this half of the file is for.               */
/* ========================================================================== */

/* Static, and their order is the point -- see the note in dm330030_board.h. */
static bool dm330030_uart1_pins_init(void);
static bool dm330030_user_io_pins_init(void);

void dm330030_board_init(void)
{
    dm330030_clock_init();
    g_dm330030_uart1_pins_init_ok = dm330030_uart1_pins_init();

    /*
     * The rate follows the Fcy we are actually RUNNING AT, not whether the request
     * succeeded -- see DM330030_CONSOLE_FAST_MIN_DIV for why those are not the same
     * question. Asked of the Clock HAL, which records what it achieved.
     */
    g_dm330030_console_fast_ok =
        ((nora_clock_get_fcy_hz() / (4UL * DM330030_CONSOLE_BAUD_FAST)) >=
         DM330030_CONSOLE_FAST_MIN_DIV);
    g_dm330030_console_baud = g_dm330030_console_fast_ok ? DM330030_CONSOLE_BAUD_FAST
                                                        : DM330030_CONSOLE_BAUD_SAFE;
    uart_platform_stdio_init(g_dm330030_console_baud);

    g_dm330030_user_io_pins_init_ok = dm330030_user_io_pins_init();

    /*
     * DMA SRAM-bus priority (MSTRPR.DMAPR), once, before any channel is armed.
     *
     * THIS CALL WAS MISSING. hal_spi_i2s_tdm's own note says "main() calls
     * nora_dma_global_init() once at startup", and ev88g73a_board.c does -- but this
     * board never did, while its WM8904 audio path drives SPI1 through exactly that
     * transport and its DMA channels. So the priority the transport assumes was left at
     * whatever reset gives. Nothing failed loudly, which is why it survived: the effect
     * of an unset bus priority is a timing margin, not a compile error.
     *
     * Last, deliberately: it configures no pin and needs no clock, so it cannot break
     * the console bring-up above, and putting it here keeps the load-bearing order
     * (clock -> pins -> console) at the top where it is read.
     */
    nora_dma_global_init();
}

/* ========================================================================== */
/* The stages themselves                                                       */
/* ========================================================================== */

/*
 * THE ANSEL SWEEP THAT USED TO OPEN THIS SECTION LEFT ON 2026-08-03, and this is the
 * whole of what it was: dm330030_ports_digital_default() walked PortA..PortE and called
 * nora_gpio_set_analog(pin, false) on all 16 bit positions of each, 80 writes, once
 * at boot before any pin stage ran. ANSELx's POR state is analog for every implemented
 * bit, so the sweep was "make the whole device digital, then hand the exceptions back".
 *
 * WHY IT WENT -- ANSEL IS PART OF WHAT A PIN IS,
 * so its owner must be the stage that configures the pin, and a boot-time sweep gives
 * every pin TWO owners. The concrete cost was not the 80 writes; it was that a pin
 * could reach a working digital state without any code saying so, so the code no
 * longer described the hardware, and the sweep had to be re-run (or carefully not
 * re-run) in a defined order relative to every stage -- see the ordering dependency
 * that vanished from dm330030_user_io_pins[] below.
 *
 * WHY DELETING IT IS SAFE HERE, audited pin by pin rather than assumed (the audit is the
 * work; the deletion is three lines). Every pin this board configures already states its
 * own analog bit:
 *
 *   - user I/O (LED1/LED2, RGB x3, SW1-3, POT) goes through nora_gpio_table_apply(),
 *     and all four shared configs in hal_gpio/nora_gpio_table_dspic33ck.c set .analog
 *     explicitly -- false for the digital three, true for cfg_analog_input.
 *   - UART1 TX and the four TDM pins (hal_spi_i2s_tdm/nora_spi_i2s_tdm_dspic33ck_pins.c) go
 *     through nora_gpio_rp_config_digital_output/_input(), which set .analog = false.
 *     (The REFO1 MCLK pin was a third case here until 2026-08-04; that stage is deleted.)
 *   - I2C1 ASCL1/ASDA1 call nora_gpio_rp_set_analog(false) directly.
 *
 * And separately, from the DFP header (p33CK256MP508.h), MOST OF THOSE PINS HAVE NO ANSEL
 * BIT AT ALL: MP508 implements ANSELA 0-4, ANSELB 0-4/7-9, ANSELC 0-3/6/7, ANSELD
 * 10/11/13, ANSELE 0-3 -- 26 bits of the 80 the sweep wrote. Of this board's pins only
 * RE3 (the pot) is analog-capable, and it is the one pin that WANTS to be analog. Nothing
 * in use here was digital because of the sweep.
 *
 * WHAT CHANGES AT RUN TIME: analog-capable pins this board never configures now stay at
 * their POR (analog) state instead of being made digital inputs. That is the intended end
 * state -- an unused pin is not this board's business -- and it is what the AK boards have
 * shipped with since fe46e0e.
 */
/*
 * Console pins: TX to the MCP2221A's RX, and -- since 2026-08-05 -- RX from its TX.
 *
 * THE RX HALF IS NEW HERE, and it is not a new capability of the board: RP67/RD3 has
 * always been wired to the bridge (dm330030_pins.h cites the User's Guide), the inherited
 * demo simply never read a character, so only TX was routed and this profile had no way to
 * answer a command. `uart_platform_stdio_init()` already enables the receiver -- it sets
 * .enable_rx = true for both boards -- so the peripheral was listening to an unrouted pin.
 *
 * Same shape as ev88g73a_uart1_pins_init(): configure the pad, then PPS. Untested on
 * hardware; no DM330030 exists here.
 */
static bool dm330030_uart1_pins_init(void)
{
    if (!nora_pinmux_route_output(NORA_PPS_OUTPUT_U1TX,
                                       DM330030_UART1_TX_RP,
                                       true)) {
        return false;
    }

    /* Unconditional, exactly like the TX half and like EV88G73A's stage: the pin is a
     * board fact, so it is routed whether or not this profile currently reads it.
     * BOARD_CONSOLE_HAS_RX (board_profile.h) is what the APP layer consults to decide
     * whether a command set may be built -- gating the routing on it as well would make
     * a capability that is true of the wiring depend on a switch, and an unrouted pad is
     * exactly the state that made this board look like it had no receive path. */
    if (!nora_pinmux_route_input(NORA_PPS_INPUT_U1RX,
                                      DM330030_UART1_RX_RP)) {
        return false;
    }

    return true;
}

/*
 * This board's nine user-I/O pins, as data (2026-08-02).
 *
 * It was nine `if (!...) return false;` lines with two locally-declared configs. The
 * list is the board fact; the walk over it is shared -- see nora_gpio_table.h.
 * EV88G73A's stage is now the same call with a two-entry table, which is the whole
 * point: what differs between the two boards here is only this table.
 *
 * THE POT ENTRY USED TO HAVE TO STAY LAST and no longer does (2026-08-03). The
 * dependency was on the ANSEL sweep above: it cleared ANSEL across every port,
 * including this one, so making RE3 analog again had to come afterwards. With the
 * sweep deleted no entry in this table depends on any other, and the table means what
 * it looks like -- nine independent pin facts. nora_gpio_table_apply() still
 * walks it top to bottom (it is a loop), but nothing here relies on that any more.
 *
 * The pot pin used to be configured by the pot module (pot.c, before that adc.c, now
 * merged into dm330030_io.c), which meant it had a different owner from every other
 * pin on this board and the ordering dependency spanned two files that merely
 * happened to be called in the right sequence. Same two-owners shape as the vendor
 * LED/button headers that dm330030_io.h replaced. The pot code now owns the ADC
 * channel and no pin at all.
 */
static const nora_gpio_table_entry_t dm330030_user_io_pins[] = {
    { DM330030_LED1_PIN,      &nora_gpio_cfg_output_low },
    { DM330030_LED2_PIN,      &nora_gpio_cfg_output_low },
    { DM330030_RGB_RED_PIN,   &nora_gpio_cfg_output_low },
    { DM330030_RGB_GREEN_PIN, &nora_gpio_cfg_output_low },
    { DM330030_RGB_BLUE_PIN,  &nora_gpio_cfg_output_low },

    { DM330030_SW1_PIN,       &nora_gpio_cfg_input_pullup },
    { DM330030_SW2_PIN,       &nora_gpio_cfg_input_pullup },
    { DM330030_SW3_PIN,       &nora_gpio_cfg_input_pullup },

    /* The one analog pin on this board, and the only one with an ANSEL bit that is
     * meant to stay set (ANSELE3). No pull, because a pot is a divider driven from
     * both rails and a pull would skew the reading. */
    { DM330030_POT_PIN,       &nora_gpio_cfg_analog_input },
};

static bool dm330030_user_io_pins_init(void)
{
    return nora_gpio_table_apply(
        dm330030_user_io_pins,
        (uint8_t)(sizeof dm330030_user_io_pins / sizeof dm330030_user_io_pins[0]));
}

/* -------------------------------------------------------------------------- */
/* SPI1/TDM pin routing on mikroBUS-A: FS=RP66 BCLK=RP72 SDO=RP70 SDI=RP71.     */
/*                                                                            */
/* This is the whole board content of app/wm8904_audio.c and of                 */
/* app/demo_tdm_master_loopback.c on this board, reached through the transport   */
/* HAL's configure_pins hook (nora_spi_i2s_tdm_port_t).                     */
/*                                                                            */
/* TAKES THE ROLE, and used not to: it was                                      */
/* dm330030_mikrobus_a_spi1_tdm_client_pins_init(), i.e. the CLIENT half only,   */
/* with the name asserting a decision the wiring does not make. Two costs came    */
/* out of that:                                                                  */
/*                                                                            */
/*   - main.c had to wrap it in a role-switched adapter that rejected MASTER, so  */
/*     "can this board be the clock master" was answered in two files.            */
/*   - app/demo_tdm_master_loopback.c, which needs the MASTER direction, could     */
/*     not use this function at all, so it carried its OWN copy of these four RP   */
/*     numbers inside app/ -- a shared module owning one board's pinout.           */
/*                                                                            */
/* The MASTER branch below is that copy, moved here rather than newly written.     */
/* Same four pins either way; only direction and PPS differ. Which side drives     */
/* BCLK/FS is the profile's decision (see main.c), which is also how EV88G73A's     */
/* ev88g73a_tdm_pins_init() has always been shaped -- one function, one role         */
/* argument, matching the single configure_pins(role) hook the HAL offers.           */
/*                                                                            */
/* AND THAT IS WHY THE BODY IS NOW FOUR NUMBERS (2026-08-02). Once both boards       */
/* had the same shape it was visible that the shape was ALL they shared: the         */
/* directions and PPS tokens are family facts, so they moved to                      */
/* hal_spi_i2s_tdm/nora_spi_i2s_tdm_dspic33ck_pins.c. This board's SLAVE branch also      */
/* interleaved direction and routing pin by pin where EV88G73A did all directions    */
/* first; the shared file keeps EV88G73A's order, because that is the one a scope     */
/* has seen.                                                                        */
/* -------------------------------------------------------------------------- */

static const nora_spi_i2s_tdm_pinmap_t dm330030_tdm_pinmap = {
    .bclk = DM330030_TDM_RP_BCLK,
    .fs   = DM330030_TDM_RP_FS,
    .sdo  = DM330030_TDM_RP_SDO,
    .sdi  = DM330030_TDM_RP_SDI,
};

bool dm330030_tdm_pins_init(nora_spi_i2s_tdm_clock_role_t role)
{
    return nora_spi_i2s_tdm_pins_configure(&dm330030_tdm_pinmap, role);
}

/*
 * THE REFO1 MCLK STAGE THAT USED TO SIT HERE IS GONE (2026-08-04), and with it the last
 * structural difference between this file and ev88g73a_board.c. It was
 * dm330030_mclk_init(): derive RODIV from the achieved Fosc, drive REFO1 at 12.5 MHz, and
 * route it to DM330030_AUDIO_RP_MCLK for the codec's SYSCLK.
 *
 * IT WAS NOT AN ASYMMETRY. IT WAS WRONG, in three independent ways:
 *
 *   1. THE PREMISE. Both boards in this repo hang the SAME WM8904 board off the MCU, and
 *      that board carries its own 12.288 MHz XTAL -- so "this board's codec has no crystal"
 *      was never true of either. (Narrowed 2026-08-09: this said "so the codec self-clocks on
 *      BOTH". The XTAL being FITTED is a fact; whether it reaches the codec's MCLK net in
 *      every jumper position is not one this repo has measured -- plan doc 20.4. The premise
 *      that was wrong is refuted by the XTAL's presence alone, so the stronger claim was
 *      never needed here.) The comment block in
 *      dm330030_board.h said as much ("the same WM8904 board hangs off both boards in this
 *      repo") two paragraphs above a seam table whose EV88G73A column it left empty for
 *      exactly the opposite reason.
 *
 *   2. THE FREQUENCY. chip_drivers/wm8904.c solves every rate row from SYSCLK = MCLK =
 *      12.288 MHz, and the 44.1 kHz family's FLL constants from FVCO = 12.288M x 7.35.
 *      12.5 MHz is +1.7% off that, which mis-scales every CLK_SYS_RATE / BCLK_DIV and
 *      moves FREF out from under those constants. The 12.5 figure came from this file's
 *      own unrelated arithmetic (256 x fs at fs ~= 48.8 kHz), not from the driver.
 *
 *   3. THE DIRECTION. The board layer must not generate MCLK here; it only ROUTES
 *      an existing clock through a CLC, and in the
 *      codec-master case routes NOTHING, with the reason spelled out: "The dsPIC must NOT
 *      drive B's MCLK net ... so it cannot contend with the jumper-supplied XTAL on B's
 *      MCLK." That is precisely what this function did. Its own header states the axis
 *      this file had crossed: MCLK's source "varies by a BOARD/compile fact, NOT by any
 *      leg's master/slave role".
 *
 * WHY THE ROLE ARGUMENT DOES NOT RESURRECT IT: WM8904 and the dsPIC can swap TDM
 * master/slave freely, on either board -- that is axis A, it is already a parameter here
 * (dm330030_tdm_pins_init(role)), and CK has run the master direction on hardware. The
 * codec needs its SYSCLK in every one of those combinations -- but a REFO1 generator here is
 * not the answer in any of them, for reasons 2 and 3 above, which hold whatever the board
 * supplies it from. (This used to close with "and in every one of them the XTAL on the codec
 * board supplies it": narrowed 2026-08-09, because how SYSCLK physically arrives in each
 * jumper position is a board fact this repo has not measured -- plan doc 20.4 -- and the
 * argument against this stage never depended on it.)
 *
 * If a future board really does need the MCU to source a reference clock, the shape to copy
 * is sonora's -- a routing decision keyed on board/compile facts -- and not this one. The
 * REFO1 register sequence is in git history (and would belong in hal_clock, not here).
 */

/*
 * static, like every other pin stage in this file: its only caller is
 * dm330030_i2c1_init() below, and EV88G73A's equivalent is static too. Exporting it let a
 * caller clear ANSEL without ever initialising the I2C module -- a bus that looks
 * configured and answers nothing.
 */
static bool dm330030_i2c1_pins_init(void)
{
    /*
     * The codec's control bus is the alternate I2C1 pair (ASCL1_RP57 / ASDA1_RP56, on
     * mikroBUS-A's SCL/SDA pins); selection is done by the ALTI2C1 config fuse
     * (config_bits.c). The I2C module owns the open-drain SCL/SDA lines directly (no PPS
     * routing).
     *
     * THESE TWO CALLS ARE NO-OPS ON THIS PART, and the comment that used to sit here
     * ("analog-capable on MP508, so forcing them digital is load-bearing") was simply
     * wrong -- measured against the DFP header on 2026-08-03, MP508 implements ANSELC
     * bits 0-3, 6 and 7 only, so RC8/RC9 have no ANSEL bit, exactly as on MC105. The
     * GPIO HAL gates on REGISTER presence, not bit presence, so the write lands in
     * ANSELC at a position the silicon does not implement and does nothing. They stay
     * because ANSEL is this stage's to own (that is the whole point of deleting the
     * sweep) and because the default SDA1/SCL1 pair this board could be retargeted to
     * is RB8/RB9 -- ANSELB bits 8 and 9, both implemented, where the call would be
     * load-bearing.
     *
     * In RP form since 2026-08-03, which is the form every other pin stage on both boards
     * uses -- see the macro comment in dm330030_pins.h. EV88G73A's equivalent is now the
     * same two calls with this board's pin names; what differs between the boards here is
     * only the pull-ups and timeout_ms below, and both of those are electrical choices.
     */
    if (!nora_gpio_rp_set_analog(DM330030_I2C1_RP_ASCL, false)) return false;
    if (!nora_gpio_rp_set_analog(DM330030_I2C1_RP_ASDA, false)) return false;

    return true;
}

/*
 * ONE RATE, and 400 kHz is it -- matching EV88G73A_I2C1_BUS_HZ.
 *
 * This used to be two public wrappers, 100 kHz for the probe and 400 kHz for the audio
 * path, on the grounds that each caller should keep the rate it was written with. That
 * argument does not survive contact with the fleet's measurement: 400 kHz WORKS where
 * 100 kHz FAILS, in hardware and not in this code. So the 100 kHz wrapper was not the
 * cautious choice, it was the known-bad one, and it made the same WM8904 board reachable
 * at two different rates depending on which exerciser was compiled in.
 *
 * THE PARAMETERISED FORM IS GONE TOO (2026-08-04). dm330030_i2c1_init_at(bus_hz) survived
 * the collapse to one rate as a static layer under dm330030_i2c1_init(), on the grounds
 * that "the rate is still the board's business and a future board may need another one".
 * Both halves of that are true and neither needs a function: the rate is the define below,
 * and a board that needs a different one changes the define. What the extra layer did buy
 * was one more function than EV88G73A has for the same job -- ev88g73a_i2c1_init() is the
 * whole of it there -- which is the kind of difference that makes a diff of these two
 * files hard to read for no gain. Same call, same order, one function each now.
 */
#define DM330030_I2C1_BUS_HZ  400000UL

/*
 * The single public I2C1 bring-up: both the probe (i2c_probe_t.bus_init) and the audio
 * path (wm8904_audio_port_t.i2c_init) call THIS, exactly as EV88G73A's single
 * ev88g73a_i2c1_init() serves both of its callers.
 */
bool dm330030_i2c1_init(void)
{
    nora_i2c_config_t cfg;

    /*
     * Pins first. The probe this serves used to call nora_i2c_init() on its own
     * (boards/dm330030/main.c's I2cWm8904Probe()) and never went through this stage at
     * all, so it drove the bus without ever stating the pins' analog bit. That was a
     * real shape problem -- two paths to the same bus, one of them incomplete -- even
     * though on THIS pair it could not have misbehaved: RC8/RC9 have no ANSEL bit (see
     * dm330030_i2c1_pins_init above). Both callers now share this one stage.
     */
    if (!dm330030_i2c1_pins_init()) {
        return false;
    }

    /*
     * timeout_ms = 0 (no timeout) is the probe's existing behaviour and is kept for both
     * callers. `get_ms` is left NULL, which disables the HAL's timeout handling outright,
     * so what bounds a stuck bus here is its pending/bus-idle guards.
     *
     * THE REASON CHANGED ON 2026-08-03. It used to be "a non-zero timeout could not be
     * honoured anyway, there is no time source on this board" -- that is no longer true:
     * profile_bring_up() starts a 1 ms running time (app/timer_app.h) before anything
     * touches this bus, so GetTicks() could be handed straight to get_ms. It is a
     * DEFERRED CHOICE now, not a limitation: making a dead timeout live changes what
     * happens on a stuck bus, no DM330030 board exists here to observe that on, and
     * EV88G73A -- which keeps timeout_ms = 10, equally inert for the same reason -- has
     * only an expected-negative probe to show. One change, one hardware pass; not this
     * commit's. Same note at ev88g73a_i2c1_init().
     */
    cfg.fcy_hz             = nora_clock_get_fcy_hz();
    cfg.bus_hz             = DM330030_I2C1_BUS_HZ;
    cfg.timeout_ms         = 0u;
    cfg.get_ms             = NULL;
    cfg.pending_timeout_ms = 0u;

    return nora_i2c_init(NORA_I2C_INST_1, &cfg) == NORA_I2C_OK;
}

/* ========================================================================== */
/* BOARD SEAM (app/app_traps.h) -- this board's half of the app-layer seam.     */
/*                                                                            */
/* Needed because the shared trap handlers (app/app_traps.c) are linked into    */
/* every configuration. Nothing on this board CALLS these yet: it has no        */
/* command interpreter, and its trap policy is APP_TRAPS_POLICY_SPIN, so a trap */
/* halts for the debugger instead of resetting and reporting. They are honest    */
/* rather than stubbed, so that whichever comes first -- a console or a change   */
/* of policy -- does not start from a lie.                                      */
/* ========================================================================== */

/*
 * RCON, read live rather than latched.
 *
 * This board does NOT latch the reset cause at startup, unlike EV88G73A: nothing
 * here clears RCON either, so the register still holds what the last reset set, and
 * adding a latch-and-clear would change what a debugger sees on a board whose
 * hardware verification is deferred. The cost is that the bits accumulate across
 * resets, which is why the string below refuses to name one.
 */
uint16_t board_reset_cause_raw(void)
{
    return RCON;
}

const char *board_reset_cause_str(void)
{
    /*
     * A REAL DECODE NOW (2026-08-03), where this used to be a flat refusal:
     * "(DM330030 does not latch RCON; read the raw word)". The reasoning behind that
     * refusal was sound and is preserved -- naming "the" cause from bits that
     * accumulated across resets is a plausible-looking wrong answer, which is the
     * failure mode this repo keeps finding in its own history -- but it was implemented
     * as "decline always", which also declines the common case where exactly one bit is
     * set and the answer is not in doubt.
     *
     * nora_reset_cause_str() takes that judgement as an argument instead. `false`
     * says this board does NOT latch, so the decoder names a cause only when one
     * candidate is present and otherwise returns its own "multiple bits set, not
     * latched" string. Same honesty, useful in the case that is not ambiguous.
     *
     * The latch policy itself is unchanged and deliberately so: nothing here captures or
     * clears RCON, because adding a latch-and-clear would change what a debugger sees on
     * a board whose hardware verification is still deferred.
     */
    return nora_reset_cause_str(RCON, false);
}

/*
 * The first data address with nothing behind it. RAM on CK256MP508 is
 * 0x1000..0x6FFF (linker script p33CK256MP508.gld: data ORIGIN 0x1000,
 * LENGTH 0x6000), so 0x7000 is the first address that does not exist.
 */
volatile uint16_t *board_trap_bad_addr(void)
{
    return (volatile uint16_t *)0x7000u;
}

/* A stack pointer past SPLIM: just below the end of RAM, leaving room for the move
 * itself to complete before the next push crosses the limit. */
uint16_t board_trap_stack_beyond_limit(void)
{
    return 0x6FF0u;
}
