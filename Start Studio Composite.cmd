@echo off
cd /d "%~dp0"
if not exist "build\studio\Release\studio_test.exe" (
  echo Please run setup-studio.ps1 first.
  pause
  exit /b 1
)
start "" "build\studio\Release\studio_test.exe"
