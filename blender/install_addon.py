"""Enable the built add-on in this Blender version's user configuration."""
from pathlib import Path
import bpy

root = Path(__file__).resolve().parents[1]
archive = root / "build/blender/realtime_spout.zip"
bpy.ops.preferences.addon_install(filepath=str(archive), overwrite=True)
bpy.ops.preferences.addon_enable(module="realtime_spout")
bpy.ops.wm.save_userpref()
print(f"Installed Realtime Spout in Blender {bpy.app.version_string}", flush=True)
