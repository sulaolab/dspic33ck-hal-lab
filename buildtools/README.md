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
| `flash-curiositynano.ps1` | Program the EV88G73A Curiosity Nano through its mass-storage interface. |
| `flashauto.ps1` | Program or reset the DM330030 profile through PKOB4. |

## Build

```powershell
# Incremental default build: EV88G73A + WM8904
.\buildtools\build.ps1

# Clean default build
.\buildtools\build.ps1 -Full

# Explicit board profile
.\buildtools\build.ps1 -Full -Configuration CK256MP508_DM330030

# Regenerate MPLAB makefiles after changing configurations.xml
.\buildtools\build.ps1 -Generate -Configuration CK64MC105_EV88G73A

# Remove generated outputs
.\buildtools\build.ps1 -Clean

# Compile-only transport checks
.\buildtools\check-tdm-feasibility.ps1
.\buildtools\check-dma-feasibility.ps1
```

`CK64MC105_EV88G73A` is the default and hardware-verified profile.
`CK256MP508_DM330030` is build-verified only. Board-owned source files have
configuration guards, so an incorrect inclusion fails the build instead of
silently applying one board's register settings to another.

## Program a board

```powershell
# EV88G73A Curiosity Nano: list or program the mass-storage target
.\buildtools\flash-curiositynano.ps1 -List
.\buildtools\flash-curiositynano.ps1

# DM330030: list the available PKOB4 target, then program a selected board
.\buildtools\flashauto.ps1 -List
.\buildtools\flashauto.ps1 -Configuration CK256MP508_DM330030 -Serial <SERIAL>
```

The EV88G73A and DM330030 use different programming mechanisms. Do not use a
successful operation on one profile as evidence that the other profile was
programmed or verified.

## Audio-safe programming

Before programming a running EV88G73A audio board, stop the stream and confirm
that the codec is analog-muted. `flash-curiositynano.ps1` performs this check
by default and refuses the copy if the expected confirmation is absent.

The opt-out is intentional and should be used only after the output has been
made safe by another means:

```powershell
.\buildtools\flash-curiositynano.ps1 -StopAudioCommand none
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
