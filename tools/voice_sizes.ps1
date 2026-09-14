<#
voice_sizes.ps1 -- the three images phase 7 has to be judged by, built from one tree
with one clean build each so the numbers are comparable:

  type_ty only   must stay at the pre-phase-7 size (the run-time bound is #if'd out)
  type_lb only    likewise
  both          the new one: two resident voices, run-time exclusive

Throwaway measurement harness, same shape and same -D smuggling trick as
romopt_sweep.ps1 (a -Define value that starts with -D reaches the compiler verbatim).
#>
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$variants = [ordered]@{
    'type_ty only' = @()
    'type_lb only'  = @('AVAS_CK_VOICE_TYPE_LB=1')
    'both'        = @('-DAVAS_CK_VOICE_TYPE_LB=1 -DAVAS_CK_VOICE_BOTH=1')
}

$results = [ordered]@{}
foreach ($name in $variants.Keys) {
    Write-Host "=== $name ===" -ForegroundColor Cyan
    if ($variants[$name].Count -eq 0) {
        $out = & ./buildtools/build.ps1 -Full -Configuration CK64MC105_EV88G73A -BuildId sizeref 2>&1
    } else {
        $out = & ./buildtools/build.ps1 -Full -Configuration CK64MC105_EV88G73A `
                   -Define $variants[$name] -BuildId sizeref 2>&1
    }
    $line = $out | Select-String -Pattern 'Total "program" memory used' | Select-Object -Last 1
    $dline = $out | Select-String -Pattern 'Total "data" memory used' | Select-Object -Last 1
    if ($line -match '\((\d+)\)\s+(\d+)%') {
        $b = [int]$Matches[1]; $p = [int]$Matches[2]
        $d = if ($dline -match '\((\d+)\)\s+(\d+)%') { [int]$Matches[1] } else { -1 }
        $results[$name] = [pscustomobject]@{ Bytes = $b; Pct = $p; Data = $d }
    } else {
        $results[$name] = [pscustomobject]@{ Bytes = -1; Pct = -1; Data = -1 }
        $out | Select-String -Pattern 'error|Error' | Select-Object -First 12 |
            ForEach-Object { Write-Host $_ }
    }
    Write-Host ("    {0} B program, {1} B data" -f $results[$name].Bytes, $results[$name].Data)
}

Write-Host ''
Write-Host '| image | program | % of 66 432 | free | data |'
Write-Host '|---|---|---|---|---|'
foreach ($name in $results.Keys) {
    $r = $results[$name]
    if ($r.Bytes -lt 0) { Write-Host ("| {0} | BUILD FAILED | | | |" -f $name); continue }
    Write-Host ("| {0} | {1} | {2} % | {3} | {4} |" -f $name, $r.Bytes, $r.Pct,
                (66432 - $r.Bytes), $r.Data)
}
