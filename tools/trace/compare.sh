#!/bin/sh
# Run the same scripted input through the original (under Wine) and the port, and diff
# their per-frame traces (frame, room, player x/y, instance count, ordered instance list).
#
#   tools/trace/compare.sh KEYS [SEED] [SECONDS]
#     KEYS     "frame:vk[:frames],..."  e.g. "330:88,1350:39:25,1390:90:10", or @file from record.sh
#     SEED     random seed for both (default 1)
#     SECONDS  how long to run the original (default: long enough for the last key + 20s)
#
# The original's window opens while it runs: don't type into it.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KEYS="$1"; SEED="${2:-1}"
case "$KEYS" in @*) KEYTEXT="$(cat "${KEYS#@}")" ;; *) KEYTEXT="$KEYS" ;; esac
LAST=$(printf "%s" "$KEYTEXT" | tr ',' '\n' | cut -d: -f1 | sort -n | tail -1)
SECS="${3:-$(( ${LAST:-0} / 30 + 20 ))}"
OUT="$ROOT/build-trace/run"
rm -rf "$OUT/game" "$OUT/portsave"
echo "building the instrumented original and port..."
python3 -I "$ROOT/tools/trace/instrument.py" --exe "$ROOT/original/windows/Tower of Heaven.exe" \
    --project "$ROOT/extracted/project" --out "$OUT" --seed "$SEED" --keys "$KEYTEXT"
cmake -S "$ROOT" -B "$ROOT/build-trace" -DPROJECT_DIR="$OUT/project" >/dev/null
cmake --build "$ROOT/build-trace" -j >/dev/null

echo "running the original under Wine for ${SECS}s..."
(cd "$OUT/game" && timeout "$SECS" wine "Tower of Heaven.exe" >wine.log 2>&1 || true)
wineserver -k 2>/dev/null || true
tr -d '\r' < "$OUT/game/trace.txt" > "$OUT/orig.txt"
FRAMES=$(wc -l < "$OUT/orig.txt")

mkdir -p "$OUT/portsave"
"$ROOT/build-trace/toh" --pak "$ROOT/build-trace/toh.pak" --save-dir "$OUT/portsave" --headless "$((FRAMES + 600))" \
    2>"$OUT/port.log" || true
tr -d '\r' < "$OUT/portsave/trace.txt" | head -n "$FRAMES" > "$OUT/port.txt"

if cmp -s "$OUT/orig.txt" "$OUT/port.txt"; then
    echo "MATCH: all $FRAMES frames identical"
else
    N=$(paste -d'|' "$OUT/orig.txt" "$OUT/port.txt" | awk -F'|' '$1!=$2' | wc -l)
    echo "MISMATCH: $N of $FRAMES frames differ; first differences:"
    paste -d'|' "$OUT/orig.txt" "$OUT/port.txt" | awk -F'|' '$1!=$2{print "  orig: "$1"\n  port: "$2}' | head -6
    exit 1
fi
