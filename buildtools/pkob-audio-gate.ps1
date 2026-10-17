# Helpers for flashauto.ps1. HTTP only: the serial-monitor owns the COM port.
function Resolve-PkobMonitorUrl {
    param([string]$Root, [string]$MonitorRoot, [string]$MonitorUrl)
    . (Join-Path $PSScriptRoot 'serial-monitor-resolver.ps1')
    return Resolve-RunningSerialMonitorUrl -RequestedUrl $MonitorUrl `
        -ExpectedProfile 'ck-ev97' -RepoRoot $Root -MonitorRoot $MonitorRoot
}

function Get-Pkob4PortInventory {
    $json = & (Join-Path $PSScriptRoot 'identify-pkob4.ps1') -Json
    return @((($json -join "`n") | ConvertFrom-Json))
}

function Invoke-PkobVerifiedAudioStop {
    param(
        [string]$MonitorUrl, [string]$Serial,
        [int]$TimeoutSec = 10, [int]$SettleMs = 1500
    )
    $status = Invoke-RestMethod -Uri "$MonitorUrl/status" -TimeoutSec 5
    if ($status.profile -ne 'ck-ev97' -or -not $status.connected) { throw 'Audio stop refused: monitor must be connected with profile ck-ev97.' }
    $inventory = @(Get-Pkob4PortInventory)
    $mapped = @($inventory | Where-Object { $_.com_ports -contains $status.port })
    if ($mapped.Count -ne 1 -or $mapped[0].serial -ne $Serial) {
        throw "Audio stop refused: monitor port $($status.port) does not uniquely belong to PKoB4 $Serial. No command sent."
    }
    $body = @{cmd='*ts'; contains='analog mute verified, TDM/DMA halted'; timeout=$TimeoutSec} | ConvertTo-Json -Compress
    # command_wait arms its matcher before sending; old buffered replies cannot pass.
    $reply = Invoke-RestMethod -Method Post -Uri "$MonitorUrl/command_wait" -ContentType application/json -Body $body -TimeoutSec ($TimeoutSec + 5)
    if (-not $reply.matched -or -not $reply.ok) { throw 'Audio stop refused: codec analog mute was not verified.' }
    if ($SettleMs -gt 0) { Start-Sleep -Milliseconds $SettleMs }
    Write-Host "Audio stop verified: $($status.port), PKoB4 $Serial; *ts, analog mute, TDM/DMA halted; settled ${SettleMs}ms."
}
