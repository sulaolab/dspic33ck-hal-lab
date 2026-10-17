/* EV97U97A audio port, adapted from the DM330030 board implementation. */
#ifndef DSPIC33CK_BOARD_EV97U97A
#error "boards/ev97u97a/config_bits.c is EV97U97A-owned. Build it only in the CK1024MP710_EV97U97A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include <xc.h>

/* FRC boots independently of the codec XTAL; board_init switches to 100 MHz Fcy.
 * PKoB4 uses PGD3. I2C3 alternate pads reach mikroBUS A (RD3/RD4). */

#pragma config BWRP = OFF
#pragma config BSS = DISABLED
#pragma config BSEN = OFF
#pragma config GWRP = OFF
#pragma config GSS = DISABLED
#pragma config CWRP = OFF
#pragma config CSS = DISABLED
#pragma config AIVTDIS = OFF

#pragma config BSLIM = 8191

#pragma config FNOSC = FRC
#pragma config IESO = OFF

#pragma config POSCMD = NONE
#pragma config OSCIOFNC = ON

#pragma config FCKSM = CSECMD
#pragma config PLLKEN = ON
#pragma config XTCFG = G3
#pragma config XTBST = ENABLE

#pragma config RCLKSEL = LPRC
#pragma config WINDIS = OFF
#pragma config WDTWIN = WIN25
#pragma config FWDTEN = ON_SW

#pragma config BISTDIS = DISABLED

#pragma config ICS = PGD3
#pragma config JTAGEN = OFF
#pragma config NOBTSWP = DISABLED

#pragma config DMTIVTL = 0

#pragma config DMTIVTH = 0

#pragma config DMTCNTL = 0

#pragma config DMTCNTH = 0

#pragma config DMTDIS = OFF

#pragma config ALTI2C1 = OFF
#pragma config ALTI2C2 = OFF
#pragma config ALTI2C3 = ON
#pragma config SMB3EN = SMBUS2
#pragma config SPI2PIN = PPS

#pragma config CTXT1 = OFF
#pragma config CTXT2 = OFF
#pragma config CTXT3 = OFF
#pragma config CTXT4 = OFF

#pragma config BSEQ = 4095
#pragma config IBSEQ = 4095

#pragma config BTMODE = SINGLE
