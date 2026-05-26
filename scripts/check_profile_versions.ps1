param([string]$Expected = '3.0.0.4')
$root = 'resources\profiles\Ginger Additive'
$files = Get-ChildItem -Recurse -LiteralPath $root -Filter '*.json'
$mismatch = @()
foreach ($x in $files) {
    $c = [System.IO.File]::ReadAllText($x.FullName)
    $m = [regex]::Match($c, '"version":\s*"([^"]+)"')
    $v = if ($m.Success) { $m.Groups[1].Value } else { 'NONE' }
    if ($v -ne $Expected) {
        $mismatch += [PSCustomObject]@{ Version = $v; Path = $x.FullName }
    }
}
Write-Host "Total JSONs: $($files.Count)"
Write-Host "Mismatched: $($mismatch.Count)"
$mismatch | Format-Table -AutoSize
