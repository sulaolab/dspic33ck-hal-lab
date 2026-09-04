# dsPIC33CK HAL lab

Firmware and HAL integration lab for dsPIC33CK boards. The default target is
the **dsPIC33CK64MC105 EV88G73A Curiosity Nano** with a WM8904 mikroBUS codec
board. The repository contains the application used to exercise the CK HALs,
the board profiles, and build helpers for the supported configurations.

## Supported configurations

| Configuration | Board / MCU | Status |
| --- | --- | --- |
| `CK64MC105_EV88G73A` | EV88G73A Curiosity Nano / dsPIC33CK64MC105 | Default configuration; hardware-verified for UART, I2C, TDM8 audio, AVAS, and traps. |
| `CK256MP508_DM330030` | DM330030 Curiosity / dsPIC33CK256MP508 | Build coverage only. Hardware behaviour has not been verified on this board. |

The two profiles have different devices, linker scripts, pin routing,
configuration bits, and programming paths. A successful build for one profile
does not validate the other.

## Hardware

The verified EV88G73A setup uses a Curiosity Nano with the WM8904 mikroBUS
codec board. The codec board design is available separately at
[sulaolab/EasyEDA-WM8904-mikroBUS](https://github.com/sulaolab/EasyEDA-WM8904-mikroBUS).

<img src="docs/images/dspic33ck-snap001.jpg" alt="dsPIC33CK Curiosity Nano with the WM8904 mikroBUS codec board" width="600">

The default audio path is TDM8 with the WM8904 board. Other audio formats,
clock roles, and board combinations require validation on the target hardware.

## Prerequisites

- MPLAB X v6.30
- XC-DSC v3.31.01
- dsPIC33CK-MC_DFP 1.10.386 for `CK64MC105_EV88G73A`
- dsPIC33CK-MP_DFP 1.15.423 for `CK256MP508_DM330030`

The source project is `firmware.X`; application and HAL source is under `src`.

## Quick start

From the repository root:

```powershell
# Default EV88G73A build
.\buildtools\build.ps1

# Explicit clean build
.\buildtools\build.ps1 -Full -Configuration CK64MC105_EV88G73A

# Build-only DM330030 configuration
.\buildtools\build.ps1 -Full -Configuration CK256MP508_DM330030
```

See [`buildtools/README.md`](buildtools/README.md) for build, programming, and
audio-safety guidance.

## Audio controls

The default EV88G73A ROMイメージ supports these audio commands:

| Command | Effect |
| --- | --- |
| `*tp` / `?tp` | Advance or report the block path (`copy`, `mute`, `tone`, `gain`). |
| `*ti<NNNN>` / `*to<NNNN>` | Set pre/post gain in signed tenths of a dB. |
| `*tq` / `*tq0000` / `*tq0001` / `?tq` | Stop, start, or report the fixed-period TDM1 load line. |
| `*td[NN]` / `?td` | Run or report the declick restart strategy. |
| `*ts` / `?ts` | Stop the stream with analog mute, or report its state. |
| `*tr` | Restart the stream after a stop. |

`SW0` on the Curiosity Nano toggles audio mute. The audio ramp is deliberately
long enough to be audible during demonstration; tune it in the application
configuration if a shorter transition is required.

## Documentation

| Document | Contents |
| --- | --- |
| [`docs/ck_hal_reference.md`](docs/ck_hal_reference.md) | CK HAL constraints for clock, DMA, I2C, PPS, and TDM. |
| [`docs/ck_hardware_notes.md`](docs/ck_hardware_notes.md) | Hardware verification scope and current board/toolchain limits. |
| [`docs/ck_source_layout.md`](docs/ck_source_layout.md) | Source-tree ownership and board/application boundaries. |

Standalone CK HAL repositories are available for
[clock](https://github.com/sulaolab/nora-hal-dspic33ck-clock),
[dma](https://github.com/sulaolab/nora-hal-dspic33ck-dma),
[gpio](https://github.com/sulaolab/nora-hal-dspic33ck-gpio),
[i2c](https://github.com/sulaolab/nora-hal-dspic33ck-i2c),
[spi-i2s-tdm](https://github.com/sulaolab/nora-hal-dspic33ck-spi-i2s-tdm),
[timer](https://github.com/sulaolab/nora-hal-dspic33ck-timer), and
[uart](https://github.com/sulaolab/nora-hal-dspic33ck-uart).

## Licensing

Project-owned source is licensed under [MIT-0](LICENSE). Third-party notices
and license texts are in [`LICENSES/`](LICENSES/).
