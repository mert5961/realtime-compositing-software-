$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (-not (Test-Path 'build/deps/libajantv2/CMakeLists.txt')) {
    git clone --depth 1 --branch ntv2_18_0_0 https://github.com/aja-video/libajantv2.git build/deps/libajantv2
    if ($LASTEXITCODE -ne 0) { throw 'AJA SDK download failed' }
}
cmake -S . -B build/windows -DCOMPOSITOR_BUILD_AJA=ON
if ($LASTEXITCODE -ne 0) { throw 'Configuration failed' }
cmake --build build/windows --config Release --target sdi_test -j 8
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
Write-Host 'Ready. Double-click Start SDI Test.cmd.'
