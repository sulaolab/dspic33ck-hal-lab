# Save a complete CK256MC005 PWM bench ROM image. No hardware access.
[CmdletBinding()]
param(
    [ValidateSet('ScopeOnly', 'Normal')][string]$Mode = 'ScopeOnly',
    [ValidateSet(48000, 96000, 192000)][int]$CarrierHz = 192000,
    [ValidateRange(-240, 240)][int]$PwmGainDbX10 = 0
)
$ErrorActionPreference = 'Stop'
if (($PwmGainDbX10 % 5) -ne 0) { throw 'PWM gain must use 0.5 dB steps' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$configuration = 'CK256MC005_EV08P02A'
$scope = $Mode -eq 'ScopeOnly'
$factor = [int]($CarrierHz / 48000)
$gain = if ($scope) { 0 } else { $PwmGainDbX10 }
$defines = @('EV08P02A_PWM_AUDIO=1',
             'EV08P02A_PWM_AUDIO_HAL_FRC=1', "EV08P02A_PWM_CARRIER_FACTOR=$factor",
             "EV08P02A_PWM_GAIN_DB_X10=$gain", "EV08P02A_PWM_SCOPE_ONLY=$([int]$scope)")
$stamp = Get-Date -Format 'yyyyMMddTHHmmssfff'
$out = Join-Path $repo "artifacts/pwm_bench/$stamp-$($Mode.ToLowerInvariant())-${CarrierHz}hz"
New-Item -ItemType Directory -Path $out | Out-Null
$log = Join-Path $out 'build.log'
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration $configuration -Define $defines -Full *> $log
if ($LASTEXITCODE -ne 0) {
    Get-Content $log -Tail 25
    throw "Build failed; see $log"
}
$name = "ck256mc005_pwm_$($Mode.ToLowerInvariant())_${CarrierHz}hz.factory.hex"
$hex = Join-Path $out $name
Copy-Item -LiteralPath (Join-Path $repo "firmware.X/dist/$configuration/production/firmware.X.production.hex") -Destination $hex
$header = Get-Content (Join-Path $repo "firmware.X/build/$configuration/production/generated/ev08p02a_build_id.h") -Raw
$buildId = [regex]::Match($header, '#define EV08P02A_BUILD_ID "([^"]+)"').Groups[1].Value
if (!$buildId) { throw 'Build ID missing' }
$sourceFiles = @('src/app/wm8904_audio.c', 'src/app/wm8904_audio.h',
    'src/boards/ev08p02a/main.c', 'src/boards/ev08p02a/board_profile.h',
    'src/boards/ev08p02a/ev08p02a_bench_controls.h', 'src/boards/ev08p02a/ev08p02a_pwm_audio.h',
    'src/boards/ev08p02a/ev08p02a_pwm_audio_hal.c',
    'src/hal_pwm_audio/nora_pwm_audio.c', 'src/hal_pwm_audio/nora_pwm_audio.h',
    'src/hal_pwm_audio/nora_pwm_audio_backend.h', 'src/hal_pwm_audio/nora_pwm_audio_modulator.c',
    'src/hal_pwm_audio/nora_pwm_audio_modulator.h',
    'src/nora_pwm_audio_conf.h')
$hashes = [ordered]@{}
foreach ($file in $sourceFiles) { $hashes[$file] = (Get-FileHash (Join-Path $repo $file) -Algorithm SHA256).Hash }
$period = ([int][Math]::Floor(200000000.0 / $CarrierHz) + 1) -band (-bnot 1)
$guard = if ($scope) { [int][Math]::Floor(($period+512) / 1024.0) } else { 100 }
$manifest = [ordered]@{
    configuration = $configuration
    mode = $Mode
    source_commit = (& git -C $repo rev-parse HEAD)
    source_hashes = $hashes
    defines = $defines
    build_id = $buildId
    image_file = $name
    sha256 = (Get-FileHash $hex -Algorithm SHA256).Hash
    fosc_hz = 200000000
    sample_rate_hz = 48000
    carrier_requested_hz = $CarrierHz
    carrier_actual_hz = 200000000.0 / $period
    carrier_factor = $factor
    period_counts = $period
    pg4per = $period - 1
    duty_min = $guard
    duty_neutral = $period / 2
    duty_max = $period - $guard
    pwm_gain_db_x10 = $gain
    scope_only = $scope
    normal_fault_check = !$scope
    jumper_required = $false
    programmed = $false
}
$manifestPath = Join-Path $out 'manifest.json'
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
& (Join-Path $PSScriptRoot 'verify_pwm_bench.ps1') -Manifest $manifestPath
if ($LASTEXITCODE -ne 0) { throw 'ROM image verification failed' }
Write-Output "Build ID: $buildId"
Write-Output "Manifest: $manifestPath"
Write-Output "ROM image: $hex"
Write-Output 'Prepared only; not programmed.'
