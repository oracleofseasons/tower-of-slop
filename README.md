# Tower of Heaven port

A C++/SDL2 reimplementation of the Game Maker 7 runner for Askiisoft's *Tower of Heaven*
(2009), aiming to play the same as the original frame for frame. Linux first, Switch 2 homebrew later.

No game assets are in this repo. You need your own copy of the game.

## Setup

1. Put the Windows release in `original/windows/` (`Tower of Heaven.exe`, `supersound.dll`, ...).
2. Run `tools/extract.sh`. It decrypts the exe and writes the project to `extracted/project/`:
   sprites and backgrounds as PNGs, sounds as WAV/OGG, GML scripts and per-event code, and rooms and objects as JSON.

## Build and run (Linux)

Needs CMake, a C++20 compiler, SDL3, Python 3 and ffmpeg.

```sh
tools/extract.sh                      # once: decrypt your exe into extracted/
cmake -S . -B build && cmake --build build -j
./build/toh --pak build/toh.pak
```

Controls are the original's: arrows to move, Up or Z to jump, Down or X to advance dialogue and use doors, Q for the menu, W for the rulebook.

To run without a window (for testing), use `--headless FRAMES --shot-every N DIR --keys "frame:vk[:frames],..."`.
This runs N frames deterministically, saves screenshots, and feeds scripted key presses.
`--warp ROOM` boots normally and swaps the first entry into level 1 for ROOM (select New Game with `--keys 330:88`).

## Checking against the original

`tools/trace/compare.sh "330:88,1350:39:25" [SEED] [SECONDS]` injects the same tracing and
scripted-input GML into a copy of the original exe and into the port, runs the original under
Wine and the port headless, and diffs a per-frame trace of room, player position and the
ordered instance list. Your original exe is not modified.

## How it works

1. `tools/transpiler/gml2cpp.py` turns every piece of the game's GML into a C++ function.
   Its lexer and parser copy OpenGMK's, so GML's quirks are kept.
2. `tools/pack/pack.py` packs sprites, rooms, sounds and music into `build/toh.pak`.
3. `runtime/` is a reimplementation of the Game Maker runner (event order, collisions, views, drawing),
   following [OpenGMK](https://github.com/OpenGMK/OpenGMK). Because of that, the runtime should be treated as
   GPL-2.0, like OpenGMK.

## Layout

- `tools/gm7extract/gm7decrypt.py`: standalone GM7 asset-stream decryptor (GMKrypt)
- `tools/gm7extract/dump.py`: converts gm_reader output into plain files
- `tools/gm7extract/inventory.py`: lists the GM built-ins the game uses (writes `docs/inventory.md`)
- `tools/transpiler/`: GML to C++ transpiler
- `tools/pack/`: asset packer
- `runtime/`: the C++/SDL3 GM7 runner
- `docs/`: notes on the port
