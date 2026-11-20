#!/bin/sh
# Play and record your inputs for tools/trace/compare.sh.
#   tools/trace/record.sh [--original] OUT.keys [SEED]
# Starts a fresh game (empty save folder, same seed as the replay) in an instrumented build:
# the port by default, or with --original the original exe under Wine.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ORIG=0; if [ "$1" = "--original" ]; then ORIG=1; shift; fi
OUT="$(realpath -m "$1")"; SEED="${2:-1}"
if [ "$ORIG" = 1 ]; then
    REC="$ROOT/build-trace/rec-orig"
    rm -rf "$REC/game"
    python3 -I "$ROOT/tools/trace/instrument.py" --exe "$ROOT/original/windows/Tower of Heaven.exe" \
        --project "$ROOT/extracted/project" --out "$REC" --seed "$SEED" --record >/dev/null
    echo "starting the original under Wine: choose New Game and play; quit when done"
    (cd "$REC/game" && wine "Tower of Heaven.exe" >wine.log 2>&1 || true)
    python3 -I "$ROOT/tools/trace/rec2keys.py" "$REC/game/keys_rec.txt" "$OUT"
    exit 0
fi
REC="$ROOT/build-trace/rec"
echo "preparing the instrumented build (about a minute the first time, seconds after that)..."
python3 -I "$ROOT/tools/trace/instrument.py" --exe "$ROOT/original/windows/Tower of Heaven.exe" \
    --project "$ROOT/extracted/project" --out "$REC" --seed "$SEED" --keys ""
cmake -S "$ROOT" -B "$ROOT/build-trace" -DPROJECT_DIR="$REC/project" >/dev/null
cmake --build "$ROOT/build-trace" -j 2>&1 | grep -E "error|Building CXX|Packing" || true
rm -rf "$REC/save"
mkdir -p "$REC/save"
echo "1" > "$REC/save/quality.txt"
echo "starting the game: choose New Game and play; quit when done"
"$ROOT/build-trace/toh" --pak "$ROOT/build-trace/toh.pak" --save-dir "$REC/save" --record "$OUT"
