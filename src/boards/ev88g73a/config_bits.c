#include <xc.h>

#ifndef DSPIC33CK_BOARD_EV88G73A
#error "boards/ev88g73a/config_bits.c is EV88G73A-owned. Build it only in the CK64MC105_EV88G73A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

/*
 * First bring-up configuration for EV88G73A:
 * - internal FRC with postscaler, no primary oscillator assumption; the runtime
 *   then switches FRC->PLL up to the device maximum (see ev88g73a_clock_init)
 * - clock switching enabled (CSECMD), which that runtime switch requires
 * - PLL lock safety OFF, deliberately: PLLKEN = ON gates the PLL output away
 *   when lock is lost, which stops the core dead with no clock and no way to
 *   report it. This image instead checks OSCCON.LOCK in software and falls back
 *   to the FRC (see ev88g73a_clock_init), and that fallback can only execute if
 *   the hardware has not already removed the clock. Revisit once the FRC->PLL
 *   path is hardware-proven: ON additionally covers losing lock at runtime,
 *   which software polling cannot catch.
 * - watchdog controlled by software and left off by runtime default
 * - debug channel on PGC3/PGD3 per Curiosity Nano board routing
 * - I2C1 mapped to the ASCL1/ASDA1 alternate pins (RP57/RP56, i.e. RC9/RC8),
 *   NOT the default SCL1/SDA1 pins (RP40/RP41 = RB8/RB9). The default pins
 *   double as PGC1/PGD1, which on a Curiosity Nano board are wired to the
 *   on-board debugger -- reusing them for I2C would contend with that. This is
 *   the same choice the DM330030 profile's mikroBUS-A I2C1 makes (see
 *   docs/ck_source_layout.md Sec.4), confirmed against this part's
 *   own DFP (DSPIC33CK64MC105.PIC): ALTI2C1 = ON selects ASDA1/ASCL1 (field=0),
 *   OFF selects the default SDA1/SCL1 pins (field=1) -- opposite of what the
 *   bit's raw datasheet description reads at face value, so read the pragma's
 *   own semantic name, not the raw bit table, when porting this elsewhere.
 */
#pragma config FNOSC = FRCDIVN
#pragma config POSCMD = NONE
#pragma config FCKSM = CSECMD
#pragma config PLLKEN = OFF
#pragma config FWDTEN = ON_SW
#pragma config ICS = PGD3
#pragma config JTAGEN = OFF
#pragma config ALTI2C1 = ON
