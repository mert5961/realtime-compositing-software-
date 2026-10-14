@echo off
cd /d "%~dp0"
if not exist "build\blender\Release\blender_texture_test.exe" (
  echo Please run setup-blender.ps1 first.
  pause
  exit /b 1
)
start "" "build\blender\Release\blender_texture_test.exe"
