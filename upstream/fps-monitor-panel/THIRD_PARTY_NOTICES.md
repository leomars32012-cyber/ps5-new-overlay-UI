# Third-party notices

fps-monitor-panel is licensed under the GNU GPL-3.0 (see [`LICENSE`](LICENSE)).
That license does not replace the copyright or license terms attached to the
third-party files listed below; their file-level notices are kept unchanged.

## Components in this repository

| Component | Location | Author | Terms |
| --- | --- | --- | --- |
| cJSON | `third_party/cjson/` (`cJSON.*`, `cJSON_Utils.*`) | Dave Gamble and cJSON contributors | MIT, full text in each file header |
| SHA-1 in C | `third_party/sha1/sha1.hpp` | Steve Reid | Public domain, as stated in the file |
| inih | `source/include/ini.h` | Ben Hoyt ([benhoyt/inih](https://github.com/benhoyt/inih)) | BSD-3-Clause (`SPDX-License-Identifier` in the file) |
| Hacker Disassembler Engine 64 | `source/libonion_detour/include/onion/hde64.h`, `table64.h`, `source/libonion_detour/source/hde64.cpp` | Vyacheslav Patkov | Copyright notice retained in each file |
| ELF loader and ptrace helpers | `source/libonion_elfldr/` (`elfldr.*`, `elfldr_log.h`, `pt.*`) | John Törnblom | GNU GPL-3.0-or-later, notice in each file |
| Overlay, FPS sampler and `libonion_*` support libraries | `source/` | OnionHEN / LightningMods | GNU GPL-3.0 |
| libhijacker, libNineS | `source/libhijacker/`, `source/libNineS/` | CMake headers credit ChendoChap, Specter, Znullptr (PS5SDK template) | Received under GNU GPL-3.0 with the upstream project; no file-level notice |
| Mono runtime struct layouts | `source/shellui/include/monodef.h` | Mono project contributors | MIT (Mono runtime license) |
| FreeBSD `kinfo_proc` layout | `source/include/freebsd-helper.h` | The Regents of the University of California / FreeBSD contributors | BSD-3-Clause |

The FPS sampling approach follows PHU Games Tools by ArkSama
(<https://github.com/ArkSama>); no code from it is included.

## Link stubs

`source/platform/ps5/stubs/*.so` are link-time stubs for PS5 system libraries.
They let the payload link against system functions and are not shipped in, or
loaded from, the built payload.

## External build dependencies

The [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) (toolchain, libc,
libc++) is required to build but is not part of this repository. Its own
license terms apply.

## Preservation rule

Do not remove or rewrite upstream copyright headers or license notices. When
adding a third-party component, add a row to this file in the same change.
