$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
if (-not (Test-Path build/deps/Spout2/CMakeLists.txt)) { git clone --depth 1 --branch 2.007.018 https://github.com/leadedge/Spout2.git build/deps/Spout2; if ($LASTEXITCODE) { throw 'Spout download failed' } }
if (-not (Test-Path build/deps/libajantv2/CMakeLists.txt)) { git clone --depth 1 --branch ntv2_18_0_0 https://github.com/aja-video/libajantv2.git build/deps/libajantv2; if ($LASTEXITCODE) { throw 'AJA download failed' } }
if ((git -C build/deps/Spout2 rev-parse HEAD) -ne '6b68a5b4fdeca6130b851e5f9366467a844860e5') { throw 'Spout revision mismatch' }
if ((git -C build/deps/libajantv2 rev-parse HEAD) -ne '4add45239a03960aa36de6fef326ee1b753e8ec9') { throw 'AJA revision mismatch' }
cmake -S . -B build/studio -DCOMPOSITOR_BUILD_AJA=ON -DCOMPOSITOR_BUILD_SPOUT=ON
if ($LASTEXITCODE) { throw 'Configuration failed' }
cmake --build build/studio --config Release -j 8
if ($LASTEXITCODE) { throw 'Studio build failed' }
New-Item -ItemType Directory -Force build/studio-addon/realtime_spout | Out-Null
Copy-Item blender/realtime_spout/*.py build/studio-addon/realtime_spout/
Copy-Item build/studio/Release/blender_spout_bridge.dll build/studio-addon/realtime_spout/
Copy-Item build/studio/Release/Spout-LICENSE.txt build/studio-addon/realtime_spout/
Compress-Archive -Path build/studio-addon/realtime_spout -DestinationPath build/studio/realtime_spout.zip -Force
Write-Host 'Ready: Start Studio Composite.cmd'
