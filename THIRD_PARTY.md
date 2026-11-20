# Third-party code

Source code: https://github.com/oracleofseasons/tower-of-slop (GPL-2.0, see LICENSE).

| What | Where | Licence |
|---|---|---|
| [OpenGMK](https://github.com/OpenGMK/OpenGMK): the runtime follows its Game Maker runner (event order, collisions, GML semantics, value rules) | `runtime/` | GPL-2.0, so this project is GPL-2.0 too |
| [gm_reader](https://github.com/elipsitz/gm_reader): the GM7 exe reading in `runtime/extract.cpp` is a port of its decoder | `runtime/extract.cpp` | MIT / Apache-2.0 |
| [stb_vorbis](https://github.com/nothings/stb) v1.22 and stb_image_write v1.16 | `third_party/` | Public domain / MIT |
| [SDL3](https://github.com/libsdl-org/SDL), linked into the Linux release | release builds | zlib |
| devkitPro / libnx / libounce, EGL and glad, linked into the Switch build | Switch builds | see their own licences |

*Tower of Heaven* itself is © Askiisoft. None of the game is in this repository or in the release builds:
the engine reads it from your own copy of the game.

## gm_reader licence (MIT)

`runtime/extract.cpp` ports gm_reader's GM7 decoder. gm_reader is by Eli Lipsitz and is
dual-licensed MIT / Apache-2.0; it is used here under the MIT licence:

```
Permission is hereby granted, free of charge, to any
person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the
Software without restriction, including without
limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software
is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice
shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```
