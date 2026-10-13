bl_info = {
    "name": "Realtime Compositor Spout Sender",
    "author": "Realtime Compositor contributors",
    "version": (0, 1, 0),
    "blender": (4, 5, 0),
    "location": "3D View > Sidebar > Realtime",
    "description": "Share an offscreen camera frame through native GPU Spout interop",
    "category": "Render",
}

import ctypes
from pathlib import Path
import time

import bpy
import gpu
from bpy.props import IntProperty, StringProperty

_active = None
_bridge = None


def bridge():
    global _bridge
    if _bridge is None:
        _bridge = ctypes.CDLL(str(Path(__file__).with_name("blender_spout_bridge.dll")))
        _bridge.blender_spout_start.argtypes = [ctypes.c_char_p]
        _bridge.blender_spout_start.restype = ctypes.c_int
        _bridge.blender_spout_send.argtypes = [ctypes.c_uint, ctypes.c_uint]
        _bridge.blender_spout_send.restype = ctypes.c_int
        _bridge.blender_spout_stop.argtypes = []
        _bridge.blender_spout_stop.restype = ctypes.c_int
        _bridge.blender_spout_error.argtypes = []
        _bridge.blender_spout_error.restype = ctypes.c_char_p
    return _bridge


def native_error():
    return bridge().blender_spout_error().decode("utf-8", errors="replace")


class RealtimeSettings(bpy.types.PropertyGroup):
    sender: StringProperty(name="Sender", default="RealtimeBlender", maxlen=128)
    width: IntProperty(name="Width", default=1920, min=64, max=4096)
    height: IntProperty(name="Height", default=1080, min=64, max=2160)
    fps: IntProperty(name="Target FPS", default=50, min=1, max=120)
    status: StringProperty(default="Stopped")


class REALTIME_OT_start(bpy.types.Operator):
    bl_idname = "realtime_spout.start"
    bl_label = "Start camera sharing"
    _handle = None
    _timer = None
    _offscreen = None
    _started = False
    _drawing = False
    stop_requested = False

    def execute(self, context):
        global _active
        if _active is not None:
            return {'CANCELLED'}
        if gpu.platform.backend_type_get() != 'OPENGL':
            self.report({'ERROR'}, "This prototype requires Blender's OpenGL backend")
            return {'CANCELLED'}
        if not context.scene.camera or context.area.type != 'VIEW_3D':
            self.report({'ERROR'}, "Select a scene camera and start in a 3D View")
            return {'CANCELLED'}
        try:
            bridge()
        except OSError as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        self._area = context.area
        self._window = context.window
        self._scene = context.scene
        settings = self._scene.realtime_spout
        self._width, self._height, self._fps = settings.width, settings.height, settings.fps
        self._name = settings.sender
        self.frames = 0
        self.skipped = 0
        self.render_ms = self.send_ms = 0.0
        self._next = 0.0
        self._done = False
        self.stop_requested = False
        self._handle = bpy.types.SpaceView3D.draw_handler_add(self.draw_frame, (), 'WINDOW', 'POST_PIXEL')
        self._timer = context.window_manager.event_timer_add(1/self._fps, window=context.window)
        context.window_manager.modal_handler_add(self)
        settings.status = "Starting GPU sender..."
        _active = self
        self._area.tag_redraw()
        return {'RUNNING_MODAL'}

    def draw_frame(self):
        if self._drawing or bpy.context.area != self._area:
            return
        self._drawing = True
        try:
            if self.stop_requested:
                self.release_gpu()
                self._done = True
                return
            now = time.perf_counter()
            if now < self._next:
                return
            self._next = max(self._next + 1/self._fps, now)
            if self._offscreen is None:
                self._offscreen = gpu.types.GPUOffScreen(self._width, self._height, format='RGBA8')
            context = bpy.context
            camera = self._scene.camera
            if camera is None:
                raise RuntimeError("Scene camera was removed")
            evaluated = camera.evaluated_get(context.evaluated_depsgraph_get())
            projection = evaluated.calc_matrix_camera(context.evaluated_depsgraph_get(),
                x=self._width, y=self._height,
                scale_x=self._scene.render.pixel_aspect_x, scale_y=self._scene.render.pixel_aspect_y)
            render_start = time.perf_counter()
            self._offscreen.draw_view3d(self._scene, context.view_layer, context.space_data, context.region,
                evaluated.matrix_world.inverted(), projection, do_color_management=True, draw_background=True)
            send_start = time.perf_counter()
            with self._offscreen.bind():
                if not self._started:
                    if not bridge().blender_spout_start(self._name.encode('utf-8')):
                        raise RuntimeError(native_error())
                    self._started = True
                result = bridge().blender_spout_send(self._width, self._height)
                if not result:
                    raise RuntimeError(native_error())
            end = time.perf_counter()
            if result == 1:
                self.frames += 1
            else:
                self.skipped += 1
            self.render_ms = (send_start-render_start)*1000
            self.send_ms = (end-send_start)*1000
        except Exception as error:
            self._scene.realtime_spout.status = f"Error: {error}"
            print(f"Realtime Spout: {error}", flush=True)
            self.release_gpu()
            self._done = True
        finally:
            self._drawing = False

    def release_gpu(self):
        # Draw callback provides the original OpenGL context for SDK teardown.
        if self._started:
            if not bridge().blender_spout_stop():
                raise RuntimeError(native_error())
            self._started = False
        if self._offscreen is not None:
            self._offscreen.free()
            self._offscreen = None

    def modal(self, context, event):
        global _active
        if self._done:
            bpy.types.SpaceView3D.draw_handler_remove(self._handle, 'WINDOW')
            context.window_manager.event_timer_remove(self._timer)
            if self.stop_requested:
                self._scene.realtime_spout.status = "Stopped"
            _active = None
            return {'FINISHED'}
        if event.type == 'TIMER':
            if self.frames:
                self._scene.realtime_spout.status = (
                    f"GPU | {self.frames} frames | render {self.render_ms:.2f} ms | send {self.send_ms:.2f} ms")
            self._area.tag_redraw()
        return {'PASS_THROUGH'}


class REALTIME_OT_stop(bpy.types.Operator):
    bl_idname = "realtime_spout.stop"
    bl_label = "Stop sharing"

    def execute(self, context):
        if _active is not None:
            _active.stop_requested = True
            _active._area.tag_redraw()
        return {'FINISHED'}


class REALTIME_PT_sender(bpy.types.Panel):
    bl_label = "Compositor camera texture"
    bl_idname = "REALTIME_PT_sender"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Realtime'

    def draw(self, context):
        settings = context.scene.realtime_spout
        layout = self.layout
        column = layout.column()
        column.enabled = _active is None
        column.prop(settings, "sender")
        row = column.row(align=True)
        row.prop(settings, "width")
        row.prop(settings, "height")
        column.prop(settings, "fps")
        if _active is None:
            layout.operator("realtime_spout.start")
        else:
            layout.operator("realtime_spout.stop")
        layout.label(text=settings.status)
        layout.label(text="OpenGL GPU sharing; CPU fallback disabled")
        layout.label(text="Display color / RGBA8 camera preview")


classes = (RealtimeSettings, REALTIME_OT_start, REALTIME_OT_stop, REALTIME_PT_sender)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.realtime_spout = bpy.props.PointerProperty(type=RealtimeSettings)


def unregister():
    if _active is not None:
        raise RuntimeError("Stop sharing before disabling this add-on")
    del bpy.types.Scene.realtime_spout
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
