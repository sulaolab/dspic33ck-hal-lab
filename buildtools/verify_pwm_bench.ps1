# Read-only verification of a saved CK PWM bench ROM image and configuration.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Manifest)
$ErrorActionPreference = 'Stop'
$path = (Resolve-Path -LiteralPath $Manifest).Path
$m = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
if ($m.configuration -ne 'CK256MC005_EV08P02A' -or $m.fosc_hz -ne 200000000 -or
    $m.sample_rate_hz -ne 48000 -or $m.jumper_required) { throw 'Wrong board/clock profile' }
if ($m.carrier_factor -notin @(1,2,4) -or $m.carrier_requested_hz -ne 48000*$m.carrier_factor) { throw 'Invalid carrier' }
$period = ([int][Math]::Floor(200000000.0 / $m.carrier_requested_hz) + 1) -band (-bnot 1)
if ($m.period_counts -ne $period -or $m.pg4per -ne $period-1 -or
    $m.duty_neutral -ne $period/2) { throw 'Wrong PWM geometry' }
$scope = $m.mode -eq 'ScopeOnly'
if ($m.mode -notin @('ScopeOnly','Normal') -or $m.scope_only -ne $scope -or
    $m.normal_fault_check -eq $scope) { throw 'Wrong stage mode' }
$guard = if ($scope) { [int][Math]::Floor(($period+512)/1024.0) } else { 100 }
if ($m.duty_min -ne $guard -or $m.duty_max -ne $period-$guard) { throw 'Wrong duty limits' }
if (($scope -and $m.pwm_gain_db_x10 -ne 0) -or $m.pwm_gain_db_x10 -lt -240 -or
    $m.pwm_gain_db_x10 -gt 240 -or ($m.pwm_gain_db_x10 % 5) -ne 0) { throw 'Wrong gain' }
foreach ($d in @('EV08P02A_PWM_AUDIO=1','EV08P02A_PWM_AUDIO_HAL_FRC=1',
    "EV08P02A_PWM_CARRIER_FACTOR=$($m.carrier_factor)", "EV08P02A_PWM_GAIN_DB_X10=$($m.pwm_gain_db_x10)",
    "EV08P02A_PWM_SCOPE_ONLY=$([int]$scope)")) {
    if ($d -notin $m.defines) { throw "Missing define: $d" }
}
if (!$m.build_id -or [IO.Path]::GetFileName($m.image_file) -ne $m.image_file) { throw 'Invalid image identity' }
$hex = Join-Path (Split-Path $path -Parent) $m.image_file
if ((Get-FileHash -LiteralPath $hex -Algorithm SHA256).Hash -ne $m.sha256) { throw 'ROM image hash mismatch' }
$hexLines = Get-Content -LiteralPath $hex
foreach ($line in $hexLines) {
    if ($line -notmatch '^:[0-9A-Fa-f]+$') { throw 'Invalid Intel HEX record' }
    $record = $line.Substring(1)
    if (($record.Length % 2) -ne 0) { throw 'Invalid HEX length' }
    $bytes = for ($i=0; $i -lt $record.Length; $i+=2) { [Convert]::ToInt32($record.Substring($i,2),16) }
    if ($bytes.Count -ne $bytes[0]+5 -or (($bytes | Measure-Object -Sum).Sum % 256) -ne 0) { throw 'Invalid HEX checksum' }
}
if ($hexLines[-1] -ne ':00000001FF') { throw 'HEX EOF missing' }
Write-Output "PASS saved ROM: mode=$($m.mode), carrier=$($m.carrier_actual_hz) Hz, duty=$guard..$($m.duty_max), PWM gain=$($m.pwm_gain_db_x10/10) dB"
Write-Output "SHA256: $($m.sha256)"
