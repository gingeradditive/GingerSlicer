param(
    [string]$Old = '3.0.0.3',
    [string]$New = '3.0.0.4'
)
$ErrorActionPreference = 'Stop'
# UTF-8 without BOM. The OrcaSlicer profile validator rejects BOMs.
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$root = 'resources\profiles\Ginger Additive'
$files = Get-ChildItem -Recurse -LiteralPath $root -Filter '*.json'
$oldPattern = '"version": "' + [regex]::Escape($Old) + '"'
$newStr = '"version": "' + $New + '"'
$count = 0
foreach ($f in $files) {
    $c = [System.IO.File]::ReadAllText($f.FullName)
    $n = [regex]::Replace($c, $oldPattern, $newStr)
    if ($n -ne $c) {
        [System.IO.File]::WriteAllText($f.FullName, $n, $utf8NoBom)
        $count++
    }
}
Write-Host "Bumped $count files from $Old to $New"
