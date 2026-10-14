"""Short exclusive UDP diagnostic capture; does not change the sender."""
import argparse
import json
import socket
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--port', type=int, default=8873)
parser.add_argument('--seconds', type=float, default=3)
parser.add_argument('--output', type=Path, default=Path('output/traxis-capture.json'))
args = parser.parse_args()
packets = []
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
    receiver.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    receiver.bind(('0.0.0.0', args.port))
    receiver.settimeout(0.2)
    end = time.monotonic() + args.seconds
    while time.monotonic() < end and len(packets) < 2000:
        try:
            data, peer = receiver.recvfrom(65535)
            # Python 3.11 monotonic() uses 15.625 ms GetTickCount64 on Windows.
            # Use QPC for inter-packet timing, as the native video path does.
            packets.append({'peer': peer, 'received_ns': time.perf_counter_ns(), 'size': len(data), 'hex': data.hex()})
        except socket.timeout:
            pass
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(packets, indent=2))
print(f'{len(packets)} datagrams captured; lengths: {sorted({p["size"] for p in packets})}')
if packets:
    print('First packet:', packets[0])
