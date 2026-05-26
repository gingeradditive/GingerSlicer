$ErrorActionPreference = 'Stop'
$base = 'resources\profiles\Ginger Additive\filament'

$updates = @{
    'fdm_filament_common.json'    = @{ old = '"5.2"'; new = '"26.7"' }
    'Generic ABS.json'            = @{ old = '"2.7"'; new = '"7.4"' }
    'Generic ASA.json'            = @{ old = '"2.8"'; new = '"7.6"' }
    'Generic HIPS.json'           = @{ old = '"3.0"'; new = '"8.2"' }
    'Generic PETG.json'           = @{ old = '"4.9"'; new = '"13.3"' }
    'Generic PETG GF.json'        = @{ old = '"4.0"'; new = '"10.9"' }
    'Generic PP.json'             = @{ old = '"8.0"'; new = '"21.6"' }
    'Azure PETG.json'             = @{ old = '"4.9"'; new = '"13.3"' }
    'FormFutura PP Centaur.json'  = @{ old = '"8.0"'; new = '"21.6"' }
    'FormFutura rPETG.json'       = @{ old = '"4.9"'; new = '"13.3"' }
    'FormFutura rPETG 10GF.json'  = @{ old = '"4.0"'; new = '"10.9"' }
    'FormFutura rPETG 20GF.json'  = @{ old = '"4.0"'; new = '"10.9"' }
    'MAIP ASA 5GF.json'           = @{ old = '"2.5"'; new = '"6.8"' }
    'Polymaker PETG 10GF.json'    = @{ old = '"4.0"'; new = '"10.9"' }
    'Polymaker PETG 20GF.json'    = @{ old = '"4.0"'; new = '"10.9"' }
}

foreach ($f in $updates.Keys) {
    $path = Join-Path $base $f
    if (-not (Test-Path -LiteralPath $path)) { Write-Host "MISSING: $f"; continue }
    $content = [System.IO.File]::ReadAllText($path)
    $key = 'cooling_time_per_cross_section'
    $idx = $content.IndexOf($key)
    if ($idx -lt 0) { Write-Host "SKIP (no key): $f"; continue }

    $head = $content.Substring(0, $idx)
    $tail = $content.Substring($idx)
    $oldVal = $updates[$f].old
    $newVal = $updates[$f].new

    # Replace only the FIRST occurrence in tail (the cooling key's value).
    $regex = [regex]::new([regex]::Escape($oldVal))
    $tailNew = $regex.Replace($tail, $newVal, 1)

    if ($tailNew -eq $tail) {
        Write-Host "NO MATCH ($oldVal): $f"
        continue
    }
    [System.IO.File]::WriteAllText($path, $head + $tailNew)
    Write-Host "OK: $f  ($oldVal -> $newVal)"
}
