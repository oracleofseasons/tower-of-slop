#!/usr/bin/env python3
"""List every GM built-in the game's GML uses, cross-referenced with OpenGMK.

usage: inventory.py <extracted/project> <OpenGMK/gm8emulator/src> <out.md> <out.json>
"""
import collections
import glob
import json
import os
import re
import sys

KEYWORDS = set('''if then else while do until for repeat switch case default break continue
exit return with var globalvar begin end and or not xor div mod self other all noone global
true false'''.split())


def gml_sources(proj):
    """Yield (label, code) for every piece of GML in the project."""
    for f in sorted(glob.glob(f'{proj}/scripts/*.gml')):
        yield 'script:' + os.path.basename(f)[:-4], open(f).read()
    for f in sorted(glob.glob(f'{proj}/objects/*/*.gml')):
        yield 'object:' + os.path.relpath(f, f'{proj}/objects')[:-4], open(f).read()
    for f in sorted(glob.glob(f'{proj}/rooms/*.json')):
        r = json.load(open(f))
        if r['creation_code'].strip():
            yield f'room:{r["name"]}', r['creation_code']
        for inst in r['instances']:
            if inst['creation_code'].strip():
                yield f'room:{r["name"]}#inst{inst["id"]}', inst['creation_code']


def strip(code):
    """Remove comments and string literals (GM7 strings have no escapes)."""
    out, i, n = [], 0, len(code)
    while i < n:
        c = code[i]
        if code.startswith('//', i):
            j = code.find('\n', i)
            i = n if j < 0 else j
        elif code.startswith('/*', i):
            j = code.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c in '"\'':
            j = code.find(c, i + 1)
            i = n if j < 0 else j + 1
            out.append(' "" ')
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def load_opengmk(src):
    m = open(f'{src}/gml/mappings.rs').read()
    consts = dict(re.findall(r'pub const (\w+): f64 = ([^;]+);', m))
    ivars = set(re.findall(r'\("(\w+)", InstanceVariable::\w+\)', m))
    funcs = {name: (kind, fn) for name, kind, fn in
             re.findall(r'"(\w+)" => Function::(\w+)\(Game::(\w+)\)', m)}
    # locate implementations
    impl = {}
    for f in glob.glob(f'{src}/**/*.rs', recursive=True):
        text = open(f).read()
        for mm in re.finditer(r'\n    pub fn (\w+)\(', text):
            line = text.count('\n', 0, mm.start()) + 2
            body = text[mm.end():text.find('\n    }', mm.end())]
            impl.setdefault(mm.group(1), (os.path.relpath(f, src), line, 'unimplemented!' in body))
    return {k.lower(): v for k, v in consts.items()}, ivars, funcs, impl


def main():
    proj, gmk, out_md, out_json = sys.argv[1:5]
    consts, ivars, funcs, impl = load_opengmk(gmk)
    game = json.load(open(f'{proj}/game.json'))
    assets = {}
    for kind in ('sprites', 'backgrounds', 'sounds', 'objects', 'rooms', 'fonts'):
        for p in glob.glob(f'{proj}/{kind}/*'):
            assets[os.path.basename(p).split('.')[0]] = kind
    scripts = {os.path.basename(f)[:-4] for f in glob.glob(f'{proj}/scripts/*.gml')}
    user_consts = {c['name'] for c in game['constants']}

    calls = collections.defaultdict(collections.Counter)
    words = collections.defaultdict(collections.Counter)
    kw = collections.Counter()
    lines = 0
    for label, code in gml_sources(proj):
        lines += code.count('\n') + 1
        s = strip(code)
        for mm in re.finditer(r'\b([A-Za-z_]\w*)\b(\s*\()?', s):
            name, is_call = mm.group(1), bool(mm.group(2))
            if mm.start() > 0 and s[mm.start() - 1] == '.':
                is_call and calls[name].update([label])
                if not is_call:
                    words['.' + name][label] += 1
                continue
            if name in KEYWORDS:
                kw[name] += 1
            elif is_call:
                calls[name][label] += 1
            else:
                words[name][label] += 1

    report = {'gml_pieces': 0, 'gml_lines': lines, 'keywords': dict(kw.most_common()),
              'functions': [], 'unknown_functions': [], 'script_calls': {},
              'builtin_variables': {}, 'constants': {}, 'asset_refs': collections.Counter(),
              'user_variables': []}
    report['gml_pieces'] = sum(1 for _ in gml_sources(proj))

    for name, where in sorted(calls.items(), key=lambda kv: -sum(kv[1].values())):
        n = sum(where.values())
        if name in scripts:
            report['script_calls'][name] = n
        elif name in funcs:
            kind, fn = funcs[name]
            f, line, unimpl = impl.get(fn, ('?', 0, False))
            report['functions'].append({'name': name, 'uses': n, 'files': len(where),
                                        'opengmk_kind': kind, 'opengmk_fn': fn,
                                        'opengmk_at': f'{f}:{line}', 'opengmk_unimplemented': unimpl})
        else:
            report['unknown_functions'].append({'name': name, 'uses': n, 'where': sorted(where)[:5]})

    for name, where in sorted(words.items(), key=lambda kv: -sum(kv[1].values())):
        n = sum(where.values())
        bare = name.lstrip('.')
        if bare in ivars:
            report['builtin_variables'][bare] = report['builtin_variables'].get(bare, 0) + n
        elif bare.lower() in consts or bare in user_consts:
            report['constants'][bare] = n
        elif bare in assets:
            report['asset_refs'][assets[bare]] += n
        elif not name.startswith('.'):
            report['user_variables'].append(bare)
    report['asset_refs'] = dict(report['asset_refs'])

    json.dump(report, open(out_json, 'w'), indent=1)

    F = report['functions']
    md = [f'# GM built-ins used by Tower of Heaven',
          '',
          f'Generated by `tools/gm7extract/inventory.py` from {report["gml_pieces"]} pieces of GML '
          f'({lines} lines). OpenGMK locations are relative to `tools/OpenGMK/gm8emulator/src`.',
          '',
          f'- Built-in functions: **{len(F)}** ({sum(f["uses"] for f in F)} call sites)',
          f'- Built-in variables: **{len(report["builtin_variables"])}**',
          f'- Constants: **{len(report["constants"])}**',
          f'- User scripts called: **{len(report["script_calls"])}**',
          f'- Unknown calls (not a script, not in OpenGMK): **{len(report["unknown_functions"])}**',
          '', '## Functions', '',
          '| Function | Uses | Files | OpenGMK | Notes |', '| --- | --- | --- | --- | --- |']
    for f in F:
        note = 'unimplemented in OpenGMK' if f['opengmk_unimplemented'] else ''
        md.append(f'| `{f["name"]}` | {f["uses"]} | {f["files"]} | `{f["opengmk_at"]}` | {note} |')
    md += ['', '## Built-in variables', '', '| Variable | Uses |', '| --- | --- |']
    md += [f'| `{k}` | {v} |' for k, v in sorted(report['builtin_variables'].items(), key=lambda kv: -kv[1])]
    md += ['', '## Constants', '', ', '.join(f'`{k}` ({v})' for k, v in report['constants'].items())]
    md += ['', '## Unknown calls', '']
    md += [f'- `{u["name"]}` ({u["uses"]}): {", ".join(u["where"])}' for u in report['unknown_functions']] or ['None.']
    md += ['', '## GML keywords', '', ', '.join(f'`{k}` ({v})' for k, v in kw.most_common())]
    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    open(out_md, 'w').write('\n'.join(md) + '\n')
    print(f'{len(F)} functions, {len(report["builtin_variables"])} builtin vars, '
          f'{len(report["unknown_functions"])} unknown calls, {lines} lines')


if __name__ == '__main__':
    main()
