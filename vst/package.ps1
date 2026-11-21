# ============================================================================
#  Builds the release package dist\IWubYou-<version>-win64.zip: the VST3 bundle,
#  the Standalone app, install / uninstall scripts, the 96 factory presets as
#  .wubpreset files, the web build, the user manual and the licence notes.
#  Needs a Release build (vst\build.bat). The version comes from CMakeLists.txt.
#  Usage: powershell -ExecutionPolicy Bypass -File vst\package.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'
$vst = $PSScriptRoot
$repo = Split-Path $vst -Parent
$version = (Select-String -Path (Join-Path $vst 'CMakeLists.txt') -Pattern 'project\(IWubYouVST VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$art = Join-Path $env:LOCALAPPDATA 'IWubYouVST\build\IWubYouVST_artefacts\Release'
$vst3 = Join-Path $art 'VST3\I Wub You.vst3'
$exe = Join-Path $art 'Standalone\I Wub You.exe'
$tests = Join-Path $env:LOCALAPPDATA 'IWubYouVST\build\IWubYouTests_artefacts\Release\IWubYouTests.exe'
foreach ($p in @($vst3, $exe, $tests)) { if (-not (Test-Path $p)) { throw "Missing build output: $p (run vst\build.bat)" } }

# the web build from the current sources
& (Join-Path $vst 'webhost\build-web.ps1') | Out-Null

$name = "IWubYou-$version-win64"
$dist = Join-Path $repo 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item -Recurse $vst3 (Join-Path $stage 'I Wub You.vst3')
Copy-Item $exe $stage
Copy-Item (Join-Path $vst 'install.bat') $stage
Copy-Item (Join-Path $vst 'uninstall.bat') $stage
Copy-Item (Join-Path $repo 'LICENSES.md') $stage
Copy-Item (Join-Path $repo 'docs\USER_MANUAL.md') (Join-Path $stage 'I Wub You Manual.md')
Copy-Item (Join-Path $repo 'web\index.html') (Join-Path $stage 'I Wub You Web.html')
Push-Location $stage
try { & $tests --export-presets 'Presets' | Out-Null } finally { Pop-Location }

@"
I Wub You $version for Windows x64

Install:    double-click install.bat (asks for administrator rights)
              VST3 -> C:\Program Files\Common Files\VST3\I Wub You.vst3
            then rescan plugins in your DAW. Manual install: copy the "I Wub You.vst3" folder there yourself.
Standalone: "I Wub You.exe" runs from any folder (Options > Audio/MIDI Settings for the input and output).
Web:        open "I Wub You Web.html" in Chrome or Edge: the same plugin in the browser, with built-in test
            sources, the microphone, a browser tab or an audio file as its input.
Presets:    the 96 factory presets are built into the plugin. The "Presets" folder holds them as .wubpreset
            files to share, to edit as text, or to link in the web build (FOLDER). Your own presets live in
            Documents\I Wub You\Presets.
Manual:     "I Wub You Manual.md"
Uninstall:  uninstall.bat (your presets are kept)

No development tools are needed to run this release.
"@ | Set-Content -Encoding utf8 (Join-Path $stage 'README.txt')

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host ("Package: {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
