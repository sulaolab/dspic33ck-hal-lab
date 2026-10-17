<#
List PKoB4 serial numbers and their CDC COM ports through Windows PnP parents.
No COM port is opened. -Probe connects MDB to ONE explicitly selected serial;
this can reset/halt that target, but never programs or erases it.
Requires PowerShell 7 on Windows. Dot-source to use ConvertFrom-PkobMdbLog.
#>
[CmdletBinding()]
param(
    [switch]$Probe,
    [ValidatePattern('^[A-Za-z0-9]+$')][string]$Serial,
    [ValidatePattern('^[A-Za-z0-9]+$')][string]$ExpectedDevice = 'dsPIC33CK1024MP710',
    [string]$MdbPath = 'C:\Program Files\Microchip\MPLABX\v6.35\mplab_platform\bin\mdb.bat',
    [ValidateRange(10, 300)][int]$TimeoutSeconds = 120,
    [string]$LogDir = (Join-Path $PSScriptRoot '../build/pkob-identify'),
    [switch]$Json
)
$ErrorActionPreference = 'Stop'

function ConvertFrom-PkobMdbLog {
    param([string]$Text, [string]$Device)
    $found = $Text -match ('(?m)^Target device ' + [regex]::Escape($Device) + ' found\.')
    $id = [regex]::Match($Text, '(?mi)^Device Id\s*=\s*0x([0-9a-f]+)\s*$')
    $nonzero = $id.Success -and ([Convert]::ToUInt32($id.Groups[1].Value, 16) -ne 0)
    $udid = @([regex]::Matches($Text, '(?mi)^UDID\d+\s*=\s*[0-9a-f]+\s*$') | ForEach-Object { $_.Value.Trim() })
    [pscustomobject]@{
        matched = [bool]($found -and $nonzero)
        expected_device = $Device
        device_id = if ($id.Success) { '0x' + $id.Groups[1].Value } else { $null }
        udid = $udid
    }
}
if ($MyInvocation.InvocationName -eq '.') { return }
if ($Probe -and -not $Serial) { throw '-Probe requires an explicit -Serial; no automatic selection is allowed.' }

$devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_04D8&PID_810B*' })
$parents = @($devices | Where-Object { $_.InstanceId -match '^USB\\VID_04D8&PID_810B\\[^\\]+$' })
$children = @($devices | Where-Object { $_.InstanceId -like '*&MI_01\*' })
$inventory = @(foreach ($parent in $parents) {
    $ports = @(foreach ($child in $children) {
        $parentId = (Get-PnpDeviceProperty -InstanceId $child.InstanceId -KeyName DEVPKEY_Device_Parent).Data
        if ($parentId -eq $parent.InstanceId -and $child.FriendlyName -match '\((COM\d+)\)') { $Matches[1] }
    })
    [pscustomobject]@{
        serial = ($parent.InstanceId -split '\\')[-1]
        com_ports = $ports
        usb_parent = $parent.InstanceId
    }
})
if (-not $Probe) {
    if ($Json) { ConvertTo-Json -InputObject $inventory -Depth 4 }
    else { $inventory | Format-Table serial, @{n='COM';e={$_.com_ports -join ','}}, usb_parent -AutoSize }
    return
}
$selected = @($inventory | Where-Object serial -eq $Serial)
if ($selected.Count -ne 1) { throw "Serial $Serial does not identify exactly one present PKoB4." }
$mdbFile = (Resolve-Path -LiteralPath $MdbPath).Path
$resolvedLogDir = [IO.Path]::GetFullPath($LogDir)
New-Item -ItemType Directory -Path $resolvedLogDir -Force | Out-Null
$stem = Join-Path $resolvedLogDir ('identify-' + $Serial + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$scriptPath = $stem + '.mdb'
$logPath = $stem + '.log'
@("Device $ExpectedDevice", "Hwtool PKOB4 -p <sn>$Serial", 'Quit') | Set-Content -LiteralPath $scriptPath -Encoding ascii
Write-Host "MDB connect/reset: $Serial ($($selected[0].com_ports -join ',')), expected $ExpectedDevice. No programming."
$psi = [Diagnostics.ProcessStartInfo]::new()
$psi.FileName = (Get-Command pwsh).Source
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.ArgumentList.Add('-NoProfile')
$psi.ArgumentList.Add('-Command')
# PowerShell single-quoted literals, not JSON or command-substitution escaping.
$psi.ArgumentList.Add("& '" + $mdbFile.Replace("'", "''") + "' '" + $scriptPath.Replace("'", "''") + "'; exit `$LASTEXITCODE")
$process = [Diagnostics.Process]::new()
$process.StartInfo = $psi
try {
    if (-not $process.Start()) { throw 'Could not launch MDB.' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
    $logText = $stdout.GetAwaiter().GetResult() + "`n" + $stderr.GetAwaiter().GetResult()
    Set-Content -LiteralPath $logPath -Value $logText -Encoding utf8
    if ($timedOut) { throw "MDB timeout; only this process tree was stopped. Evidence: $logPath" }
    $result = ConvertFrom-PkobMdbLog -Text $logText -Device $ExpectedDevice
    $result | Add-Member NoteProperty serial $Serial
    $result | Add-Member NoteProperty com_ports $selected[0].com_ports
    $result | Add-Member NoteProperty log_file $logPath
    if ($Json) { $result | ConvertTo-Json -Depth 4 } else { $result | Format-List }
    if (-not $result.matched -or $process.ExitCode -ne 0) {
        throw "Target identity was not confirmed. Do not program it. Evidence: $logPath"
    }
} finally { $process.Dispose() }
