# Studio tracking export

On 2026-10-14, UDP 8873 and 8878 carried matching TraxisHubTrack JSON
payloads at approximately 50 Hz. Port 8873 remains the production application
input; 8878 is the diagnostic sender.

A physical pan-only test changed JSON `roll` by 53.2 degrees, `tilt` by
3.0 degrees, and `pan` by 0.73 degrees. The earlier assumption that these
field names directly described Unreal yaw, pitch, and camera roll does not
match this installation's export.

The add-on now exposes two explicit rotation mappings. The studio demo selects
**Studio export: roll is yaw**: yaw = JSON roll, pitch = JSON tilt, camera roll =
JSON pan. The original named pan/tilt/roll mapping remains available for other
senders. Position conversion remains centimeters with X forward, Y right,
Z up into Blender coordinates (x, -y, z), with no origin subtraction.

This is an observed installation-specific mapping, not a claim about every
TraxisHubTrack sender. Pan sign, tilt sign, roll, and position axes still require
physical validation against fixed studio references. Traxis Hub's native
right-handed +Y-up coordinates do not establish the outgoing sender's axes:
https://docs.zerodensity.io/traxishub-3.1/traxis-hub/lens-calibration/configure-tracker/configure-tracker-ptz

The demo status JSON includes the selected mapping, camera world matrix, and
evaluated camera world matrix to distinguish incoming-packet progress from
actual rendered-camera movement.

The studio demo now enables timed tracking: fixed 20 ms frame steps, bounded
receive-time pose interpolation, and at least one frame of compositor
presentation delay. Render callbacks may still arrive unevenly; frame content
and selection follow the fixed timeline instead. The demo cube has no animation.
See STUDIO_RUNTIME.md for measured age, remaining repeats, and clock limitations.

Undo/redo stops the sender before Blender replaces scene RNA. Start sharing
again to acquire the restored scene. Errors are saved in
output/blender-spout-errors.log for the demo, and the launcher saves stdout and
stderr to dated output/blender-studio logs.
