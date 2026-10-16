@echo off
cd /d "%~dp0"
set "APP=build\studio\studio_control_artefacts\Release\Realtime Studio Control.exe"
if not exist "%APP%" (
  echo Please run setup-studio.ps1 first.
  pause
  exit /b 1
)
rem Add --fake to try the UI without the AJA card (simulated engine, nothing reaches SDI).
start "" "%APP%" %*
