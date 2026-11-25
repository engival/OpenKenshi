# Credits

## Kenshi

Kenshi is made by Lo-Fi Games. This engine requires a purchased copy of Kenshi (Steam or GOG) and reads its data and art from your
install; none of it is included here. "Kenshi" is a trademark of Lo-Fi Games Ltd.

## Reverse-engineering and format references

Read as documentation only. No code was copied from them; our implementations are our own.

- **wwiser** (bnnm): Wwise SoundBank format notes.
- **ww2ogg** (hcs64): Wwise Vorbis packet notes. Its packed aoTuV 6.03 codebooks ship as data (see below).
- **vgmstream**: Wwise audio format reference.
- **KenshiLib / RE_Kenshi**: reverse-engineered Kenshi structures and enums, used as cross-checks.
- **OGRE** documentation: the `.mesh` / `.skeleton` binary formats.
- **ParticleUniverse** plugin manual (Henry van Bakel): the `.pu` particle script format.
- **Kenshi Wiki** (kenshi.fandom.com, CC BY-SA): community numbers, checked against the game's data.

## Bundled libraries

| Library | Version | License | Where |
|---|---|---|---|
| sokol (Andre Weissflog) | see `client/third_party/sokol` | zlib | `client/third_party/sokol/` |
| tinyfiledialogs (Guillaume Vareille) | see `client/third_party/tinyfiledialogs/VERSION.txt` | zlib | `client/third_party/tinyfiledialogs/` |
| stb (Sean Barrett) | see `client/third_party/STB_VERSION` | MIT / public domain | `client/third_party/`, `cpp/third_party/stb/` |
| Jolt Physics (Jorrit Rouwe) | 5.6.0 | MIT | `client/third_party/JoltPhysics/` |
| flecs (Sander Mertens) | 4.0.4 | MIT | `cpp/third_party/flecs/` |
| Lua (PUC-Rio) | 5.5.0 | MIT | `cpp/third_party/lua-5.5.0/` |
| SMAA (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez) | 2013 | MIT | `client/src/smaa_lut.*`, `client/shaders/smaa.inc` |
| Clipper2 (Angus Johnson) | 2.0.1 | BSL-1.0 | `cpp/third_party/clipper2-2.0.1/` (`LICENSE`) |
| aoTuV 6.03 packed codebooks (Xiph.Org Foundation, Adam Gashlin, via ww2ogg) | ww2ogg 14ed9b0 | BSD-3-Clause | `client/third_party/wwise_codebooks/` (`COPYING`) |

Each library's own license text is in its directory or at the top of its files; binary distributions must carry
those notices.

## System libraries (linked, not bundled)

recastnavigation (zlib), libvorbis and libogg (BSD-3-Clause), Xlib and its extensions (MIT/X11), OpenGL, and on
Linux PulseAudio's client library (LGPL-2.1, dynamically linked).

## Build tools

sokol-shdc (Andre Weissflog, MIT), fetched by `client/tools/get_shdc.sh` at build time.
