# Source layout

The source tree separates portable HAL interfaces, device-register adapters,
board wiring, and application policy. This boundary keeps a board-specific pin
or clock decision from becoming an implicit property of a reusable interface.

## Top-level source areas

| Path | Responsibility |
| --- | --- |
| `src/hal_clock`, `hal_dma`, `hal_gpio`, `hal_i2c`, `hal_reset`, `hal_spi_i2s_tdm`, `hal_timer`, `hal_uart` | Public HAL interfaces and dsPIC33CK implementations. |
| `src/hal_adc` | Board-facing analog adapter boundary. |
| `src/chip_drivers` | Device-oriented helpers not exposed as HAL APIs. |
| `src/boards/ev88g73a`, `src/boards/dm330030`, `src/boards/ev08p02a` | Board pin routing, boot policy, and other physical facts. |
| `src/app` | Audio, DSP, codec, timer, and application policy. |
| `src/uart_app`, `src/uart_platform` | Console commands and board-independent console transport support. |

## Ownership rules

- HAL code owns reusable operations and validates parameters at its public
  boundary.
- Board code owns connector routing, configuration bits, oscillator selection,
  and peripheral ownership for a concrete board profile.
- Application code owns audio routing, DSP policy, console command behaviour,
  and feature selection.
- Device-register accesses that express a reusable peripheral operation belong
  behind the corresponding HAL adapter. Pin or board facts remain in the board
  layer.

## Board profiles

The MPLAB configurations select one board profile at a time. Board-owned source
files include configuration guards so an incorrect inclusion is diagnosed at
build time instead of compiling one board's register writes into another board
image.

`CK64MC105_EV88G73A` is the default profile. `CK256MP508_DM330030` remains a
separate build profile with its own device, linker script, and board source.
Neither profile may assume the other's pin routing, debugger, oscillator, or
codec-clock arrangement.

## Audio and console boundaries

The WM8904 integration lives in the application and board layers. The SPI/I2S/
TDM HAL owns framed transport and DMA mechanics; it does not own codec policy or
application buffers.

Console commands are implemented in `src/uart_app`. Board-specific output and
transport initialization stay behind their platform and board seams, so command
semantics do not encode a physical UART or debugger choice.

For CK-specific peripheral limitations, see
[`ck_hal_reference.md`](ck_hal_reference.md). For verified board behaviour and
remaining hardware limits, see [`ck_hardware_notes.md`](ck_hardware_notes.md).
