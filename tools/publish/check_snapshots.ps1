#requires -Version 7
<#
.SYNOPSIS
  Verify the published NORA-HAL dsPIC33CK snapshots against this tree.

.DESCRIPTION
  Three checks, in the order that makes a failure readable:

    1. BLOB IDENTITY -- every file in each snapshot's src/ has the same git blob hash as
       its counterpart in this repository's src/hal_<module>/. Blob, not md5 of the
       working file: two files in this tree are checked out with LF while the rest are
       CRLF, so a disk-byte comparison reports a difference that does not exist in either
       repository's history.

    2. EOL -- every tracked file in each snapshot is LF in the repository. This is what
       .gitattributes in the first commit is for; if it ever reports CRLF, the snapshot
       was populated before that file landed.

    3. STANDALONE SYNTAX -- each snapshot's .c files compile with -fsyntax-only -Wall for
       both supported devices, with ONLY the snapshots on the include path (plus the
       TDM conf.h_example copied in as conf.h). This is what proves the snapshots are
       self-contained as published.

  What check 3 is NOT: -fsyntax-only generates no code, links nothing, and runs nothing.
  A green result here is not a build and is not hardware evidence. Do not quote it as
  "the HALs work".

.PARAMETER SnapshotRoot
  Directory holding the four snapshot clones. Default: ..\_publish_nora_ck relative to
  this repository.

.PARAMETER SkipCompile
  Run checks 1 and 2 only (no compiler needed).

.EXAMPLE
  pwsh tools/publish/check_snapshots.ps1
#>
[CmdletBinding()]
param(
    [string] $SnapshotRoot,
    [string] $Compiler = 'C:/Program Files/Microchip/xc-dsc/v3.31.01/bin/xc-dsc-gcc.exe',
    [string] $PackRoot = "$HOME/.mchp_packs/Microchip",
    [switch] $SkipCompile
)

$ErrorActionPreference = 'Stop'

$labRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $SnapshotRoot) { $SnapshotRoot = (Join-Path (Split-Path $labRoot -Parent) '_publish_nora_ck') }
if (-not (Test-Path $SnapshotRoot)) { throw "Snapshot root not found: $SnapshotRoot" }

# module <-> snapshot repository, and the DFP each device needs.
$modules = [ordered]@{
    'dma'          = 'hal_dma'
    'timer'        = 'hal_timer'
    'gpio'         = 'hal_gpio'
    'spi-i2s-tdm'  = 'hal_spi_i2s_tdm'
    'clock'        = 'hal_clock'
    'i2c'          = 'hal_i2c'
    'uart'         = 'hal_uart'
}
$devices = [ordered]@{
    '33CK256MP508' = 'dsPIC33CK-MP_DFP'
    '33CK64MC105'  = 'dsPIC33CK-MC_DFP'
}

$fail = 0

# ---- 1. blob identity -------------------------------------------------------------
Write-Host "== blob identity vs $labRoot" -ForegroundColor Cyan
$labHead = (git -C $labRoot rev-parse --short HEAD)
$n = 0
foreach ($m in $modules.Keys) {
    $repo = Join-Path $SnapshotRoot "nora-hal-dspic33ck-$m"
    if (-not (Test-Path $repo)) { Write-Host "  $m : clone missing -- skipped" -ForegroundColor Yellow; continue }
    foreach ($rel in (git -C $repo ls-files 'src')) {
        $name = Split-Path $rel -Leaf
        if ($name -eq 'README.md') { continue }      # publication-only, declared in the snapshot README
        $a = git -C $repo  rev-parse "HEAD:$rel"                        2>$null
        $b = git -C $labRoot rev-parse "HEAD:src/$($modules[$m])/$name"  2>$null
        $n++
        if ($a -ne $b) { Write-Host "  DIFF $m/$name" -ForegroundColor Red; $fail++ }
    }
}
Write-Host "  $($n - $fail)/$n identical (lab $labHead)" -ForegroundColor $(if ($fail) { 'Red' } else { 'Green' })

# ---- 2. EOL -----------------------------------------------------------------------
Write-Host "== repository EOL" -ForegroundColor Cyan
foreach ($m in $modules.Keys) {
    $repo = Join-Path $SnapshotRoot "nora-hal-dspic33ck-$m"
    if (-not (Test-Path $repo)) { continue }
    $bad = @(git -C $repo ls-files --eol | Where-Object { $_ -notmatch 'i/lf' })
    if ($bad.Count) { Write-Host "  $m : $($bad.Count) non-LF blob(s)" -ForegroundColor Red; $fail += $bad.Count }
    else { Write-Host "  $m : all LF" -ForegroundColor Green }
}

if ($SkipCompile) {
    Write-Host ""
    Write-Host $(if ($fail) { "$fail problem(s)." } else { "OK (compile check skipped)." })
    exit ($fail ? 1 : 0)
}

# ---- 3. standalone syntax ---------------------------------------------------------
Write-Host "== standalone syntax (-fsyntax-only -Wall; NOT a build)" -ForegroundColor Cyan
if (-not (Test-Path $Compiler)) { throw "Compiler not found: $Compiler (pass -Compiler or -SkipCompile)" }

$srcDirs = $modules.Keys | ForEach-Object { Join-Path $SnapshotRoot "nora-hal-dspic33ck-$_/src" }
$includes = $srcDirs | ForEach-Object { "-I$_" }

# The TDM HAL requires a project-supplied conf.h; the shipped example IS one.
$synth = Join-Path ([IO.Path]::GetTempPath()) "nora_ck_snapshot_conf"
New-Item -ItemType Directory -Force -Path $synth | Out-Null
Copy-Item (Join-Path $SnapshotRoot 'nora-hal-dspic33ck-spi-i2s-tdm/src/nora_spi_i2s_tdm_conf.h_example') `
          (Join-Path $synth 'nora_spi_i2s_tdm_conf.h') -Force
$includes += "-I$synth"

$pass = 0; $warn = 0
foreach ($dev in $devices.Keys) {
    $packDir = Join-Path $PackRoot $devices[$dev]
    # Sort as versions, not strings: "1.8.299" sorts ABOVE "1.10.386" lexically, which
    # would silently syntax-check against an older DFP than MPLAB X itself would pick.
    $ver = Get-ChildItem $packDir -Directory |
           Sort-Object { try { [version]$_.Name } catch { [version]'0.0.0' } } -Descending |
           Select-Object -First 1
    if (-not $ver) { throw "No DFP version installed under $packDir" }
    $dfp = Join-Path $ver.FullName 'xc16'      # the dir that holds bin/c30_device.info

    foreach ($dir in $srcDirs) {
        foreach ($c in (Get-ChildItem $dir -Filter *.c)) {
            $out = & $Compiler "-mcpu=$dev" "-mdfp=$dfp" '-fsyntax-only' '-Wall' @includes $c.FullName 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Host "  FAIL $dev $($c.Name)" -ForegroundColor Red
                $out | Select-Object -First 4 | ForEach-Object { Write-Host "    $_" }
                $fail++
            } else {
                $pass++
                $w = @($out | Where-Object { $_ -match 'warning:' })
                if ($w.Count) { $warn += $w.Count; Write-Host "  WARN $dev $($c.Name): $($w.Count)" -ForegroundColor Yellow }
            }
        }
    }
    Write-Host "  $dev (DFP $($ver.Name)) done" -ForegroundColor DarkGray
}
Write-Host "  $pass translation unit(s) clean, $warn warning(s)" -ForegroundColor $(if ($fail) { 'Red' } else { 'Green' })

Write-Host ""
if ($fail) { Write-Host "$fail problem(s)." -ForegroundColor Red; exit 1 }
Write-Host "All checks passed. Remember: check 3 is a syntax check, not a build." -ForegroundColor Green
