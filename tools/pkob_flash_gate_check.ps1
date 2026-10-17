# Offline checks: no HTTP request, COM open, MDB connection or flash.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../buildtools/identify-pkob4.ps1')
. (Join-Path $PSScriptRoot '../buildtools/pkob-audio-gate.ps1')
$fixture = "Target device dsPIC33CK1024MP710 found.`nDevice Id = 0xa0650000`nUDID1 = 0e135c`n"
if (-not (ConvertFrom-PkobMdbLog $fixture dsPIC33CK1024MP710).matched) { throw 'Valid target rejected' }
if ((ConvertFrom-PkobMdbLog ($fixture.Replace('0xa0650000','0x0')) dsPIC33CK1024MP710).matched) { throw 'Zero Device ID accepted' }
if ((ConvertFrom-PkobMdbLog $fixture dsPIC33CK256MP508).matched) { throw 'Different device accepted' }

$script:scenario = 'ok'
$script:sends = 0
function Get-Pkob4PortInventory {
    @([pscustomobject]@{serial='TARGET';com_ports=@('COM99')})
}
function Invoke-RestMethod {
    param($Uri, $Method, $Body, $ContentType, $TimeoutSec)
    if ($Uri -like '*/status') {
        return @{profile=$(if ($script:scenario -eq 'profile') {'other'} else {'ck-ev97'});connected=$true;port='COM99'}
    }
    $script:sends++
    if ($Uri -notlike '*/command_wait' -or $Method -ne 'Post') { throw 'Must arm before sending' }
    $request = $Body | ConvertFrom-Json
    if ($request.cmd -ne '*ts' -or $request.contains -ne 'analog mute verified, TDM/DMA halted') { throw 'Incorrect mute contract' }
    if ($script:scenario -eq 'timeout') { throw 'mock timeout' }
    return @{ok=$true;matched=($script:scenario -ne 'unverified')}
}
try {
    Invoke-PkobVerifiedAudioStop -MonitorUrl http://test -Serial TARGET -SettleMs 0
    if ($script:sends -ne 1) { throw 'Expected one mute command' }
    foreach ($scenario in @('serial','profile','unverified','timeout')) {
        $script:scenario = $scenario
        $script:sends = 0
        $expected = switch ($scenario) {
            serial {'does not uniquely belong'}
            profile {'profile ck-ev97'}
            unverified {'analog mute was not verified'}
            timeout {'mock timeout'}
        }
        $failed = $false
        try {
            $targetSerial = if ($scenario -eq 'serial') {'OTHER'} else {'TARGET'}
            Invoke-PkobVerifiedAudioStop -MonitorUrl http://test -Serial $targetSerial -SettleMs 0
        } catch {
            if ($_.Exception.Message -notlike "*$expected*") { throw }
            $failed = $true
        }
        if (-not $failed) { throw "Gate accepted $scenario" }
        if ($scenario -in @('serial','profile') -and $script:sends -ne 0) { throw 'Wrong board received a command' }
    }
    Write-Output 'PASS identity parser, atomic mute contract, wrong serial/profile, unverified mute, timeout'
} finally {
    Remove-Item Function:\Invoke-RestMethod
    Remove-Item Function:\Get-Pkob4PortInventory
}
