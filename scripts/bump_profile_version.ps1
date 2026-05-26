param(
    [string]$Old = '3.0.0.3',
    [string]$New = '3.0.0.4'
)
$ErrorActionPreference = 'Stop'
$root = 'resources\profiles\Ginger Additive'
$files = Get-ChildItem -Recurse -LiteralPath $root -Filter '*.json'
$oldPattern = '"version": "' + [regex]::Escape($Old) + '"'
$newStr = '"version": "' + $New + '"'
$count = 0
foreach ($f in $files) {
    $c = [System.IO.File]::ReadAllText($f.FullName)
    $n = [regex]::Replace($c, $oldPattern, $newStr)
    if ($n -ne $c) {
        [System.IO.File]::WriteAllText($f.FullName, $n)
        $count++
    }
}
Write-Host "Bumped $count files from $Old to $New"
