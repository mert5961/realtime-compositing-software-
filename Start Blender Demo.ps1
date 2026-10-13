$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$pathFile = Join-Path $PSScriptRoot 'output/blender-path.txt'
if (-not (Test-Path $pathFile)) { throw 'Run setup-blender.ps1 -BlenderPath <path to blender.exe> first' }
$blender = (Get-Content $pathFile -Raw).Trim()
if (-not (Test-Path -LiteralPath $blender)) { throw "Blender not found at $blender" }
$script = Join-Path $PSScriptRoot 'blender/start_texture_demo.py'
Start-Process -FilePath $blender -ArgumentList @('--factory-startup', '--gpu-backend', 'opengl', '--python', ('"' + $script + '"')) -WorkingDirectory $PSScriptRoot -WindowStyle Normal
