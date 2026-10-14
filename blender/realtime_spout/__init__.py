bl_info = {
    "name": "Realtime Compositor Spout Sender",
    "author": "Realtime Compositor contributors",
    "version": (0, 2, 0),
    "blender": (4, 5, 0),
    "location": "3D View > Sidebar > Realtime",
    "description": "Share an offscreen camera frame through native GPU Spout interop",
    "category": "Render",
}

import ctypes
from pathlib import Path
import time
import tempfile
import traceback

import bpy
import gpu
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, StringProperty
from mathutils import Matrix
from . import traxis
import math

_active = None
_bridge = None
_faulted = None
_validation_phase = 0
error_log_path = Path(tempfile.gettempdir()) / 'RealtimeCompositor' / 'blender-spout-errors.log'


def record_error(error):
    # Record before touching RNA: undo can invalidate the retained scene.
    message = f'Realtime Spout: {error}'
    print(message, flush=True)
    try:
        error_log_path.parent.mkdir(parents=True, exist_ok=True)
        with error_log_path.open('a', encoding='utf-8') as log:
            log.write(time.strftime('%Y-%m-%d %H:%M:%S') + ' ' + message + '\n')
            log.write(traceback.format_exc() + '\n')
    except OSError:
        pass


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
        _bridge.blender_spout_name.argtypes = []
        _bridge.blender_spout_name.restype = ctypes.c_char_p
        _bridge.blender_spout_pattern.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
        _bridge.blender_spout_pattern.restype = ctypes.c_int
        _bridge.blender_spout_clock.argtypes = []
        _bridge.blender_spout_clock.restype = ctypes.c_uint64
        _bridge.blender_spout_send_tagged.argtypes = [ctypes.c_uint,ctypes.c_uint,ctypes.c_uint64,ctypes.c_uint64,ctypes.c_uint64,ctypes.c_uint64]
        _bridge.blender_spout_send_tagged.restype = ctypes.c_int
    return _bridge


def native_error():
    return bridge().blender_spout_error().decode("utf-8", errors="replace")


class RealtimeSettings(bpy.types.PropertyGroup):
    sender: StringProperty(name="Sender", default="RealtimeBlender", maxlen=128)
    width: IntProperty(name="Width", default=1920, min=64, max=4096)
    height: IntProperty(name="Height", default=1080, min=64, max=2160)
    fps: IntProperty(name="Target FPS", default=50, min=1, max=120)
    status: StringProperty(default="Stopped")
    tracking: BoolProperty(name="TraxisHubTrack camera", default=False)
    tracking_port: IntProperty(name="UDP port", default=8873,min=1,max=65535)
    tracking_scale: FloatProperty(name="Units to meters",default=.01,min=.000001,max=100)
    tracking_rotation: EnumProperty(name="Rotation mapping", items=[
        ('UNREAL', 'Named pan / tilt / roll', 'Yaw from pan, pitch from tilt, camera roll from roll'),
        ('Z_YAW', 'Studio export: roll is yaw', 'Observed studio export: yaw from roll, pitch from tilt, camera roll from pan'),
    ], default='UNREAL')
    tracking_status: StringProperty(default="Tracking disabled")
    timed_tracking: BoolProperty(name="Timed tracking (20 ms frame buffer)",default=False)


class REALTIME_OT_start(bpy.types.Operator):
    bl_idname = "realtime_spout.start"
    bl_label = "Start camera sharing"
    _handle = None
    _timer = None
    _offscreen = None
    _started = False
    _drawing = False
    _tracking = None
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
        self._manager = context.window_manager
        self._screen = context.window.screen
        settings = self._scene.realtime_spout
        self._width, self._height, self._fps = settings.width, settings.height, settings.fps
        self._name = settings.sender
        if settings.timed_tracking and settings.fps != 50:
            self.report({'ERROR'}, 'Timed tracking currently requires 50 FPS')
            return {'CANCELLED'}
        self._tracking = None
        self._tracking_scale = settings.tracking_scale
        self._tracking_rotation = settings.tracking_rotation
        self._timed_tracking = settings.timed_tracking
        self._last_tick = -1
        self.scheduled_ns = self.pose_target_ns = 0
        self.tracking_sample = None
        self._tracking_ns = 1 if settings.tracking else 0  # 1 = required but no valid sample yet.
        if settings.tracking:
            try:
                self._tracking = traxis.Receiver(settings.tracking_port, bridge().blender_spout_clock)
            except OSError as error:
                self.report({'ERROR'}, f'Tracking port unavailable: {error}')
                return {'CANCELLED'}
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
        if self._drawing:
            return
        if self.stop_requested:
            self.finish()
            return
        if bpy.context.area != self._area:
            return
        self._drawing = True
        try:
            global _faulted
            if _faulted is not None:
                if _faulted._window != bpy.context.window or _faulted._screen != bpy.context.window.screen:
                    raise RuntimeError('Return to the original window/workspace to recover GPU resources')
                _faulted.release_gpu()  # Context checked natively; retry only if safe.
                _faulted = None
            camera = self._scene.camera
            now_ns = bridge().blender_spout_clock()
            if self._timed_tracking:
                period_ns = round(1e9/self._fps)
                tick = now_ns//period_ns
                if tick <= self._last_tick:
                    return
                self._last_tick = tick
                self.scheduled_ns = tick*period_ns
            else:
                now = time.perf_counter()
                if now < self._next:
                    return
                self._next = max(self._next + 1/self._fps, now)
            if self._offscreen is None:
                self._offscreen = gpu.types.GPUOffScreen(self._width, self._height, format='RGBA8')
            context = bpy.context
            if camera is None:
                raise RuntimeError("Scene camera was removed")
            if self._tracking:
                sample = self._tracking.sample()
                if self._timed_tracking:
                    resampled = self._tracking.sample_at(self.scheduled_ns-20000000)
                    if resampled:
                        sample,self.pose_target_ns = resampled
                    else:
                        sample = None
                if sample:
                    packet,received,sequence,peer = sample
                    age = (bridge().blender_spout_clock()-received)/1e6
                    self._scene.realtime_spout.tracking_status = f'THT #{sequence} | {age:.0f} ms | FOV {packet["effectiveFov"]:.2f}'
                    if age <= 500:
                        camera.matrix_world = Matrix(traxis.camera_matrix(packet,self._tracking_scale,self._tracking_rotation))
                        camera.data.sensor_fit = 'HORIZONTAL'
                        camera.data.sensor_width = packet['sensorSizeX']
                        camera.data.sensor_height = packet['sensorSizeY']
                        camera.data.lens = packet['sensorSizeX']/(2*math.tan(math.radians(packet['effectiveFov'])/2))
                        self._tracking_ns = received
                        self.tracking_sample = sample
                        context.view_layer.update()
                else:
                    self._scene.realtime_spout.tracking_status = 'Waiting for valid THT JSON'
            evaluated = camera.evaluated_get(context.evaluated_depsgraph_get())
            projection = evaluated.calc_matrix_camera(context.evaluated_depsgraph_get(),
                x=self._width, y=self._height,
                scale_x=self._scene.render.pixel_aspect_x, scale_y=self._scene.render.pixel_aspect_y)
            render_start = time.perf_counter()
            render_begin_ns = bridge().blender_spout_clock()
            self._offscreen.draw_view3d(self._scene, context.view_layer, context.space_data, context.region,
                evaluated.matrix_world.inverted(), projection, do_color_management=True, draw_background=True)
            send_start = time.perf_counter()
            with self._offscreen.bind():
                if _validation_phase:
                    bridge().blender_spout_pattern(self._width, self._height, _validation_phase)
                if not self._started:
                    if not bridge().blender_spout_start(self._name.encode('utf-8')):
                        raise RuntimeError(native_error())
                    self._started = True
                    self._name = bridge().blender_spout_name().decode('utf-8')
                result = bridge().blender_spout_send_tagged(self._width,self._height,render_begin_ns,self._tracking_ns,self.scheduled_ns,self.pose_target_ns)
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
            record_error(error)
            try:
                self._scene.realtime_spout.status = f"Error: {error}"
            except (ReferenceError, AttributeError):
                pass
            self.finish(fault=True)
        finally:
            self._drawing = False

    def release_gpu(self):
        # Draw callback provides the original OpenGL context for SDK teardown.
        if self._started:
            if not bridge().blender_spout_stop():
                raise RuntimeError(native_error())
            self._started = False
        if self._offscreen is not None:
            try:
                self._offscreen.free()
            except ReferenceError:
                pass  # Blender already invalidated this offscreen object.
            finally:
                self._offscreen = None

    def modal(self, context, event):
        if self._done:
            return {'FINISHED'}
        if event.type == 'TIMER':
            try:
                origin_available = self._window.screen == self._screen and self._area.type == 'VIEW_3D'
            except ReferenceError:
                origin_available = False
            if self.stop_requested or not origin_available:
                self.finish()
                return {'FINISHED'}
            if self.frames:
                self._scene.realtime_spout.status = (
                    f"GPU | {self.frames} frames | render {self.render_ms:.2f} ms | send {self.send_ms:.2f} ms")
            self._area.tag_redraw()
        return {'PASS_THROUGH'}

    def finish(self, fault=False):
        global _active, _faulted
        if self._done:
            return
        message = "Stopped"
        try:
            self.release_gpu()
        except Exception as error:
            _faulted = self  # Keep ownership, never free GL resources in a wrong context.
            message = f"Faulted: {error}. Return to original viewport to retry; restart Blender if its context was lost."
        finally:
            if self._tracking is not None:
                self._tracking.close()
                self._tracking = None
            self._done = True
            if self._handle is not None:
                try:
                    bpy.types.SpaceView3D.draw_handler_remove(self._handle, 'WINDOW')
                except (ValueError, ReferenceError):
                    pass
                self._handle = None
            if self._timer is not None:
                try:
                    self._manager.event_timer_remove(self._timer)
                except (ValueError, ReferenceError):
                    pass
                self._timer = None
            if _active is self:
                _active = None
            try:
                if not fault or _faulted is self:
                    self._scene.realtime_spout.status = message
            except (ReferenceError, AttributeError):
                pass

    def cancel(self, context):
        self.finish()


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
        column.prop(settings, "tracking")
        if settings.tracking:
            column.prop(settings, "tracking_port")
            column.prop(settings, "tracking_scale")
            column.prop(settings, "tracking_rotation")
            column.prop(settings, "timed_tracking")
        if _active is None:
            layout.operator("realtime_spout.start")
        else:
            layout.operator("realtime_spout.stop")
        layout.label(text=settings.status)
        layout.label(text=settings.tracking_status)
        layout.label(text="OpenGL GPU sharing; CPU fallback disabled")
        layout.label(text="Display color / RGBA8 camera preview")


classes = (RealtimeSettings, REALTIME_OT_start, REALTIME_OT_stop, REALTIME_PT_sender)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.realtime_spout = bpy.props.PointerProperty(type=RealtimeSettings)
    if before_load not in bpy.app.handlers.load_pre:
        bpy.app.handlers.load_pre.append(before_load)
    for handlers, callback in ((bpy.app.handlers.undo_pre, before_load),
                               (bpy.app.handlers.redo_pre, before_load),
                               (bpy.app.handlers.undo_post, after_history),
                               (bpy.app.handlers.redo_post, after_history)):
        if callback not in handlers:
            handlers.append(callback)


def unregister():
    if _active is not None:
        _active.finish()
    if before_load in bpy.app.handlers.load_pre:
        bpy.app.handlers.load_pre.remove(before_load)
    for handlers, callback in ((bpy.app.handlers.undo_pre, before_load),
                               (bpy.app.handlers.redo_pre, before_load),
                               (bpy.app.handlers.undo_post, after_history),
                               (bpy.app.handlers.redo_post, after_history)):
        if callback in handlers:
            handlers.remove(callback)
    del bpy.types.Scene.realtime_spout
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


@bpy.app.handlers.persistent
def before_load(_):
    if _active is not None:
        _active.finish()


@bpy.app.handlers.persistent
def after_history(_):
    # New RNA belongs to the restored scene. Explicit Start reacquires it and
    # retries any retained GPU teardown in its original draw context.
    for scene in bpy.data.scenes:
        scene.realtime_spout.status = 'Stopped after undo/redo. Start sharing to resume.'
