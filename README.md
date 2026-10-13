# Realtime Compositor — first foundation

A portable C++20 prototype for the scheduler and frame storage described in the research report. Build and validate the same core on macOS and Windows before adding a platform GPU backend and a real Blender adapter.

## What runs today

- Three preallocated CPU pools: synthetic camera input, synthetic Blender input, and composite output. Each pool has three RGBA8 surfaces.
- Exclusive write leases; publication exposes complete pixels and metadata. Copyable read leases prevent recycling until the final consumer releases the surface. Leases keep storage alive even if the pool object is destroyed.
- Bounded storage: a producer replaces the oldest unleased published frame. If all surfaces are busy, acquisition fails immediately instead of adding a queued allocation.
- Timestamp selection, maximum input age, maximum foreground/background skew, and deadline admission checks.
- A simple alpha-over operation using a generated foreground matte.
- A deterministic 120-tick simulation: 60 FPS camera, 30 FPS scene, a scene stall on ticks 44–69, and an injected deadline miss on tick 95. Rejected work repeats the last completed composite.
- A CSV trace and a final PPM image. The simulation runs as fast as the CPU allows; it does **not** pace a display in real time or benchmark end-to-end latency.

There is no Blender connection, GPU execution, camera capture, key extraction, AI, or graphical editor yet. The demo uses synthetic display RGB and straight alpha; the blend is intentionally not a production color-managed operation.

## Windows

### Live AJA SDI test UI

This Windows-only test uses AJA NTV2 SDK **18.0.0** and its `NTV2Burn`
capture/playout implementation. It sends camera video through host memory back
to SDI, with a generated timecode overlay for checking the television picture.
The current test supports SD/HD/2K video, 8-bit YCbCr, with audio and ancillary
data disabled. UHD/4K/8K routing is not implemented in this first test.

Build once with `powershell -ExecutionPolicy Bypass -File .\setup-sdi.ps1`, then
double-click **Start SDI Test.cmd** in this folder. No Qt or browser is needed.

1. Stop any other application session using the selected AJA card. The test
   checks device ownership and refuses to interrupt a running owner. If the
   recorded owner has exited, Start releases its stale ownership before testing.
2. Select the Corvid44 and the physical camera input and television output.
   They must be different connectors. Initial defaults are SDI 1 and SDI 2.
3. Select **Input buffers** and **Output buffers** independently (3–7 frames;
   default 4). Counts can only be changed while stopped and are remembered.
   Click **Start test**. Confirm the camera picture and advancing timecode on
   the television. The app takes exclusive control and configures card routing,
   frame stores and the free-running reference clock.
4. Watch capture/output frame totals, measured FPS, dropped-frame counts,
   elapsed time and card occupancy/capacity. Click **Stop** to retain final counters.
   A new `output/sdi-*.csv` log is written for every successful run, at 250 ms
   intervals. **Open logs** opens the directory. SDK diagnostics are appended
   to `output/sdi-test.log`.

The profiling panel displays cumulative average/maximum timings for capture
and output DMA API calls, both threads' host-frame acquisition, input/output
device waits, and capture processing. Host occupancy is split into capture,
ready and output stages, plus a run peak. Timing is measured with a monotonic
clock; API elapsed times include driver work and thread scheduling. Device
waits include normal frame pacing, not just unexpected stalls.

After Stop, each run also saves `sdi-*-trace.csv` (per-operation timestamps,
durations, success flags, ownership-stage snapshots) and `sdi-*-profile.csv`
(cumulative timing summary). Shutdown operations are labeled in the trace and
excluded from run aggregates. The two frame threads use separate preallocated
65,536-event trace arrays. On longer runs, recording stops when an array fills;
aggregate metrics continue and `trace_discarded` reports omitted events.
Frame loops do not allocate trace memory or write trace files.

Summarize traces with Python 3 using only the standard library:

```powershell
python tools/analyze_sdi_profile.py output/sdi-RUN-trace.csv --output output/profile-report.md
```

Replace `RUN` with the actual filename's run ID. The analyzer excludes the
first second and shutdown by default, computes per-operation P95 timing and
reports failures; use `--warmup 0` to include startup. Multiple trace paths can
be supplied for comparison. See [SDI_PROFILING.md](SDI_PROFILING.md).

Counters come from AJA AutoCirculate: input drops indicate capture overruns;
output drops indicate playout underruns. They include startup, and do not
prove whether the camera or television dropped frames. Short FPS samples can
oscillate around the nominal rate; evaluate totals over a longer run. Seven
device buffers per direction masked a missing output-interrupt subscription in
the earlier SDK path. After correcting it, four slots per direction became the
default and passed live tests without drops. The selectable device rings are
separate from the fixed 10-frame host ring. Three-slot output
starts after two queued frames; larger rings start after three. A signal loss
or format change stops the test; restart after it stabilizes.
The video-only capture path now reads one completed frame when available.
The SDK's two-frame threshold is retained when audio is enabled. Output
interrupts are subscribed on the actual output frame store, which may differ
from the selected physical SDI jack; the profiling panel identifies frame stores.
Stop releases device ownership; card routing remains configured for the test.
Restart/reconfigure your prior AJA session to restore its routing.

The optional **Listen UDP 8873** button reports packet count, byte count and
time since the latest datagram. It uses an exclusive bind and does not steal an
occupied port. It does not decode tracking or estimate tracking packet loss.
On this computer, `nosLauncher` was using both the Corvid44 and UDP 8873 at
setup time; SDI 1 on the Corvid44 reported 1080p50a. Stop its AJA/UDP session
before testing those resources here.

For diagnostics without changing card configuration:

```powershell
.\build\windows\Release\sdi_test.exe --probe
```

To release a stale process owner without starting video:

```powershell
.\build\windows\Release\sdi_test.exe --recover-owner 0
```

Recovery checks that the recorded process has exited and releases ownership
using that PID. A running process or an inaccessible process is left alone.
The UI remembers the last selected input/output connectors between launches.

For a timed live test (device index 0, input SDI 1, output SDI 2, 60 seconds):

```powershell
.\build\windows\Release\sdi_test.exe --test 0 1 2 60
```

Append input/output buffer counts to compare configurations, for example:

```powershell
.\build\windows\Release\sdi_test.exe --test 0 1 4 30 3 3
.\build\windows\Release\sdi_test.exe --test 0 1 4 30 4 4
```

CSV logs include the actual device ring capacities reported by the driver and
the fixed host capacity, so comparisons remain identifiable. The UI samples
occupancy every 250 ms; it does not observe every transient queue change or
measure camera-to-television latency. See [BUFFERING.md](BUFFERING.md) for the
live path and its relationship to the synthetic compositor pools.

The AJA SDK is downloaded into ignored `build/deps` using tag `ntv2_18_0_0`.
The CMake build compiles a corrected copy of its Windows WMI helper to fix
MSVC errors from jumps over initialized declarations. The original downloaded
SDK sources are preserved. See [AJA's capture/playout demo documentation](https://sdkdocs.aja.com/public/ntv2/18_0_0/d8/dc7/class_n_t_v2_burn.html).

Prerequisites: a C++20 compiler and CMake 3.20 or newer. For MSVC, install Visual Studio's **Desktop development with C++** workload and CMake tools, then use a developer PowerShell or command prompt.

From this project directory:

```powershell
cmake -S . -B build/windows
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release --output-on-failure
.\build\windows\Release\compositor_demo.exe output
```

Those executable paths assume CMake selected a Visual Studio generator. For a single-configuration generator such as Ninja, the executable is `build/windows/compositor_demo.exe`; configure with `-DCMAKE_BUILD_TYPE=Release`.

## macOS / Linux with CMake

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
ctest --test-dir build/native --output-on-failure
./build/native/compositor_demo output
```

## macOS without CMake

The initial development Mac has Apple Clang but no CMake. These commands exercise the same sources directly:

```sh
mkdir -p build/local
clang++ -std=c++20 -Wall -Wextra -Wpedantic -pthread -Iinclude src/core.cpp tests/core_tests.cpp -o build/local/compositor_tests
clang++ -std=c++20 -Wall -Wextra -Wpedantic -pthread -Iinclude src/core.cpp src/main.cpp -o build/local/compositor_demo
./build/local/compositor_tests
./build/local/compositor_demo output
```

## Expected demo result

The run produces **94 composites, 26 repeated frames, and zero unavailable frames**. Pixel storage stays at **2,073,600 bytes across nine surfaces**, excluding descriptors, synchronization primitives, and file I/O buffers. `output/frames.csv` records each tick, selection reason, source sequences used in the displayed composite, and output content age. `output/composite.ppm` contains the final image. CTest runs both the unit tests and this complete demo scenario.

The scheduler chooses the freshest eligible image independently from each source and rejects excessive skew. It does not search older camera history for a better pair. That pairing policy is deliberately limited and will need refinement for tracked cameras.

## Ownership and timing rules

1. Write pixel data only while its write lease is valid. Publishing or destroying that lease invalidates all spans obtained from it.
2. Read pixel spans remain valid only while a corresponding read lease remains alive. Different lease copies can be held by different consumers; the same lease object must not be mutated concurrently.
3. All timestamps currently use one nonnegative session-relative monotonic timeline. Content time identifies the represented moment; ready time identifies completed production. External clock mapping belongs in future adapters.
4. `ready_time` is simulated metadata, not a GPU fence. GPU resources will require explicit completion tracking before publication and reuse; a CPU lease alone cannot establish GPU readiness.
5. Pool bookkeeping uses short mutex-protected sections. This version makes no lock-free or hard-real-time guarantee. Pixel processing occurs outside the mutex, and pixel arrays are allocated at pool creation.
6. The scheduler checks a configured processing estimate before starting. Real GPU/output deadline monitoring, device recovery, runtime format changes, and reconnection generations remain future work.

## Next milestone

Prototype the Blender frame exchange on Windows early, then add a GPU surface implementation with explicit synchronization and a display output clock. Preserve these ownership and scheduling semantics while validating actual transfer cost. Introduce advanced keying after the full frame path is measurable and stable.
