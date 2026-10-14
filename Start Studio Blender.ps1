$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
$blender=(Get-Content output/blender-path.txt -Raw).Trim()
$script=Join-Path $PSScriptRoot 'blender/start_texture_demo.py'
$logStamp=Get-Date -Format 'yyyyMMdd-HHmmss'
Start-Process -FilePath $blender -ArgumentList @('--factory-startup','--gpu-backend','opengl','--python',('"'+$script+'"'),'--','--studio') -WorkingDirectory $PSScriptRoot -WindowStyle Normal -RedirectStandardOutput (Join-Path $PSScriptRoot "output/blender-studio-$logStamp.log") -RedirectStandardError (Join-Path $PSScriptRoot "output/blender-studio-$logStamp-error.log")
