# Blender camera texture prototype

The Windows branch receives a live Blender camera texture on the GPU and
previews it in a native Direct3D 11 window. It is a separate test from the AJA
capture/playout tool. Blender-to-SDI output and camera compositing are not wired
up yet.

## Run on this computer

Blender **4.5.9 LTS** is installed per user at
`%LOCALAPPDATA%\Programs\Blender\blender-4.5.9-windows-x64\blender.exe`.
The official ZIP was checked against Blender's published SHA-256 file. A
**Blender 4.5 LTS** desktop shortcut is available. The Realtime Spout add-on is
installed and enabled in Blender's 4.5 user preferences.

1. Double-click **Start Blender Demo.cmd**. It opens a disposable default scene,
   animates the cube, and starts camera sharing at 1920 x 1080 with a 50 FPS target.
2. Double-click **Start Blender Texture Test.cmd**. The receiver connects to
   `RealtimeBlender` and displays the GPU texture and live counters.
3. In Blender's 3D View, open the sidebar with **N**, then select **Realtime**.
   Use **Stop sharing** before changing size/name/rate. Use **Start camera
   sharing** to resume. Keep the originating 3D View open while streaming.
4. The receiver's **Connect / reset** button connects to the entered sender
   name and resets frame totals. Close the receiver independently when done.
   Logs are in `output/blender-texture-*.csv`.

For your own scene, open Blender normally, select the scene camera, and use
the **Realtime** sidebar. This version requires the **OpenGL** backend and a
compatible GPU shared by both applications. The demo launcher explicitly selects
OpenGL. Camera rendering follows the originating viewport's shading settings.
The demo uses Solid shading and disables overlays; it does not benchmark EEVEE
or Cycles. Normal add-on use respects your viewport settings, including overlays.

## Build and install the add-on

```powershell
powershell -ExecutionPolicy Bypass -File .\setup-blender.ps1 -BlenderPath 'C:\path\to\blender.exe' -InstallAddon
```

This downloads Spout **2.007.018**, pinned to
`6b68a5b4fdeca6130b851e5f9366467a844860e5`, into ignored `build/deps/Spout2`.
It builds the C++ receiver and native sender DLL, packages
`build/blender/realtime_spout.zip`, and optionally installs that ZIP into the
specified Blender. Without `-InstallAddon`, you can install the ZIP through
Blender Preferences > Add-ons > Install from Disk. Run setup again after
changing the source. Stop Blender before replacing a loaded DLL.

Blender itself is a separate prerequisite; the setup script does not download
it. Its executable path is remembered locally in ignored `output/blender-path.txt`.
No SDK, Blender binary, local installation path, image, or runtime CSV is committed.
Spout's BSD license is copied beside the built DLL and into the add-on archive.

## Frame path and buffers

```text
Blender camera scene
  -> Blender GPUOffScreen RGBA8 texture
  -> native DLL: GPU framebuffer blit through OpenGL/DirectX interop
  -> Spout shared D3D11 texture
  -> receiver-local D3D11 texture copy
  -> GPU preview shader / window back buffers
```

There is no pixel readback to host RAM in normal sending, receiving, or preview.
There are GPU copies; this is not a claim of zero GPU copies. Python controls
rendering and passes dimensions to the DLL, not an array of image pixels.
The native bridge checks hardware interop and disables CPU fallback. Vulkan
and other Blender backends are rejected rather than silently using RAM.

The offscreen texture belongs to Blender's originating draw context. Spout owns
the shared texture and coordinates access with its interprocess lock and
GL/DX interop. The receiver uses Spout's local texture, copied under that lock,
then samples it on the same D3D11 device/context. Preview resources are recreated
when the sender's size or resource generation changes. The application has no
FIFO of pending Blender images: it consumes the latest available publication.
This is independent of the AJA device rings and ten-frame host ring.

The optional CLI `--snapshot output/frame.ppm` explicitly performs a **one-time
GPU readback** to inspect received pixels. That diagnostic operation adds a
stall and should be omitted when measuring the normal transfer path.

## What the counters mean

- **New frames:** distinct Spout publication counts observed by the receiver.
- **Skipped sender frames:** gaps in that count within one connection generation.
  These are publications the receiver did not observe, not AJA drops or proof
  that Blender failed to render. Reconnecting establishes a fresh baseline.
- **Observed FPS:** newly received frames per sampling interval. Repeated polls
  do not count. If a sender has no counter, FPS is `-1` (unavailable).
- **Receive API avg/max:** wall time spent in `ReceiveTexture` across polls in
  the last interval. It includes waiting/copy submission and scheduling; it is
  not a GPU timestamp or camera-to-display latency measurement.
- **Last new frame age:** time since the last observed publication. The UI marks
  a connected sender stalled after 500 ms. Sender disappearance clears the preview.
- **Render/send in Blender:** wall duration of the two API calls on the draw
  thread; not GPU execution timestamps. Target FPS is a request, not a guarantee.

Spout defaults to disabling frame counts when its registry setting is absent.
CMake builds a generated SDK source copy enabling counters **only in our
binaries**, preserving the downloaded source and system-wide preferences.
The bridge checks the sender counter after publishing because the SDK's boolean
return can be true even when its interprocess lock did not permit a new frame.

## Validation

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\test_blender_texture.ps1
```

The test launches a disposable Blender scene and receiver, exercises
1080p -> Stop -> 720p -> Stop -> 1080p, then cleanly stops Blender. It checks
counted frames, three connection generations, resolution changes, a diagnostic
image, process exit codes, and detection of sender shutdown. It takes about
50 seconds; close other `RealtimeBlender` senders before running it.

Initial live validation on the RTX PRO 6000 Blackwell and NVIDIA 610.47:

- 2,066 distinct received frames across three connection generations.
- Approximately 50 FPS within the active portions of each generation.
- One skipped sender publication across startup/reconnection and steady operation.
- Both 1920 x 1080 and 1280 x 720 textures received, with a visually checked
  diagnostic image. Blender stopped normally and the receiver detected its exit.
- Portable compositor unit tests and demo scenario also passed.

These are short prototype checks with the default Solid scene, not a sustained
performance guarantee or a full rendering/compositing benchmark. Logs remain
local under `output`.

## Remaining integration

The first version exports a display-color RGBA8 camera preview. Linear HDR,
alpha conventions, production color management, tracked camera metadata,
frame deadlines, EEVEE/Cycles performance, and AJA upload/download still need
separate validation. Stop sharing before loading another file, closing the
originating viewport, changing graphics backend, or disabling the add-on.

Primary references:
[Blender GPU API](https://docs.blender.org/api/4.5/gpu.html),
[Spout SDK](https://github.com/leadedge/Spout2/tree/2.007.018).
The existing [TextureSharing add-on](https://github.com/maybites/TextureSharing)
was investigated; its current Windows sender reads texture pixels before
`sendImage`. This prototype instead supplies a native framebuffer bridge.
