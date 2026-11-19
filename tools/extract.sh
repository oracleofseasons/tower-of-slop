#!/bin/sh
# Rebuild extracted/project from the original GM7 exe.
# Needs: git, cargo, python3. Run from anywhere.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXE="$ROOT/original/windows/Tower of Heaven.exe"
T="$ROOT/tools"
GM_READER_REV=73611a4df929ad144de399828b90eb8fed40ef09

[ -f "$EXE" ] || { echo "missing $EXE (put your copy of the game in original/windows/)"; exit 1; }

# gm_reader (elipsitz/gm_reader); 2020 code, so two deps are pinned to build today
if [ ! -d "$T/gm_reader" ]; then
    git clone https://github.com/elipsitz/gm_reader.git "$T/gm_reader"
    git -C "$T/gm_reader" checkout -q "$GM_READER_REV"
    (cd "$T/gm_reader" && cargo update -p rmp --precise 0.8.9 && cargo update -p image --precise 0.23.2)
fi
[ -x "$T/gm_reader/target/release/gm_reader" ] || (cd "$T/gm_reader" && cargo build --release)
[ -x "$T/.venv/bin/python" ] || (python3 -m venv "$T/.venv" && "$T/.venv/bin/pip" install -q msgpack pillow)

mkdir -p "$ROOT/extracted"
"$T/gm_reader/target/release/gm_reader" "$EXE" "$ROOT/extracted/game.msgpack"
rm -rf "$ROOT/extracted/project"
"$T/.venv/bin/python" -I "$T/gm7extract/dump.py" "$T/gm_reader/src/game.rs" \
    "$ROOT/extracted/game.msgpack" "$ROOT/extracted/project"

# Built-in usage report (needs tools/OpenGMK for the cross-reference)
if [ -d "$T/OpenGMK" ]; then
    python3 -I "$T/gm7extract/inventory.py" "$ROOT/extracted/project" \
        "$T/OpenGMK/gm8emulator/src" "$ROOT/docs/inventory.md" "$ROOT/extracted/inventory.json"
fi
