#requires -Version 7
<#
.SYNOPSIS
  Apply the GitHub description and topics from nora_repo_meta.json to each repository.

.DESCRIPTION
  Typing topics into the GitHub web UI for every repository is the part of publishing
  that is pure transcription, so it lives in a file instead. This script reads
  nora_repo_meta.json, compares it against what GitHub currently holds, and prints a
  per-repository diff. Without -WhatIf it then applies the difference.

  Idempotent: a repository already matching the file is reported as "up to date" and
  no API write is made. Topics are compared as a SET (GitHub sorts and lowercases
  them itself), so re-ordering the JSON array is not a change.

  Requires the gh CLI, authenticated with a token that can edit the repositories.

.PARAMETER Repo
  Only act on repositories whose name matches this wildcard (e.g. '*ck-*').

.PARAMETER WhatIf
  Show the diff and exit without writing anything.

.EXAMPLE
  pwsh tools/publish/set_repo_meta.ps1 -WhatIf
  pwsh tools/publish/set_repo_meta.ps1 -Repo '*dspic33ck*'
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string] $MetaFile = (Join-Path $PSScriptRoot 'nora_repo_meta.json'),
    [string] $Repo     = '*'
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "gh CLI not found on PATH. Install it, or run 'gh auth login' first."
}
if (-not (Test-Path $MetaFile)) { throw "Metadata file not found: $MetaFile" }

$meta  = Get-Content -Raw -LiteralPath $MetaFile | ConvertFrom-Json
$owner = $meta.owner
if ([string]::IsNullOrWhiteSpace($owner)) { throw "'owner' missing from $MetaFile" }

$changed = 0
$checked = 0

foreach ($entry in $meta.repos) {
    if ($entry.name -notlike $Repo) { continue }
    $checked++

    $full = "$owner/$($entry.name)"
    $want = @{
        description = [string]$entry.description
        topics      = @($entry.topics | ForEach-Object { $_.ToString().ToLowerInvariant() } | Sort-Object -Unique)
    }

    # Reject values GitHub will not accept, before we start writing anything.
    $bad = @($want.topics | Where-Object { $_ -notmatch '^[a-z0-9][a-z0-9-]{0,49}$' })
    if ($bad.Count -gt 0) {
        throw "$full : invalid topic(s) -- GitHub allows lowercase letters, digits and hyphens only: $($bad -join ', ')"
    }

    try {
        $current = gh api "repos/$full" --jq '{description, topics}' 2>$null | ConvertFrom-Json
    } catch {
        Write-Host "$full : NOT FOUND (or no access) -- skipped" -ForegroundColor Yellow
        continue
    }

    $haveDesc   = [string]$current.description
    $haveTopics = @($current.topics | Sort-Object -Unique)

    $descDiffers   = ($haveDesc -ne $want.description)
    $topicsDiffers = (Compare-Object -ReferenceObject $haveTopics -DifferenceObject $want.topics `
                        -SyncWindow 0 | Measure-Object).Count -gt 0

    if (-not ($descDiffers -or $topicsDiffers)) {
        Write-Host "$full : up to date" -ForegroundColor DarkGray
        continue
    }

    Write-Host "$full" -ForegroundColor Cyan
    if ($descDiffers) {
        Write-Host "  description:" -ForegroundColor Yellow
        Write-Host "    -  $(if ($haveDesc) { $haveDesc } else { '(empty)' })"
        Write-Host "    +  $($want.description)"
    }
    if ($topicsDiffers) {
        $add = @($want.topics | Where-Object { $_ -notin $haveTopics })
        $del = @($haveTopics  | Where-Object { $_ -notin $want.topics })
        Write-Host "  topics:" -ForegroundColor Yellow
        if ($del.Count) { Write-Host "    -  $($del -join ', ')" }
        if ($add.Count) { Write-Host "    +  $($add -join ', ')" }
    }

    if (-not $PSCmdlet.ShouldProcess($full, 'update description/topics')) { continue }

    if ($descDiffers) {
        gh repo edit $full --description $want.description | Out-Null
    }
    if ($topicsDiffers) {
        # PUT replaces the whole set, which is why the JSON file is the source of truth.
        $args = @('api', '-X', 'PUT', "repos/$full/topics",
                  '-H', 'Accept: application/vnd.github+json')
        foreach ($t in $want.topics) { $args += @('-f', "names[]=$t") }
        gh @args | Out-Null
    }

    Write-Host "  applied" -ForegroundColor Green
    $changed++
}

if ($checked -eq 0) {
    Write-Host "No repository in $MetaFile matched -Repo '$Repo'." -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host "$checked repositor$(if ($checked -eq 1) { 'y' } else { 'ies' }) checked, $changed updated."
}
