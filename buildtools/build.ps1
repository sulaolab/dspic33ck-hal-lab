<#
build.ps1 -- build a configuration of firmware.X.

WHAT CHANGED, AND WHY IT MATTERS
--------------------------------
This script used to carry its own list of source files, its own -I list, and its
own per-configuration table of device/DFP/linker-script, and it invoked the
compiler once per file itself. That made it a SECOND source of truth alongside
firmware.X/nbproject/configurations.xml -- and the two drifted, badly: the xml
ended up naming 22 files that no longer existed, missing four whole HAL
directories, and defining no DM330030 configuration at all. MPLAB X could not
open and build this project while build.ps1 worked fine, which is the worst
possible split because nothing announces it.

So the truth now lives in configurations.xml, and this script drives the same
tools MPLAB X drives:

    prjMakefilesGenerator.bat .          (xml -> nbproject/Makefile-<conf>.mk)
    make -f nbproject/Makefile-<conf>.mk .build-conf

Same arrangement as dspic33ak-audio-dsp-sonora/buildtools/build.ps1. Adding or
removing a source file is now done in the IDE (or by hand in the xml) and both
build paths see it at once. There is nothing here to keep in sync.

WHAT STILL LIVES HERE
---------------------
Only things that are not per-configuration project settings:

  -Define      extra preprocessor macros for a one-off build
  -BuildId     the identity stamp EV88G73A prints in its banner
  -Clean/-Full removing generated makefiles and build output

WHAT DELIBERATELY NO LONGER LIVES HERE
--------------------------------------
  the source list        configurations.xml
  include directories    configurations.xml (extra-include-directories)
  device / DFP / pack    configurations.xml (MPLAB X manages pack versions)
  optimisation level     configurations.xml (see the note on -Define below)

The DFP one is not academic. MPLAB X installed the current MC pack (1.10.386)
into C:/Users/<user>/.mchp_packs/, NOT under the MPLAB X install directory,
which still only carries 1.9.370. The old script resolved the DFP from the
MPLAB X install and would now silently build against the older pack. The
generated makefiles already resolve it correctly, so deferring to them fixes a
bug rather than merely moving code.
#>

param(
    # Remove generated makefiles and build output before building.
    [switch]$Clean,
    # Clean, regenerate makefiles, then build.
    [switch]$Full,
    # Regenerate makefiles and stop.
    [switch]$Generate,

    <#
      Which configuration a bare `build.ps1` builds.

      EV88G73A, because that is the board this lab actually has on the bench: it
      is the one that gets flashed, listened to and measured, and the one whose
      ROM ceiling decides design questions here. DM330030 remains buildable and
      is still worth building (it is the roomy configuration, so it catches
      board-ownership and exclusion mistakes that 97 %-full EV88G73A hides), but
      it has to be asked for by name now -- defaulting to the absent board meant
      a bare invocation built something nobody could run, and did it silently.
    #>
    [ValidateSet('CK256MP508_DM330030', 'CK64MC105_EV88G73A', 'CK256MC005_EV08P02A')]
    [string]$Configuration = 'CK64MC105_EV88G73A',

    <#
      Extra preprocessor macros, e.g. -Define EV88G73A_WM8904_DSPIC_IS_MASTER=1.

      These reach the compiler through MP_EXTRA_CC_PRE, the MPLAB makefile's own
      hook. Note PRE means exactly that: it is placed BEFORE the flags the
      makefile generates from the xml, so for any macro the xml also defines, the
      xml wins and what is passed here is silently discarded. That is a trap this
      project has already been bitten by in another repo, so a collision is
      detected and refused below rather than quietly ignored.

      The same ordering is why there is no -Optimization parameter any more: the
      xml's -O flag would always override one passed here. Change the
      optimisation level in MPLAB X (or in the xml) where it is a property of the
      configuration, which is what it actually is.
    #>
    [string[]]$Define = @(),

    <#
      Identity stamp for the EV88G73A banner, so a console check can prove which
      IMAGE is running rather than merely that the board restarted. Default is
      generated per build; pass an explicit value for a reproducible hex, or
      'none' to omit it.

      Limit worth knowing: it identifies the artifact, not a write transaction.
      Re-flashing the same hex without rebuilding leaves the same ID, so a failed
      re-write of an image already on the board still matches.
    #>
    [string]$BuildId = '',

    [string]$ProjectDir = (Join-Path $PSScriptRoot '..\firmware.X'),
    [string]$MplabXVersion = '6.30',
    [int]$Jobs = [Math]::Min([Environment]::ProcessorCount, 8)
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingPath {
    param([string]$Path, [string]$Description)
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Description path is empty" }
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction SilentlyContinue
    if (-not $resolved) { throw "$Description not found: $Path" }
    return $resolved.Path
}

function Invoke-Checked {
    param([string]$Description, [scriptblock]$Command)
    Write-Host "==> $Description"
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Description failed with exit code $LASTEXITCODE" }
}

$projectDir = Resolve-ExistingPath -Path $ProjectDir -Description 'MPLAB X project directory'
$mplabRoot  = Resolve-ExistingPath -Path "C:\Program Files\Microchip\MPLABX\v$MplabXVersion" `
                                   -Description "MPLAB X v$MplabXVersion install"

$generator = Resolve-ExistingPath -Path (Join-Path $mplabRoot 'mplab_platform\bin\prjMakefilesGenerator.bat') `
                                  -Description 'prjMakefilesGenerator.bat'
$makeTool  = Resolve-ExistingPath -Path (Join-Path $mplabRoot 'gnuBins\GnuWin32\bin\make.exe') `
                                  -Description 'make.exe'
$xmlPath   = Resolve-ExistingPath -Path (Join-Path $projectDir 'nbproject\configurations.xml') `
                                  -Description 'configurations.xml'

Write-Host "Project:       $projectDir"
Write-Host "Configuration: $Configuration"
Write-Host "MPLAB X:       $mplabRoot"

# --- the configuration must exist in the xml -------------------------------
# ValidateSet above only checks the spelling against what this script was
# written for. The xml is the truth, so ask it.
$xml = New-Object System.Xml.XmlDocument
$xml.Load($xmlPath)
$confNode = $xml.SelectNodes('//conf') | Where-Object { $_.GetAttribute('name') -eq $Configuration }
if (-not $confNode) {
    $have = (($xml.SelectNodes('//conf') | ForEach-Object { $_.GetAttribute('name') }) -join ', ')
    throw "configurations.xml has no configuration '$Configuration'. It defines: $have"
}
$device = ($confNode.SelectSingleNode('.//targetDevice')).InnerText
$pack   = $confNode.SelectSingleNode('.//pack')
Write-Host "Device:        $device"
Write-Host "DFP:           $($pack.GetAttribute('name')) $($pack.GetAttribute('version'))"

# --- refuse -Define values the xml would override --------------------------
# See the -Define comment: MP_EXTRA_CC_PRE lands before the xml's own -D flags,
# so a collision means the value passed here is discarded without a word. Fail
# instead, and say where the winning definition lives.
$confMacros = @()
foreach ($p in $confNode.SelectNodes('.//property')) {
    if ($p.GetAttribute('key') -eq 'preprocessor-macros') {
        $v = $p.GetAttribute('value')
        if (-not [string]::IsNullOrWhiteSpace($v)) { $confMacros += ($v -split ';') }
    }
}
$confMacroNames = @($confMacros | ForEach-Object { ($_ -split '=')[0].Trim() } | Where-Object { $_ } | Select-Object -Unique)

foreach ($d in $Define) {
    $name = (($d -replace '^-D', '') -split '=')[0].Trim()
    if ($confMacroNames -contains $name) {
        throw ("-Define $d cannot take effect: configuration '$Configuration' already defines " +
               "$name in configurations.xml, and the xml's flags come after MP_EXTRA_CC_PRE, " +
               "so the xml wins. Change it in MPLAB X (or the xml) instead, or add a separate " +
               "configuration if both values are wanted.")
    }
}

# --- clean -----------------------------------------------------------------
if ($Clean -or $Full) {
    foreach ($d in @('build', 'dist')) {
        $p = Join-Path $projectDir $d
        if (Test-Path -LiteralPath $p) {
            Write-Host "==> Remove $d/"
            Remove-Item -LiteralPath $p -Recurse -Force
        }
    }
    Get-ChildItem (Join-Path $projectDir 'nbproject') -Filter 'Makefile-CK*.mk' -ErrorAction SilentlyContinue |
        Remove-Item -Force
}

# --- generate makefiles ----------------------------------------------------
# Always, unless the makefile for this configuration already exists and is newer
# than the xml. Regenerating is a couple of seconds, and a stale makefile after
# an xml edit is a silent wrong build.
$makefile = Join-Path $projectDir "nbproject\Makefile-$Configuration.mk"
$needGen  = $Generate -or $Full -or $Clean -or
            (-not (Test-Path -LiteralPath $makefile)) -or
            ((Get-Item -LiteralPath $xmlPath).LastWriteTimeUtc -gt (Get-Item -LiteralPath $makefile).LastWriteTimeUtc)

if ($needGen) {
    # The generator rewrites configurations.xml in passing -- reindenting it, and
    # sometimes reordering. Left alone that shows up as permanent working-tree
    # noise and buries a real edit. Snapshot it, and put it back if the only
    # difference is formatting.
    $before = [System.IO.File]::ReadAllBytes($xmlPath)

    Push-Location $projectDir
    try {
        Invoke-Checked -Description 'Generate MPLAB X makefiles' -Command { & $generator '.' }
    } finally {
        Pop-Location
    }

    $after = [System.IO.File]::ReadAllBytes($xmlPath)
    if (-not [System.Linq.Enumerable]::SequenceEqual([byte[]]$before, [byte[]]$after)) {
        $normalise = { param($b) (([System.Text.Encoding]::UTF8.GetString($b)) -replace '\s+', ' ').Trim() }
        if ((& $normalise $before) -eq (& $normalise $after)) {
            [System.IO.File]::WriteAllBytes($xmlPath, $before)
            Write-Host '    (configurations.xml reformatted by the generator; restored)'
        } else {
            Write-Host '    NOTE: the generator made a real change to configurations.xml -- review it.'
        }
    }
}

if ($Generate) {
    Write-Host "Generated: $makefile"
    return
}

# --- build ID --------------------------------------------------------------
# Delivered as a GENERATED HEADER rather than a string-valued -D. A -D with
# quotes has to survive PowerShell's native-argument handling and then make's
# own parsing; neither "..." nor \"...\" did (the failure was an opaque
# "invalid suffix on integer constant"). A header has no quoting layer at all.
#
# Only the Nano boards' main.c consume it -- so it is gated on the
# configuration rather than stamped into an image that never mentions it.
# EV08P02A (dsPIC33CK256MC005) is EV88G73A's port to a newer Nano kit and
# reuses this exact stamping mechanism under its own prefix.
$extraCcPre = @()

$nanoBoardPrefix = switch ($Configuration) {
    'CK64MC105_EV88G73A'  { 'EV88G73A' }
    'CK256MC005_EV08P02A' { 'EV08P02A' }
    default               { $null }
}

if ($nanoBoardPrefix -and $BuildId -ne 'none') {
    $effectiveBuildId = $BuildId
    if ([string]::IsNullOrWhiteSpace($effectiveBuildId)) {
        $gitDesc = 'nogit'
        try {
            $sha = (& git -C $PSScriptRoot rev-parse --short HEAD 2>$null)
            if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($sha)) {
                $gitDesc = $sha.Trim()
                $porcelain = (& git -C $PSScriptRoot status --porcelain 2>$null)
                if (-not [string]::IsNullOrWhiteSpace(($porcelain -join ''))) { $gitDesc = "$gitDesc-dirty" }
            }
        } catch { $gitDesc = 'nogit' }

        # A short digest of any -Define, so two builds of the same commit that
        # differ only in a macro cannot present the same identity.
        $defTag = ''
        if ($Define.Count -gt 0) {
            $md5 = [System.Security.Cryptography.MD5]::Create()
            $h = $md5.ComputeHash([System.Text.Encoding]::ASCII.GetBytes(($Define -join ',')))
            $defTag = '-d' + (([System.BitConverter]::ToString($h) -replace '-', '').Substring(0, 6).ToLower())
        }

        $stamp = Get-Date -Format 'yyyyMMddTHHmmssfff'
        $nonce = [guid]::NewGuid().ToString('N').Substring(0, 10)
        $effectiveBuildId = "$gitDesc$defTag-$stamp-$nonce"
    }

    # Constrained because it becomes a C string literal and
    # flash-curiositynano.ps1 matches it as a literal substring: this keeps
    # quotes, backslashes, newlines and non-ASCII out of the header rather than
    # letting them fail confusingly later.
    if ($effectiveBuildId -notmatch '^[A-Za-z0-9][A-Za-z0-9._:-]{0,95}$') {
        throw ("BuildId must be 1-96 chars, start alphanumeric, and use only A-Z a-z 0-9 . _ : - ; " +
               "got '$effectiveBuildId'")
    }
    Write-Host "Build ID:      $effectiveBuildId"

    $genDir = Join-Path $projectDir "build\$Configuration\production\generated"
    New-Item -ItemType Directory -Force $genDir | Out-Null
    $headerName = "$($nanoBoardPrefix.ToLower())_build_id.h"
    @(
        '/* Generated by buildtools/build.ps1 -- do not edit, not checked in. */',
        "#ifndef ${nanoBoardPrefix}_BUILD_ID_H",
        "#define ${nanoBoardPrefix}_BUILD_ID_H",
        "#define ${nanoBoardPrefix}_BUILD_ID `"$effectiveBuildId`"",
        '#endif'
    ) -join "`r`n" | Set-Content -LiteralPath (Join-Path $genDir $headerName) -Encoding ascii

    # Forward slashes: this string is handed to make, which treats a backslash as
    # an escape.
    $extraCcPre += "-I`"$($genDir -replace '\\', '/')`""
    $extraCcPre += "-D${nanoBoardPrefix}_HAVE_BUILD_ID_H=1"
}

foreach ($d in $Define) {
    $extraCcPre += if ($d -match '^-D') { $d } else { "-D$d" }
}

# --- make ------------------------------------------------------------------
$makeArgs = @("-j$Jobs", '-f', "nbproject/Makefile-$Configuration.mk", 'SUBPROJECTS=')
if ($extraCcPre.Count -gt 0) {
    Write-Host "Extra flags:   $($extraCcPre -join ' ')"
    $makeArgs += "MP_EXTRA_CC_PRE=$($extraCcPre -join ' ')"
}
$makeArgs += '.build-conf'

# make needs its own GnuWin32 bin on PATH (it shells out to rm, mkdir, ...).
$savedPath = $env:PATH
$env:PATH = (Join-Path $mplabRoot 'gnuBins\GnuWin32\bin') + ';' + $env:PATH
Push-Location $projectDir
try {
    Invoke-Checked -Description "Build $Configuration" -Command { & $makeTool @makeArgs }
} finally {
    Pop-Location
    $env:PATH = $savedPath
}

# --- report ----------------------------------------------------------------
$hex = Join-Path $projectDir "dist\$Configuration\production\firmware.X.production.hex"
$map = Join-Path $projectDir "dist\$Configuration\production\firmware.X.production.map"
if (Test-Path -LiteralPath $hex) { Write-Host "Artifact:      $hex" }
if (Test-Path -LiteralPath $map) {
    foreach ($k in @('Total "program" memory used', 'Total "data" memory used')) {
        $line = (Select-String -LiteralPath $map -Pattern ([regex]::Escape($k))).Line
        if ($line) { Write-Host ('  ' + ($line -replace '\s+', ' ').Trim()) }
    }
}
