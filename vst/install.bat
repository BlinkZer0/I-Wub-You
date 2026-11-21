@echo off
rem Copies the built "I Wub You.vst3" into the system VST3 folder (asks for admin).
rem Usage: install.bat [Debug|Release]
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "SRC=%LOCALAPPDATA%\IWubYouVST\build\IWubYouVST_artefacts\%CONFIG%\VST3\I Wub You.vst3"
set "DST=%CommonProgramFiles%\VST3\I Wub You.vst3"

if not exist "%SRC%" (
  echo Not built yet: %SRC%
  echo Run build.bat first.
  exit /b 1
)

echo [I Wub You] Installing to %DST%  (close your DAW / Minihost first)
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-Command','New-Item -ItemType Directory -Force ''%CommonProgramFiles%\VST3'' | Out-Null; Remove-Item -Recurse -Force ''%DST%'' -ErrorAction SilentlyContinue; Copy-Item -Recurse -Force ''%SRC%'' ''%DST%'''"
if exist "%DST%\Contents\x86_64-win\I Wub You.vst3" (echo [I Wub You] Installed. Rescan plugins in your host.) else (echo [I Wub You] Install failed or was cancelled.)
