# Third-party code in the Player

The Player (`app.wasm`) is BSD-3-Clause, copyright Max Brito and XPRS
contributors, like the rest of XPRS. It statically links the decoders below.
Their licences are in this directory and travel inside every `.wapp`
package, and the source of every one of them is in the `xprs-dev/wapps`
repository under `mp4player/vendor/`.

| Component | Used for | Licence | Text |
|---|---|---|---|
| PacketVideo AAC decoder (Android AOSP) | AAC, HE-AAC, HE-AACv2 | Apache-2.0 | `pvaac-NOTICE` |
| OpenH264 (Cisco) | H.264 | BSD-2-Clause | `openh264-LICENSE` |
| libvpx (WebM Project) | VP8, VP9 | BSD-3-Clause, plus a patent grant | `libvpx-LICENSE`, `libvpx-PATENTS` |
| libopus (Xiph.Org and others) | Opus | BSD-3-Clause | `opus-COPYING` |
| libnestegg (Mozilla) | WebM demuxing | ISC | `nestegg-LICENSE` |
| dav1d (VideoLAN) | AV1 | BSD-2-Clause | `dav1d-COPYING` |
| libde265 (struktur AG) | HEVC / H.265 | LGPL-3.0 | `libde265-COPYING` |
| dr_libs (David Reid) | MP3, FLAC, WAV | public domain (Unlicense) or MIT-0 | in the source |
| stb_vorbis (Sean Barrett) | Ogg Vorbis | public domain (Unlicense) or MIT | in the source |
| minimp4 | MP4 demuxing | CC0-1.0 | in the source |

libde265 is LGPL-3.0 and linked statically. The complete source of the
Player, including libde265 and the Makefile that links it, is published in
`xprs-dev/wapps`, so anyone can relink it against a modified libde265.

The desktop build of the Player also carries static ffmpeg executables in
`bin/` (LGPL-2.1-or-later, see `bin/README.md`). They are separate programs,
not linked into `app.wasm`, and never ship in the Android package.
