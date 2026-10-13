param([string]$BlenderPath = '', [switch]$InstallAddon)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$spoutCommit = '6b68a5b4fdeca6130b851e5f9366467a844860e5'
if (-not (Test-Path 'build/deps/Spout2/CMakeLists.txt')) {
    git clone --depth 1 --branch 2.007.018 https://github.com/leadedge/Spout2.git build/deps/Spout2
    if ($LASTEXITCODE -ne 0) { throw 'Spout SDK download failed' }
}
if ((git -C build/deps/Spout2 rev-parse HEAD) -ne $spoutCommit) { throw 'Expected Spout 2.007.018; SDK revision differs' }
cmake -S . -B build/blender -DCOMPOSITOR_BUILD_SPOUT=ON
if ($LASTEXITCODE -ne 0) { throw 'Configuration failed' }
cmake --build build/blender --config Release --target blender_texture_test blender_spout_bridge -j 8
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
New-Item -ItemType Directory -Force build/blender-addon/realtime_spout | Out-Null
Copy-Item blender/realtime_spout/__init__.py build/blender-addon/realtime_spout/
Copy-Item build/blender/Release/blender_spout_bridge.dll build/blender-addon/realtime_spout/
Copy-Item build/blender/Release/Spout-LICENSE.txt build/blender-addon/realtime_spout/
Compress-Archive -Path build/blender-addon/realtime_spout -DestinationPath build/blender/realtime_spout.zip -Force
if ($BlenderPath) {
    if (-not (Test-Path -LiteralPath $BlenderPath)) { throw 'Blender executable not found' }
    New-Item -ItemType Directory -Force output | Out-Null
    Set-Content -Path output/blender-path.txt -Value (Resolve-Path -LiteralPath $BlenderPath).Path
}
if ($InstallAddon) {
    if (-not $BlenderPath) {
        $BlenderPath = (Get-Content output/blender-path.txt -Raw).Trim()
    }
    & $BlenderPath --background --python-exit-code 1 --python blender/install_addon.py
    if ($LASTEXITCODE -ne 0) { throw 'Blender add-on installation failed' }
}
Write-Host 'Ready: Start Blender Texture Test.cmd. Add-on zip: build/blender/realtime_spout.zip'
