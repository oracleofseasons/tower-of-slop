#!/bin/sh
# Build the Switch 2 release: dist/tower-of-slop-switch2.zip, laid out as switch/toh/ for the SD card root.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$ROOT/tools/switch/build.sh" >/dev/null
mkdir -p "$ROOT/dist"
python3 -I - "$ROOT" <<'PY'
import sys, zipfile
root = sys.argv[1]
out = f'{root}/dist/tower-of-slop-switch2.zip'
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    z.write(f'{root}/platform/switch/toh.nro', 'switch/toh/toh.nro')
    z.write(f'{root}/tools/release/settings-switch.ini', 'switch/toh/settings.ini')
    for f in ('PLAYING.txt', 'LICENSE', 'THIRD_PARTY.md'):
        src = f'{root}/tools/release/{f}' if f == 'PLAYING.txt' else f'{root}/{f}'
        z.write(src, f'switch/toh/{f}')
print(out)
PY
