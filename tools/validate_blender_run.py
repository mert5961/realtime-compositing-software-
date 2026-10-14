"""Validate ordered frame delivery and exact diagnostic pixels for every phase."""
import csv
import sys
from pathlib import Path


def validate(csv_path, snapshot):
    with Path(csv_path).open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError('No samples')
    expected = [(1920, 1080, (255, 0, 0)), (1280, 720, (0, 255, 255)), (1920, 1080, (255, 0, 255))]
    for generation, (width, height, first_color) in enumerate(expected, 1):
        phase = [r for r in rows if int(r['generation']) == generation and r['connected'] == '1']
        if len(phase) < 3 or any((int(r['width']), int(r['height'])) != (width, height) for r in phase):
            raise ValueError(f'Generation {generation}: missing or incorrect dimensions')
        if any(r['frame_counter_available'] != '1' for r in phase):
            raise ValueError(f'Generation {generation}: missing counter')
        if int(phase[-1]['new_frames'])-int(phase[0]['new_frames']) < 100:
            raise ValueError(f'Generation {generation}: insufficient frame progress')
        if generation > 1:
            previous = [r for r in rows if int(r['generation']) == generation-1 and r['connected'] == '1']
            if float(phase[0]['elapsed_s'])-float(previous[-1]['elapsed_s']) > 5:
                raise ValueError('Reconnect exceeded five seconds')
        image = Path(snapshot).with_name(f'{Path(snapshot).stem}-gen{generation}.ppm')
        with image.open('rb') as stream:
            if stream.readline() != b'P6\n' or tuple(map(int, stream.readline().split())) != (width, height) or stream.readline() != b'255\n':
                raise ValueError(f'Generation {generation}: incorrect PPM header')
            pixels = stream.read()
        if len(pixels) != width*height*3:
            raise ValueError(f'Generation {generation}: truncated image')
        for (x, y), color in zip([(width//4,height//4),(3*width//4,height//4),(width//4,3*height//4),(3*width//4,3*height//4)], [first_color,(0,255,0),(0,0,255),(255,255,0)]):
            offset = (y*width+x)*3
            if any(abs(actual-target)>2 for actual,target in zip(pixels[offset:offset+3],color)):
                raise ValueError(f'Generation {generation}: wrong orientation, channels, or stale image')
    if rows[-1]['connected'] != '0':
        raise ValueError('Sender shutdown not detected')
    print('PASS: every generation delivered frames and correct pixels; resize, reconnect, shutdown')


if __name__ == '__main__':
    validate(*sys.argv[1:])
