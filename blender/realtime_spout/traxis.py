"""TraxisHubTrack JSON as observed on this installation; bounded latest sample.

Unreal-compatible preset: centimeters/degrees, X forward, Y right, Z up.
Lens distortion fields are retained; no undocumented warp model is guessed.
"""
import json
import math
import socket
import threading

FIELDS = ('x','y','z','pan','tilt','roll','effectiveFov','sensorSizeX','sensorSizeY')


def parse(data):
    value = json.loads(data)
    if not isinstance(value, dict):
        raise ValueError('Tracking packet must be an object')
    for field in FIELDS:
        if isinstance(value.get(field), bool) or not isinstance(value.get(field), (int, float)) or not math.isfinite(value[field]):
            raise ValueError(f'Missing or non-finite {field}')
    if not 1 < value['effectiveFov'] < 179 or value['sensorSizeX'] <= 0 or value['sensorSizeY'] <= 0:
        raise ValueError('Invalid calibrated lens field of view or sensor size')
    return value


def camera_matrix(packet, scale=0.01):
    # Unreal FRotator basis, converted from left-handed world to Blender's
    # right-handed world. Blender camera looks along local -Z with local +Y up.
    yaw,pitch,roll = (math.radians(packet[k]) for k in ('pan','tilt','roll'))
    cy,sy,cp,sp,cr,sr = math.cos(yaw),math.sin(yaw),math.cos(pitch),math.sin(pitch),math.cos(roll),math.sin(roll)
    forward=(cp*cy,cp*sy,sp)
    right=(sr*sp*cy-cr*sy,sr*sp*sy+cr*cy,-sr*cp)
    up=(-(cr*sp*cy+sr*sy),cy*sr-cr*sp*sy,cr*cp)
    right=(right[0],-right[1],right[2]); up=(up[0],-up[1],up[2]); backward=(-forward[0],forward[1],-forward[2])
    location=(packet['x']*scale,-packet['y']*scale,packet['z']*scale)
    return tuple(tuple((right[row],up[row],backward[row],location[row])) for row in range(3))+((0,0,0,1),)


class Receiver:
    def __init__(self, port, clock):
        self.socket=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.socket.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
        try:
            self.socket.bind(('0.0.0.0',port))
        except Exception:
            self.socket.close()
            raise
        self.socket.settimeout(.1)
        self.clock=clock
        self.lock=threading.Lock()
        self.latest=None
        self.accepted=self.rejected=0
        self.stop=threading.Event()
        self.thread=threading.Thread(target=self.run,name='TraxisHubTrack',daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop.is_set():
            try:
                data,peer=self.socket.recvfrom(65535)
                packet=parse(data)
                received=self.clock()
                with self.lock:
                    self.accepted+=1
                    self.latest=(packet,received,self.accepted,peer)
            except socket.timeout:
                pass
            except (ValueError,UnicodeDecodeError):
                self.rejected+=1
            except OSError:
                if not self.stop.is_set():
                    self.rejected+=1
                break

    def sample(self):
        with self.lock:
            return self.latest

    def close(self):
        self.stop.set()
        self.thread.join(.3)
        self.socket.close()
