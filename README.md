# Tower of Heaven port

A C++/SDL2 reimplementation of the Game Maker 7 runner for Askiisoft's *Tower of Heaven*
(2009), aiming to play the same as the original frame for frame. Linux first, Switch 2 homebrew later.

No game assets are in this repo. You need your own copy of the game.

## Setup

1. Put the Windows release in `original/windows/` (`Tower of Heaven.exe`, `supersound.dll`, ...).
2. Run `tools/extract.sh`. It decrypts the exe and writes the project to `extracted/project/`:
   sprites and backgrounds as PNGs, sounds as WAV/OGG, GML scripts and per-event code, and rooms and objects as JSON.

## Layout

- `tools/gm7extract/gm7decrypt.py`: standalone GM7 asset-stream decryptor (GMKrypt)
- `tools/gm7extract/dump.py`: converts gm_reader output into plain files
