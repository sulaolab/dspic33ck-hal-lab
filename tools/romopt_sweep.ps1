<#
romopt_sweep.ps1 -- price dead-code/dead-data elimination options on the
Type_LB + Type_TY both-voices-resident image.

Throwaway measurement harness for the ROM-option sweep. It
builds the EV88G73A configuration once per flag set and prints one table, so the
numbers come from the same tree, the same DFP and the same clean build every
time. -Define values that begin with -D are passed to the compiler verbatim,
which is how a non-macro flag gets in without build.ps1 growing an option.
#>
param([switch]$IncludeSingleVoice)

$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$both = 'AVAS_CK_VOICE_TYPE_LB=1'

$variants = [ordered]@{
    'both, baseline'                 = @($both, '-DAVAS_CK_VOICE_BOTH=1')
    'both, -fdata-sections'          = @($both, '-DAVAS_CK_VOICE_BOTH=1 -fdata-sections')
    'both, -mpa'                     = @($both, '-DAVAS_CK_VOICE_BOTH=1 -mpa')
    'both, -fdata-sections -mpa'     = @($both, '-DAVAS_CK_VOICE_BOTH=1 -fdata-sections -mpa')
}
if ($IncludeSingleVoice) {
    $variants['type_lb only, -fdata-sections -mpa'] = @($both, '-Dx1=1 -fdata-sections -mpa')
    $variants['type_ty only, -fdata-sections -mpa'] = @('-Dx1=1 -fdata-sections -mpa')
}

$results = [ordered]@{}
foreach ($name in $variants.Keys) {
    Write-Host "=== $name ===" -ForegroundColor Cyan
    $out = & ./buildtools/build.ps1 -Full -Configuration CK64MC105_EV88G73A `
               -Define $variants[$name] -BuildId sizeref 2>&1
    $line = $out | Select-String -Pattern 'Total "program" memory used' | Select-Object -Last 1
    if ($line -match '\((\d+)\)\s+(\d+)%') {
        $results[$name] = [pscustomobject]@{ Bytes = [int]$Matches[1]; Pct = [int]$Matches[2] }
    } else {
        $results[$name] = [pscustomobject]@{ Bytes = -1; Pct = -1 }
        $out | Select-String -Pattern 'error|Error' | Select-Object -First 5 | ForEach-Object { Write-Host $_ }
    }
    Write-Host ("    {0} B" -f $results[$name].Bytes)
}

Write-Host ''
Write-Host '| variant | program | % of 66 432 | free | delta vs baseline |'
Write-Host '|---|---|---|---|---|'
$base = $results['both, baseline'].Bytes
foreach ($name in $results.Keys) {
    $b = $results[$name].Bytes
    if ($b -lt 0) { Write-Host ("| {0} | BUILD FAILED | | | |" -f $name); continue }
    Write-Host ("| {0} | {1} | {2} % | {3} | {4:+#;-#;0} |" -f `
        $name, $b, $results[$name].Pct, (66432 - $b), ($b - $base))
}
