param(
    [switch]$List,
    [switch]$DryRun,
    [switch]$Verbose,
    [switch]$Quiet,
    [ValidateSet('CK64MC105_EV88G73A')]
    [string]$Configuration = 'CK64MC105_EV88G73A',
    [string]$Root = (Get-Location).Path,
    [string]$ProjectDir,
    [string]$Hex,
    [string]$DriveLetter,
    [string]$ExpectedDevice = 'dsPIC33CK64MC105',
    [int]$StatusTimeoutSec = 30,
    # How long to keep watching for evidence that a stable, plausible-looking
    # STATUS.TXT read is actually post-copy rather than the OS-cached pre-copy
    # value. Exceeded => Indeterminate, not Success. See Wait-ProgrammingStatus.
    [int]$TransitionTimeoutSec = 8,
    <#
      Authoritative verification: after programming, ask the running
      serial_monitor to wait for this substring on the board's console. Turns a
      guess into a real confirmation.

      Left empty for an EV88G73A build, this now DEFAULTS to the build ID that
      build.ps1 stamped, read from the generated header beside the object files.
      That is deliberate rather than convenient. STATUS.TXT is documented by the
      kit itself as OS-cached, so on a run where the volume does not visibly drop
      there is no post-copy evidence and the verdict is Indeterminate -- which is
      what every flash returned until the ID was passed by hand. Since build.ps1
      writes the ID to a known path, requiring the caller to copy it across was
      just an opportunity to forget.

      Pass -VerifyUartContains 'none' to opt out, or any other string to match
      something else.
    #>
    [string]$VerifyUartContains,
    <#
      Best-effort mute before programming: this console command is sent through
      the serial_monitor just before the copy, so the target is not reset while
      the codec is driving HPOUT. Pass 'none' to skip it.

      Deliberately NOT a safety gate. Nothing here checks that the mute took, and
      `*ts` has a documented failure shape that this cannot see -- it halts the
      TDM/DMA transport and answers `analog mute NOT verified` when its codec
      readback fails, which leaves the output possibly live. When the mute has to
      be certain, use the gated sequence in buildtools/README.md, which matches on
      `analog mute verified` and refuses to flash without it.
    #>
    [string]$StopAudioCommand = '*ts',
    [string]$MonitorUrl = 'http://127.0.0.5:8080',
    [int]$UartTimeoutSec = 20
)

$ErrorActionPreference = 'Stop'

if ($Verbose -and $Quiet) {
    throw "Use either -Verbose or -Quiet, not both."
}

function Write-Status {
    param(
        [string]$Message
    )

    if (-not $Quiet) {
        Write-Host $Message
    }
}

function Resolve-BuildRoot {
    param(
        [string]$RequestedRoot
    )

    $resolvedRoot = (Resolve-Path -LiteralPath $RequestedRoot).Path

    if ((Split-Path -Leaf $resolvedRoot) -like '*.X' -and
        (Test-Path -LiteralPath (Join-Path $resolvedRoot 'nbproject'))) {
        return (Split-Path -Parent $resolvedRoot)
    }

    return $resolvedRoot
}

function Resolve-MplabProjectDir {
    param(
        [string]$Root,
        [string]$RequestedProjectDir
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedProjectDir)) {
        if ([System.IO.Path]::IsPathRooted($RequestedProjectDir)) {
            return (Resolve-Path -LiteralPath $RequestedProjectDir).Path
        }
        return (Resolve-Path -LiteralPath (Join-Path $Root $RequestedProjectDir)).Path
    }

    $projects = @(Get-ChildItem -LiteralPath $Root -Directory -Filter '*.X' |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'nbproject') })

    if ($projects.Count -eq 0) {
        throw "No MPLAB X project directory (*.X with nbproject) found under: $Root"
    }
    if ($projects.Count -gt 1) {
        $names = ($projects | ForEach-Object { $_.Name }) -join ', '
        throw "Multiple MPLAB X project directories found: $names. Specify -ProjectDir."
    }

    return $projects[0].FullName
}

function Resolve-ProductionHex {
    param(
        [string]$RequestedHex,
        [string]$ProjectDir,
        [string]$Configuration
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedHex)) {
        return (Resolve-Path -LiteralPath $RequestedHex).Path
    }

    $projectName = Split-Path -Leaf $ProjectDir

    <#
      One deterministic path per configuration, because the MPLAB make system owns
      the output layout: dist\<conf>\production\<project>.production.hex.

      This replaces a scheme where build.ps1 wrote each -Define combination to its
      own dist\<conf>-<variant>\ directory, and this function refused to
      auto-select the plain dist\<conf>\ path on the grounds that it could only be
      a stale leftover. Under the make build that plain path is the ONLY path, so
      the old rule now rejects the single correct answer.

      The concern behind that scheme was real and has not gone away: two builds of
      one configuration differing only in -Define land on the same file, so the
      hex on disk does not say which one it is. What changed is where the
      protection lives. Distinct source sets are now distinct CONFIGURATIONS, each
      with its own directory, so they cannot collide at all. For the remaining
      define-only differences the answer is -VerifyUartContains with the build ID
      that build.ps1 prints, which checks the image that is actually RUNNING
      rather than guessing from a filename -- strictly better evidence than
      directory separation ever gave.
    #>
    $hex = Join-Path $ProjectDir "dist\$Configuration\production\$projectName.production.hex"

    if (Test-Path -LiteralPath $hex) {
        # Leftover per-variant directories from the previous scheme are harmless
        # to the build but confusing to a human comparing timestamps, so say they
        # are there and say they are ignored.
        $stale = @(Get-ChildItem -LiteralPath (Join-Path $ProjectDir 'dist') -Directory `
                     -Filter "$Configuration-*" -ErrorAction SilentlyContinue)
        if ($stale.Count -gt 0) {
            Write-Host "Ignoring $($stale.Count) stale per-variant dist directory(ies) from the pre-make layout:"
            foreach ($s in $stale) { Write-Host "  $($s.FullName)" }
            Write-Host "  (safe to delete; build.ps1 -Full no longer writes there)"
        }
        Write-Status "HEX: $hex"
        return (Resolve-Path -LiteralPath $hex).Path
    }

    Write-Status "Looked for: dist\$Configuration\production\$projectName.production.hex"
    throw ("No built HEX found for $Configuration. Build it first " +
           "(.\buildtools\build.ps1 -Configuration $Configuration prints the 'Artifact:' path), " +
           'or pass -Hex.')
}

function Get-CuriosityVolumes {
    # Always a fresh call: the drive letter is not stable and the drive can
    # transiently disappear/reappear during programming (see Wait-ProgrammingStatus).
    @(Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemLabel -eq 'CURIOSITY' })
}

function Read-KitInfo {
    param(
        [string]$DriveLetter
    )

    $path = "${DriveLetter}:\KIT-INFO.TXT"
    if (-not (Test-Path -LiteralPath $path)) {
        throw "KIT-INFO.TXT not found on ${DriveLetter}: -- is this really a Curiosity Nano drag-and-drop drive?"
    }

    $lines = Get-Content -LiteralPath $path
    $info = [ordered]@{
        Device = $null
        KitName = $null
        SerialNumber = $null
    }

    foreach ($line in $lines) {
        if ($line -match '^\s*Device:\s*(.+?)\s*$') {
            $info.Device = $matches[1]
        } elseif ($line -match '^\s*Kit name:\s*(.+?)\s*$') {
            $info.KitName = $matches[1]
        } elseif ($line -match '^\s*Kit USB serial number:\s*(.+?)\s*$') {
            $info.SerialNumber = $matches[1]
        }
    }

    return [pscustomobject]$info
}

function Resolve-CuriosityDrive {
    param(
        [string]$RequestedDriveLetter
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedDriveLetter)) {
        $letter = $RequestedDriveLetter.TrimEnd(':')
        $volumes = Get-CuriosityVolumes
        $match = $volumes | Where-Object { $_.DriveLetter -eq $letter }
        if (-not $match) {
            throw "No CURIOSITY-labeled drive found at ${letter}:. Connected CURIOSITY drives: $(($volumes | ForEach-Object { $_.DriveLetter }) -join ', ')"
        }
        return $letter
    }

    $volumes = Get-CuriosityVolumes
    if ($volumes.Count -eq 0) {
        Write-Host "No CURIOSITY-labeled drive found."
        Write-Host "Connect the Curiosity Nano board. If it's already connected, check the USB cable --"
        Write-Host "a marginal cable can enumerate only the debugger interface without the mass-storage drive"
        Write-Host "or CDC COM port (seen this session)."
        exit 2
    }
    if ($volumes.Count -eq 1) {
        $letter = $volumes[0].DriveLetter
        Write-Status "Drive: ${letter}: (auto-detected)"
        return $letter
    }

    Write-Host "Multiple CURIOSITY-labeled drives found. Refusing to choose a target automatically."
    Write-Host "Connected drives:"
    foreach ($vol in $volumes) {
        $info = Read-KitInfo -DriveLetter $vol.DriveLetter
        Write-Host "  $($vol.DriveLetter): device=$($info.Device) serial=$($info.SerialNumber)"
    }
    Write-Host ""
    Write-Host "Run again with an explicit drive, for example:"
    Write-Host "  .\buildtools\flash-curiositynano.ps1 -DriveLetter $($volumes[0].DriveLetter)"
    exit 2
}

function Assert-ExpectedDevice {
    param(
        [string]$DriveLetter,
        [string]$ExpectedDevice
    )

    $info = Read-KitInfo -DriveLetter $DriveLetter
    if ([string]::IsNullOrWhiteSpace($info.Device)) {
        throw "KIT-INFO.TXT on ${DriveLetter}: has no 'Device:' line -- refusing to flash an unidentified board."
    }
    if ($info.Device.Trim() -ne $ExpectedDevice.Trim()) {
        throw "Device mismatch: ${DriveLetter}: reports '$($info.Device)', expected '$ExpectedDevice'. Refusing to flash the wrong board. Pass -ExpectedDevice to override."
    }

    Write-Status "Device check: ${DriveLetter}: reports '$($info.Device)' (matches -ExpectedDevice)"
    return $info
}

function Get-StatusLine {
    param(
        [string]$RawContent
    )

    if ($RawContent -match '(?im)^Status:\s*(.+?)\s*$') {
        return $matches[1]
    }

    return $null
}

function Get-StatusVerdict {
    <#
      Classify the *text* of a stable STATUS.TXT read. Deliberately separate
      from deciding the overall outcome: this file is served from the OS cache
      (it says so itself), so text alone cannot tell a post-programming read
      from a pre-programming one. Whether the text may be believed at all is
      Wait-ProgrammingStatus's job -- see the note there.
    #>
    param(
        [string]$StatusLine
    )

    if ([string]::IsNullOrWhiteSpace($StatusLine)) {
        return 'Unknown'
    }
    if ($StatusLine -match '(?i)fail|error|bad|invalid|denied') {
        return 'Failure'
    }
    if ($StatusLine -match '(?i)ready') {
        return 'Ready'
    }

    return 'Unknown'
}

function Wait-ProgrammingStatus {
    <#
      Decide the outcome of a drag-and-drop programming attempt.

      The hard part is that STATUS.TXT is served from the OS file cache -- the
      file's own body says so and asks you to power-cycle before trusting it.
      A stable read therefore proves nothing on its own:

        * a leftover pre-flash "Ready." reads back stable and would look like
          a success we never actually observed, and
        * a leftover "Failed - hex parser failed." reads back stable and would
          condemn a programming run that in fact succeeded.

      Both were observed on this board. So a "Ready." text is only believed
      when something independent proves the read is *not* the pre-copy value:

        1. the CURIOSITY volume disappeared and/or came back (the board resets
           and re-enumerates when it programs), or a read of STATUS.TXT threw
           mid-operation -- either way the drive was disturbed after the copy;
        2. the status line differs from the pre-copy baseline.

      With neither, the verdict is Indeterminate -- for a "Failed" text just as
      much as for a "Ready." one. That is the honest answer: nothing
      distinguishes the read from a cached one. Note that a successful flash
      following a successful flash legitimately leaves the text at "Ready.", so
      evidence #2 alone can never be required.

      A "Failure" text needs the same evidence. It is tempting to treat failure
      as the safe direction and believe it unconditionally, but that is how a
      cached "Failed - hex parser failed." condemns a flash that worked -- which
      is exactly what happened on this board, repeatedly, across several later
      successful programmings. Unproven failure is not a failure, it is an
      unknown. So:

        evidence + Failure -> Failure
        evidence + Ready   -> Success
        no evidence        -> Indeterminate (either text)

      and only an image-specific UART marker may promote an evidence-free
      Failure to Success (see the caller). Evidence-backed Failure always wins
      over the UART, because then the debugger really did report a failure for
      this operation.

      Re-enumeration is followed by KIT SERIAL, not by "whichever CURIOSITY
      drive is left". With two Nanos attached, the target briefly vanishing
      while the other stays mounted would otherwise make the helper adopt the
      *other* board and read its STATUS.TXT -- programming board A and then
      verifying board B. Any drive whose KIT-INFO.TXT serial does not match is
      ignored.

      Polling starts immediately and runs fast (no initial settle sleep): the
      re-enumeration window is short, and a 1-2s sleep is how it gets missed.
    #>
    param(
        [string]$DriveLetter,
        [AllowNull()][string]$BaselineStatus,
        # Two separate pre-copy facts, because they license different evidence:
        #
        #   BaselineStatusAvailable -- the 'Status:' line was parsed before the
        #     copy, so a post-copy line differing from it is a real transition.
        #     Without this, comparing against a null baseline makes the first
        #     readable line differ by definition (PowerShell coerces a null
        #     [string] argument to empty), manufacturing evidence from nothing.
        #
        #   BaselineReadable -- the file itself could be read before the copy.
        #     Only then does a post-copy read EXCEPTION mean anything: the
        #     inference is "it was readable, now it is not, so the debugger is
        #     mid-operation". If it was already unreadable before the copy, an
        #     exception after the copy is just the same pre-existing condition
        #     and proves nothing.
        [bool]$BaselineStatusAvailable,
        [bool]$BaselineReadable,
        # Kit USB serial of the board actually being programmed.
        [AllowNull()][string]$KitSerial,
        [int]$StatusTimeoutSec,
        [int]$TransitionTimeoutSec
    )

    $pollMs = 250
    $startTime = Get-Date
    $deadline = $startTime.AddSeconds($StatusTimeoutSec)
    # Kept apart so each can be licensed independently -- see the param block.
    $volumeDisturbed = $false        # volume vanished / returned / changed letter
    $statusReadInterrupted = $false  # post-copy read of STATUS.TXT threw
    $changedFromBaseline = $false
    $previousLine = $null
    $rawContent = $null
    $stableCount = 0
    $firstStableAt = $null
    $currentDrive = $DriveLetter
    $targetEverLost = $false

    function Test-SameKit {
        param([string]$Letter, [string]$Serial)
        if ([string]::IsNullOrWhiteSpace($Serial)) { return $false }
        try {
            $info = Read-KitInfo -DriveLetter $Letter
            return ($info.SerialNumber -eq $Serial)
        } catch {
            return $false
        }
    }

    while ((Get-Date) -lt $deadline) {
        $volumes = Get-CuriosityVolumes
        $present = $volumes | Where-Object { $_.DriveLetter -eq $currentDrive }

        if ($present -and -not [string]::IsNullOrWhiteSpace($KitSerial)) {
            # The letter came back (or never left) -- make sure it is still OUR
            # board behind it. Drive letters get recycled between devices.
            if (-not (Test-SameKit -Letter $currentDrive -Serial $KitSerial)) {
                $present = $null
            }
        }

        if (-not $present) {
            # Tolerate either observed hardware behavior: the drive staying
            # mounted throughout programming, or briefly disappearing. When it
            # disappears, re-find it by kit serial -- the letter is not stable,
            # and another attached Nano must never be mistaken for this one.
            $reacquired = $null
            foreach ($v in $volumes) {
                if (Test-SameKit -Letter $v.DriveLetter -Serial $KitSerial) {
                    $reacquired = $v.DriveLetter
                    break
                }
            }

            if ($null -ne $reacquired) {
                if ($reacquired -ne $currentDrive) {
                    $volumeDisturbed = $true
                    Write-Status "Target kit $KitSerial reappeared as ${reacquired}: -- following it"
                    $currentDrive = $reacquired
                    $previousLine = $null
                    $stableCount = 0
                    $firstStableAt = $null
                    continue
                }
            } else {
                $volumeDisturbed = $true
                $targetEverLost = $true
                $previousLine = $null
                $stableCount = 0
                $firstStableAt = $null
                Write-Status "Target kit not currently present (re-enumerating) -- waiting..."
                Start-Sleep -Milliseconds $pollMs
                continue
            }
        }

        $statusPath = "${currentDrive}:\STATUS.TXT"
        try {
            $rawContent = [System.IO.File]::ReadAllText($statusPath)
        } catch {
            # The read threw. Whether that means anything depends on whether the
            # file was readable BEFORE the copy -- see the param block. Recorded
            # unconditionally, licensed as evidence below only if it was.
            $statusReadInterrupted = $true
            $previousLine = $null
            $stableCount = 0
            $firstStableAt = $null
            Start-Sleep -Milliseconds $pollMs
            continue
        }

        $line = Get-StatusLine -RawContent $rawContent
        if ($BaselineStatusAvailable -and
            -not [string]::IsNullOrWhiteSpace($line) -and
            $line -ne $BaselineStatus) {
            $changedFromBaseline = $true
        }

        if ($null -ne $previousLine -and $line -eq $previousLine) {
            $stableCount++
        } else {
            $stableCount = 1
            $firstStableAt = $null
        }
        $previousLine = $line

        if ($stableCount -ge 2) {
            if ($null -eq $firstStableAt) { $firstStableAt = Get-Date }

            $verdict = Get-StatusVerdict -StatusLine $line

            # A read exception only counts if the file HAD been readable before
            # the copy. Otherwise "it threw" describes a condition that already
            # existed and says nothing about this operation -- which is how an
            # unreadable baseline plus one post-copy exception plus the same old
            # cached "Ready." used to add up to a confident Success.
            $interruptionCounts = ($statusReadInterrupted -and $BaselineReadable)
            $haveEvidence = ($volumeDisturbed -or $interruptionCounts -or $changedFromBaseline)

            $classification = $null
            $reason = $null

            if ($verdict -eq 'Unknown') {
                $classification = 'Unknown'
            } elseif ($haveEvidence) {
                # The read is demonstrably post-copy, so its text is the answer.
                $classification = if ($verdict -eq 'Failure') { 'Failure' } else { 'Success' }
            } elseif (((Get-Date) - $firstStableAt).TotalSeconds -ge $TransitionTimeoutSec) {
                $classification = 'Indeterminate'
                $missing = @('the target volume never dropped or changed letter')
                if (-not $BaselineStatusAvailable) {
                    $missing += 'no pre-copy status line was captured, so a text change cannot be evidence'
                } else {
                    $missing += 'the status text never changed from the pre-copy baseline'
                }
                if ($statusReadInterrupted -and -not $BaselineReadable) {
                    $missing += 'STATUS.TXT also threw after the copy, but it was already unreadable before it, so that proves nothing'
                }
                $reason = ($missing -join '; ') +
                          ' -- this read is indistinguishable from the OS-cached pre-copy value'
            }
            # else: stable but unproven -- keep watching for evidence.

            if ($null -ne $classification) {
                return [pscustomobject]@{
                    DriveLetter = $currentDrive
                    RawContent = $rawContent
                    StatusLine = $line
                    Classification = $classification
                    StatusVerdict = $verdict
                    VolumeDisturbed = $volumeDisturbed
                    StatusReadInterrupted = $statusReadInterrupted
                    InterruptionCounts = $interruptionCounts
                    ChangedFromBaseline = $changedFromBaseline
                    BaselineStatusAvailable = $BaselineStatusAvailable
                    BaselineReadable = $BaselineReadable
                    HaveEvidence = $haveEvidence
                    TargetEverLost = $targetEverLost
                    IndeterminateReason = $reason
                    ElapsedSec = [math]::Round(((Get-Date) - $startTime).TotalSeconds, 1)
                }
            }
        }

        Start-Sleep -Milliseconds $pollMs
    }

    $timeoutReason = if ($targetEverLost) {
        "the target kit ($KitSerial) never came back under any drive letter"
    } else {
        "no stable STATUS.TXT reading within ${StatusTimeoutSec}s"
    }

    return [pscustomobject]@{
        DriveLetter = $currentDrive
        RawContent = $rawContent
        StatusLine = $previousLine
        Classification = 'Timeout'
        StatusVerdict = (Get-StatusVerdict -StatusLine $previousLine)
        VolumeDisturbed = $volumeDisturbed
        StatusReadInterrupted = $statusReadInterrupted
        InterruptionCounts = ($statusReadInterrupted -and $BaselineReadable)
        ChangedFromBaseline = $changedFromBaseline
        BaselineStatusAvailable = $BaselineStatusAvailable
        BaselineReadable = $BaselineReadable
        HaveEvidence = ($volumeDisturbed -or ($statusReadInterrupted -and $BaselineReadable) -or $changedFromBaseline)
        TargetEverLost = $targetEverLost
        IndeterminateReason = $timeoutReason
        ElapsedSec = $StatusTimeoutSec
    }
}

function Get-UartLogMark {
    <#
      Remember where the console log ended just before the copy, so the check
      afterwards can tell a fresh boot banner from an old one. Returns the last
      log line verbatim (lines are timestamped, so one line is unique), '' when
      the log is empty, or $null when the monitor is not reachable at all.
    #>
    param([string]$MonitorUrl)

    try {
        $r = Invoke-RestMethod -Uri "$MonitorUrl/log?tail=1" -TimeoutSec 5
        $lines = @($r.lines)
        if ($lines.Count -gt 0) { return [string]$lines[-1] }
        return ''
    } catch {
        return $null
    }
}

function Send-StopAudioCommand {
    <#
      Fire-and-forget the mute command at the board through the monitor's HTTP
      API. Never throws: this is a courtesy step, and aborting a flash because the
      console was momentarily unavailable would be a worse outcome than the noise
      it avoids. It does warn, because a failure to send means the audio is still
      running when programming resets the part -- silence here would misrepresent
      that as "muted".

      The monitor owns the COM port; do not open it directly from this script.
    #>
    param(
        [string]$MonitorUrl,
        [string]$Command
    )

    try {
        $body = @{ cmd = $Command } | ConvertTo-Json -Compress
        Invoke-RestMethod -Method Post -Uri "$MonitorUrl/command" `
            -ContentType 'application/json' -Body $body -TimeoutSec 5 | Out-Null
    } catch {
        Write-Warning ("Could not send '$Command' via ${MonitorUrl}: $($_.Exception.Message)")
        Write-Warning '  -> audio may still be running while the board is programmed.'
        return
    }

    # Says exactly what was established: the monitor accepted it. Whether the
    # firmware acted on it is not checked here by design.
    Write-Status "Sent '$Command' to stop audio (delivered to the monitor; not verified at the codec)."

    # The copy resets the target within moments. Give the firmware time to run the
    # mute first -- unverified need not also mean untimed.
    Start-Sleep -Milliseconds 300
}

function Resolve-MonitorKitSerial {
    <#
      Which board is the serial_monitor actually listening to?

      Without this, the STATUS.TXT side is pinned to the target kit serial while
      the console side just reads whatever log the monitor happens to produce --
      so with two Nanos attached the verdict could combine board A's STATUS with
      board B's boot banner. Marker uniqueness makes an accidental match
      unlikely, but "authoritative evidence from the target board" should not
      rest on that.

      The chain is: /status -> COM port -> that port's PnP device -> its USB
      parent, whose instance ID ends in the kit USB serial. Measured on this
      hardware:

        USB\VID_03EB&PID_2175&MI_01\6&2179F488&0&0001   (the CDC child)
          parent -> USB\VID_03EB&PID_2175\MC020023603RYN000772
                                          ^^^^^^^^^^^^^^^^^^^^ = KIT-INFO.TXT's
                                                                 serial exactly

      Returns Ok=$false with a reason when the chain cannot be walked (monitor
      down, non-USB port, PnP property unavailable) -- the caller then warns
      rather than failing, since being unable to identify the port is not
      evidence of a mismatch.
    #>
    param([string]$MonitorUrl)

    try {
        $status = Invoke-RestMethod -Uri "$MonitorUrl/status" -TimeoutSec 5
    } catch {
        return [pscustomobject]@{ Ok = $false; Serial = $null; Port = $null
                                  Detail = "monitor not reachable at $MonitorUrl" }
    }

    $port = [string]$status.port
    if ([string]::IsNullOrWhiteSpace($port)) {
        return [pscustomobject]@{ Ok = $false; Serial = $null; Port = $null
                                  Detail = 'monitor reported no port' }
    }

    try {
        $dev = @(Get-PnpDevice -Class Ports -PresentOnly -ErrorAction Stop |
                    Where-Object { $_.FriendlyName -match "\($port\)" })
        if ($dev.Count -ne 1) {
            return [pscustomobject]@{ Ok = $false; Serial = $null; Port = $port
                                      Detail = "found $($dev.Count) PnP entries for $port" }
        }
        $parent = (Get-PnpDeviceProperty -InstanceId $dev[0].InstanceId `
                    -KeyName 'DEVPKEY_Device_Parent' -ErrorAction Stop).Data
        if ([string]::IsNullOrWhiteSpace($parent)) {
            return [pscustomobject]@{ Ok = $false; Serial = $null; Port = $port
                                      Detail = "no PnP parent for $port" }
        }
        return [pscustomobject]@{ Ok = $true; Serial = $parent.Split('\')[-1]; Port = $port
                                  Detail = "$port -> $parent" }
    } catch {
        return [pscustomobject]@{ Ok = $false; Serial = $null; Port = $port
                                  Detail = $_.Exception.Message }
    }
}

function Test-UartBootMarker {
    <#
      The only genuinely authoritative check: did the firmware that just
      started actually say something recognisable on the console? Opt-in via
      -VerifyUartContains, because it needs serial_monitor to be up and already
      holding the board's console port.

      This scans the log *history* after $LogMark rather than using the API's
      POST /wait. /wait is forward-only: it blocks for output arriving from the
      moment it is called, and the board finishes programming and prints its
      banner within about a second of the copy -- long before the STATUS.TXT
      polling above has finished. A forward-only wait therefore reliably misses
      the very banner it is looking for (measured: banner at 09:17:57, /wait
      armed ~9 s later, 408 timeout on a flash that had in fact succeeded).
    #>
    param(
        [string]$MonitorUrl,
        [string]$Marker,
        [int]$TimeoutSec,
        [AllowNull()][string]$LogMark
    )

    if ($null -eq $LogMark) {
        return [pscustomobject]@{
            Seen = $false
            Detail = "serial_monitor not reachable at $MonitorUrl -- cannot verify (start it, or drop -VerifyUartContains)"
        }
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $lastError = 'marker never appeared after the pre-copy log position'

    while ((Get-Date) -lt $deadline) {
        try {
            $r = Invoke-RestMethod -Uri "$MonitorUrl/log?tail=500" -TimeoutSec 5
            $lines = @($r.lines)

            # Only look at lines added after the mark. If the mark itself has
            # already scrolled out of the buffer, everything visible is newer.
            $startIdx = 0
            if (-not [string]::IsNullOrEmpty($LogMark)) {
                for ($i = $lines.Count - 1; $i -ge 0; $i--) {
                    if ($lines[$i] -eq $LogMark) { $startIdx = $i + 1; break }
                }
            }

            for ($j = $startIdx; $j -lt $lines.Count; $j++) {
                # Ordinal literal substring, NOT -like: -like treats the marker as
                # a PowerShell wildcard pattern, so a marker containing * ? or
                # [...] would match lines it has nothing to do with (a bare '*'
                # would match everything and pass unconditionally).
                if ($lines[$j].IndexOf($Marker, [System.StringComparison]::Ordinal) -ge 0) {
                    return [pscustomobject]@{
                        Seen = $true
                        Detail = "matched '$Marker' in: $($lines[$j])"
                    }
                }
            }
        } catch {
            $lastError = $_.Exception.Message
        }

        Start-Sleep -Milliseconds 500
    }

    return [pscustomobject]@{ Seen = $false; Detail = $lastError }
}

function Invoke-DragAndDropCopy {
    param(
        [string]$HexPath,
        [string]$DriveLetter,
        [bool]$DryRun
    )

    $destName = Split-Path -Leaf $HexPath
    $destPath = "${DriveLetter}:\$destName"

    if ($DryRun) {
        Write-Status "Dry-run: would copy '$HexPath' -> '$destPath'"
        return $destPath
    }

    Write-Status "Copying '$HexPath' -> '$destPath' (this triggers programming)..."
    Copy-Item -LiteralPath $HexPath -Destination $destPath -Force
    return $destPath
}

# ============================================================================

if ($List) {
    Write-Status "flash-curiositynano: list connected Curiosity Nano drag-and-drop drives"
    $volumes = Get-CuriosityVolumes
    if ($volumes.Count -eq 0) {
        Write-Host "No CURIOSITY-labeled drive found."
        exit 2
    }
    foreach ($vol in $volumes) {
        $info = Read-KitInfo -DriveLetter $vol.DriveLetter
        Write-Host "$($vol.DriveLetter): kit='$($info.KitName)' device=$($info.Device) serial=$($info.SerialNumber)"
    }
    return
}

$repoRoot = Resolve-BuildRoot -RequestedRoot $Root
$projectDir = Resolve-MplabProjectDir -Root $repoRoot -RequestedProjectDir $ProjectDir
$hexPath = Resolve-ProductionHex -RequestedHex $Hex -ProjectDir $projectDir -Configuration $Configuration

Write-Status "Root: $repoRoot"
Write-Status "Project: $projectDir"
Write-Status "Configuration: $Configuration"
Write-Status "HEX: $hexPath"

# Default the verification marker to the build ID build.ps1 stamped. See the
# -VerifyUartContains comment for why this is a default and not an opt-in.
if ([string]::IsNullOrWhiteSpace($VerifyUartContains)) {
    $idHeader = Join-Path $projectDir "build\$Configuration\production\generated\ev88g73a_build_id.h"
    if (Test-Path -LiteralPath $idHeader) {
        $m = Select-String -LiteralPath $idHeader -Pattern '#define\s+EV88G73A_BUILD_ID\s+"([^"]+)"'
        if ($m) {
            $VerifyUartContains = $m.Matches[0].Groups[1].Value
            Write-Status "Verification marker: build ID $VerifyUartContains (from $((Split-Path -Leaf $idHeader)))"
        }
    }
    if ([string]::IsNullOrWhiteSpace($VerifyUartContains)) {
        # Not an error: DM330030 stamps no ID, and an -Hex from elsewhere has no
        # header to read. Say so, because it means the verdict can be
        # Indeterminate and the reason should not be a mystery.
        Write-Status 'Verification marker: none available -- STATUS.TXT alone may be inconclusive'
    }
} elseif ($VerifyUartContains -eq 'none') {
    Write-Status 'Verification marker: explicitly disabled (-VerifyUartContains none)'
    $VerifyUartContains = ''
}

$driveLetter = Resolve-CuriosityDrive -RequestedDriveLetter $DriveLetter
$deviceInfo = Assert-ExpectedDevice -DriveLetter $driveLetter -ExpectedDevice $ExpectedDevice

# Mute before the copy, and specifically before the console log mark below, so
# that this command and its reply stay OUT of the window the build-ID check
# searches. Sent only once the expected device is confirmed present: there is no
# point talking to a board this run is not going to program.
if ($StopAudioCommand -eq 'none') {
    Write-Status 'Stop-audio command: explicitly disabled (-StopAudioCommand none)'
} elseif (-not [string]::IsNullOrWhiteSpace($StopAudioCommand)) {
    if ($DryRun) {
        Write-Status "Dry-run: would send '$StopAudioCommand' via $MonitorUrl to stop audio"
    } else {
        Send-StopAudioCommand -MonitorUrl $MonitorUrl -Command $StopAudioCommand
    }
}

# Captured BEFORE the copy specifically so the post-copy read can be tested
# against it -- a post-copy line that still equals this one is not evidence that
# anything was programmed. See Wait-ProgrammingStatus.
$baselineStatus = $null
$baselineStatusAvailable = $false
$baselineReadable = $false
try {
    $baselineRaw = [System.IO.File]::ReadAllText("${driveLetter}:\STATUS.TXT")
    $baselineReadable = $true
    $baselineStatus = Get-StatusLine -RawContent $baselineRaw
    $baselineStatusAvailable = -not [string]::IsNullOrWhiteSpace($baselineStatus)
    Write-Status "Pre-copy STATUS.TXT baseline: '$baselineStatus'"
    if (-not $baselineStatusAvailable) {
        Write-Status "  -> readable but no 'Status:' line, so a text change cannot count as evidence."
    }
} catch {
    Write-Status "Pre-copy STATUS.TXT unreadable: $($_.Exception.Message)"
    Write-Status "  -> neither a text change nor a post-copy read error can count as evidence."
}

# Where the console log ends right now, i.e. before anything is programmed.
# Anything after this point was produced by the firmware we are about to write.
$uartLogMark = $null
if (-not [string]::IsNullOrWhiteSpace($VerifyUartContains)) {
    $uartLogMark = Get-UartLogMark -MonitorUrl $MonitorUrl
    if ($null -eq $uartLogMark) {
        Write-Status "serial_monitor not reachable at $MonitorUrl -- UART verification will report that."
    } else {
        Write-Status "Pre-copy console log mark: '$uartLogMark'"
    }
}

if ($DryRun) {
    Write-Status "Dry-run: would poll STATUS.TXT for up to ${StatusTimeoutSec}s (transition window ${TransitionTimeoutSec}s)"
    Invoke-DragAndDropCopy -HexPath $hexPath -DriveLetter $driveLetter -DryRun $true | Out-Null
    Write-Status "flash-curiositynano: dry-run complete, nothing written"
    exit 0
}

try {
    Invoke-DragAndDropCopy -HexPath $hexPath -DriveLetter $driveLetter -DryRun $false | Out-Null
} catch {
    throw "Copy to ${driveLetter}: failed: $($_.Exception.Message)"
}

Write-Status "No reset performed -- the board auto-restarts after drag-and-drop programming."

$result = Wait-ProgrammingStatus -DriveLetter $driveLetter -BaselineStatus $baselineStatus `
            -BaselineStatusAvailable $baselineStatusAvailable -BaselineReadable $baselineReadable `
            -KitSerial $deviceInfo.SerialNumber `
            -StatusTimeoutSec $StatusTimeoutSec -TransitionTimeoutSec $TransitionTimeoutSec

Write-Host "Final drive: $($result.DriveLetter):"
Write-Host "Final STATUS.TXT content:"
Write-Host $result.RawContent
Write-Host "Parsed status line: '$($result.StatusLine)' (text verdict: $($result.StatusVerdict))"
Write-Host ("Evidence the read is post-copy: {0}" -f $(if ($result.HaveEvidence) { 'YES' } else { 'NO' }))
Write-Host ("  volumeDisturbed={0} changedFromBaseline={1} statusReadInterrupted={2} (counts={3})" -f `
    $result.VolumeDisturbed, $result.ChangedFromBaseline, `
    $result.StatusReadInterrupted, $result.InterruptionCounts)
Write-Host ("  pre-copy baseline: readable={0} statusLineParsed={1}" -f `
    $result.BaselineReadable, $result.BaselineStatusAvailable)
Write-Host "Elapsed: $($result.ElapsedSec)s"

<#
  The console check, when asked for. Note carefully what it can and cannot
  establish: it proves the board RESTARTED and emitted the marker after the
  copy. Whether the *new* image is the one running depends entirely on the
  marker being unique to it. A generic boot banner is also printed by the old
  firmware, so if the hex parser rejected the file and the board simply reset,
  a generic marker still matches. Pass something image-specific -- build.ps1
  stamps a build ID into the banner for exactly this (see -VerifyUartContains
  in the docs); the wording below distinguishes the two cases.

  Promotion rules:
    evidence-backed Failure   -> stays Failure (the debugger reported a real
                                 failure for this operation; a restart does not
                                 override that)
    evidence-free  Failure    -> may be promoted, since that text was never
                                 shown to belong to this operation at all
    Indeterminate             -> may be promoted
    Success                   -> demoted to Indeterminate if the marker is
                                 missing, since we asked for confirmation and
                                 did not get it
#>
if (-not [string]::IsNullOrWhiteSpace($VerifyUartContains)) {
    # Confirm the console we are about to trust belongs to the board we flashed.
    $monKit = Resolve-MonitorKitSerial -MonitorUrl $MonitorUrl
    $wrongBoard = $false
    if ($monKit.Ok) {
        if ($monKit.Serial -eq $deviceInfo.SerialNumber) {
            Write-Status "Monitor port $($monKit.Port) belongs to the target kit $($monKit.Serial) -- OK"
        } else {
            $wrongBoard = $true
            Write-Host "UART verification REFUSED: the monitor is on $($monKit.Port), which belongs to"
            Write-Host "kit $($monKit.Serial), but the hex was written to kit $($deviceInfo.SerialNumber)."
            Write-Host "Console output from a different board is not evidence about this one."
        }
    } else {
        Write-Status "Could not confirm which kit the monitor's port belongs to ($($monKit.Detail))."
        Write-Status "  -> proceeding; being unable to identify the port is not proof of a mismatch."
    }

    $uart = if ($wrongBoard) {
        [pscustomobject]@{ Seen = $false
                           Detail = "not checked -- monitor is on another kit ($($monKit.Serial))" }
    } else {
        Write-Status "Verifying via serial_monitor at ${MonitorUrl}: looking for '$VerifyUartContains' after the pre-copy mark..."
        Test-UartBootMarker -MonitorUrl $MonitorUrl -Marker $VerifyUartContains `
            -TimeoutSec $UartTimeoutSec -LogMark $uartLogMark
    }

    $evidenceBackedFailure = ($result.StatusVerdict -eq 'Failure') -and $result.HaveEvidence

    if ($uart.Seen) {
        Write-Host "UART verification: marker seen ($($uart.Detail))"
        if ($evidenceBackedFailure) {
            Write-Host "  ...but STATUS.TXT reported a failure for THIS operation (evidence-backed)."
            Write-Host "  A restart does not override that -- keeping the failure verdict."
        } else {
            $result.Classification = 'Success'
        }
    } else {
        Write-Host "UART verification: marker NOT seen ($($uart.Detail))"

        # Record the miss whatever the STATUS verdict was. Leaving only the
        # STATUS reason in place would hide the more actionable fact -- an
        # explicitly requested marker did not show up -- behind a sentence about
        # the drive not dropping.
        $uartReason = "the requested console marker '$VerifyUartContains' never appeared after the copy ($($uart.Detail))"
        if ($result.Classification -eq 'Success') {
            $result.Classification = 'Indeterminate'
            $result.IndeterminateReason = "STATUS.TXT looked good, but $uartReason"
        } elseif ($result.Classification -eq 'Indeterminate') {
            $result.IndeterminateReason = "$($result.IndeterminateReason); and $uartReason"
        }
    }
}

Write-Host "Classification: $($result.Classification)"

switch ($result.Classification) {
    'Success' {
        Write-Status "flash-curiositynano: programming succeeded"
        exit 0
    }
    'Failure' {
        Write-Host "flash-curiositynano: programming reported a failure status"
        exit 4
    }
    'Timeout' {
        Write-Host "flash-curiositynano: timed out -- $($result.IndeterminateReason)"
        exit 3
    }
    'Indeterminate' {
        Write-Host "flash-curiositynano: CANNOT CONFIRM (neither success nor failure)."
        Write-Host "  STATUS.TXT reads: '$($result.StatusLine)' (text verdict: $($result.StatusVerdict))"
        Write-Host "  Why unconfirmed: $($result.IndeterminateReason)"
        if ($result.StatusVerdict -eq 'Failure') {
            Write-Host "  Note: the failure text was NOT shown to belong to this operation, so it is"
            Write-Host "  not reported as a failure either -- a cached 'hex parser failed' has been"
            Write-Host "  observed on this board surviving several later successful programmings."
        }
        Write-Host "  Confirm what is actually running, e.g.:"
        Write-Host "    .\buildtools\flash-curiositynano.ps1 -Hex '<hex>' -VerifyUartContains '<build id>'"
        Write-Host "  (use a marker unique to the image -- a generic boot banner only proves a restart)"
        Write-Host "  or power-cycle the board and re-read STATUS.TXT as its own text instructs."
        exit 6
    }
    default {
        Write-Host "flash-curiositynano: status text did not match a known vocabulary -- inspect the content above and judge manually"
        exit 5
    }
}
