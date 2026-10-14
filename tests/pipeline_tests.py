import contextlib
import csv
import importlib.util
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import types
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from validate_blender_run import validate
from analyze_sdi_profile import analyze


class PipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fake=types.ModuleType('bpy'); fake.types=types.SimpleNamespace(Operator=object,Panel=object,PropertyGroup=object,Scene=type('Scene',(),{}))
        fake.props=types.ModuleType('bpy.props')
        for name in ('BoolProperty','FloatProperty','IntProperty','StringProperty','PointerProperty'):
            setattr(fake.props,name,lambda **kwargs:None)
        fake.app=types.SimpleNamespace(handlers=types.SimpleNamespace(persistent=lambda function:function,load_pre=[]))
        fake.utils=types.SimpleNamespace(register_class=lambda cls:None,unregister_class=lambda cls:None)
        cls.removed=[]
        fake.types.SpaceView3D=types.SimpleNamespace(draw_handler_remove=lambda *args:cls.removed.append('draw'))
        sys.modules['bpy']=fake; sys.modules['bpy.props']=fake.props; sys.modules['gpu']=types.ModuleType('gpu')
        sys.modules['mathutils']=types.SimpleNamespace(Matrix=lambda value:value)
        spec=importlib.util.spec_from_file_location('test_addon',ROOT/'blender/realtime_spout/__init__.py',submodule_search_locations=[str(ROOT/'blender/realtime_spout')])
        cls.addon=importlib.util.module_from_spec(spec); sys.modules['test_addon']=cls.addon; spec.loader.exec_module(cls.addon)

    def operator(self, stop_succeeds):
        addon=self.addon; operator=addon.REALTIME_OT_start()
        operator._started=True; operator._done=False; operator._handle=object(); operator._timer=object(); operator._tracking=None
        operator._manager=types.SimpleNamespace(event_timer_remove=lambda value:self.removed.append('timer'))
        operator._scene=types.SimpleNamespace(realtime_spout=types.SimpleNamespace(status='Streaming'))
        self.freed=[]; operator._offscreen=types.SimpleNamespace(free=lambda:self.freed.append(True))
        self.stop_succeeds=stop_succeeds
        addon.bridge=lambda:types.SimpleNamespace(blender_spout_stop=lambda:int(self.stop_succeeds),blender_spout_error=lambda:b'context lost')
        addon._active=operator; addon._faulted=None; self.removed.clear()
        return operator

    def test_lifecycle_context_error_does_not_strand_active_state(self):
        operator=self.operator(False); operator.finish()
        self.assertTrue(operator._done); self.assertIsNone(self.addon._active)
        self.assertIs(self.addon._faulted,operator); self.assertEqual(self.freed,[])
        self.assertEqual(self.removed,['draw','timer'])
        operator.finish(); self.assertEqual(self.removed,['draw','timer'])
        self.stop_succeeds=True; operator.release_gpu(); self.assertFalse(operator._started); self.assertEqual(self.freed,[True])

    def test_file_load_and_cancel_stop_cleanly(self):
        operator=self.operator(True); self.addon.before_load(None)
        self.assertIsNone(self.addon._active); self.assertTrue(operator._done)
        operator=self.operator(True); operator.cancel(None)
        self.assertIsNone(self.addon._active)

    def test_traxis_packet_and_unreal_camera_basis(self):
        traxis=self.addon.traxis
        packet=dict(x=100,y=200,z=300,pan=0,tilt=0,roll=0,effectiveFov=90,sensorSizeX=9.6,sensorSizeY=5.4)
        value=traxis.parse(json.dumps(packet)); matrix=traxis.camera_matrix(value)
        self.assertEqual(tuple(row[3] for row in matrix[:3]),(1,-2,3))
        # Optical forward is -camera local Z, mapping to Unreal world +X.
        self.assertEqual(tuple(-row[2] for row in matrix[:3]),(1,0,0))
        for pan,tilt,roll in [(90,0,0),(20,30,40),(-120,-60,70)]:
            matrix=traxis.camera_matrix(dict(packet,pan=pan,tilt=tilt,roll=roll))
            for i in range(3):
                for j in range(3):
                    self.assertAlmostEqual(sum(matrix[k][i]*matrix[k][j] for k in range(3)),float(i==j))
        for key,bad in [('x',float('nan')),('sensorSizeX',0),('effectiveFov',180),('pan',True)]:
            with self.assertRaises(ValueError): traxis.parse(json.dumps(dict(packet,**{key:bad})))
        with self.assertRaises(ValueError): traxis.parse('{}')

    def test_header_only_analyzer_preserves_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'run-trace.csv'
            path.write_text('end_seconds,shutdown,event,duration_ms,success,host_capture,host_ready,host_output,host_occupied\n')
            path.with_name('run.csv').write_text('seconds,captured\n')
            self.assertIn('No samples collected',analyze(path,1))

    def test_validator_rejects_false_resize_and_stale_pixels(self):
        with tempfile.TemporaryDirectory() as directory:
            csv_path=Path(directory)/'run.csv'; image=Path(directory)/'image.ppm'
            rows=[]
            for generation,(width,height,color) in enumerate([(1920,1080,b'\xff\x00\x00'),(1280,720,b'\x00\xff\xff'),(1920,1080,b'\xff\x00\xff')],1):
                for i in range(3): rows.append(dict(generation=generation,connected=1,width=width,height=height,frame_counter_available=1,new_frames=123*(generation-1)+1+61*i,elapsed_s=generation*4+i))
                top=color*(width//2)+b'\x00\xff\x00'*(width//2)
                bottom=b'\x00\x00\xff'*(width//2)+b'\xff\xff\x00'*(width//2)
                path=image.with_name(f'image-gen{generation}.ppm')
                path.write_bytes(f'P6\n{width} {height}\n255\n'.encode()+top*(height//2)+bottom*(height//2))
            rows.append(dict(rows[-1],connected=0))
            def write():
                with csv_path.open('w',newline='') as stream:
                    writer=csv.DictWriter(stream,fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
            write()
            with contextlib.redirect_stdout(io.StringIO()): validate(csv_path,image)
            original=rows[4]['height']; rows[4]['height']=721; write()
            with self.assertRaises(ValueError): validate(csv_path,image)
            rows[4]['height']=original
            original=rows[7]['new_frames']; rows[7]['new_frames']=rows[6]['new_frames']; rows[8]['new_frames']=rows[6]['new_frames']; write()
            with self.assertRaises(ValueError): validate(csv_path,image)
            rows[7]['new_frames']=original; rows[8]['new_frames']=rows[6]['new_frames']+122; write()
            third=image.with_name('image-gen3.ppm'); data=third.read_bytes()
            header_size=data.index(b'255\n')+4; changed=bytearray(data)
            offset=header_size+(1080//4*1920+1920//4)*3; changed[offset:offset+3]=b'\xff\x00\x00'
            third.write_bytes(changed)
            with self.assertRaises(ValueError): validate(csv_path,image)
            third.write_bytes(data[:-1])
            with self.assertRaises(ValueError): validate(csv_path,image)


if __name__=='__main__': unittest.main()
