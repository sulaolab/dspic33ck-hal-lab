<#
migrate_configurations_xml.ps1 -- ONE-SHOT migration aid. NOT part of the build.

WHY THIS EXISTS
---------------
firmware.X/nbproject/configurations.xml had gone stale to the point of being
unusable: 22 of the files it listed no longer existed, four whole HAL
directories (hal_clock, hal_dma, hal_i2c, hal_spi_i2s_tdm) were never added,
and the DM330030 configuration was absent entirely -- only `default` and
`CK64MC105_EV88G73A` were defined. Repairing that by hand across
files x configurations is exactly the kind of combinatorial edit where an
entry gets silently dropped, so it is done once, mechanically, here.

WHAT IT IS NOT
--------------
It is NOT the source of truth and must not become part of the build. After this
runs, configurations.xml is authoritative and is maintained by MPLAB X (or by
hand). A script that regenerates the file list from the filesystem on every
build would put the truth back in a script -- the very thing this migration is
undoing.

Keep it in the tree as the record of how the file was produced, and to re-run
if the migration needs a second pass before landing. Delete it once the new
xml has been verified by an MPLAB X clean build and committed.

IF YOU DO RE-RUN IT: follow it with one `prjMakefilesGenerator.bat .` pass and
commit THAT output, not this script's. The generator normalises the file --
notably it sorts the <item> entries by path, where this script emits them in
build order -- and its output is idempotent (verified: two further passes leave
the bytes unchanged). Committing the un-normalised version instead makes
build.ps1 report "the generator made a real change to configurations.xml" on
every single build, which is a warning that is only worth anything while it
stays rare.

HOW IT WORKS
------------
The per-configuration toolchain settings are long (about 350 lines of <C30>,
<C30-AS>, <C30-LD> ... property blocks) and none of them should change. So
rather than author them, this reuses the EXISTING CK64MC105_EV88G73A <conf>
block as a template: it splits it at the <item> list, substitutes the handful
of values that differ per configuration, and regenerates only the item list.
That keeps every compiler/linker property byte-identical to what MPLAB X
itself last wrote.
#>

param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'

$xmlPath = Join-Path $Root 'firmware.X\nbproject\configurations.xml'
if (-not (Test-Path $xmlPath)) { throw "not found: $xmlPath" }

# ---------------------------------------------------------------------------
# Source sets.
#
# Derived from build.ps1's per-configuration object lists as they stood at
# commit e682e4b, i.e. from what the two images ACTUALLY linked -- not from a
# fresh guess at what they ought to contain. The union below is the global file
# list; each configuration then excludes what it does not build.
# ---------------------------------------------------------------------------

$halShared = @(
    'hal_clock/dspic33ck_clock.c',
    'hal_clock/dspic33ck_clock_reg.c',
    'hal_clock/dspic33ck_clock_device.c',
    'hal_dma/dspic33ck_dma.c',
    'hal_gpio/dspic33ck_gpio.c',
    'hal_gpio/dspic33ck_pps.c',
    'hal_timer/dspic33ck_tick_timer.c',
    'hal_timer/dspic33ck_high_res_timer.c',
    'hal_uart/dspic33ck_uart.c',
    'hal_uart/dspic33ck_uart_device.c',
    'hal_i2c/dspic33ck_i2c_common.c',
    'hal_i2c/dspic33ck_i2c_device.c',
    'hal_i2c/dspic33ck_i2c_master.c',
    'hal_i2c/dspic33ck_i2c_slave.c',
    'hal_spi_i2s_tdm/dspic33ck_spi_i2s_tdm.c',
    'hal_spi_i2s_tdm/dspic33ck_spi_i2s_tdm_hw.c',
    'hal_spi_i2s_tdm/dspic33ck_spi_i2s_tdm_diag.c',
    'hal_spi_i2s_tdm/dspic33ck_spi_i2s_tdm_fs_clc.c'
)

# HAL pieces only the DM330030 image links today. Not "unavailable to
# EV88G73A" -- just not currently used by it.
$halDm330030Only = @(
    'hal_gpio/dspic33ck_gpio_event.c',
    'hal_uart/dspic33ck_uart_rx_isr_ring.c',
    'hal_uart/dspic33ck_uart_isr.c'
)

$dm330030Sources = @(
    'boards/dm330030/main.c',
    'boards/dm330030/board.c',
    'boards/dm330030/system.c',
    'boards/dm330030/config_bits.c',
    'boards/dm330030/traps.c',
    'boards/dm330030/led_sw.c',
    'boards/dm330030/led3_rgb.c',
    'boards/dm330030/adc.c',
    'app/timer_1ms.c',
    'app/demo_tdm_master_loopback.c',
    'app/demo_wm8904_audio.c',
    # The codec driver: this image builds it because demo_wm8904_audio.c uses it.
    # Also built by CK64MC105_EV88G73A_WM8904 -- the one source shared by a
    # DM330030 and an EV88G73A configuration, and the reason chip_drivers/ is not
    # split per board.
    'chip_drivers/wm8904.c',
    # This board's implementation of app/console_out.h.
    'boards/dm330030/dm330030_console_out.c'
)

$ev88g73aCommon = @(
    'boards/ev88g73a/main.c',
    'boards/ev88g73a/board_ev88g73a.c',
    'boards/ev88g73a/config_bits.c',
    'boards/ev88g73a/ev88g73a_console.c',
    'boards/ev88g73a/ev88g73a_traps.c',
    'boards/ev88g73a/ev88g73a_dma_selftest.c',
    'boards/ev88g73a/ev88g73a_dsp_load.c',
    'boards/ev88g73a/ev88g73a_i2c1_probe.c',
    # This board's implementation of app/console_out.h.
    'boards/ev88g73a/ev88g73a_console_out.c'
)

# EV88G73A audio path: the WM8904 codec. This is the only one built -- every
# EV88G73A build uses the codec, so there is no non-WM8904 variant to tell it
# apart from and the configuration is just CK64MC105_EV88G73A.
$ev88g73aWm8904 = @(
    'boards/ev88g73a/ev88g73a_wm8904_audio.c',
    'boards/ev88g73a/ev88g73a_gain_ctrl.c',
    'chip_drivers/wm8904.c'
)

# Nothing currently. The Phase A TDM master loopback exerciser sat here briefly
# -- listed for the IDE but excluded from every configuration -- and was deleted
# outright on 2026-08-02 rather than left as code no build compiles. Its findings
# are in docs/ck_silicon_findings.md.
$ev88g73aUnbuilt = @()

# The console: vendored parser + the dispatch switch, both board-independent.
# Each board additionally implements app/console_out.h -- see the per-board lists.
$alwaysSources = @('main.c', 'app/console_stdio.c',
                   'app/app_console.c', 'app/console_dispatch.c',
                   'app/console_diag.c')

# $ev88g73aUnbuilt is in the union but in no configuration's Build set: listed for
# the IDE, compiled by nothing. See its comment above.
$allSources = @($alwaysSources + $halShared + $halDm330030Only + $dm330030Sources +
                $ev88g73aCommon + $ev88g73aWm8904 + $ev88g73aUnbuilt) | Select-Object -Unique

# Headers: listed so the IDE shows them in the project tree and indexes them.
# They are never compiled, so they carry no per-configuration exclusion.
$allHeaders = Get-ChildItem (Join-Path $Root 'src') -Recurse -Filter *.h |
    ForEach-Object { $_.FullName.Substring((Join-Path $Root 'src').Length + 1).Replace('\', '/') } |
    Sort-Object

# ---------------------------------------------------------------------------
# Configurations.
# ---------------------------------------------------------------------------

$includeCommon = @('..\src', '..\src\app', '..\src\chip_drivers', '..\src\hal_clock',
                   '..\src\hal_dma', '..\src\hal_gpio', '..\src\hal_timer',
                   '..\src\hal_uart', '..\src\hal_i2c', '..\src\hal_spi_i2s_tdm')

$configs = @(
    [ordered]@{
        Name    = 'CK256MP508_DM330030'
        Device  = 'dsPIC33CK256MP508'
        PackName= 'dsPIC33CK-MP_DFP'
        PackVer = '1.15.423'
        Macros  = 'DSPIC33CK_BOARD_DM330030=1'
        Optimization = 's'
        Include = ($includeCommon + '..\src\boards\dm330030') -join ';'
        Build   = @($alwaysSources + $halShared + $halDm330030Only + $dm330030Sources)
    },
    [ordered]@{
        Name    = 'CK64MC105_EV88G73A'
        Device  = 'dsPIC33CK64MC105'
        PackName= 'dsPIC33CK-MC_DFP'
        # 1.10.386, not the 1.9.370 this project used before: updated in MPLAB X
        # when the three configurations were clean-built, and mirrored here so
        # re-running the generator does not quietly downgrade it again.
        PackVer = '1.10.386'
        # Just the board. board_ev88g73a.h now defaults WM8904_AUDIO=1 and
        # I2C1_PROBE=0, which IS this configuration, so spelling those out here
        # would only be three more macros to keep in step with the header -- and
        # every macro named here is one that build.ps1 must then refuse to accept
        # via -Define, since the xml would silently win.
        Macros  = 'DSPIC33CK_BOARD_EV88G73A=1'
        Optimization = 's'
        Include = ($includeCommon + '..\src\boards\ev88g73a') -join ';'
        Build   = @($alwaysSources + $halShared + $ev88g73aCommon + $ev88g73aWm8904)
    }
)

# ---------------------------------------------------------------------------
# Split the existing CK64MC105_EV88G73A <conf> into head / items / tail so the
# ~350 lines of toolchain properties survive verbatim.
# ---------------------------------------------------------------------------

$lines = [System.IO.File]::ReadAllLines($xmlPath)

$confStart = ($lines | Select-String -Pattern '<conf name="CK64MC105_EV88G73A"' | Select-Object -First 1).LineNumber
if (-not $confStart) { throw 'template conf CK64MC105_EV88G73A not found -- has the xml already been migrated?' }
$confStart--   # to 0-based

$confEnd = $confStart
while ($lines[$confEnd] -notmatch '^\s*</conf>') { $confEnd++ }

$confLines = $lines[$confStart..$confEnd]
$firstItem = 0
while ($confLines[$firstItem] -notmatch '^\s*<item path=') { $firstItem++ }
$lastItem = $confLines.Count - 1
while ($confLines[$lastItem] -notmatch '^\s*</item>') { $lastItem-- }

$confHead = $confLines[0..($firstItem - 1)]
$confTail = $confLines[($lastItem + 1)..($confLines.Count - 1)]

function New-ConfBlock {
    param($Cfg, $Head, $Tail, $AllSources)

    $out = New-Object System.Collections.Generic.List[string]

    foreach ($l in $Head) {
        $x = $l
        $x = $x -replace '<conf name="[^"]*"', "<conf name=`"$($Cfg.Name)`""
        $x = $x -replace '<targetDevice>[^<]*</targetDevice>', "<targetDevice>$($Cfg.Device)</targetDevice>"
        $x = $x -replace '<pack name="[^"]*" vendor="Microchip" version="[^"]*"/>', "<pack name=`"$($Cfg.PackName)`" vendor=`"Microchip`" version=`"$($Cfg.PackVer)`"/>"
        $out.Add($x)
    }

    # One <item> per source, ex="true" when this configuration does not build it.
    foreach ($s in $AllSources) {
        $ex = if ($Cfg.Build -contains $s) { 'false' } else { 'true' }
        $out.Add("      <item path=`"../src/$s`" ex=`"$ex`" overriding=`"false`">")
        foreach ($t in @('C30', 'C30-AR', 'C30-AS', 'C30-CO', 'C30-LD', 'C30Global')) {
            $out.Add("        <$t>")
            $out.Add("        </$t>")
        }
        $out.Add('      </item>')
    }

    # Tail: substitute the two per-configuration properties. Written as
    # single-line properties even where the template wrapped them, which the
    # generator normalises away -- MPLAB X reformats on next write anyway.
    $i = 0
    while ($i -lt $Tail.Count) {
        $l = $Tail[$i]
        if ($l -match '<property key="preprocessor-macros"') {
            $indent = ($l -replace '^(\s*).*', '$1')
            $out.Add("$indent<property key=`"preprocessor-macros`" value=`"$($Cfg.Macros)`"/>")
            # the template may spill value=".." onto the following line
            if ($l -notmatch '/>\s*$') { $i++ }
            $i++
            continue
        }
        if ($l -match '<property key="extra-include-directories"') {
            $indent = ($l -replace '^(\s*).*', '$1')
            $out.Add("$indent<property key=`"extra-include-directories`" value=`"$($Cfg.Include)`"/>")
            if ($l -notmatch '/>\s*$') { $i++ }
            $i++
            continue
        }
        # The template configuration carried -O0, which is NOT what this project
        # builds with: build.ps1 has defaulted to -Os since the optimisation work
        # (docs record why -Os for the 64 KB CK64MC105). Left at -O0 the migration
        # would silently inflate every image -- measured 48,120 bytes vs 32,187 for
        # EV88G73A, and 94% of flash for the WM8904 configuration. Note MPLAB writes
        # this as a bare level, so -Os is value="s".
        if ($l -match '<property key="optimization-level"') {
            $indent = ($l -replace '^(\s*).*', '$1')
            $out.Add("$indent<property key=`"optimization-level`" value=`"$($Cfg.Optimization)`"/>")
            if ($l -notmatch '/>\s*$') { $i++ }
            $i++
            continue
        }
        $out.Add($l)
        $i++
    }

    return $out
}

# ---------------------------------------------------------------------------
# Assemble the whole document.
# ---------------------------------------------------------------------------

$doc = New-Object System.Collections.Generic.List[string]
$doc.Add('<?xml version="1.0" encoding="UTF-8"?>')
$doc.Add('<configurationDescriptor version="65">')
$doc.Add('  <logicalFolder name="root" displayName="root" projectFiles="true">')
$doc.Add('    <logicalFolder name="HeaderFiles" displayName="Header Files" projectFiles="true">')
foreach ($h in $allHeaders) { $doc.Add("      <itemPath>../src/$h</itemPath>") }
$doc.Add('    </logicalFolder>')
# projectFiles="true" because that is what prjMakefilesGenerator normalises it to.
# Written as "false" first, which made build.ps1 report "the generator made a real
# change to configurations.xml" on every run -- a warning that is only useful while
# it stays rare, so matching the generator here keeps it meaningful.
$doc.Add('    <logicalFolder name="ExternalFiles" displayName="Important Files" projectFiles="true">')
$doc.Add('      <itemPath>Makefile</itemPath>')
$doc.Add('    </logicalFolder>')
$doc.Add('    <logicalFolder name="LinkerScript" displayName="Linker Files" projectFiles="true">')
$doc.Add('    </logicalFolder>')
$doc.Add('    <logicalFolder name="SourceFiles" displayName="Source Files" projectFiles="true">')
foreach ($s in $allSources) { $doc.Add("      <itemPath>../src/$s</itemPath>") }
$doc.Add('    </logicalFolder>')
$doc.Add('  </logicalFolder>')
$doc.Add('  <sourceRootList>')
$doc.Add('    <Elem>../src</Elem>')
$doc.Add('  </sourceRootList>')
$doc.Add('  <projectmakefile>Makefile</projectmakefile>')
$doc.Add('  <confs>')
foreach ($cfg in $configs) {
    foreach ($l in (New-ConfBlock -Cfg $cfg -Head $confHead -Tail $confTail -AllSources $allSources)) {
        $doc.Add($l)
    }
}
$doc.Add('  </confs>')
$doc.Add('</configurationDescriptor>')

Write-Host "configurations: $($configs.Name -join ', ')"
Write-Host "sources: $($allSources.Count)   headers: $($allHeaders.Count)"
foreach ($cfg in $configs) {
    $built = @($allSources | Where-Object { $cfg.Build -contains $_ }).Count
    Write-Host ("  {0,-28} builds {1,2} / {2}" -f $cfg.Name, $built, $allSources.Count)
    $missing = @($cfg.Build | Where-Object { $allSources -notcontains $_ })
    if ($missing) { throw "config $($cfg.Name) lists sources absent from the union: $($missing -join ', ')" }
}

# Every listed source must exist on disk -- the failure mode this migration is
# fixing in the first place.
$absent = @($allSources | Where-Object { -not (Test-Path (Join-Path $Root "src/$_")) })
if ($absent) { throw "listed sources do not exist: $($absent -join ', ')" }

if ($WhatIf) {
    Write-Host 'WhatIf: not writing.'
    return
}

# CRLF, no BOM -- matching what MPLAB X writes and what .gitattributes expects
# for this path (-text, so git stores the bytes as-is).
$nl = "`r`n"
[System.IO.File]::WriteAllText($xmlPath, (($doc -join $nl) + $nl), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote $xmlPath"
