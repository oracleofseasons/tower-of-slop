# Tower of Heaven port

A C++/SDL3 reimplementation of the Game Maker 7 runner for Askiisoft's *Tower of Heaven*
(2009), aiming to play the same as the original frame for frame. Linux first, Switch 2 homebrew later.

Nothing from the game is in this repo or in the engine: its code, art, sound and music are read from your
own copy of the game.

## Playing

Put `toh` next to `Tower of Heaven.exe` from the original game and run it. The first start reads the
game's data out of the exe (under a second) and keeps it in your data folder
(`~/.local/share/Askiisoft/Tower of Heaven/` on Linux); after that the exe isn't needed.
`--game PATH` points at an exe somewhere else.

## Setup for development

1. Put the Windows release in `original/windows/` (`Tower of Heaven.exe`, `supersound.dll`, ...).
2. Run `tools/extract.sh`. It decrypts the exe and writes the project to `extracted/project/`:
   sprites and backgrounds as PNGs, sounds as WAV/OGG, GML scripts and per-event code, and rooms and objects as JSON.

## Build and run (Linux)

Needs CMake, a C++20 compiler, SDL3, zlib and Python 3.

```sh
cmake -S . -B build && cmake --build build -j
./build/toh                           # first run: extracts from Tower of Heaven.exe as above
./build/toh --extract "original/windows/Tower of Heaven.exe" my.pak   # or just write the pak
```

The development tools (trace comparison, instrumented builds) also need `tools/extract.sh`, which writes
`extracted/project/`; with it, the build also packs `build/toh.pak` (`./build/toh --pak build/toh.pak`).
That pak is byte-for-byte what the engine's own extractor writes.

Controls are the original's: arrows to move, Up or Z to jump, Down or X to advance dialogue and use doors, Q for the menu, W for the rulebook.

To run without a window (for testing), use `--headless FRAMES --shot-every N DIR --keys "frame:vk[:frames],..."`.
This runs N frames deterministically, saves screenshots, and feeds scripted key presses.
`--warp ROOM` boots normally and swaps the first entry into level 1 for ROOM (select New Game with `--keys 330:88`).

## Checking against the original

`tools/trace/compare.sh "330:88,1350:39:25" [SEED] [SECONDS]` injects the same tracing and
scripted-input GML into a copy of the original exe and into the port, runs the original under
Wine and the port headless, and diffs a per-frame trace of room, player position and the
ordered instance list. Your original exe is not modified.

To check a real playthrough, record one and replay it:

```sh
tools/trace/record.sh --original my-run.keys   # play the original under Wine; quit when done
tools/trace/record.sh my-run.keys              # or play the port
tools/trace/compare.sh @my-run.keys            # replays it in the original under Wine and in the port
```

Keep the Wine window focused while the original runs: losing focus drops keys.

## How it works

1. `tools/pack/pack.py` packs sprites, rooms, sounds, music and the game's GML source into `build/toh.pak`.
2. `runtime/interp.cpp` parses that GML when the game starts and interprets it. Its lexer and parser copy
   OpenGMK's, so GML's quirks are kept. The engine binary contains nothing from the game.
3. `runtime/` is a reimplementation of the Game Maker runner (event order, collisions, views, drawing),
   following [OpenGMK](https://github.com/OpenGMK/OpenGMK). Because of that, the runtime should be treated as
   GPL-2.0, like OpenGMK.

## Layout

- `tools/gm7extract/gm7decrypt.py`: standalone GM7 asset-stream decryptor (GMKrypt)
- `tools/gm7extract/dump.py`: converts gm_reader output into plain files
- `tools/gm7extract/inventory.py`: lists the GM built-ins the game uses (writes `docs/inventory.md`)
- `tools/gen_tables.py`: builds the interpreter's name tables (runtime builtins, GM constants)
- `tools/pack/`: asset packer
- `runtime/`: the C++/SDL3 GM7 runner
- `docs/`: notes on the port
