$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
$blender=(Get-Content output/blender-path.txt -Raw).Trim()
$script=Join-Path $PSScriptRoot 'blender/start_texture_demo.py'
Start-Process -FilePath $blender -ArgumentList @('--factory-startup','--gpu-backend','opengl','--python',('"'+$script+'"'),'--','--studio') -WorkingDirectory $PSScriptRoot -WindowStyle Normal
