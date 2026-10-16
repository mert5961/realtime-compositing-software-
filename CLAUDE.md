# CLAUDE.md — Realtime Compositing Software

Handoff notes for Claude Code. Read this first, then README.md and STUDIO_RUNTIME.md.

## What this project is

A Windows real-time virtual-production green-screen compositor for broadcast (C++20, ~2.5k lines):

```
AJA Corvid44 SDI in -> host RAM -> D3D11 green key + spill  ┐
Blender (OpenGL offscreen) -> Spout -> 4 owned GPU surfaces ┴-> over -> GPU UYVY pack
   -> readback -> 3 host surfaces -> AJA SDI out (hardware-paced) -> TV
TraxisHubTrack JSON (UDP 8873, ~50 Hz) -> Blender camera pose/FOV (fixed 20 ms timeline)
```

Key files:
- `src/studio_engine.*` — `studio::Engine`: thin, UI-thread-safe control API (start/stop/wait, cached `snapshot()`, live `setKeyParams`, bounded event queue, `enumerateDevices()` — the latter calls AJA, run it off the UI thread). Its monitor thread does ownership check, connector validation, BurnConfig, CSV logging, fault/signal-loss detection and pass/fail.
- `src/studio_types.hpp` — AJA/Spout/D3D-free types (config, stats, snapshot, events, key params) plus pure monitor logic (`assess`, `passed`, CSV row). JUCE code and portable tests include only this + `studio_engine.hpp`.
- `src/studio_runtime.*` — capture/output threads, inherits AJA's `NTV2Burn` demo class. Reads the live key from the engine's lock-free `std::atomic<KeyParams>` before each composite.
- `src/studio_gpu.*` — D3D11 devices, Spout receiver worker, green key, UYVY pack, `gpuSelfTest()` (now also proves a live key change reaches the pixels).
- `src/studio_test_win.cpp` — thin Win32 UI **and** CLI over `studio::Engine`.
- `src/studio_control.hpp` — `studio::Control`, the UI-facing interface; `makeEngineControl()` (real, in `studio_engine`) and `makeFakeControl()` (`src/studio_fake.cpp`, portable synthetic engine; tested by `studio_fake_engine`).
- `app/` — JUCE app "Realtime Studio Control" (`Main.cpp`, `StudioPanels.*`, `RustLookAndFeel.*`). `--fake` runs the simulated engine; `--fake-fault N` simulates a fault after N s. Settings: `%APPDATA%\RealtimeStudio\RealtimeStudioControl.settings`.
- CMake targets: `studio_engine` (static lib: engine+runtime+GPU+AJA demo glue), `studio_fake`, `studio_test`, `studio_control` (needs `-DCOMPOSITOR_BUILD_JUCE_UI=ON`; JUCE 9.0.3 pinned at `be29c81` in `build/deps/JUCE`).
- `src/sdi_test_win.cpp` — older standalone SDI test UI (duplicates some of that logic).
- `include/compositor/live_pool.hpp` — bounded lease-based frame pool used by the live path.
- `include/compositor/core.hpp`, `src/core.cpp`, `src/main.cpp` — portable synthetic scheduler prototype.
- `blender/realtime_spout/` — Blender add-on (Spout sender, tracking, timed poses).

## The four movie rules (must never be broken)

1. **Night of the Living Dead** — failed transfers never produce valid frames.
2. **No Country for Old Men** — SDI output keeps running when Blender stalls; stale (>500 ms) or missing Blender/pose -> camera passthrough.
3. **Fight Club** — object lifetime doesn't preserve pixels; leases do. No reuse while a reader (GPU or DMA) holds it.
4. **Perfect Blonde** — a test passes only if the correct image is actually delivered.

Also: pools are bounded and never block; full pools discard new work. No allocation or file I/O in frame loops.

## Build & test (Developer PowerShell for VS, project root)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup-studio.ps1   # fetches pinned AJA/Spout, builds build/studio
ctest --test-dir build/studio -C Release --output-on-failure
.\build\studio\Release\studio_test.exe --gpu-selftest
```

Hardware runs (ASK THE USER FIRST every time — they take exclusive control of the AJA card):
```powershell
.\build\studio\Release\studio_test.exe --seconds 60 --input 1 --output 4 --sender RealtimeStudio
```
- Never start AJA/UDP 8873 runs while the studio may be on air or another app (e.g. nosLauncher/Reality) owns the card.
- The user verifies the picture on the TV; Claude cannot see SDI output. Give them a short checklist of what to look for.
- Stop Blender before rebuilding the add-on DLL.

## Goal: JUCE UI

The user wants a new JUCE app with: **start/stop + routing, live key tuning, a node graph, diagnostics**, and later **shader editing**. No video preview in the UI — the TV monitor is enough. Keep the CLI and old Win32 test tools working as diagnostics.

### Visual direction
- **Pink is the UI's main/accent color** (the chroma key stays GREEN — do not change the keyer).
- "Rusted edges": weathered/rusted borders on panels, knobs, node boxes. Implement via a custom `LookAndFeel_V4` subclass; generate rust textures procedurally (or embed PNGs via BinaryData) **once** at startup into cached `juce::Image`s — never regenerate per repaint.
- Dark background so pink + rust read well; the UI must stay legible at a glance during a show.

### Threading rule for the UI
The JUCE message thread must **never** call AJA, Spout or D3D11. It only calls a thin engine API and polls `snapshot()` from a `juce::Timer` (4–10 Hz), exactly like the current Win32 UI.

## Plan (do in order; user approves each phase)

**Phase 0 — engine extraction (no behaviour change).** *Status 2026-10-16: code done; ctest (6 targets) + `--gpu-selftest` pass. Hardware 60 s run PASS: 3,007 captured / 3,008 submitted, 2 startup repeats, zero AJA drops (matches previous 3,010/3,011/2/0). Live-key check on the TV via the Win32 UI still to confirm.*
- New `studio::Engine` with roughly: `enumerateDevices()`, `start(EngineConfig)`, `stop()`, `snapshot()`, `setKeyParams(threshold, softness)`, fault/event callback or queue.
- Move ownership check, connector validation, BurnConfig building, CSV logging, signal-loss/fault detection and timed pass/fail out of `studio_test_win.cpp` into the engine.
- Make threshold/softness adjustable **live** (atomics or a small param block read by `GpuCompositor` per frame).
- CLI + existing Win32 UI become thin clients. Prove with ctest, `--gpu-selftest`, and (with user permission) a 60 s hardware run matching previous results (zero AJA drops, ~startup repeats only).

**Phase 1 — JUCE app shell.** *Status 2026-10-16: built (JUCE 9.0.3); 7 ctest targets pass; UI verified with the simulated engine (idle/running/stopped/failed screens, on-air quit confirmation, settings + window persistence). Real-engine run of the JUCE app on the card still to do with the user.*
- Fetch JUCE into `build/deps` like AJA/Spout (pinned tag + commit check in a setup script); `juce_add_gui_app`; new CMake option e.g. `COMPOSITOR_BUILD_JUCE_UI`. GUI modules only, no audio.
- Panels: Transport (card, input/output SDI, Blender sender, Start/Stop), Key (live sliders), Diagnostics (captured/submitted, repeats, fallbacks, pool full, AJA drops & buffer levels, camera/Blender/render/tracking/pose ages with small sparklines, timestamped fault log, CSV path, "Open logs").
- Persist settings with `ValueTree` + `PropertiesFile`.
- Tip: JUCE is cross-platform — UI pieces can be developed against a **fake engine** that produces synthetic stats/faults.

**Phase 2 — node graph editor.**
- Custom JUCE component (JUCE has none built in): node boxes, typed ports, bezier cables, pan/zoom, save/load (ValueTree/JSON).
- Default graph = current pipeline: `SDI In -> Green Key -> Over <- Blender (Spout) -> SDI Out`.
- The graph is a **description only**. Edit -> validate -> compile to an immutable render plan -> swap at a frame boundary. Invalid graph never reaches output; last valid plan keeps running. Camera-passthrough fallback still applies.

**Phase 3 — shader nodes.**
- The engine is D3D11, so shaders are **HLSL**. GLSL needs cross-compilation (glslang -> SPIR-V -> SPIRV-Cross -> HLSL). Decide with the user when we get there.
- Editor via `juce::CodeEditorComponent`; compile on a worker thread; swap only on success; keep last good shader.

## Known limits to surface in the UI (don't hide)
Progressive 1080p/720p only, 8-bit UYVY BT.709 limited, no audio; timed tracking requires 50 fps (20 ms steps); 3 completed host output surfaces; 4 AJA slots per direction. Not glass-to-glass latency or exposure-aligned.

## Licensing note
JUCE 9 (pinned 9.0.3) is dual-licensed: AGPLv3 or the commercial JUCE 9 EULA (https://juce.com/legal/juce-9-licence/). A commercial licence may be required — the user must choose and check terms before shipping/distributing. Spout (BSD) license is copied next to the bridge DLL.

## Working style
- Code style in this repo is dense; new code may be more readable but keep consistency within a file.
- Commit only when asked. Don't modify downloaded SDK sources (the CMake patches copy them).
- The `output/` and `build/` folders are git-ignored; logs live in `output/`.
