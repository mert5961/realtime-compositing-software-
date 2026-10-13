"""Launch with Blender --factory-startup --gpu-backend opengl --python this_file.

Uses a disposable startup scene; does not overwrite a user's .blend or preferences.
"""
from pathlib import Path
import json
import sys
import time
import bpy

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "build/blender-addon"))
import realtime_spout
realtime_spout.register()
scene = bpy.context.scene
scene.render.fps = 50
scene.frame_end = 250
scene.realtime_spout.width = 1920
scene.realtime_spout.height = 1080
cube = bpy.data.objects.get("Cube")
if cube:
    cube.rotation_euler = (0, 0, 0)
    cube.keyframe_insert(data_path="rotation_euler", frame=1)
    cube.rotation_euler = (1.0, 0.6, 6.283185)
    cube.keyframe_insert(data_path="rotation_euler", frame=250)
scene.frame_set(1)
started = time.monotonic()
validation = '--validate' in sys.argv
phase = 0


def start():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.spaces.active.shading.type = 'SOLID'
                area.spaces.active.overlay.show_overlays = False
                area.spaces.active.show_region_ui = True
                region = next(r for r in area.regions if r.type == 'WINDOW')
                with bpy.context.temp_override(window=window, area=area, region=region):
                    bpy.ops.realtime_spout.start()
                    if not bpy.context.screen.is_animation_playing:
                        bpy.ops.screen.animation_play()
                return None
    return 1.0


def report():
    global phase
    active = realtime_spout._active
    data = {"blender": bpy.app.version_string, "status": scene.realtime_spout.status,
            "elapsed_s": time.monotonic()-started, "frames": active.frames if active else 0,
            "render_api_ms": active.render_ms if active else 0,
            "send_api_ms": active.send_ms if active else 0}
    (root / "output/blender-sender-status.json").write_text(json.dumps(data, indent=2))
    if validation:
        elapsed = time.monotonic()-started
        if phase == 0 and elapsed >= 15 and active:
            active.stop_requested = True
            phase = 1
        elif phase == 1 and active is None:
            scene.realtime_spout.width = 1280
            scene.realtime_spout.height = 720
            start()
            phase = 2
        elif phase == 2 and elapsed >= 30 and active:
            active.stop_requested = True
            phase = 3
        elif phase == 3 and active is None:
            scene.realtime_spout.width = 1920
            scene.realtime_spout.height = 1080
            start()
            phase = 4
        elif phase == 4 and elapsed >= 45 and active:
            active.stop_requested = True
            phase = 5
        elif phase == 5 and active is None:
            bpy.ops.wm.quit_blender()
            return None
    return 1.0


bpy.app.timers.register(start, first_interval=2.0)
bpy.app.timers.register(report, first_interval=3.0)
