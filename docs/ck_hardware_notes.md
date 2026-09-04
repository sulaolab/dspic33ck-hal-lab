# dsPIC33CK hardware notes

These notes distinguish tested behaviour from build-only coverage for the two
board profiles in this repository. They focus on current constraints that an
integrator can act on.

## Verification scope

| Area | EV88G73A / dsPIC33CK64MC105 | DM330030 / dsPIC33CK256MP508 |
| --- | --- | --- |
| Build | Verified | Verified |
| UART console | Verified | Not hardware-verified |
| I2C / WM8904 | Verified at 400 kHz | Not hardware-verified |
| SPI/I2S/TDM audio | Verified for the default TDM8 path | Not hardware-verified |
| AVAS and DSP load reporting | Verified | Not hardware-verified |
| Debugger programming path | Curiosity Nano mass-storage path | PKOB4 path only |

Treat the DM330030 profile as compile coverage until it has been exercised on
the target board.

## DMA and framed-audio constraints

The CK DMA engine uses 16-bit elements. The audio transport therefore carries a
32-bit sample slot as two wire words. DMA requires enhanced buffering to remain
disabled, and a DMA count is the requested count, not `count - 1`.

Frame count is measured in wire words. Incorrectly treating it as an audio-slot
count produces an invalid frame geometry. The default EV88G73A path is verified
for TDM8. Other format, clock-role, and board combinations require target-board
confirmation.

## Clock and console

The default EV88G73A configuration runs at `Fosc = 200 MHz` / `Fcy = 100 MHz`.
UART baud selection must follow the achieved clock. If a configuration selects a
lower clock, the console uses a representable fallback rate rather than assuming
230400 baud is valid.

The DM330030 configuration has different oscillator and configuration-bit
constraints. A successful compile does not confirm its clock switch, codec clock,
or console behaviour on hardware.

## I2C and codec board

The verified codec path uses I2C1 at 400 kHz. The 100 kHz BRG delay term has not
been validated on this board, so check the waveform before relying on that rate.

The EV88G73A audio path uses the WM8904 mikroBUS add-on board. Before copying a
new ROMイメージ to a running audio board, stop the stream and mute the codec as
described in [`../buildtools/README.md`](../buildtools/README.md). After
programming, power-cycle the kit before judging audible output.

## Reset and traps

A debugger reset and a power-on reset are different conditions. Reset-cause
reporting captures the device state during board initialization; do not infer a
power cycle from a USB reconnect or from a debugger reconnect.

The trap handlers provide diagnostic evidence but cannot make every hardware
fault recoverable. In particular, a stack overflow can prevent useful software
reporting. Use the board's reset and diagnostic output when qualifying fault
behaviour.

## Toolchain limits

The project uses the MPLAB X and XC-DSC versions listed in the root
README. High optimization and register pressure can expose compiler or assembler
limitations; retain the project's configuration-specific optimization settings
when reproducing a measured result.
