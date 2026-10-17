# Offline GPIO port verification; no flash, monitor or board access.
param(
    [string]$Python = 'python',
    [string]$MplabXVersion = '6.30'
)
$ErrorActionPreference = 'Stop'
$repoPath = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runPath = Join-Path $repoPath ('build/gpio_port_verify/' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $runPath | Out-Null
$results = @()
Push-Location $repoPath
try {
    & $Python tools/gpio_check/run_gpio_check.py *> (Join-Path $runPath 'gpio_check.log')
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $runPath 'gpio_check.log') -Tail 30
        throw 'GPIO target/host verification failed'
    }
    Write-Host 'PASS GPIO target and host checks (four devices)'
    & $Python tools/irq_atomicity_source_lint.py --root src *> (Join-Path $runPath 'irq_lint.log')
    if ($LASTEXITCODE -ne 0) { throw 'IRQ atomicity source lint failed' }
    Write-Host 'PASS IRQ atomicity source lint'
    foreach ($configuration in @('CK256MC005_EV08P02A', 'CK256MP508_DM330030')) {
        $logPath = Join-Path $runPath ($configuration + '.log')
        & pwsh -NoProfile -File buildtools/build.ps1 -Full -Configuration $configuration `
            -MplabXVersion $MplabXVersion *> $logPath
        if ($LASTEXITCODE -ne 0) {
            Get-Content $logPath -Tail 30
            throw "Build failed: $configuration"
        }
        $logText = Get-Content $logPath -Raw
        $memory = ($logText -split '\r?\n' | Where-Object { $_ -match 'Total "(program|data)" memory used' } | Select-Object -Last 2)
        # -Full cleans previous configurations' output. Preserve each artifact now.
        $artifactPath = Join-Path $repoPath "firmware.X/dist/$configuration/production/firmware.X.production.hex"
        Copy-Item -LiteralPath $artifactPath -Destination (Join-Path $runPath ($configuration + '.hex'))
        $results += [pscustomobject]@{
            configuration = $configuration
            exit_code = 0
            memory = $memory
            hex_sha256 = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash
        }
        Write-Host "PASS full build $configuration"
        $memory | ForEach-Object { Write-Host $_.Trim() }
    }
    $results | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $runPath 'results.json') -Encoding utf8
    Write-Host "Evidence: $runPath"
} finally {
    Pop-Location
}
