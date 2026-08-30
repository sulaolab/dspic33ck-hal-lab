# Build Tools

Command-line helpers for this dsPIC33CK lab repository.

These tools are intentionally CK-specific and do not depend on the AK HAL
starter tools.

## Scripts

| Script | Purpose |
| --- | --- |
| `build.ps1` | Build a configuration of `firmware.X` by driving the same tools MPLAB X drives: `prjMakefilesGenerator` then `make`. |
| `migrate_configurations_xml.ps1` | One-shot, already applied. Rebuilt `configurations.xml` when it had gone stale. Not part of the build — see its header. |
| `add_board_ownership_guards.ps1` | One-shot, already applied. Added the `#error` ownership guard to every board-owned `.c`. |
| `check-tdm-feasibility.ps1` | Compile the CK SPI framed-mode TDM feasibility probe without linking it into the demo firmware. |
| `check-dma-feasibility.ps1` | Compile the CK SPI DMA ping-pong feasibility probe without linking it into the demo firmware. |
| `flashauto.ps1` | Flash `firmware.X` to the CK Curiosity over its on-board PKOB4, then reset. Wraps the self-made `flash_pkob4.exe` / `reset_pkob4.exe`. |
| `flash-curiositynano.ps1` | Flash `CK64MC105_EV88G73A` to the Curiosity Nano via its drag-and-drop mass-storage interface (copy HEX, poll `STATUS.TXT`). Separate mechanism from `flashauto.ps1`'s PKOB4/ICSP path. |

## Where the build is defined

`firmware.X/nbproject/configurations.xml` — **not** `build.ps1`.

It lists every source once and each configuration excludes what it does not build
(`<item ex="true">`), and it also owns the device, the DFP pack, the include
directories and the optimisation level. `build.ps1` reads it via
`prjMakefilesGenerator`, so the command line and MPLAB X build the same thing and
there is no list to keep in sync. Add or remove a source in the IDE (or by hand in
the xml); do not add it to a script.

Two configurations:

| Configuration | Board | Part |
| --- | --- | --- |
| `CK256MP508_DM330030` | DM330030 Curiosity | dsPIC33CK256MP508 |
| **`CK64MC105_EV88G73A`** (**default**) | EV88G73A Curiosity Nano, WM8904 codec | dsPIC33CK64MC105 |

`build.ps1` with no `-Configuration` builds **`CK64MC105_EV88G73A`** — the board this
lab has on the bench. It used to default to `CK256MP508_DM330030`, which meant a bare
invocation built the board nobody here can run, and said nothing about it. DM330030 is
still worth building on purpose: it is the roomy configuration (14 % full against
EV88G73A's 97 %), so it catches exclusion and board-ownership mistakes that fit
silently into the other one.

Each board-owned source `#error`s if it is compiled into the wrong
configuration, so an exclusion mistake in the xml stops the build with a message
naming the file and where to fix it, instead of quietly compiling one board's
register writes into the other board's image.

## Common Commands

Run these from the repository root:

```powershell
# Incremental build of the default configuration (EV88G73A + WM8904)
.\buildtools\build.ps1

# Clean build (removes build/, dist/ and the generated makefiles first)
.\buildtools\build.ps1 -Full

# The other board, by name
.\buildtools\build.ps1 -Full -Configuration CK256MP508_DM330030

# A one-off macro for this build only. Refused if the configuration already
# defines it in the xml -- MP_EXTRA_CC_PRE lands before the xml's own flags, so
# the xml would win and the value here would be discarded in silence.
.\buildtools\build.ps1 -Configuration CK64MC105_EV88G73A -Define EV88G73A_WM8904_DSPIC_IS_MASTER=1

# Regenerate the makefiles after editing the xml, without building
.\buildtools\build.ps1 -Generate -Configuration CK64MC105_EV88G73A

# Clean outputs only
.\buildtools\build.ps1 -Clean

# Compile-only CK SPI TDM feasibility check
.\buildtools\check-tdm-feasibility.ps1

# Compile-only CK SPI DMA ping-pong feasibility check
.\buildtools\check-dma-feasibility.ps1

# Flash + reset the DM330030 profile (auto-detects the serial if exactly one PKOB4 is connected)
.\buildtools\flashauto.ps1 -Configuration CK256MP508_DM330030

# List connected PKOB4 targets / flash a specific board / reset only / dry-run
.\buildtools\flashauto.ps1 -List
.\buildtools\flashauto.ps1 -Serial <SERIAL>
.\buildtools\flashauto.ps1 -Reset -Serial <SERIAL>
.\buildtools\flashauto.ps1 -DryRun -Serial <SERIAL>

# Flash the EV88G73A profile via Curiosity Nano drag-and-drop (auto-detects the drive if exactly one is connected)
.\buildtools\flash-curiositynano.ps1

# List connected Curiosity Nano drag-and-drop drives / dry-run / explicit drive override
.\buildtools\flash-curiositynano.ps1 -List
.\buildtools\flash-curiositynano.ps1 -DryRun
.\buildtools\flash-curiositynano.ps1 -DriveLetter D

# Skip the automatic pre-copy mute (see the next section for what it is and is not)
.\buildtools\flash-curiositynano.ps1 -StopAudioCommand none
```

### EV88G73A / WM8904: POWER-CYCLE AFTER FLASHING BEFORE JUDGING THE AUDIO

Observed 2026-08-07 and reproduced from a clean explanation: the first boot after
drag-and-drop programming produced a loud **square-wave-like tone** on the analog output on
every path carrying non-zero samples. The identical image, after a power OFF/ON, was
correct. Nothing in the image was at fault.

> **2026-08-09: the explanation below did not survive re-testing — the rule did.**
> Phase 3 of the NORA alignment work reproduced this exact condition (`*ts`, drag-and-drop
> write, then `*sr` — a software reset, so the codec stayed warm) and the audio was **clean on
> all four paths**; a power cycle afterwards was clean too. A warm codec is therefore **not
> sufficient** to produce the square wave. Two things were fixed between 08-07 and 08-09 and
> are the better suspects: the codec not actually being muted before the write, and the
> 1-bit-delay framing defect. Keep flashing and power-cycling as written — it is cheap and it
> keeps the verdict clean — but **do not close a square wave as "the codec was warm."** Check
> that `*ts` actually logged `analog mute verified`, then check the framing.

The cause was believed to be this procedure. `*ts` below leaves the WM8904 analog-muted with TDM/DMA halted,
and drag-and-drop programming resets only the dsPIC, via **MCLR** — the boot banner says
`Reset = EXTR(MCLR)`, against `POR(power-on)` after a real power cycle. So the codec never
loses power: it keeps the register and clock state `*ts` left it in, and the new image
initialises on top of that, through the driver's distinct warm path
(`wm8904_apply_dc_servo_warm()`).

So: **flash, then cycle kit power, and only then listen.** A square wave on the first boot
after programming is this, not a regression in what you just built — check it before
bisecting anything. Two traps if you do go after it with a scope:

- `*tp0001` (mute) going silent proves nothing about wire alignment: all-zero samples
  survive a one-bit shift unchanged.
- `*tp0002` (tone) is not a trusted reference on this board — the console log shows it has
  never been exercised, so a bad tone may be the tone path rather than the framing.

### EV88G73A / WM8904: stop audio before flashing

When an EV88G73A is running the WM8904 TDM loopback, **send `*ts` and confirm its
reply before copying the HEX file.** The Curiosity Nano debugger's console traffic can
otherwise be received as TDM data and heard as noise while the programmer resets the
target. `*ts` applies the WM8904 analog mute first and then halts the TDM/DMA instance;
`?ts` reports the state without changing it.

**`flash-curiositynano.ps1` makes `*ts` a safety gate.** Before it copies the
ROMイメージ it confirms that the monitor is the connected CK board and the same Curiosity
Nano kit selected for the copy, rejects a held transmit gate, records the monitor-log
position, sends `*ts`, then requires the new response `analog mute verified, TDM/DMA
halted`. A delivery to the monitor, an old log line, `analog mute NOT verified`, or an
unimplemented `*ts` cannot pass this check; each aborts before the copy. The monitor's
HTTP API is the only console path used.

`-StopAudioCommand none` is an explicit escape hatch for a board whose output has been
made safe by another means. It prints a warning and intentionally disables this gate;
it is not the normal flashing procedure.

The script copies to a content-hash-qualified **8.3 filename** on the `CURIOSITY` volume
(for example `F1a2b3c.hex`). This is intentional: the debugger's mass-storage emulator
and Windows can retain an old same-name file, resetting the target without changing its
ROMイメージ. The kit's volume rejects long filenames, so `-ProgramFileName` must also use
at most eight characters before `.hex`.

**Gate on the phrase `analog mute verified`, and on nothing weaker.** The firmware
readback-checks the mute at the codec and says so; when that check fails it answers
`analog mute NOT verified` — same command, transport halted, but HPOUT possibly still
live, which is precisely the hazard this step exists to prevent. The two wordings are
chosen so that a substring match for `analog mute verified` cannot pass on the failure
line. (Older firmware said only `analog mute`, with no verification behind it; a match
against that string is not a safety check.)

Do **not** open the board's COM port directly. The lab monitor owns that port, and a
second serial client can both disrupt the monitor and add the very traffic this step is
meant to prevent. The script uses its HTTP API. Its default safe invocation is:

```powershell
.\buildtools\flash-curiositynano.ps1 -DriveLetter D `
    -MonitorUrl 'http://127.0.0.5:8080'
```

`*ts` is available only in firmware that contains this command. For the one bootstrap
flash from an older image, reduce or physically mute the analog output first and pass
`-StopAudioCommand none` deliberately. A reboot after programming runs the normal
boot-time codec start sequence and enables audio again.

## Tool Versions

The current defaults match the locally successful MPLAB X build:

- MPLAB X: `v6.30`
- XC-DSC: `v3.31.01`
- DFP: `dsPIC33CK-MP_DFP 1.15.423`

Override them if needed:

```powershell
.\buildtools\build.ps1 -XcDscVersion 3.31.01 -MplabXVersion 6.30 -DfpVersion 1.15.423
```

## Notes

- The script is based on the successful MPLAB X generated build log, but avoids
  committing `nbproject/Makefile-*.mk` and other generated files.
- `flashauto.ps1` currently supports only the DM330030 profile
  (`-Device dsPIC33CK256MP508`, config `CK256MP508_DM330030`) — the flash/reset exes are
  device-parameterized, not AK-specific. The `flash_pkob4.exe` / `reset_pkob4.exe`
  binaries (~68 MB each) are NOT vendored here; the script resolves them from the
  shared `vscode-home/_flash_reset_tools` (sibling), or pass `-ToolsDir` / set
  `$FLASH_RESET_TOOLS`.
- HW to confirm on first connect: the DM330030 debugger enumerates as PKOB4
  (`flashauto.ps1 -List` shows a serial) and `/TPPKOB4` is the right tool type.
   If `-List` is empty, the debugger USB PID differs — add it in `flash_pkob4`.
- `CK64MC105_EV88G73A` is hardware-verified (see
  `docs/ck_silicon_findings.md`) and flashes via `flash-curiositynano.ps1`,
  which uses the Curiosity Nano's drag-and-drop mass-storage interface, not
  ICSP. `flashauto.ps1` still intentionally refuses this configuration -- that
  guard is unrelated to and unaffected by the new script, since the two use
  entirely different programming mechanisms.
- `flash-curiositynano.ps1` detects the target drive by its `CURIOSITY`
  filesystem label (drive letters are not stable) and checks `KIT-INFO.TXT`'s
  `Device:` line against `-ExpectedDevice` before ever copying, so it refuses to
  flash a different board that happens to be connected at the same time.
  `STATUS.TXT` is confirmed to be a firmware-virtualized view (fixed timestamp,
  fixed 512-byte size), not a real file -- the script polls it for a *stable*
  reading (same content on two consecutive 1s-apart reads) rather than trusting
  the first read, since an immediate read after copying can return a stale
  cached value.
