# ============================================================================
#  Level-matches the factory presets: runs IWubYouTests --loudness (each preset's
#  output RMS on its category's test source), writes the suggested outGain into
#  vst/presets/factory_presets.json, and measures again. Target -18 dBFS +- 1 dB.
#  Needs a build after it (the presets are compiled into the binary).
#  Usage: powershell -ExecutionPolicy Bypass -File level-match.ps1 [-Passes 2]
# ============================================================================
param([int]$Passes = 2)
$ErrorActionPreference = 'Stop'
$vst = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $env:LOCALAPPDATA 'IWubYouVST\build\IWubYouTests_artefacts\Release\IWubYouTests.exe'
if (-not (Test-Path $exe)) { throw "Build IWubYouTests first: $exe" }
$jsonPath = Join-Path $vst 'presets\factory_presets.json'
$utf8 = [Text.UTF8Encoding]::new($false)

# The measurement comes from the built binary, so each pass needs a rebuild to take effect; the
# first pass computes the gains from the current build, applies them, and the caller rebuilds.
$res = (& $exe --loudness | Out-String) | ConvertFrom-Json
$text = [IO.File]::ReadAllText($jsonPath, $utf8)
$changed = 0
foreach ($r in $res) {
    $old = [double]$r.outGain; $new = [double]$r.suggested
    Write-Host ("{0,-28} {1,6:N1} dB  outGain {2,6:N1} -> {3,6:N1}" -f "$($r.category)/$($r.name)", $r.loudnessDb, $old, $new)
    if ([Math]::Abs($new - $old) -lt 0.05) { continue }
    # the preset's own params block: add or replace its outGain
    $pattern = '(\{\s*"name":\s*"' + [regex]::Escape($r.name) + '"[^}]*?"params":\s*\{)([^}]*)(\})'
    $m = [regex]::Match($text, $pattern)
    if (-not $m.Success) { throw "preset $($r.name) not found in factory_presets.json" }
    $params = $m.Groups[2].Value
    $gainText = $new.ToString('0.0', [Globalization.CultureInfo]::InvariantCulture)
    if ($params -match '"outGain":\s*-?[0-9.]+') { $params = [regex]::Replace($params, '"outGain":\s*-?[0-9.]+', "`"outGain`": $gainText") }
    elseif ($params.Trim() -eq '') { $params = " `"outGain`": $gainText " }
    else { $params = $params.TrimEnd() + ", `"outGain`": $gainText " }
    $text = $text.Substring(0, $m.Groups[2].Index) + $params + $text.Substring($m.Groups[2].Index + $m.Groups[2].Length)
    $changed++
}
[IO.File]::WriteAllText($jsonPath, $text, $utf8)
Write-Host "$changed preset(s) updated in $jsonPath. Rebuild (vst\build.bat) and run IWubYouTests.exe to verify."
