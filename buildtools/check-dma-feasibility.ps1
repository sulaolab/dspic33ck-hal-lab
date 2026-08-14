param(
    [string]$ProjectDir = (Join-Path $PSScriptRoot '..\firmware.X'),
    [string]$SourceDir = (Join-Path $PSScriptRoot '..\src'),
    [string]$XcDscVersion = '3.31.01',
    [string]$MplabXVersion = '6.30',
    [string]$DfpVersion = '1.15.423'
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description not found: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Command,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    Write-Host "==> $Description"
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

$projectRoot = Resolve-ExistingPath -Path $ProjectDir -Description 'MPLAB X project directory'
$sourceRoot = Resolve-ExistingPath -Path $SourceDir -Description 'source directory'
$xcRoot = Resolve-ExistingPath -Path "C:\Program Files\Microchip\xc-dsc\v$XcDscVersion" -Description 'XC-DSC install'
$mplabRoot = Resolve-ExistingPath -Path "C:\Program Files\Microchip\MPLABX\v$MplabXVersion" -Description 'MPLAB X install'
$dfpRoot = Resolve-ExistingPath -Path (Join-Path $mplabRoot "packs\Microchip\dsPIC33CK-MP_DFP\$DfpVersion\xc16") -Description 'dsPIC33CK-MP DFP'
$gcc = Resolve-ExistingPath -Path (Join-Path $xcRoot 'bin\xc-dsc-gcc.exe') -Description 'xc-dsc-gcc'

$objectRoot = Join-Path $projectRoot 'build\dma-feasibility'
New-Item -ItemType Directory -Force $objectRoot | Out-Null

$sourceFile = Join-Path $sourceRoot 'spikes\ck_spi_dma_pingpong_feasibility_probe.c'
$objectFile = Join-Path $objectRoot 'ck_spi_dma_pingpong_feasibility_probe.o'
$depFile = "$objectFile.d"

Write-Host "Project: $projectRoot"
Write-Host "Source:  $sourceRoot"
Write-Host "DFP:     $dfpRoot"

Invoke-CheckedCommand -Description 'Compile CK SPI DMA ping-pong feasibility probe' -Command {
    & $gcc $sourceFile `
        -o $objectFile `
        -c `
        -mcpu=33CK256MP508 `
        -MP `
        -MMD `
        -MF $depFile `
        -mno-eds-warn `
        -g `
        -omf=elf `
        -O0 `
        "-I$sourceRoot" `
        '-Wall' `
        '-msfr-warn=off' `
        "-mdfp=$dfpRoot"
}

Write-Host "Feasibility object: $objectFile"
