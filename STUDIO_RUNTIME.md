# Studio runtime: our four movie rules

| Movie | Rule | Implementation |
|---|---|---|
| Night of the Living Dead | Failed transfers never produce valid frames | Capture errors are rejected before processing/publication. The inherited SDI test marks failed ring slots invalid and releases them without playout. |
| No Country for Old Men | Output keeps running when Blender stalls | Spout waits run on a separate worker. SDI output follows AJA vertical opportunities and consumes completed host frames. It repeats valid output when necessary; a missing or >500 ms stale Blender/pose source selects live camera passthrough. |
| Fight Club | Object lifetime does not preserve pixels | Three owned GPU surfaces publish only after completed copy queries. Reader leases prevent reuse through compositing readback. Three completed host surfaces remain leased through output DMA. |
| Perfect Blonde | A pass proves image delivery | Every reconnect phase must deliver progress, exact dimensions, and distinct asymmetric color patterns. Negative fixtures reject false progress, wrong geometry, stale colors, and truncated images. |

## Run

Build: `powershell -NoProfile -ExecutionPolicy Bypass -File .\setup-studio.ps1`.
Launch **Start Studio Blender.cmd**, then **Start Studio Composite.cmd** and
click **Start studio**. Defaults: Corvid44 device 0, camera SDI 1, television
SDI 4, sender `RealtimeStudio`. Threshold and softness are adjustable while stopped.
For a normally opened Blender scene, install `build/studio/realtime_spout.zip`
through Preferences > Add-ons > Install from Disk; use the **Realtime** sidebar.
Stop Blender before rebuilding/replacing a loaded native DLL.

Stop any other session owning this AJA card. The ownership check releases only
owners confirmed to have exited. Only one application can receive UDP 8873;
the studio Blender sender owns it while tracking is enabled. The old
`RealtimeBlender` demonstration may coexist under its different sender name.

This is a basic green-dominance key with simple spill suppression. Supported
video is progressive 1080p/720p, 8-bit UYVY, BT.709 limited range, without audio.
Production color management, HDR, advanced edges, interlaced/SD/UHD, and complete
lens distortion correction are outside this first integrated milestone.

## Buffers and clock

```text
THT JSON -> bounded latest tracking sample -> Blender camera pose/FOV
Blender offscreen -> Spout worker -> 3 owned, completed GPU surfaces
                                          |
Camera -> AJA input ring (4) -> DMA -> RAM -> GPU key/composite
                                          |
                    GPU UYVY pack -> completed readback -> 3 host surfaces
                                          |
                    hardware output opportunity -> DMA -> AJA ring (4) -> TV
```

The integrated runtime replaces the ten-slot SDK host FIFO with a bounded
latest-frame handoff. Held surfaces cannot be overwritten. Full pools discard
new work instead of adding backlog. Source reception and compositing use
separate D3D11 devices/contexts on the same GPU. GPU completion is established
before handoff. Output uses completed CPU images and makes no Spout/GPU calls.

Output reference is selected from the camera input and accepted by the driver.
The hardware video format drives cadence, including fractional rates. Startup
explicitly prefills three black frames. Four card slots per direction remain
the default. Repeats are reported separately from hardware dropped frames.

Blender pixels stay on the GPU through reception and compositing. Camera input
is uploaded from RAM, and the finished packed image is read back for ordinary
AJA DMA. Windows GPUDirect capability is not asserted.

## Tracking and lens

A five-second capture received 251 TraxisHubTrack JSON packets from
`192.168.0.123` on UDP 8873. Fields include position, pan/tilt/roll,
`effectiveFov`, sensor dimensions, focus/focus distance, zoom, lens center,
`k1`, `k2`, and `distortionScale`. No source timestamp or sequence was present.

The adapter validates finite pose/FOV/sensor values and applies an exposed
Unreal-compatible preset: centimeters/degrees, Z up, X forward, Y right.
Conversion accounts for Blender handedness and its camera optical axes.
Units-to-meters defaults to 0.01 and is editable. No tracker-origin subtraction
or user axis-negation offsets are applied. Physical camera movement must still
verify the units, directions, and rotation conventions for this installation.

Calibrated horizontal `effectiveFov` and sensor dimensions drive Blender focal
length. Other lens values are retained in diagnostics; the complete distortion
and DOF model is not guessed from their field names.

The disposable studio scene places a small animated cube in front of the
absolute tracked camera after the first valid packet. This places an asset,
rather than changing tracking origin. Use your real studio scene for alignment.

## Frame identity and measured ages

Native metadata contains session identity, exact Spout publication count,
dimensions, render-start time, and associated tracking-packet receive time.
An odd/even guard spans image publication. Reception requires identical even
guards before/after the copy and an exact image-count/dimension match. Failed
or busy sends never relabel old pixels with new timestamps. Session/resource
transitions invalidate pending selections.

Clocks use this host's QPC-derived steady timeline. CSV/UI separately report
camera capture-completion age, Blender receive age, render-start age, and
tracking-receive age at **output DMA submission**. `-1` means unavailable.
Metadata-free legacy senders have no render/pose age. Tracking-required sources
without a sample select camera fallback.

These exclude earlier exposure/SDI transport and later card queue/TV delay.
They are not glass-to-glass latency or proof of exposure/pose alignment.
Calibrating those offsets and choosing a temporal pairing policy remain necessary.

## Blender lifecycle

Stop, cancellation, file load, workspace/editor change, and unregister remove
active bookkeeping/timers idempotently. Native cleanup checks the owning
OpenGL context. If it cannot safely run there, resources are quarantined and
reported as `Faulted`. Start can retry when the original context returns; if
that context was destroyed, restart Blender. Resources are never deliberately
deleted in an arbitrary context. Duplicate sender names are explicitly rejected.

## Validation on this workstation

- Five CTest targets passed: immutable ownership/concurrency, lifecycle/parser/
  validator faults, SDI instrumentation, portable core, and synthetic scenario.
- Real GPU test: numeric key/BT.709 packing; a held blue image stayed blue while
  30 newer red publications arrived; fresh red delivery and stale-source fallback.
- Blender phases: exact size/orientation/colors in all three generations;
  2,062 frames received, six skipped publications in that run.
- Initial 30-second composite: 1,503 captured / 1,504 submitted, zero AJA drops,
  58 application repeats with the free-running reference.
- Input-reference fault test: 100 partial-buffer failures rejected; 908 good
  captures / 1,009 submissions, 103 repeats, zero AJA drops.
- Tracked 60-second run: 3,010 captures / 3,011 submissions, two startup repeats,
  zero AJA drops. A two-second Spout access-mutex hold did not interrupt SDI;
  stale-source camera fallback increased and Blender recovered.

Raw logs stay in ignored `output/studio-*.csv`, with trace/profile companions.
CSV writes occur on the monitor thread, outside frame loops. Timing traces are
bounded and report discarded events when full. Short tests establish a baseline,
not a sustained workload guarantee; EEVEE load, device loss, exposure alignment,
full lens warp, and long soak testing remain.

```powershell
ctest --test-dir build/studio -C Release --output-on-failure
.\build\studio\Release\studio_test.exe --gpu-selftest
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test_blender_texture.ps1
.\build\studio\Release\studio_test.exe --seconds 60 --input 1 --output 4 --sender RealtimeStudio
.\build\studio\Release\studio_test.exe --seconds 20 --input 1 --output 4 --sender RealtimeStudio --fault-every 10
```

The last command deliberately corrupts part of every tenth captured host
buffer and marks the transfer failed. It tests application rejection, rather
than reproducing a physical driver failure. `injected_failures` distinguishes
it. Real DMA, signal, logging, or completion errors fail timed CLI runs.
