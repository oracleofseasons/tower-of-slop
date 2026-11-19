#!/usr/bin/env python3
"""Pack extracted/project into a single binary asset file for the runtime.

Format (little-endian): magic 'TOHPAK01', then sections in order:
  sprites, backgrounds, fonts, sounds, objects, rooms, includes, room_order
Each section: u32 count, then per slot: u8 present, payload.
Strings are u32 length + bytes. Images are raw RGBA8 (fonts: 8-bit gray).
Music (.ogg includes) is decoded to 16-bit PCM WAV with ffmpeg.

usage: pack.py <extracted/project> <out.pak>      (needs Pillow, ffmpeg)
"""
import glob
import json
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image


class W:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v): self.b += struct.pack('<B', v)
    def u32(self, v): self.b += struct.pack('<I', v)
    def i32(self, v): self.b += struct.pack('<i', v)
    def f64(self, v): self.b += struct.pack('<d', v)
    def bool(self, v): self.u8(1 if v else 0)

    def str(self, s):
        e = s.encode('latin-1', 'replace')
        self.u32(len(e))
        self.b += e

    def blob(self, d):
        self.u32(len(d))
        self.b += d


def load_dir(pattern, key='id'):
    items = {}
    for f in glob.glob(pattern):
        d = json.load(open(f))
        d['_path'] = f
        items[d[key]] = d
    return items


def slots(w, items, fn):
    n = max(items) + 1 if items else 0
    w.u32(n)
    for i in range(n):
        if i in items:
            w.u8(1)
            fn(items[i])
        else:
            w.u8(0)


def rgba(path):
    im = Image.open(path).convert('RGBA')
    return im.size, im.tobytes()


def main():
    proj, out = sys.argv[1], sys.argv[2]
    w = W()
    w.b += b'TOHPAK01'

    def sprite(s):
        d = os.path.dirname(s['_path'])
        frames = [rgba(f'{d}/{i}.png') for i in range(s['frame_count'])]
        w.str(s['name'])
        (fw, fh) = frames[0][0] if frames else (0, 0)
        w.u32(fw); w.u32(fh)
        w.i32(s['origin'][0]); w.i32(s['origin'][1])
        w.u32(len(frames))
        for (_, px) in frames:
            w.b += px
        w.u32(len(s['masks']))
        for i, m in enumerate(s['masks']):
            w.u32(m['size'][0]); w.u32(m['size'][1])
            w.i32(m['left']); w.i32(m['right']); w.i32(m['top']); w.i32(m['bottom'])
            w.b += open(f'{d}/mask{i}.bin', 'rb').read()
    slots(w, load_dir(f'{proj}/sprites/*/sprite.json'), sprite)

    def background(b):
        (bw, bh), px = rgba(b['_path'][:-5] + '.png') if os.path.exists(b['_path'][:-5] + '.png') else ((0, 0), b'')
        w.str(b['name']); w.u32(bw); w.u32(bh); w.b += px
    slots(w, load_dir(f'{proj}/backgrounds/*.json'), background)

    def font(f):
        im = Image.open(f['_path'][:-5] + '.png').convert('L')
        w.str(f['name']); w.u32(f['size']); w.bool(f['bold']); w.bool(f['italic'])
        w.u32(f['range_start']); w.u32(f['range_end'])
        w.u32(im.size[0]); w.u32(im.size[1]); w.b += im.tobytes()
        glyphs = f['atlas']['glyphs']
        w.u32(len(glyphs))
        for g in glyphs:
            w.i32(g['pos'][0]); w.i32(g['pos'][1]); w.i32(g['size'][0]); w.i32(g['size'][1])
            w.i32(g['horizontal_advance']); w.i32(g['kerning'])
    slots(w, load_dir(f'{proj}/fonts/*.json'), font)

    def sound(s):
        p = f'{proj}/sounds/{s["name"]}{s["filetype"]}'
        data = open(p, 'rb').read() if os.path.exists(p) else b''
        w.str(s['name']); w.u32(s['kind']); w.str(s['filetype'])
        w.f64(s['volume']); w.f64(s['pan']); w.bool(s['preload']); w.blob(data)
    slots(w, load_dir(f'{proj}/sounds/*.json'), sound)

    def obj(o):
        w.str(o['name']); w.i32(o['sprite']); w.bool(o['solid']); w.bool(o['visible'])
        w.i32(o['depth']); w.bool(o['persistent']); w.i32(o['parent']); w.i32(o['mask'])
    slots(w, load_dir(f'{proj}/objects/*/object.json'), obj)

    def room(r):
        w.str(r['name']); w.str(r['caption']); w.u32(r['width']); w.u32(r['height'])
        w.u32(r['speed']); w.bool(r['persistent']); w.u32(r['clear_color']); w.bool(r['clear'])
        w.bool(r['enable_views'])
        w.u32(len(r['backgrounds']))
        for b in r['backgrounds']:
            w.bool(b['visible']); w.bool(b['foreground']); w.i32(b['background'])
            w.i32(b['x']); w.i32(b['y']); w.bool(b['tile_h']); w.bool(b['tile_v'])
            w.i32(b['h_speed']); w.i32(b['v_speed']); w.bool(b['stretch'])
        w.u32(len(r['views']))
        for v in r['views']:
            w.bool(v['visible'])
            for k in ('view_x', 'view_y', 'view_width', 'view_height', 'port_x', 'port_y',
                      'port_width', 'port_height', 'h_border', 'v_border', 'h_speed', 'v_speed',
                      'target_object'):
                w.i32(v[k])
        w.u32(len(r['instances']))
        for i in r['instances']:
            w.i32(i['x']); w.i32(i['y']); w.i32(i['object']); w.i32(i['id'])
        w.u32(len(r['tiles']))
        for t in r['tiles']:
            for k in ('x', 'y', 'background', 'tile_x', 'tile_y', 'width', 'height', 'depth', 'id'):
                w.i32(t[k])
    slots(w, load_dir(f'{proj}/rooms/*.json'), room)

    incs = sorted(p for p in glob.glob(f'{proj}/includes/*') if not p.endswith('.dll'))
    w.u32(len(incs))
    with tempfile.TemporaryDirectory() as tmp:
        for p in incs:
            name = os.path.basename(p)
            data = open(p, 'rb').read()
            if name.lower().endswith('.ogg'):
                wav = os.path.join(tmp, 'out.wav')
                subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', p, '-acodec', 'pcm_s16le', wav],
                               check=True)
                data = open(wav, 'rb').read()
            w.str(name); w.blob(data)

    game = json.load(open(f'{proj}/game.json'))
    room_ids = {r['name']: r['id'] for r in load_dir(f'{proj}/rooms/*.json').values()}
    w.u32(len(game['room_order']))
    for name in game['room_order']:
        w.i32(room_ids[name])

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'wb').write(w.b)
    print(f'{out}: {len(w.b) / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
