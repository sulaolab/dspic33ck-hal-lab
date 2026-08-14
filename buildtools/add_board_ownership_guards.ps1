<#
add_board_ownership_guards.ps1 -- ONE-SHOT. Same status as
migrate_configurations_xml.ps1: a mechanical edit applied once, kept as the
record of what was done, not part of the build.

WHY
---
configurations.xml is now the source of truth for which sources each
configuration compiles, expressed as <item ex="true"> exclusions. That is a
better place for the truth than a hand-maintained list in build.ps1, but it has
its own failure mode: get an exclusion wrong and a board's source is compiled
into the other board's image, where it will reference registers and pins that
mean something different -- or nothing -- on that part. The compiler would be
perfectly happy.

So each board-owned source asserts its own ownership. If the xml is wrong the
build stops with a message that names the file, the configuration it belongs to,
and where to go and fix it. This is the pattern dspic33ak-audio-dsp-sonora uses (see
src/apps/asrc/asrc_app.c), adopted here for the same reason.

Only .c files get the guard: they are what gets compiled, so they are where an
exclusion mistake shows up. Headers are pulled in by whoever needs them.
#>

param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'

$boards = @{
    'dm330030' = @{ Define = 'DSPIC33CK_BOARD_DM330030'; Conf = 'CK256MP508_DM330030' }
    'ev88g73a' = @{ Define = 'DSPIC33CK_BOARD_EV88G73A'; Conf = 'CK64MC105_EV88G73A' }
}

$changed = 0
$skipped = 0

foreach ($board in $boards.Keys) {
    $def  = $boards[$board].Define
    $conf = $boards[$board].Conf
    $dir  = Join-Path $Root "src\boards\$board"

    foreach ($file in (Get-ChildItem $dir -Filter *.c)) {
        $path = $file.FullName
        $rel  = "boards/$board/$($file.Name)"

        # Read as bytes -> text so the original line ending can be reproduced
        # exactly. A stray CRLF->LF conversion here would show up as a
        # whole-file diff, which is how this kind of edit hides a real change.
        $bytes = [System.IO.File]::ReadAllBytes($path)
        $text  = [System.Text.Encoding]::UTF8.GetString($bytes)
        $nl    = if ($text -match "`r`n") { "`r`n" } else { "`n" }
        $lines = $text -split "`r?`n"

        $guard = @(
            "#ifndef $def",
            "#error `"$rel is $($board.ToUpper())-owned. Build it only in the $conf configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml.`"",
            '#endif'
        )

        # Already guarded? Replace just the #error text so the message points at
        # configurations.xml, and leave everything else alone.
        $existing = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match "^\s*#ifndef\s+$def\s*$") { $existing = $i; break }
        }
        if ($existing -ge 0) {
            $errLine = $existing + 1
            while ($errLine -lt $lines.Count -and $lines[$errLine] -notmatch '^\s*#error') { $errLine++ }
            if ($errLine -lt $lines.Count -and $lines[$errLine] -ne $guard[1]) {
                $lines[$errLine] = $guard[1]
                if (-not $WhatIf) {
                    [System.IO.File]::WriteAllText($path, ($lines -join $nl), (New-Object System.Text.UTF8Encoding($false)))
                }
                Write-Host "  reworded  $rel"
                $changed++
            } else {
                $skipped++
            }
            continue
        }

        # Not guarded: insert before the first #include, i.e. after any leading
        # licence/description comment. Preprocessor-only, so it needs no header.
        $ins = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^\s*#include') { $ins = $i; break }
        }
        if ($ins -lt 0) { throw "$rel has no #include -- insertion point unclear, handle by hand" }

        $new = New-Object System.Collections.Generic.List[string]
        if ($ins -gt 0) { $new.AddRange([string[]]$lines[0..($ins - 1)]) }
        $new.AddRange([string[]]$guard)
        $new.Add('')
        $new.AddRange([string[]]$lines[$ins..($lines.Count - 1)])

        if (-not $WhatIf) {
            [System.IO.File]::WriteAllText($path, ($new -join $nl), (New-Object System.Text.UTF8Encoding($false)))
        }
        Write-Host "  guarded   $rel"
        $changed++
    }
}

Write-Host "changed=$changed already-correct=$skipped$(if ($WhatIf) { '  (WhatIf: nothing written)' })"
