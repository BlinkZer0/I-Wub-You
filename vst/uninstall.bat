@echo off
rem Removes the installed "I Wub You.vst3" from the system VST3 folder (asks for admin).
rem Your presets in Documents\I Wub You\Presets are kept.
setlocal
set "DST=%CommonProgramFiles%\VST3\I Wub You.vst3"
if not exist "%DST%" (
  echo [I Wub You] Not installed: %DST%
  exit /b 0
)
echo [I Wub You] Removing %DST%  (close your DAW first)
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-Command','Remove-Item -Recurse -Force ''%DST%'' -ErrorAction SilentlyContinue'"
if exist "%DST%" (echo [I Wub You] Uninstall failed or was cancelled.) else (echo [I Wub You] Removed. Your presets in Documents\I Wub You\Presets were kept.)
