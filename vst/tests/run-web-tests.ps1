# ============================================================================
#  Headless tests of the web build: loads a copy of web/index.html (plus
#  tests/web-tests.js) in headless Edge, drives the UI and prints the results.
#  Runs twice: opened from disk (file://, ScriptProcessor audio) and served over
#  http://localhost (AudioWorklet audio, the start.bat path).
#  Builds web/index.html first (webhost/build-web.ps1).
#  Usage: powershell -ExecutionPolicy Bypass -File run-web-tests.ps1 [-Port 8793] [-Browser Edge|Chrome]
# ============================================================================
param([int]$Port = 8793, [ValidateSet('Edge', 'Chrome')] [string]$Browser = 'Edge')
$ErrorActionPreference = 'Stop'
$vst = Split-Path $PSScriptRoot -Parent
$repo = Split-Path $vst -Parent
$candidates = if ($Browser -eq 'Chrome') {
    @("$env:ProgramFiles\Google\Chrome\Application\chrome.exe", "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe", "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe")
} else {
    @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe")
}
$edge = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw "$Browser not found (needed for the headless web tests)" }
Write-Host "browser: $edge"

& (Join-Path $vst 'webhost\build-web.ps1') *> $null
$utf8 = [Text.UTF8Encoding]::new($false)
$html = [IO.File]::ReadAllText((Join-Path $repo 'web\index.html'), $utf8)
$tests = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'web-tests.js'), $utf8)

# error collector first (before any app script), test script last
$collector = '<script>window.__wubErrors = []; window.addEventListener("error", (e) => window.__wubErrors.push(String(e.message || e)));' +
             ' window.addEventListener("unhandledrejection", (e) => window.__wubErrors.push("unhandled rejection: " + String(e.reason)));</script>'
$html = $html.Replace('<head>', "<head>`n$collector")
$html = $html.Replace('</body>', "<script>`n$tests`n</script>`n</body>")

$tmp = Join-Path $env:TEMP "iwubyou-webtests-$Browser"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force $tmp | Out-Null
$pagePath = Join-Path $tmp 'index.html'
[IO.File]::WriteAllText($pagePath, $html, $utf8)

# a tiny server for the http run (answers 'ping' too: the test uses it to let real time pass)
$server = Start-Job -ArgumentList $tmp, $Port -ScriptBlock {
    param($root, $port)
    $l = [System.Net.HttpListener]::new()
    $l.Prefixes.Add("http://localhost:$port/")
    $l.Start()
    while ($l.IsListening) {
        $ctx = $l.GetContext()
        $rel = $ctx.Request.Url.AbsolutePath.TrimStart('/')
        $file = Join-Path $root $(if ($rel) { $rel } else { 'index.html' })
        if ($rel -eq 'quit') { $ctx.Response.Close(); $l.Stop(); break }
        if ($ctx.Request.HttpMethod -eq 'POST' -and $rel -eq 'result') {
            $body = [IO.StreamReader]::new($ctx.Request.InputStream, [Text.Encoding]::UTF8).ReadToEnd()
            [IO.File]::WriteAllText((Join-Path $root 'result-http.json'), $body, [Text.UTF8Encoding]::new($false))
            $ctx.Response.StatusCode = 204
        } elseif (Test-Path $file -PathType Leaf) {
            $bytes = [IO.File]::ReadAllBytes($file)
            $ctx.Response.ContentType = 'text/html; charset=utf-8'
            $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
        } else { Start-Sleep -Milliseconds 25; $ctx.Response.StatusCode = 204 }
        $ctx.Response.Close()
    }
}

# Opened from disk: Edge's virtual time runs the page quickly and the results are read from the DOM dump.
function Invoke-DumpRun([string]$label, [string]$url) {
    $prof = Join-Path $tmp "profile-$label"          # fresh profile: empty localStorage
    $domFile = Join-Path $tmp "dom-$label.txt"
    Start-Process -FilePath $edge -Wait -RedirectStandardOutput $domFile -RedirectStandardError (Join-Path $tmp "err-$label.txt") -ArgumentList @(
        '--headless=new', '--disable-gpu', '--no-first-run', "--user-data-dir=$prof", '--window-size=1400,1100',
        '--virtual-time-budget=900000', '--dump-dom', $url)
    $dom = [IO.File]::ReadAllText($domFile)
    if ($dom -notmatch '(?s)<pre id="wub-test-out">(\[.*?)</pre>') { throw "web tests ($label) produced no output (see $domFile)" }
    return [Net.WebUtility]::HtmlDecode($Matches[1]) | ConvertFrom-Json
}

# Served: real time (virtual time would pause the audio thread); the page POSTs its results to the server.
function Invoke-ServedRun([string]$label, [string]$url) {
    $prof = Join-Path $tmp "profile-$label"
    $resultFile = Join-Path $tmp 'result-http.json'
    $p = Start-Process -FilePath $edge -PassThru -RedirectStandardOutput (Join-Path $tmp "out-$label.txt") -RedirectStandardError (Join-Path $tmp "err-$label.txt") -ArgumentList @(
        '--headless=new', '--disable-gpu', '--no-first-run', "--user-data-dir=$prof", '--window-size=1400,1100',
        '--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling', '--disable-renderer-backgrounding', $url)
    try {
        $deadline = (Get-Date).AddSeconds(90)
        while (-not (Test-Path $resultFile) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
        if (-not (Test-Path $resultFile)) { throw "web tests ($label) produced no results within 90 s" }
        Start-Sleep -Milliseconds 200
        return [IO.File]::ReadAllText($resultFile) | ConvertFrom-Json
    } finally { cmd /c "taskkill /T /F /PID $($p.Id) >nul 2>&1" | Out-Null }   # through cmd: a refusal must not throw here
}

$fail = 0; $count = 0
try {
    $runs = @(
        @{ label = 'file'; url = ('file:///' + ($pagePath -replace '\\', '/')) },
        @{ label = 'http'; url = "http://localhost:$Port/index.html" }
    )
    foreach ($run in $runs) {
        Write-Host "[$($run.label)] $($run.url)  ($(Get-Date -Format HH:mm:ss))"
        $res = if ($run.label -eq 'http') { Invoke-ServedRun $run.label $run.url } else { Invoke-DumpRun $run.label $run.url }
        foreach ($r in $res) {
            $count++
            if ($r.ok) { Write-Host ('  ok    ' + $r.name) }
            else { $fail++; Write-Host ('  FAIL  ' + $r.name + $(if ($r.detail) { "  [$($r.detail)]" } else { '' })) }
        }
    }
} finally {
    try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 "http://localhost:$Port/quit" | Out-Null } catch { }   # GetContext() blocks: stop it from inside
    Stop-Job $server -ErrorAction SilentlyContinue; Remove-Job $server -Force -ErrorAction SilentlyContinue
}
if ($fail -eq 0) { Write-Host "WEB TESTS PASSED ($count checks)"; exit 0 }
Write-Host "WEB TESTS: $fail of $count FAILED"; exit 1
