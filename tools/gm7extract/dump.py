#!/usr/bin/env python3
"""Turn gm_reader's MessagePack output into plain project files.

rmp-serde 0.14 writes structs as positional arrays, so field order is taken
from gm_reader's src/game.rs at run time.

usage: dump.py <game.rs> <game.msgpack> <out_dir>
"""
import json
import os
import re
import sys

import msgpack
from PIL import Image

EVENT_NAMES = {0: 'create', 1: 'destroy', 2: 'alarm', 3: 'step', 4: 'collision',
               5: 'keyboard', 6: 'mouse', 7: 'other', 8: 'draw', 9: 'keypress',
               10: 'keyrelease', 11: 'trigger'}
STEP_NAMES = {0: 'step', 1: 'begin_step', 2: 'end_step'}


def parse_structs(src):
    structs = {}
    for m in re.finditer(r'pub struct (\w+)\s*\{(.*?)\n\}', src, re.S):
        fields = re.findall(r'^\s*pub (\w+):\s*([^,\n]+),', m.group(2), re.M)
        structs[m.group(1)] = [(n, t.strip()) for n, t in fields]
    return structs


def convert(v, t, S):
    t = t.strip()
    if t in S:
        return {n: convert(x, ft, S) for (n, ft), x in zip(S[t], v)}
    m = re.fullmatch(r'(Vec|Option)<(.+)>', t)
    if m and m.group(2) != 'u8':
        if v is None:
            return None
        if m.group(1) == 'Vec':
            return [convert(x, m.group(2), S) for x in v]
        return convert(v, m.group(2), S)
    return v


def safe(name):
    return re.sub(r'[^A-Za-z0-9_.-]', '_', name)


def save_image(img, path):
    if not img or not img['width']:
        return False
    # color_type's enum encoding is unreliable; infer from bytes per pixel
    mode = 'L' if len(img['data']) == img['width'] * img['height'] else 'RGBA'
    Image.frombytes(mode, (img['width'], img['height']), bytes(img['data'])).save(path)
    return True


def event_label(ev, names):
    t, n = ev['event_type'], ev['event_number']
    base = EVENT_NAMES.get(t, f'ev{t}')
    if t == 3:
        return STEP_NAMES.get(n, f'step{n}')
    if t == 4:
        return f'collision_{names["objects"].get(n, n)}'
    return f'{base}_{n}'


def write(path, data, mode='w'):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as f:
        f.write(data)


def main():
    rs, mp, out = sys.argv[1:4]
    S = parse_structs(open(rs).read())
    raw = msgpack.unpackb(open(mp, 'rb').read(), raw=False, strict_map_key=False)
    g = convert(raw, 'Game', S)

    names = {k: {a['id']: a['name'] for a in g[k] if a} for k in
             ('sprites', 'sounds', 'backgrounds', 'paths', 'scripts', 'fonts',
              'timelines', 'objects', 'rooms')}

    for s in g['sprites']:
        d = f'{out}/sprites/{safe(s["name"])}'
        os.makedirs(d, exist_ok=True)
        for i, fr in enumerate(s['frames']):
            save_image(fr, f'{d}/{i}.png')
        meta = {k: v for k, v in s.items() if k not in ('frames', 'masks')}
        meta['frame_count'] = len(s['frames'])
        meta['masks'] = [{k: v for k, v in mk.items() if k != 'data'} for mk in s['masks']]
        write(f'{d}/sprite.json', json.dumps(meta, indent=1))
        # precise collision masks, one byte per pixel
        for i, mk in enumerate(s['masks']):
            write(f'{d}/mask{i}.bin', bytes(1 if b else 0 for b in mk['data']), 'wb')

    for b in g['backgrounds']:
        os.makedirs(f'{out}/backgrounds', exist_ok=True)
        save_image(b['image'], f'{out}/backgrounds/{safe(b["name"])}.png')
        write(f'{out}/backgrounds/{safe(b["name"])}.json',
              json.dumps({k: v for k, v in b.items() if k != 'image'}, indent=1))

    for s in g['sounds']:
        ext = s['filetype'] or '.bin'
        if s['data']:
            write(f'{out}/sounds/{safe(s["name"])}{ext}', bytes(s['data']), 'wb')
        write(f'{out}/sounds/{safe(s["name"])}.json',
              json.dumps({k: v for k, v in s.items() if k != 'data'}, indent=1))

    for s in g['scripts']:
        write(f'{out}/scripts/{safe(s["name"])}.gml', s['script'])

    for o in g['objects']:
        d = f'{out}/objects/{safe(o["name"])}'
        meta = {k: v for k, v in o.items() if k != 'events'}
        meta['sprite_name'] = names['sprites'].get(o['sprite'])
        meta['parent_name'] = names['objects'].get(o['parent'])
        meta['events'] = []
        for ev in o['events']:
            label = event_label(ev, names)
            acts = []
            for j, a in enumerate(ev['actions']):
                if a['library_id'] == 1 and a['action_id'] == 603:  # execute code
                    fn = f'{label}.gml' if len(ev['actions']) == 1 else f'{label}_{j}.gml'
                    write(f'{d}/{fn}', a['arguments'][0] if a['arguments'] else '')
                    acts.append({'code_file': fn, 'target': a['target']})
                else:
                    acts.append({k: a[k] for k in ('library_id', 'action_id', 'name', 'code',
                                                   'target', 'relative', 'negate', 'arguments')})
            meta['events'].append({'type': ev['event_type'], 'number': ev['event_number'],
                                   'label': label, 'actions': acts})
        write(f'{d}/object.json', json.dumps(meta, indent=1))

    for r in g['rooms']:
        write(f'{out}/rooms/{safe(r["name"])}.json', json.dumps(r, indent=1))

    for t in g['timelines']:
        write(f'{out}/timelines/{safe(t["name"])}.json', json.dumps(t, indent=1))
    for f in g['fonts']:
        d = f'{out}/fonts'
        os.makedirs(d, exist_ok=True)
        save_image(f['atlas']['image'], f'{d}/{safe(f["name"])}.png')
        f2 = dict(f)
        f2['atlas'] = {'glyphs': f['atlas']['glyphs']}
        write(f'{d}/{safe(f["name"])}.json', json.dumps(f2, indent=1))
    for p in g['paths']:
        write(f'{out}/paths/{safe(p["name"])}.json', json.dumps(p, indent=1))
    for inc in g['includes']:
        write(f'{out}/includes/{safe(inc["name"])}', bytes(inc['data']), 'wb')

    st = dict(g['settings'])
    for k in ('loading_bar_back', 'loading_bar_front', 'loading_background'):
        st[k] = bool(st.get(k))
    game = {k: g[k] for k in ('version', 'debug', 'pro', 'game_id', 'last_instance_id',
                             'last_tile_id', 'constants', 'triggers', 'library_init_scripts')}
    game['settings'] = st
    game['help'] = g['help']
    game['room_order'] = [names['rooms'].get(i, i) for i in g['room_order']]
    game['counts'] = {k: len(v) for k, v in names.items()}
    write(f'{out}/game.json', json.dumps(game, indent=1, default=str))
    print(json.dumps(game['counts']))


if __name__ == '__main__':
    main()
