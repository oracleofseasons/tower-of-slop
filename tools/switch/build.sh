#!/bin/sh
# Build the Switch 2 homebrew (platform/switch/toh.nro) in a devkitPro container with libounce.
#   tools/switch/build.sh [IMAGE]     (default: localhost/ounce-wwhd)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="${1:-localhost/ounce-wwhd}"
engine=$(command -v podman || command -v docker)
"$engine" run --rm -v "$ROOT:/src:Z" -w /src/platform/switch "$IMAGE" \
    sh -c 'export DEVKITPRO=/opt/devkitpro; make -j"$(nproc)"'
ls -la "$ROOT/platform/switch/toh.nro"
