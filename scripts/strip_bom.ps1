param(
    [string]$Root = 'resources\profiles\Ginger Additive',
    [string]$IndexFile = 'resources\profiles\Ginger Additive.json',
    [switch]$Fix
)
$ErrorActionPreference = 'Stop'

function Test-Bom([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    return ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
}

function Remove-Bom([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    $stripped = New-Object byte[] ($b.Length - 3)
    [Array]::Copy($b, 3, $stripped, 0, $b.Length - 3)
    [System.IO.File]::WriteAllBytes($path, $stripped)
}

$paths = @()
$paths += Get-ChildItem -Recurse -LiteralPath $Root -Filter '*.json' | ForEach-Object { $_.FullName }
if (Test-Path -LiteralPath $IndexFile) { $paths += (Resolve-Path -LiteralPath $IndexFile).Path }

$bomFiles = @()
foreach ($p in $paths) {
    if (Test-Bom $p) { $bomFiles += $p }
}

Write-Host "Total scanned: $($paths.Count)"
Write-Host "Files with BOM: $($bomFiles.Count)"
foreach ($f in $bomFiles) { Write-Host "  $f" }

if ($Fix -and $bomFiles.Count -gt 0) {
    foreach ($f in $bomFiles) {
        Remove-Bom $f
        Write-Host "Stripped: $f"
    }
}
