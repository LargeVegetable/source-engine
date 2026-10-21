# Third-party software in the PS Vita port

The VPK's `eboot.bin` (the launcher, `launcher_vita/`) links these libraries statically. Four of them are under the LGPL; the port's complete source, launcher included, is published, so the launcher can be rebuilt against modified versions of them. The engine modules carry Valve's own third-party components, listed in `thirdpartylegalnotices.txt`.

| Library | Use | License |
|---|---|---|
| [vitaGL](https://github.com/Rinnegatamante/vitaGL) (the port's fork, branch `hl2-vita`) | OpenGL on sceGxm | LGPL-3.0 (`COPYING.LESSER` in the repository). The fork's source is published with the port. |
| [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) | Runtime shader compiling for vitaGL | LGPL-3.0 |
| [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) | Shader compiler extensions for vitaGL | LGPL-2.1 |
| [math-neon](https://github.com/andrepuschmann/math-neon) | NEON math for vitaGL | LGPL-3.0 or later (© 2009 Lachlan Tychsen-Smith) |
| [SDL 2](https://www.libsdl.org) | Window, input, audio | zlib |
| [FreeType](https://freetype.org) | Font rendering | FreeType License (FTL) |
| [libpng](http://www.libpng.org) | PNG images | libpng License |
| [libjpeg](https://ijg.org) | JPEG images | IJG License: this software is based in part on the work of the Independent JPEG Group |
| [zlib](https://zlib.net) | Compression | zlib |
| [bzip2](https://sourceware.org/bzip2/) | Compression | bzip2 License (BSD-style) |
| [Opus](https://opus-codec.org) | Voice codec | BSD 3-Clause |
| [kubridge](https://github.com/bythos14/kubridge) (import stubs only) | Kernel memory functions | No license stated. Only the import stubs that VitaSDK generates from its export list are linked; the plugin itself isn't part of the VPK, and players install it themselves. |
| [VitaSDK](https://vitasdk.org) newlib, libstdc++, system stubs | C/C++ runtime, system calls | newlib licenses; GCC Runtime Library Exception |

The engine's MP3 decoder module (`libvaudio_minimp3.so`) uses [minimp3](https://github.com/lieff/minimp3) (CC0-1.0).

FreeType: portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
