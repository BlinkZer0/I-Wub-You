# ============================================================================
#  Screenshots of the UI for the README: serves the built web build (the real
#  UI with the JS engine behind it) and captures each page in headless Edge.
#  Writes docs/screenshots/<page>.png. Builds web/index.html first.
#  Usage: powershell -ExecutionPolicy Bypass -File screenshots.ps1 [-Port 8792] [-Out <folder>]
# ============================================================================
param([int]$Port = 8792, [string]$Out = '')
$ErrorActionPreference = 'Stop'
$vst = Split-Path $PSScriptRoot -Parent
$repo = Split-Path $vst -Parent
if (-not $Out) { $Out = Join-Path $repo 'docs\screenshots' }
New-Item -ItemType Directory -Force $Out | Out-Null
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }

& (Join-Path $vst 'webhost\build-web.ps1') *> $null
$utf8 = [Text.UTF8Encoding]::new($false)
$html = [IO.File]::ReadAllText((Join-Path $repo 'web\index.html'), $utf8)
# a small script that opens the page named in the hash, picks the drum loop, and lets the engine settle
$inject = @'
<script>
window.addEventListener('load', () => {
  setTimeout(() => { const b = window.__JUCE__ && window.__JUCE__.backend; if (b) { b.emitEvent('selectInput', { id: 'drums' }); b.emitEvent('loadPreset', { id: 'factory:48' }); } }, 300);   // Drums / Wobbly Breaks (16 per category)
});
</script>
'@
$html = $html.Replace('</body>', "$inject`n</body>")
$pageBytes = $utf8.GetBytes($html)

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
$server = {
  param($listener, $pageBytes)
  while ($true) {
    try { $ctx = $listener.GetContext() } catch { break }
    $res = $ctx.Response
    $res.ContentType = 'text/html; charset=utf-8'
    $res.OutputStream.Write($pageBytes, 0, $pageBytes.Length)
    $res.Close()
  }
}
$job = [PowerShell]::Create().AddScript($server).AddArgument($listener).AddArgument($pageBytes)
$handle = $job.BeginInvoke()
$profile = Join-Path $env:TEMP 'iwubyou-screenshots-profile'
try {
  foreach ($p in 'wobble', 'bands', 'source', 'output') {
    $png = Join-Path $Out "$p.png"
    if (Test-Path $png) { Remove-Item $png -Force }
    $ErrorActionPreference = 'Continue'   # Edge reports the written file on stderr; that is not an error
    & $edge --headless=new --disable-gpu "--user-data-dir=$profile" --window-size=1300,820 --hide-scrollbars `
        --autoplay-policy=no-user-gesture-required --disable-background-timer-throttling --disable-renderer-backgrounding `
        --disable-backgrounding-occluded-windows --disable-features=CalculateNativeWinOcclusion --virtual-time-budget=4000 `
        "--screenshot=$png" "http://localhost:$Port/shot.html#$p" 2>&1 | Out-Null
    $ErrorActionPreference = 'Stop'
    if (Test-Path $png) { Write-Host "wrote $png" } else { Write-Host "FAILED $p" }
  }
} finally {
  $listener.Stop()
  $job.Stop() | Out-Null
  $job.Dispose()
}
