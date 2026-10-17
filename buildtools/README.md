# Build tools

Command-line helpers for building and programming the dsPIC33CK lab profiles.
Run them from the repository root. The build definition remains in
`firmware.X/nbproject/configurations.xml`; `build.ps1` drives the same MPLAB X
tools that the project uses.

## Supported scripts

| Script | Use |
| --- | --- |
| `build.ps1` | Build, clean, or regenerate MPLAB makefiles for a configuration. |
| `check-tdm-feasibility.ps1` | Compile the CK framed-TDM feasibility probe without linking it into firmware. |
| `check-dma-feasibility.ps1` | Compile the CK DMA ping-pong feasibility probe without linking it into firmware. |
| `flash-mdb-nano.ps1` | Program the EV08P02A Curiosity Nano with mdb in programmer mode, behind the pre-flash mute. |
| `flash-curiositynano.ps1` | Program a Curiosity Nano through its mass-storage interface (only kits whose `KIT-INFO.TXT` says `Drag and drop: Yes`; EV08P02A says No). |
| `flashauto.ps1` | Program or reset DM330030 / EV97U97A through PKoB4. |
| `identify-pkob4.ps1` | Map PKoB4 USB serial to CDC COM; optionally probe a named MCU without programming. |

## Build

```powershell
# Incremental default build: EV08P02A + WM8904
.\buildtools\build.ps1

# Clean default build
.\buildtools\build.ps1 -Full

# Explicit board profile
.\buildtools\build.ps1 -Full -Configuration CK256MP508_DM330030

# Regenerate MPLAB makefiles after changing configurations.xml
.\buildtools\build.ps1 -Generate -Configuration CK256MC005_EV08P02A

# Remove generated outputs
.\buildtools\build.ps1 -Clean

# Compile-only transport checks
.\buildtools\check-tdm-feasibility.ps1
.\buildtools\check-dma-feasibility.ps1
```

`CK256MC005_EV08P02A` is the default profile. `CK64MC105_EV88G73A` was retired
on 2026-10-15; its last buildable tree is retained in the development archive,
not in this public snapshot (maintainer tag `ck-ev88g73a-final-20261014`).
`CK256MP508_DM330030` is build-verified only. Board-owned source files have
configuration guards, so an incorrect inclusion fails the build instead of
silently applying one board's register settings to another.

## Program a board

```powershell
# EV08P02A Curiosity Nano: mdb in programmer mode (this kit has no drag-and-drop)
.\buildtools\flash-mdb-nano.ps1 -DryRun
.\buildtools\flash-mdb-nano.ps1

# DM330030: list the available PKOB4 target, then program a selected board
.\buildtools\flashauto.ps1 -List
.\buildtools\flashauto.ps1 -Configuration CK256MP508_DM330030 -Serial <SERIAL>
```

The EV08P02A and DM330030 use different programming mechanisms. Do not use a
successful operation on one profile as evidence that the other profile was
programmed or verified.

## Audio-safe programming

Before programming a running EV08P02A audio board, stop the stream and confirm
that the codec is analog-muted. `flash-mdb-nano.ps1` sends `*ts` and waits a
bounded time for the confirmation by default; `-RequireVerifiedMute` makes it
refuse the write when the confirmation is absent.

The opt-out is intentional and should be used only after the output has been
made safe by another means:

```powershell
.\buildtools\flash-mdb-nano.ps1 -StopAudioCommand none
```

After programming, power-cycle the kit before judging audible output. The
programming reset does not necessarily remove the codec's prior state.

## Tool versions

The current project defaults are MPLAB X v6.30, XC-DSC v3.31.01, and the DFP
version selected by the target configuration. Override a version only when the
installed toolchain requires it:

```powershell
.\buildtools\build.ps1 -XcDscVersion 3.31.01 -MplabXVersion 6.30 -DfpVersion 1.15.423
```

Generated makefiles and build outputs can be removed with `build.ps1 -Clean`.

## EV97U97A / WM8904

Build with `build.ps1 -Configuration CK1024MP710_EV97U97A`. WM8904 uses
mikroBUS A / J9 and its XTAL-to-MCLK jumper setting. The 2026-10-13 bring-up
confirmed AVAS sound and POT pitch change; see
[board port and operation guide](../docs/ck1024_touch_can_lin_port.md).

Before **flash, reset, or an MDB identity probe**, send `*ts` through the
serial-monitor bridge, confirm the fresh response
`analog mute verified, TDM/DMA halted`, and allow 1.5 seconds to settle.
Stopping AVAS or pressing the DSP mute button does not replace analog mute.
Use `/command_wait` to arm the reply matcher before the command.

For this configuration, `flashauto.ps1` performs the stop automatically and
requires its confirmation. It resolves the monitor URL from the local
serial-monitor configuration, checks profile `ck`, and maps the monitor COM
back to the selected PKoB4 USB serial before sending anything. `-MonitorRoot`
can locate a separate serial-monitor checkout; `-MonitorUrl` accepts a URL
already resolved by the operator. No COM port is opened directly.

```powershell
# List only: no target reset and no COM open
.\buildtools\identify-pkob4.ps1 -Json

# Dry-run includes the planned stop, without sending it
.\buildtools\flashauto.ps1 -Configuration CK1024MP710_EV97U97A `
    -Serial <SERIAL> -ToolsDir <HELPER_DIRECTORY> -DryRun

# Strict *ts / analog mute readback / settle, then flash and reset
.\buildtools\flashauto.ps1 -Configuration CK1024MP710_EV97U97A `
    -Serial <SERIAL> -ToolsDir <HELPER_DIRECTORY>
```

After flash/reset, startup resumes audio automatically. Check `?gv` and `?ts`;
a manual stop can be undone with `*tr`. For recovery of an unresponsive image,
first disconnect or externally mute the analog output, then explicitly use
`-StopAudioCommand none`. This override is not the normal programming sequence.
The stop sequence reduces programming POP risk; pop-free startup and analog
output quality have not been qualified.
