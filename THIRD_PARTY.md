# Third-party code

| What | Where | Licence |
|---|---|---|
| [OpenGMK](https://github.com/OpenGMK/OpenGMK): the runtime follows its Game Maker runner (event order, collisions, GML semantics, value rules) | `runtime/` | GPL-2.0, so this project is GPL-2.0 too |
| [gm_reader](https://github.com/elipsitz/gm_reader): the GM7 exe reading in `runtime/extract.cpp` is a port of its decoder | `runtime/extract.cpp` | MIT / Apache-2.0 |
| [stb_vorbis](https://github.com/nothings/stb) v1.22 and stb_image_write v1.16 | `third_party/` | Public domain / MIT |
| [SDL3](https://github.com/libsdl-org/SDL), linked into the Linux release | release builds | zlib |
| devkitPro / libnx / libounce, EGL and glad, linked into the Switch build | Switch builds | see their own licences |

*Tower of Heaven* itself is © Askiisoft. None of the game is in this repository or in the release builds:
the engine reads it from your own copy of the game.
