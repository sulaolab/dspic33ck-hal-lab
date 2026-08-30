<#
flash-mdb-nano.ps1 -- program a CK Curiosity Nano with mdb in PROGRAMMER mode,
through the same verified mute gate flash-curiositynano.ps1 uses.

WHY THIS EXISTS
---------------
The EV08P02A kit (dsPIC33CK256MC005) has mass-storage programming disabled in its
debugger firmware (KIT-INFO.TXT: "Drag and drop: No"), so flash-curiositynano.ps1
cannot program it at all. The
mdb recipe was therefore driven by hand, and driving it by hand SKIPPED THE MUTE
GATE: the owner heard the board crackle through HPOUT while it was being written
(2026-08-29). Programming a live analog output is exactly the event that gate
exists to prevent, so the recipe needed to be a script, not a habit.

The gate itself is not reimplemented here -- both scripts dot-source
nano-console-gate.ps1, so there is one definition of "verified mute" to change.

THE -p IS LOAD-BEARING
----------------------
`hwtool pkobnano -p 0` connects for programming only. WITHOUT -p, `program`
writes a DEBUG image: it still prints "Program succeeded", but the board does not
run standalone (symptom: total console silence after a power cycle, which reads
like a dead board), and the mdb session halts the target when it exits. This
script always passes -p and refuses to offer a way not to.

USAGE
    .\buildtools\flash-mdb-nano.ps1                       # build already done
    .\buildtools\flash-mdb-nano.ps1 -DryRun               # gate + checks only
    .\buildtools\flash-mdb-nano.ps1 -StopAudioCommand none  # external mute only
#>
param(
    [switch]$DryRun,
    [switch]$Quiet,
    [ValidateSet('CK256MC005_EV08P02A', 'CK64MC105_EV88G73A')]
    [string]$Configuration = 'CK256MC005_EV08P02A',
    [string]$Hex,
    [string]$ProjectDir = (Join-Path $PSScriptRoot '..\firmware.X'),
    # mdb's tool name for the Curiosity Nano's on-board nEDBG debugger.
    [string]$HwTool = 'pkobnano',
    [int]$ToolIndex = 0,
    [string]$MplabXVersion = '6.30',
    <#
      Pre-programming mute, identical to flash-curiositynano.ps1's: *ts is sent and
      a bounded time is allowed for the board's verified-mute reply, then the write
      proceeds regardless.  'none' is an explicit statement that an operator made
      the analog output safe some other way -- it is not a shortcut.
      -RequireVerifiedMute restores refuse-on-no-reply, for a board known alive.
    #>
    [string]$StopAudioCommand = '*ts',
    [int]$MuteTimeoutSec = 5,
    [int]$MuteSettleMs = 1500,
    [switch]$RequireVerifiedMute,
    [string]$MonitorUrl = 'http://127.0.0.5:8080'
)

$ErrorActionPreference = 'Stop'

function Write-Status {
    param([string]$Message)
    if (-not $Quiet) { Write-Host $Message }
}

. (Join-Path $PSScriptRoot 'nano-console-gate.ps1')

$device = switch ($Configuration) {
    'CK256MC005_EV08P02A' { 'dsPIC33CK256MC005' }
    'CK64MC105_EV88G73A'  { 'dsPIC33CK64MC105' }
}

if ([string]::IsNullOrWhiteSpace($Hex)) {
    $Hex = Join-Path $ProjectDir "dist\$Configuration\production\firmware.X.production.hex"
}
if (-not (Test-Path -LiteralPath $Hex)) {
    throw ("No image at $Hex -- build it first: " +
           ".\buildtools\build.ps1 -Full -Configuration $Configuration. " +
           "(-Full wipes the whole build/ tree, so a build of ANOTHER configuration " +
           "since then has removed this one's generated build-ID header as well.)")
}
$hexPath = (Resolve-Path -LiteralPath $Hex).Path

$mdb = Join-Path "C:\Program Files\Microchip\MPLABX\v$MplabXVersion" 'mplab_platform\bin\mdb.bat'
if (-not (Test-Path -LiteralPath $mdb)) { throw "mdb not found: $mdb" }

Write-Status "Configuration: $Configuration"
Write-Status "Device:        $device"
Write-Status "Image:         $hexPath"

# --- the mute gate, before anything is written -----------------------------
# Which kit is on the debugger is not knowable from mdb without connecting to it,
# so the gate is asked to prove only that the monitor is on a connected CK board.
# That is weaker than the drag-and-drop path's kit-serial match (which reads the
# target's own KIT-INFO.TXT) and is stated rather than hidden: with two CK Nanos
# attached, pass -MonitorUrl for the right one and check /status yourself.
if ($StopAudioCommand -eq 'none') {
    Write-Status 'WARNING: stop-audio mute explicitly disabled (-StopAudioCommand none)'
} else {
    # Identifying the monitor's kit is informational here, not a precondition: it
    # needs the monitor, not the target, and a board too dead to answer must still
    # be programmable. Unknown = the wrong-board check is skipped, not failed.
    $monKit = Resolve-MonitorKitSerial -MonitorUrl $MonitorUrl
    if ($monKit.Ok) { Write-Status "Monitor kit:   $($monKit.Serial) on $($monKit.Port)" }
    else            { Write-Status "Monitor kit:   unknown ($($monKit.Detail))" }
    if ($DryRun) {
        Write-Status "Dry-run: would attempt a '$StopAudioCommand' mute via $MonitorUrl before programming (bounded, non-blocking)"
    } else {
        Invoke-BestEffortAudioStop -MonitorUrl $MonitorUrl -Command $StopAudioCommand `
                                 -ExpectedKitSerial $monKit.Serial `
                                 -TimeoutSec $MuteTimeoutSec -SettleMs $MuteSettleMs `
                                 -RequireVerified:$RequireVerifiedMute | Out-Null
    }
}

if ($DryRun) {
    Write-Status "Dry-run: would run mdb -- device $device / hwtool $HwTool -p $ToolIndex / program"
    return
}

# --- program ---------------------------------------------------------------
# A command file rather than stdin: mdb.bat is a batch wrapper around java and
# reads its script from a file argument, which also keeps the quoting of the hex
# path out of the shell's hands.
$script = @"
device $device
hwtool $HwTool -p $ToolIndex
program "$($hexPath.Replace([char]92, [char]47))"
quit
"@
$scriptFile = Join-Path ([System.IO.Path]::GetTempPath()) ("mdb_$Configuration.cmd")
[System.IO.File]::WriteAllText($scriptFile, $script)

Write-Status "==> mdb program (programmer mode, -p)"
$log = & $mdb $scriptFile 2>&1

# mdb's exit code is not a verdict (it exits 0 after a failed program, and prints
# ChronicleHash shutdown noise on the way out), so the log is what decides.
$logText = ($log | Out-String)
if ($logText -match 'Debug Executive') {
    throw ("mdb wrote a DEBUG image despite -p (log mentions 'Debug Executive'). " +
           "That image does not run standalone -- do not power-cycle and call it broken hardware.")
}
$succeeded = $logText -match '(?im)^\s*Program(ming)?\s+succeeded'
if (-not $succeeded) {
    $tail = ($log | Select-Object -Last 25) -join "`n"
    throw "mdb did not report success. Last lines:`n$tail"
}
Write-Status 'Programming: succeeded (programmer mode -- the board runs standalone)'
Write-Status 'Power-cycle or *sr, then check the boot banner Build ID against build.ps1 output.'
