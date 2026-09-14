/*
 * config_bits.c -- DM330030 (dsPIC33CK256MP508 Curiosity) configuration fuses.
 *
 * Split out of what is now dm330030_board.c, matching what boards/ev88g73a/config_bits.c already
 * does for the other board. Fuses are a board/silicon concern and belong on their
 * own, not interleaved with runtime init: they are written once at programming
 * time, they cannot be changed at runtime, and mixing them into an init function
 * makes it look as though they could.
 *
 * The #pragma config statements must precede project file includes, which is why
 * they get a file to themselves rather than a section of a larger one.
 *
 * Content was unchanged from what MCC emitted -- only its decorative banner rows
 * were dropped -- until the three oscillator fuses below were reviewed on
 * 2026-08-10. Everything else here is still MCC's, unreviewed.
 *
 * THE OSCILLATOR FUSES CONTRADICTED THIS BOARD'S OWN CLOCK POLICY (fixed 2026-08-10).
 * dm330030_board.c states the policy in words -- "the internal FRC (8 MHz, crystal-
 * INDEPENDENT)" -- and its bring-up asks the Clock HAL for FRC -> PLL. MCC's fuses
 * said the opposite and made that request impossible to honour:
 *
 *   FNOSC = PRI      the part came out of reset running an XT CRYSTAL, the one thing
 *                    the policy is written to avoid depending on. No crystal frequency
 *                    is documented for this board anywhere in this repo, so nothing
 *                    downstream could have computed a correct baud or BRG divisor from
 *                    it either.
 *   FCKSM = CSDCMD   clock switching DISABLED. The bring-up's OSWEN request is ignored
 *                    by the silicon, so the switch to FRC+PLL could never happen -- and
 *                    it failed silently, because the pre-NORA HAL cached the frequency
 *                    it had been asked for instead of deriving it from the hardware.
 *                    The board would have run on the crystal while every consumer
 *                    believed 200 MHz Fosc / 100 MHz Fcy.
 *
 * This is NOT a NORA-clock prerequisite that could be skipped: with the fuses as MCC
 * left them, no clock HAL of any design can bring this board up as its own board file
 * asks. It is fixed here rather than worked around there.
 *
 * UNEXECUTED. There is no DM330030 board on this bench and there is not expected to be
 * one, so this configuration is compile-only by standing decision. These three values
 * are read off the datasheet and the sibling board, they build clean, and they have
 * never run. Do not report them as tested.
 */

#ifndef DSPIC33CK_BOARD_DM330030
#error "boards/dm330030/config_bits.c is DM330030-owned. Build it only in the CK256MP508_DM330030 configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include <xc.h>

// Section: Configuration Bits

// FSEC
#pragma config BWRP = OFF    //Boot Segment Write-Protect bit->Boot Segment may be written
#pragma config BSS = DISABLED    //Boot Segment Code-Protect Level bits->No Protection (other than BWRP)
#pragma config BSEN = OFF    //Boot Segment Control bit->No Boot Segment
#pragma config GWRP = OFF    //General Segment Write-Protect bit->General Segment may be written
#pragma config GSS = DISABLED    //General Segment Code-Protect Level bits->No Protection (other than GWRP)
#pragma config CWRP = OFF    //Configuration Segment Write-Protect bit->Configuration Segment may be written
#pragma config CSS = DISABLED    //Configuration Segment Code-Protect Level bits->No Protection (other than CWRP)
#pragma config AIVTDIS = OFF    //Alternate Interrupt Vector Table bit->Disabled AIVT

// FBSLIM
#pragma config BSLIM = 8191    //Boot Segment Flash Page Address Limit bits->8191

// FOSCSEL
/*
 * Reset onto the internal FRC, undivided -- see the note at the top of the file.
 *
 * Plain FRC, not the sibling board's FRCDIVN: EV88G73A boots divided-by-N because that
 * is what its own fuse set already did, and the NORA contract has to name that selection
 * (NORA_CLOCK_SOURCE_FRC_DIVIDED) rather than call it FRC. This board has no such
 * history, so it takes the selection with nothing to decode: COSC reads 0.
 */
#pragma config FNOSC = FRC    //Oscillator Source Selection->Internal Fast RC (FRC)
#pragma config IESO = OFF    //Two-speed Oscillator Start-up Enable bit->Start up with user-selected oscillator source

// FOSC
/* Primary oscillator disabled: no crystal is depended on, so OSC1/OSC2 are I/O. The
 * XTCFG/XTBST values below are MCC's and now describe nothing -- left in place because
 * removing them is a separate, unreviewed edit, not because they mean anything. */
#pragma config POSCMD = NONE    //Primary Oscillator Mode Select bits->Primary oscillator disabled
#pragma config OSCIOFNC = ON    //OSC2 Pin Function bit->OSC2 is general purpose digital I/O pin
/* Clock switching ENABLED (was CSDCMD, which made this board's own bring-up a no-op),
 * fail-safe clock monitor left disabled -- same posture as boards/ev88g73a/config_bits.c.
 * FSCM would switch this part to BFRC behind the HAL's back on a primary-oscillator
 * failure, and with POSCMD = NONE there is no primary oscillator to fail. */
#pragma config FCKSM = CSECMD    //Clock Switching Mode bits->Clock switching is enabled, Fail-safe Clock Monitor is disabled
#pragma config PLLKEN = ON    //PLL Lock Status Control->PLL lock signal will be used to disable PLL clock output if lock is lost
#pragma config XTCFG = G3    //XT Config->24-32 MHz crystals
#pragma config XTBST = ENABLE    //XT Boost->Boost the kick-start

// FWDT
#pragma config RCLKSEL = LPRC    //Watchdog Timer Clock Select bits->Always use LPRC
#pragma config WINDIS = OFF    //Watchdog Timer Window Enable bit->Watchdog Timer in Window mode
#pragma config WDTWIN = WIN25    //Watchdog Timer Window Select bits->WDT Window is 25% of WDT period
#pragma config FWDTEN = ON_SW    //Watchdog Timer Enable bit->WDT controlled via SW, use WDTCON.ON bit

// FPOR
#pragma config BISTDIS = DISABLED    //Memory BIST Feature Disable->mBIST on reset feature disabled

// FICD
#pragma config ICS = PGD3    //ICD Communication Channel Select bits->Communicate on PGC3 and PGD3
#pragma config JTAGEN = OFF    //JTAG Enable bit->JTAG is disabled
#pragma config NOBTSWP = DISABLED    //BOOTSWP instruction disable bit->BOOTSWP instruction is disabled

// FDMTIVTL
#pragma config DMTIVTL = 0    //Dead Man Timer Interval low word->0

// FDMTIVTH
#pragma config DMTIVTH = 0    //Dead Man Timer Interval high word->0

// FDMTCNTL
#pragma config DMTCNTL = 0    //Lower 16 bits of 32 bit DMT instruction count time-out value (0-0xFFFF)->0

// FDMTCNTH
#pragma config DMTCNTH = 0    //Upper 16 bits of 32 bit DMT instruction count time-out value (0-0xFFFF)->0

// FDMT
#pragma config DMTDIS = OFF    //Dead Man Timer Disable bit->Dead Man Timer is Disabled and can be enabled by software

// FDEVOPT
#pragma config ALTI2C1 = ON    //Alternate I2C1 Pin bit->I2C1 mapped to ASDA1/ASCL1 pins (mikroBUS A I2C on RP56/RP57; WM8904 control bus)
#pragma config ALTI2C2 = OFF    //Alternate I2C2 Pin bit->I2C2 mapped to SDA2/SCL2 pins
#pragma config ALTI2C3 = OFF    //Alternate I2C3 Pin bit->I2C3 mapped to SDA3/SCL3 pins
#pragma config SMBEN = SMBUS    //SM Bus Enable->SMBus input threshold is enabled
#pragma config SPI2PIN = PPS    //SPI2 Pin Select bit->SPI2 uses I/O remap (PPS) pins

// FALTREG
#pragma config CTXT1 = OFF    //Specifies Interrupt Priority Level (IPL) Associated to Alternate Working Register 1 bits->Not Assigned
#pragma config CTXT2 = OFF    //Specifies Interrupt Priority Level (IPL) Associated to Alternate Working Register 2 bits->Not Assigned
#pragma config CTXT3 = OFF    //Specifies Interrupt Priority Level (IPL) Associated to Alternate Working Register 3 bits->Not Assigned
#pragma config CTXT4 = OFF    //Specifies Interrupt Priority Level (IPL) Associated to Alternate Working Register 4 bits->Not Assigned

// FBTSEQ
#pragma config BSEQ = 4095    //Relative value defining which partition will be active after device Reset; the partition containing a lower boot number will be active->4095
#pragma config IBSEQ = 4095    //The one's complement of BSEQ; must be calculated by the user and written during device programming.->4095

// FBOOT
#pragma config BTMODE = SINGLE    //Device Boot Mode Configuration->Device is in Single Boot (legacy) mode
