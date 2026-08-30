<#
nano-console-gate.ps1 -- the console-side safety gate shared by every path that
programs a CK Curiosity Nano board.

DOT-SOURCE THIS; do not copy the functions. It exists because there is now more
than one way to program these boards -- flash-curiositynano.ps1 for kits with
drag-and-drop, and the mdb programmer-mode path for kits without it (EV08P02A)
-- and the mute gate is the
one thing that must be identical on all of them. It was NOT identical for a
while: the mdb path was driven by hand and skipped the gate entirely, and the
owner heard the crackle that programming a live HPOUT produces (2026-08-29).

IT MUST NEVER REQUIRE THE TARGET TO ANSWER. A board is often flashed BECAUSE it
stopped responding, and a gate that waits for the board's own reply before it
lets you write turns that into a deadlock: no reply -> no flash -> no recovery.
The owner has been bitten by exactly that (recorded 2026-08-29). So the mute is
BEST EFFORT with a timeout: send *ts, wait a bounded time for the codec's
readback-verified reply, and proceed either way -- loudly saying which of the two
happened. NOTHING here refuses the write. Wrong-board evidence (the monitor is on another
profile or another kit) stops the CONSOLE SEND -- do not type into someone else's
board -- but not the programming, whose target is decided by the programmer's own
kit selection, not by this console. Callers wanting the old strict behaviour on a
board known to be alive can pass -RequireVerified.

Provides:
  Get-UartLogMark            where the console log ended, to date later evidence
  Invoke-BestEffortAudioStop the gate: send *ts, bounded wait, proceed regardless
  Resolve-MonitorKitSerial   which kit the serial_monitor is actually attached to
  Test-UartBootMarker        wait for a marker in console history since a mark

Requires the caller to define Write-Status (its verbosity policy is the caller's)
and never opens the COM port itself -- the serial_monitor owns it.
#>

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

function Invoke-BestEffortAudioStop {
    <#
      The pre-programming mute. Send *ts, give the board a BOUNDED time to report
      its readback-verified analog mute, and return either way.

      Why it does not insist: see the file header. Flashing is what you do when a
      board has stopped answering, so "no reply" must not block the write -- that
      is a deadlock with no way out, and it has happened here. An unmuted write
      costs a burst of noise; a gate that refuses to write costs the board.

      -SettleMs is the floor: even when the reply never comes, the mute command
      has been delivered and a moment is allowed for it to take effect before the
      write begins, so a board that IS alive but slow to print still goes quiet.

      Wrong-board evidence (another profile, another kit serial) suppresses the
      SEND -- muting someone else's board proves nothing and is rude to whoever
      owns it -- but is still reported as unverified rather than fatal, because
      which board gets programmed is the programmer's business, not this console's.

      Returns [pscustomobject] @{ Verified = $true|$false; Detail = <string> }.
      Throws only for a caller mistake or -RequireVerified.
    #>
    param(
        [string]$MonitorUrl,
        [string]$Command,
        # Kit serial the write is aimed at, when the caller can prove one.
        # Empty/$null = unknown (the mdb path cannot know it), which disables the
        # wrong-kit check rather than failing it.
        [string]$ExpectedKitSerial,
        [int]$TimeoutSec = 5,
        [int]$SettleMs = 1500,
        # Opt-in strictness for a board known to be alive: turns "not verified"
        # back into a refusal. Never the default -- see the file header.
        [switch]$RequireVerified
    )

    function script:__MuteResult {
        param([bool]$Verified, [string]$Detail, [bool]$Strict)
        if (-not $Verified) {
            if ($Strict) { throw "Mute not verified and -RequireVerified was passed: $Detail" }
            Write-Status "WARNING: mute NOT verified -- programming anyway (see nano-console-gate.ps1): $Detail"
        }
        return [pscustomobject]@{ Verified = $Verified; Detail = $Detail }
    }

    if ($Command -ne '*ts') {
        throw "The flash mute gate sends '*ts', not '$Command'. Use -StopAudioCommand none only for an explicit external mute."
    }

    $status = $null
    try {
        $status = Invoke-RestMethod -Uri "$MonitorUrl/status" -TimeoutSec 5
    } catch {
        return __MuteResult -Verified $false -Strict:$RequireVerified `
            -Detail "serial_monitor unreachable at $MonitorUrl, so *ts could not be sent at all"
    }
    if ([string]$status.profile -ne 'ck') {
        return __MuteResult -Verified $false -Strict:$RequireVerified `
            -Detail ("serial_monitor at $MonitorUrl reports profile '$($status.profile)', not 'ck', " +
                     "so *ts was NOT sent (it would have muted another board); point -MonitorUrl at the right profile")
    }
    if (-not [bool]$status.connected) {
        return __MuteResult -Verified $false -Strict:$RequireVerified `
            -Detail "serial_monitor at $MonitorUrl is not connected to its port, so *ts could not be sent"
    }
    if ($null -ne $status.tx_gate.held_by) {
        return __MuteResult -Verified $false -Strict:$RequireVerified `
            -Detail "serial_monitor transmit gate is held by '$($status.tx_gate.held_by)', so *ts could not be sent"
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedKitSerial)) {
        $monKit = Resolve-MonitorKitSerial -MonitorUrl $MonitorUrl
        if ($monKit.Ok -and ($monKit.Serial -ne $ExpectedKitSerial)) {
            return __MuteResult -Verified $false -Strict:$RequireVerified `
                -Detail ("serial_monitor is on kit $($monKit.Serial) but the flash target is $ExpectedKitSerial, " +
                         "so *ts was NOT sent (it would have muted the wrong board)")
        }
        if (-not $monKit.Ok) {
            Write-Status "NOTE: cannot identify the monitor's kit ($($monKit.Detail)); the wrong-board check is skipped, not failed."
        }
    }

    $logMark = Get-UartLogMark -MonitorUrl $MonitorUrl

    try {
        $body = @{ cmd = $Command } | ConvertTo-Json -Compress
        Invoke-RestMethod -Method Post -Uri "$MonitorUrl/command" `
            -ContentType 'application/json' -Body $body -TimeoutSec 5 | Out-Null
    } catch {
        return __MuteResult -Verified $false -Strict:$RequireVerified `
            -Detail "sending '$Command' via $MonitorUrl failed: $($_.Exception.Message)"
    }

    if ($SettleMs -gt 0) { Start-Sleep -Milliseconds $SettleMs }

    if ($null -eq $logMark) {
        return __MuteResult -Verified $false -Strict:$RequireVerified `
            -Detail "'$Command' was sent, but the monitor log could not be read, so the reply cannot be checked"
    }

    # The reply can arrive in a few milliseconds, so search the retained history
    # after the pre-command mark rather than using /wait (forward-looking only).
    $gate = Test-UartBootMarker -MonitorUrl $MonitorUrl `
        -Marker 'analog mute verified, TDM/DMA halted' -TimeoutSec $TimeoutSec -LogMark $logMark
    if (-not $gate.Seen) {
        return __MuteResult -Verified $false -Strict:$RequireVerified `
            -Detail ("'$Command' was sent and " + $SettleMs + " ms allowed, but no verified-mute reply within " +
                     $TimeoutSec + " s (a board that is not running cannot answer -- this is not a reason to stop). " +
                     $gate.Detail)
    }
    Write-Status "Verified '$Command': $($gate.Detail)"
    return [pscustomobject]@{ Verified = $true; Detail = $gate.Detail }
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
