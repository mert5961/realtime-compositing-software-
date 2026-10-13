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
