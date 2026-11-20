#!/usr/bin/env python3
"""Build matching instrumented versions of the original exe and of the port.

The same GML is injected into both:
  * create of obj_scale / obj_scale2: random_set_seed(SEED) once, at game start
  * begin step: scripted keyboard_key_press / keyboard_key_release on frame numbers
  * end step: append "frame room x y instances" to trace.txt, then advance the frame
Exactly one host logs per frame: obj_scale if it exists, otherwise obj_scale2
(obj_scale lives in the levels, obj_scale2 on the title and ending screens).

usage: instrument.py --exe ORIGINAL.exe --project extracted/project --out DIR
                     [--seed N] [--keys "frame:vk[:frames],..."]
writes DIR/game/  (instrumented exe + its DLL, quality.txt = 1)
       DIR/project/  (instrumented copy of the extracted project, for the port)
"""
import argparse
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'gm7extract'))
import gm7patch  # noqa: E402

HOSTS = ['obj_scale', 'obj_scale2']


def key_schedule(spec):
    """'330:88,400:39:20' -> {frame: [GML statements]}"""
    out = {}
    for item in filter(None, (spec or '').split(',')):
        parts = item.split(':')
        f, key = int(parts[0]), int(parts[1])
        dur = int(parts[2]) if len(parts) > 2 else 1
        out.setdefault(f, []).append(f'keyboard_key_press({key});')
        out.setdefault(f + dur, []).append(f'keyboard_key_release({key});')
    return out


def gml(seed, keys):
    guard = '(object_index = obj_scale or !instance_exists(obj_scale))'
    create = (f'if !variable_global_exists("__trace_f") {{ global.__trace_f = 0; random_set_seed({seed}); '
              f'__tf = file_text_open_write("trace.txt"); file_text_close(__tf); }}\n')
    lines = [f'if global.__trace_f = {f} {{ {" ".join(sts)} }}' for f, sts in sorted(keys.items())]
    begin = f'if {guard} {{\n' + '\n'.join(lines) + '\n}\n'
    end = (f'\nif {guard} {{\n'
           '__tf = file_text_open_append("trace.txt");\n'
           '__tl = string(global.__trace_f) + " " + string(room);\n'
           'if instance_exists(obj_character) { __tl = __tl + " " + string_format(obj_character.x, 0, 4) + " " '
           '+ string_format(obj_character.y, 0, 4); } else { __tl = __tl + " - -"; }\n'
           'global.__tl = __tl + " " + string(instance_count) + " [";\n'
           'with (all) { global.__tl = global.__tl + string(object_index) + ","; }\n'
           '__tl = global.__tl + "]";\n'
           'file_text_write_string(__tf, __tl); file_text_writeln(__tf); file_text_close(__tf);\n'
           'global.__trace_f += 1;\n}\n')
    return create, begin, end


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--project', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--keys', default='', help='"frame:vk[:frames],..." or @file')
    a = ap.parse_args()
    if a.keys.startswith('@'):
        a.keys = open(a.keys[1:]).read().strip()

    create, begin, end = gml(a.seed, key_schedule(a.keys))
    edits = {'create_0.gml': lambda s: create + s, 'begin_step.gml': lambda s: begin + s,
             'end_step.gml': lambda s: s + end}

    # port: patched copy of the project
    # Only rewrite files whose contents change, so the port isn't recompiled needlessly.
    pdst = os.path.join(a.out, 'project')
    wanted = {}
    for root, _, files in os.walk(a.project):
        for fn in files:
            src = os.path.join(root, fn)
            wanted[os.path.relpath(src, a.project)] = open(src, 'rb').read()
    patches = []
    for host in HOSTS:
        for fname, fn in edits.items():
            rel = os.path.join('objects', host, fname)
            old = wanted[rel].decode('latin-1')
            new = fn(old)
            wanted[rel] = new.encode('latin-1')
            patches.append((old, new))
    for rel, data in wanted.items():
        path = os.path.join(pdst, rel)
        if os.path.exists(path) and open(path, 'rb').read() == data:
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'wb').write(data)

    # original: splice the same code into the exe
    d = open(a.exe, 'rb').read()
    plain = gm7patch.decrypted(d)
    for old, new in patches:
        ob, nb = old.encode('latin-1'), new.encode('latin-1')
        needle = struct.pack('<I', len(ob)) + ob
        i = plain.find(needle)
        if i < 0:
            raise SystemExit(f'code not found in exe: {old[:50]!r}')
        # begin step code is identical in both hosts: each pass patches the next unpatched copy
        plain = plain[:i] + struct.pack('<I', len(nb)) + nb + plain[i + len(needle):]
    gdst = os.path.join(a.out, 'game')
    os.makedirs(gdst, exist_ok=True)
    src_dir = os.path.dirname(os.path.abspath(a.exe))
    for f in ('supersound.dll', 'readme.txt'):
        if os.path.exists(os.path.join(src_dir, f)):
            shutil.copy(os.path.join(src_dir, f), gdst)
    open(os.path.join(gdst, 'quality.txt'), 'w').write('1')
    open(os.path.join(gdst, 'Tower of Heaven.exe'), 'wb').write(gm7patch.rebuild(d, plain))
    print(f'instrumented build in {a.out}: game/ (original) and project/ (port), seed {a.seed}, '
          f'{len(key_schedule(a.keys))} key frames')


if __name__ == '__main__':
    main()
