#!/bin/sh
# Build a portable Linux release (dist/tower-of-slop-linux-x86_64.zip) in an Ubuntu 22.04 container, with
# SDL3 built from a pinned release and linked statically (it still loads X11/Wayland/audio at run time).
#   tools/release/linux.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDL_TAG=release-3.4.18
engine=$(command -v podman || command -v docker)
mkdir -p "$ROOT/dist"
"$engine" run --rm -v "$ROOT:/src:Z" docker.io/library/ubuntu:22.04 sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq g++-12 cmake ninja-build git python3 zip zlib1g-dev pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev \
    libwayland-dev wayland-protocols libegl-dev libgl-dev libdecor-0-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev >/dev/null
export CC=gcc-12 CXX=g++-12
git clone -q --depth 1 --branch $SDL_TAG https://github.com/libsdl-org/SDL.git /tmp/SDL
cmake -S /tmp/SDL -B /tmp/sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=/tmp/sdl >/dev/null
cmake --build /tmp/sdl-build >/dev/null && cmake --install /tmp/sdl-build >/dev/null
cmake -S /src -B /tmp/toh -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/tmp/sdl \
    -DPROJECT_DIR=/nonexistent -DTOH_RELEASE=ON >/dev/null
cmake --build /tmp/toh --target toh
strip /tmp/toh/toh
mkdir -p /tmp/pkg/toh
cp /tmp/toh/toh /src/LICENSE /src/THIRD_PARTY.md /tmp/pkg/toh/
cp /src/tools/release/PLAYING.txt /tmp/pkg/toh/
cp /src/tools/release/settings-linux.ini /tmp/pkg/toh/settings.ini
rm -f /src/dist/tower-of-slop-linux-x86_64.zip
cd /tmp/pkg && zip -qr /src/dist/tower-of-slop-linux-x86_64.zip toh
"
ls -la "$ROOT/dist/tower-of-slop-linux-x86_64.zip"
