@echo off
cd /d "%~dp0"
if not exist "build\windows\Release\sdi_test.exe" (
  echo Please run setup-sdi.ps1 first.
  pause
  exit /b 1
)
start "" "build\windows\Release\sdi_test.exe"
