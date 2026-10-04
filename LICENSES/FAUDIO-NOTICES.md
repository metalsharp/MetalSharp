# FAudio (Windows build) — third-party notices

MetalSharp ships an unmodified Windows (MinGW) build of FAudio in the
`metalsharp-assets.tar.zst` bundle (`assets/faudio/x64/`, unpacked to
`~/.metalsharp/runtime/faudio/x64/`). It is installed into a game's Wine prefix
only for titles that need xWMA audio decoding (for example Skyrim Special
Edition NPC dialogue). These are separate programs loaded by Wine; MetalSharp
does not link against them. The bundle also carries this notice and the
GPL-3.0 text under `assets/faudio/`.

Origin: FAudio 20.07 MinGW builds by Kron4ek —
https://github.com/Kron4ek/FAudio-Builds/releases/tag/20.07 (`faudio-20.07.tar.xz`).
Build scripts for these binaries are published in that repository.

| File(s) | Project | License | Source |
|---|---|---|---|
| `xaudio2_7.dll`, `x3daudio1_7.dll`, `xapofx1_5.dll`, `FAudio.dll` | FAudio 20.07 | zlib (below) | https://github.com/FNA-XNA/FAudio/tree/20.07 |
| `avcodec-58.dll`, `avutil-56.dll`, `swresample-3.dll` | FFmpeg 4.3.1 (built with GPL enabled) | GPL-3.0-or-later (`FAUDIO-FFMPEG-GPL-3.0`) | https://ffmpeg.org/releases/ffmpeg-4.3.1.tar.xz |
| `SDL2.dll` | SDL 2.0 | zlib | https://github.com/libsdl-org/SDL |
| `libwinpthread-1.dll` | mingw-w64 winpthreads | MIT / ZPL 2.1 | https://sourceforge.net/projects/mingw-w64/ |

Corresponding source for the GPL-licensed FFmpeg libraries: the FFmpeg 4.3.1
release tarball above, built with the scripts in Kron4ek/FAudio-Builds.

## FAudio license (zlib)

FAudio - XAudio Reimplementation for FNA

Copyright (c) 2011-2020 Ethan Lee, Luigi Auriemma, and the MonoGame Team

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from
the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software in a
product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3. This notice may not be removed or altered from any source distribution.

Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>



## SDL2 license (zlib)

Copyright (C) 1997-2020 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software. Permission is granted to anyone to use this software for
any purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions: 1. The origin of
this software must not be misrepresented; 2. Altered source versions must be
plainly marked as such; 3. This notice may not be removed or altered from any
source distribution.
