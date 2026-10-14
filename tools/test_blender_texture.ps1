param([string]$BlenderPath = '')
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
if (-not $BlenderPath) { $BlenderPath = (Get-Content output/blender-path.txt -Raw).Trim() }
$runId = Get-Date -Format 'yyyyMMdd-HHmmss'
$csvPath = "output/blender-validation-$runId.csv"
$snapshot = "output/blender-validation-$runId.ppm"
$demoScript = Join-Path $PWD 'blender/start_texture_demo.py'
$demo = $null
$receiver = $null
try {
    # A disposable startup scene exercises 1080p -> 720p -> 1080p and Stop/Start.
    $demo = Start-Process -FilePath $BlenderPath -ArgumentList @('--factory-startup', '--gpu-backend', 'opengl', '--python-exit-code', '1', '--python', ('"'+$demoScript+'"'), '--', '--validate') -WorkingDirectory $PWD -WindowStyle Normal -RedirectStandardOutput "output/blender-validation-$runId.log" -RedirectStandardError "output/blender-validation-$runId-error.log" -PassThru
    # Retain a handle before Blender exits so Windows PowerShell can read its
    # exit code even though the receiver is waited on first.
    $demoHandle = $demo.Handle
    $receiverExe = './build/studio/Release/blender_texture_test.exe'
    if (-not (Test-Path $receiverExe)) { $receiverExe = './build/blender/Release/blender_texture_test.exe' }
    $receiver = Start-Process -FilePath $receiverExe -ArgumentList @('--sender', 'RealtimeValidation', '--seconds', '50', '--log', $csvPath, '--snapshot', $snapshot) -WorkingDirectory $PWD -WindowStyle Normal -PassThru
    $receiverHandle = $receiver.Handle
    if (-not $receiver.WaitForExit(60000)) { throw 'Receiver did not finish within 60 seconds' }
    if ($receiver.ExitCode -ne 0) { throw "Receiver failed: $($receiver.ExitCode)" }
    if (-not $demo.WaitForExit(5000)) { throw 'Blender did not stop cleanly' }
    if ($demo.ExitCode -ne 0) { throw "Blender failed: $($demo.ExitCode)" }
    $rows = @(Import-Csv $csvPath)
    if (-not ($rows | Where-Object { $_.width -eq '1920' -and $_.frame_counter_available -eq '1' })) { throw 'No counted 1080p frames' }
    if (-not ($rows | Where-Object { $_.width -eq '1280' -and $_.frame_counter_available -eq '1' })) { throw 'Resize to 720p failed' }
    if ([int]$rows[-1].generation -lt 3 -or [int]$rows[-1].new_frames -lt 100) { throw 'Sender reconnection failed' }
    if ($rows[-1].connected -ne '0') { throw 'Sender shutdown not detected' }
    if (-not (Test-Path $snapshot)) { throw 'No diagnostic image received' }
    python tools/validate_blender_run.py $csvPath $snapshot
    if ($LASTEXITCODE -ne 0) { throw 'Per-generation image validation failed' }
    Write-Host "PASS: camera texture, resize, reconnect, shutdown. $csvPath"
    Write-Host "Observed frames: $($rows[-1].new_frames); skipped sender frames: $($rows[-1].skipped_sender_frames)"
} finally {
    # Only processes created by this disposable test are eligible for cleanup.
    if ($receiver -and -not $receiver.HasExited) { Stop-Process -Id $receiver.Id }
    if ($demo -and -not $demo.HasExited) { Stop-Process -Id $demo.Id }
    if ($receiver) { $receiver.Dispose() }
    if ($demo) { $demo.Dispose() }
}
