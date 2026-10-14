"""QPC paired-publication timing; simulated 50 Hz selector is not an SDI test."""
import argparse
from collections import deque
import json
import ctypes
from pathlib import Path
import struct
import time

parser=argparse.ArgumentParser()
parser.add_argument('--sender',default='RealtimeStudio')
parser.add_argument('--seconds',type=float,default=20)
parser.add_argument('--output',type=Path,default=Path('output/blender-timing.json'))
args=parser.parse_args()
kernel=ctypes.WinDLL('kernel32',use_last_error=True)
kernel.OpenFileMappingW.argtypes=[ctypes.c_uint,ctypes.c_int,ctypes.c_wchar_p]
kernel.OpenFileMappingW.restype=ctypes.c_void_p
kernel.MapViewOfFile.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,ctypes.c_size_t]
kernel.MapViewOfFile.restype=ctypes.c_void_p
kernel.UnmapViewOfFile.argtypes=[ctypes.c_void_p]
kernel.CloseHandle.argtypes=[ctypes.c_void_p]
deadline=time.perf_counter()+10
mapping=None
while not mapping and time.perf_counter()<deadline:
    mapping=kernel.OpenFileMappingW(4,False,'Local\\RealtimeCompositor.Meta.v2.'+args.sender)
    if not mapping: time.sleep(.05)
if not mapping: raise SystemExit('Sender v2 metadata mapping not available')
view=kernel.MapViewOfFile(mapping,4,0,0,64)
if not view:
    kernel.CloseHandle(mapping)
    raise SystemExit('Cannot read sender metadata')
frames=deque(maxlen=4); rows=[]; selected=[]; previous=None
start=time.perf_counter_ns(); next_output=start; end=start+int(args.seconds*1e9)
while time.perf_counter_ns()<end:
    data=ctypes.string_at(view,64)
    guard,magic,session,sequence,render,tracking,scheduled,pose,width,height=struct.unpack('<IIQQQQQQII',data)
    now=time.perf_counter_ns()
    if not guard%2 and data[:4]==ctypes.string_at(view,4) and magic==0x52435347 and sequence and (session,sequence)!=previous:
        if previous and session!=previous[0]: frames.clear()
        row=dict(observed_ns=now,session=session,sequence=sequence,render_ns=render,
                 tracking_ns=tracking,scheduled_ns=scheduled,pose_ns=pose)
        rows.append(row); frames.append(row); previous=(session,sequence)
    if now>=next_output:
        due=[r for r in frames if r['scheduled_ns'] and r['scheduled_ns']+20000000<=next_output and next_output-r['render_ns']<500000000]
        if due: selected.append(dict(output_ns=next_output,**due[-1]))
        next_output+=20000000
    time.sleep(.001)
kernel.UnmapViewOfFile(view); kernel.CloseHandle(mapping)
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps({'frames':rows,'simulated_selection':selected},indent=2))
if len(rows)<2: raise SystemExit('No progressing v2 frame metadata received')
for field in ('render_ns','scheduled_ns','pose_ns'):
    values=sorted((b[field]-a[field])/1e6 for a,b in zip(rows,rows[1:]) if a['session']==b['session'])
    print(field,'interval ms min/median/p95/max:',*[round(values[int((len(values)-1)*q)],3) for q in (0,.5,.95,1)])
pairs=[(a,b) for a,b in zip(selected,selected[1:]) if a['session']==b['session']]
print('Simulated 50 Hz selection:',len(selected),'samples,',sum(a['scheduled_ns']==b['scheduled_ns'] for a,b in pairs),'repeats,',sum(max(0,(b['scheduled_ns']-a['scheduled_ns'])//20000000-1) for a,b in pairs),'skipped ticks')
