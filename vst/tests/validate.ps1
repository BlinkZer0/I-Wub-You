# ============================================================================
#  Validates the built VST3 the way a host's scanner does: Steinberg's VST3 SDK
#  validator (when a build of it exists) and pluginval (strictness 10, editor
#  tests included). Both tools are shared with Pedal=Faster in
#  %LOCALAPPDATA%\PedalFaster\tools; pluginval is downloaded there on first use.
#  Usage: powershell -ExecutionPolicy Bypass -File validate.ps1 [-Strictness 10] [-SkipGui]
# ============================================================================
param([int]$Strictness = 10, [switch]$SkipGui)
$ErrorActionPreference = 'Stop'
$vst3 = Join-Path $env:LOCALAPPDATA 'IWubYouVST\build\IWubYouVST_artefacts\Release\VST3\I Wub You.vst3'
if (-not (Test-Path $vst3)) { throw "VST3 not built: $vst3" }
$tools = Join-Path $env:LOCALAPPDATA 'PedalFaster\tools'
$logs = Join-Path $env:LOCALAPPDATA 'IWubYouVST\validation'
New-Item -ItemType Directory -Force $logs | Out-Null

$pvDir = Join-Path $tools 'pluginval'
$pv = Join-Path $pvDir 'pluginval.exe'
if (-not (Test-Path $pv)) {
    New-Item -ItemType Directory -Force $pvDir | Out-Null
    $zip = Join-Path $pvDir 'pluginval.zip'
    Invoke-WebRequest 'https://github.com/Tracktion/pluginval/releases/latest/download/pluginval_Windows.zip' -OutFile $zip
    Expand-Archive $zip -DestinationPath $pvDir -Force
}
$validator = Get-ChildItem -Path (Join-Path $tools 'vst3sdk-build') -Recurse -Filter validator.exe -ErrorAction SilentlyContinue | Select-Object -First 1

$failed = $false
if ($validator) {
    Write-Host "== Steinberg VST3 validator: $($validator.FullName)"
    $vLog = Join-Path $logs 'vst3validator.txt'
    & $validator.FullName $vst3 *> $vLog
    $code = $LASTEXITCODE
    Get-Content $vLog | Where-Object { $_ -match 'Result|Tests failed|succeeded|Error|!' } | Select-Object -Last 8 | ForEach-Object { Write-Host "   $_" }
    if ($code -ne 0) { $failed = $true; Write-Host "VST3 validator FAILED (exit $code), see $vLog" -ForegroundColor Red }
} else {
    Write-Host '== Steinberg VST3 validator not available'
}

Write-Host "== pluginval (strictness $Strictness)"
$pvArgs = @('--strictness-level', $Strictness, '--output-dir', $logs)
if ($SkipGui) { $pvArgs += '--skip-gui-tests' }
if ($validator) { $pvArgs += @('--vst3validator', $validator.FullName) }
$pvArgs += @('--validate', $vst3)
# pluginval is a GUI-subsystem executable: start it explicitly and wait, otherwise PowerShell
# returns immediately and the exit code is meaningless.
$quoted = $pvArgs | ForEach-Object { if ("$_" -match '\s') { '"' + $_ + '"' } else { "$_" } }
$pvLog = Join-Path $logs 'pluginval-console.txt'
$proc = Start-Process -FilePath $pv -ArgumentList ($quoted -join ' ') -NoNewWindow -Wait -PassThru -RedirectStandardOutput $pvLog
Get-Content $pvLog | Where-Object { $_ -match 'Starting tests in|FAILED|!!!|SUCCESS|Strictness|Finished' } | ForEach-Object { Write-Host "   $_" }
if ($proc.ExitCode -ne 0) { $failed = $true; Write-Host "pluginval FAILED (exit $($proc.ExitCode)), see $pvLog" -ForegroundColor Red }

if ($failed) { exit 1 }
Write-Host 'VALIDATION PASSED' -ForegroundColor Green
