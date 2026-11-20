#!/bin/sh
# Play the port and record your inputs for tools/trace/compare.sh.
#   tools/trace/record.sh OUT.keys [SEED]
# Starts a fresh game (empty save folder, same seed as the replay) in an instrumented build.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(realpath -m "$1")"; SEED="${2:-1}"
REC="$ROOT/build-trace/rec"
rm -rf "$REC"
python3 -I "$ROOT/tools/trace/instrument.py" --exe "$ROOT/original/windows/Tower of Heaven.exe" \
    --project "$ROOT/extracted/project" --out "$REC" --seed "$SEED" --keys ""
cmake -S "$ROOT" -B "$ROOT/build-trace" -DPROJECT_DIR="$REC/project" >/dev/null
cmake --build "$ROOT/build-trace" -j >/dev/null
mkdir -p "$REC/save"
echo "1" > "$REC/save/quality.txt"
"$ROOT/build-trace/toh" --pak "$ROOT/build-trace/toh.pak" --save-dir "$REC/save" --record "$OUT"
