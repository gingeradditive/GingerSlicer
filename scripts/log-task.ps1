#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Append a task entry to progress.txt with smart defaults.

.DESCRIPTION
    Reduces the friction of keeping the personal dev log up to date.
    Auto-fills date, infers title from the last git commit, infers
    category from changed files, and prepends a properly-formatted line
    to progress.txt.

    What CANNOT be automated:
      - Cascade message count: not exposed by Windsurf. You must count
        manually (look at the conversation scroll).
      - Real elapsed time: only you know when the task actually started.
        Use `task-start.ps1` / `task-end.ps1` if you want timer-based
        tracking.

.PARAMETER Msgs
    Number of Cascade messages spent on this task. REQUIRED.
    This is the metric you actually want to measure over 2 weeks.

.PARAMETER Title
    Short task description. If omitted, uses the subject of HEAD commit.

.PARAMETER Time
    Wall-clock time spent. Free-form string, e.g. "45m", "~2h", "10m".
    Defaults to "??" so you remember to fill it later.

.PARAMETER Category
    Comma-separated category tags. If omitted, inferred from file paths
    changed in HEAD commit (see Get-CategoryFromPaths).

.PARAMETER Notes
    Optional free-form notes (kept short).

.PARAMETER LogFile
    Path to log file. Default: progress.txt at repo root.

.PARAMETER DryRun
    Print the line that WOULD be added, do not modify the file.

.EXAMPLE
    .\scripts\log-task.ps1 -Msgs 8

.EXAMPLE
    .\scripts\log-task.ps1 -Title "Fix CI version bump" -Time 25m -Msgs 6 -Category TOOLING -Notes "PR #54"

.EXAMPLE
    .\scripts\log-task.ps1 -Msgs 12 -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [int]$Msgs,
    [string]$Title = "",
    [string]$Time = "??",
    [string]$Category = "",
    [string]$Notes = "",
    [string]$LogFile = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $LogFile) {
    $LogFile = Join-Path $repoRoot "progress.txt"
}

if (-not (Test-Path $LogFile)) {
    throw "Log file not found: $LogFile. Create it first or pass -LogFile."
}

# --- Auto-fill title from HEAD commit subject ---
if (-not $Title) {
    try {
        $Title = (& git -C $repoRoot log -1 --format=%s 2>$null).Trim()
    } catch {
        $Title = "(untitled)"
    }
    if (-not $Title) { $Title = "(untitled)" }
}

# --- Auto-infer category from paths in HEAD commit ---
function Get-CategoryFromPaths {
    param([string[]]$Paths)
    $tags = New-Object System.Collections.Generic.HashSet[string]
    foreach ($p in $Paths) {
        $pn = $p -replace '\\', '/'
        switch -Regex ($pn) {
            '^resources/profiles/'                           { [void]$tags.Add("PROFILE") }
            '^\.github/|^scripts/|\.ya?ml$|\.gitignore$'     { [void]$tags.Add("TOOLING") }
            '^src/slic3r/GUI/'                               { [void]$tags.Add("UI") }
            'PressureEqualizer|pellet|PelletERS'             { [void]$tags.Add("PELLET") }
            '^src/libslic3r/(GCode|Print|PerimeterGenerator)' { [void]$tags.Add("PELLET") }
            '^docs/|\.md$|AGENTS\.md|GLOSSARY'               { [void]$tags.Add("DOCS") }
            '^tests/'                                        { [void]$tags.Add("TEST") }
        }
    }
    if ($tags.Count -eq 0) { return "MEDIUM" }
    return ($tags -join ",")
}

if (-not $Category) {
    try {
        $changed = (& git -C $repoRoot diff-tree --no-commit-id --name-only -r HEAD 2>$null) -split "`n" | Where-Object { $_ }
        if ($changed) {
            $Category = Get-CategoryFromPaths -Paths $changed
        } else {
            $Category = "MEDIUM"
        }
    } catch {
        $Category = "MEDIUM"
    }
}

# --- Format the row ---
$date = Get-Date -Format "yyyy-MM-dd"

# Truncate title to keep table readable
$maxTitleLen = 36
if ($Title.Length -gt $maxTitleLen) {
    $Title = $Title.Substring(0, $maxTitleLen - 3) + "..."
}

$titleField  = $Title.PadRight($maxTitleLen)
$timeField   = $Time.PadRight(6)
$msgsField   = ([string]$Msgs).PadRight(4)
$catField    = $Category.PadRight(18)
$line = "$date  $titleField  $timeField  $msgsField  $catField  $Notes".TrimEnd()

if ($DryRun) {
    Write-Host "DRY RUN -- would log the following line:"
    Write-Output $line
    return
}

# --- Insert the line: append at end of file (simplest, chronological) ---
# We append after the last non-empty line to keep entries chronological.
$existing = Get-Content -Path $LogFile -Raw
if (-not $existing.EndsWith("`n")) { $existing += "`r`n" }
$existing += "$line`r`n"
Set-Content -Path $LogFile -Value $existing -NoNewline

Write-Host "Logged:" -ForegroundColor Green
Write-Output $line
