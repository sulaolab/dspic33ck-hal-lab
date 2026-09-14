# dsPIC33CK HAL reference

This document records the dsPIC33CK-specific constraints of the HALs used by
this lab. It is a hardware and integration reference; application policy and
board wiring remain in the board and application layers.

## Supported profiles

| Configuration | MCU | Board | Status |
| --- | --- | --- | --- |
| `CK64MC105_EV88G73A` | dsPIC33CK64MC105 | EV88G73A Curiosity Nano | Default profile; hardware-verified for UART, I2C, TDM8/WM8904 audio, AVAS, and traps. |
| `CK256MP508_DM330030` | dsPIC33CK256MP508 | DM330030 Curiosity | Build-verified only; do not treat audio or peripheral behaviour as hardware-verified. |

Build either configuration with:

```powershell
pwsh buildtools/build.ps1 -Configuration CK64MC105_EV88G73A
pwsh buildtools/build.ps1 -Configuration CK256MP508_DM330030
```

Build outputs are isolated below `firmware.X/build/<configuration>/` and
`firmware.X/dist/<configuration>/`.

## Clock

The dsPIC33CK uses one classic system-clock tree. Clock switching is requested
through `OSCCON.OSWEN` and is complete only after the requested state and
`OSCCON.LOCK` have been observed.

- `Fvco = (Fin / PLLPRE) * PLLFBDIV`
- `Fosc = Fvco / (POST1DIV * POST2DIV)`
- `Fcy = Fosc / 2`

The default EV88G73A operating point is `Fosc = 200 MHz` and `Fcy = 100 MHz`.
Timer1 is intentionally clocked from the fixed FRC, not from `Fcy`; code that
needs a peripheral clock must use the HAL-reported operating point rather than
assuming a board frequency.

## DMA

The CK DMA controller has four channels and supports byte or 16-bit-word
elements only. A 32-bit transfer element is unsupported.

- `DMACNTn` holds the transfer count; do not subtract one.
- Program both `DMAL` and `DMAH` before enabling DMA, otherwise valid RAM can
  be outside the permitted address window.
- `DMAINTn.CHSEL` uses the controller's peripheral-trigger encoding, not a CPU
  interrupt-vector number.
- `DMAINTn` combines trigger selection, status flags, and `HALFEN`; clear only
  status bits so a running transfer retains its configuration.

The HAL exposes logical DMA triggers and rejects a trigger not supported by
this silicon.

## I2C master

The CK I2C controller is the classic 16-bit form:

- control registers are `I2CxCONL` and `I2CxCONH`;
- status is `I2CxSTAT`;
- baud rate is `I2CxBRG`; and
- there is no `STAT2` or hardware bus-idle timeout equivalent.

For master transmit, wait for the request bit to retire, then use `TRSTAT` and
`ACKSTAT` to determine completion and acknowledgement. The CK BRG calculation
uses a 130 ns delay term. Confirm a 100 kHz target on the actual bus before
depending on it; the verified audio-board path uses 400 kHz.

## GPIO and PPS

The public GPIO/PPS API accepts physical remappable pins only. On the supported
CK parts these are `RP32` through `RP79` on ports B, C, and D. Virtual PPS pins
are internal routing resources and are not GPIO handles.

PPS lock changes must use `__builtin_write_RPCON()`; direct writes to the lock
bit do not perform the required unlock sequence. Register banks are not a
contiguous pin-number array, so output routing uses the device-specific lookup
helpers rather than computed register pointers.

## SPI, I2S, and TDM

The transport uses 16-bit DMA elements. A 32-bit audio slot is carried as two
wire words, with enhanced buffering disabled for DMA operation.

- `FRMCNT` is expressed in wire words, not 32-bit audio slots.
- The implementation configures frame sync through PPS and CLC1; this routing
  is part of the transport setup and is restored when the instance is released.
- The currently verified audio envelope is one clock domain. Co-clocked
  multi-instance frame-sync sharing is not provided.
- CMSIS-SAI is a layer above this HAL and is not part of its transport API.

## Timer, UART, reset, and ADC boundaries

The Timer1 tick, UART transport, reset-cause reporting, and board-level ADC
adapters are separate HAL/application seams. Board code supplies pin routing,
clock startup, and device-specific reset or analog policy; shared interfaces do
not imply interchangeable board wiring.

See [`ck_hardware_notes.md`](ck_hardware_notes.md) for verified hardware
behaviour and operational limits, and [`ck_source_layout.md`](ck_source_layout.md)
for source-tree ownership boundaries.
