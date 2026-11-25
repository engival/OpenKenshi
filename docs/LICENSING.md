# Licensing

Status: **MIT (user, 2026-11-14, kenshi-379.24)**, replacing the PolyForm Noncommercial 1.0.0 + additional-permissions
package decided 2026-11-04 (kenshi-379.20). Files in the repo root: `LICENSE` (MIT), `README.md`, `CREDITS.md`,
`CONTRIBUTING.md`. No lawyer: the user wants baseline effort only.

## Why MIT

User 2026-11-14, after the coordinator set PolyForm NC's costs against its one benefit: "It seems lots of code bases
have no issue just throwing MIT on things and calling it a day. I barely see 'polyform' out in the wild. Is this really
the right choice?" then "Just MIT it I guess. Then we can call it open source?"

- PolyForm NC is not OSI open source (OSD clause 6), rare, its "commercial" line is fuzzy, and it locks contributors
  into the custom extra-permissions file. Its one benefit was a deterrent against selling a clone, which we never meant
  to enforce.
- The engine is useless without a purchased copy of Kenshi: it ships no game data or assets, and Lo-Fi Games owns
  those and the "Kenshi" trademark, so a clone that ships their content is already theirs to stop.
- MIT already grants everything the old `LICENSE-ADDITIONAL.md` did: Lo-Fi Games may use anything here, videos and
  streams, donations, paid server hosting, mods under their authors' own licenses.
- Free to change: no outside contributor yet, so every copyright holder agreed.

## The package

1. `LICENSE`: the MIT text, "Copyright (c) 2026 the OpenKenshi contributors". It includes the no-warranty /
   no-liability disclaimer.
2. README: "free, open-source engine", the bold not-affiliated / trademark / renamed-on-request lines at the top, the
   disclaimer below, the License section.
3. Contributing guide: contributions are accepted under MIT, with a DCO `Signed-off-by:` line. No CLA.
4. **Project name: OpenKenshi** (user 2026-11-14, kenshi-379.11; binaries OpenKenshi, OpenKenshi-client,
   OpenKenshi-server). It replaces the earlier "no 'Kenshi' in the name" rule (RefittedAICore). The user: "We put a
   clause that the project will be renamed upon LoFi's request. We put big bold letters that lofi is not affilited
   with this work. We do not misprepresent a product." So the README opens with those lines in bold; never drop them.
   "Refitted" stays the game's weapon grade: rename only our project name.

### Bundled libraries

Bundled libraries must be **permissive** (MIT, BSD, zlib, Apache-2.0, Boost (BSL-1.0), public domain); no GPL-family code. Current
vendored set: flecs (MIT), Lua (MIT), stb (MIT / public domain), sokol (zlib), tinyfiledialogs (zlib), recastnavigation (zlib), Clipper2 2.0.1 (Boost Software License 1.0, `cpp/third_party/clipper2-2.0.1`, knav::surfgen polygon booleans), Jolt Physics
v5.6.0 (MIT, `client/third_party/JoltPhysics`, client ragdolls). `client/third_party/wwise_codebooks/`: the aoTuV 6.03
packed codebooks from hcs64/ww2ogg, BSD-3-Clause (its `COPYING`; data only, the reader is ours): ships. libwebp (BSD-3-Clause) is not bundled: the Linux dev build links the system library for the lossless WebP screenshots (`client/src/assets/imagewrite.cpp`, kenshi-k16.2.470.1); Windows and macOS builds do not use it. Binary
releases carry the bundled libraries' notices (`CREDITS.md`).

### Exe-derived tables

The AI goal display names (`cpp/src/ksim/ai/task_names.cpp`, the exe's table, kenshi-k16.6.205 / .206) ship in the
repo and in the release as they are: no rewording, not read from the exe at runtime (user 2026-11-14, kenshi-4m9b:
"We're not going to cry over a string table. Ignore and ship it.").

## Draft: README disclaimer

> **OpenKenshi is not affiliated with or endorsed by Lo-Fi Games. "Kenshi" is a trademark of Lo-Fi Games Ltd. This
> project will be renamed at Lo-Fi Games' request.**
>
> This project is an independent, fan-made engine for running Kenshi. It is not supported by Lo-Fi Games. It includes
> no game assets or data: it requires a purchased copy of Kenshi (Steam or GOG), and the engine reads your installation.
>
> This is our own implementation. No Lo-Fi Games code was copied. Community reverse-engineering references (see
> CREDITS) were read as documentation only.

## Server data (architecture note)

Decided (user, 2026-11-04): **how a server operator gets the game data is not ours to enforce.** Anyone may run a
server, and not pirating the game is their job. We expect a big host to buy a copy, and a small personal VM to have
players upload their own copy (a grey area, but theirs).

- The server reads the **original game data files** from a Kenshi install directory, the same way the client does.
  Any caches it builds stay on that machine, built at runtime.
- **Every connecting player needs their own Kenshi install.** The architecture already forces this: SIMLINK carries
  state and ids, never assets, and the client draws meshes, textures and audio from its local install. A server can't
  stand in for buying the game. Keep it that way: no asset-streaming frames in the protocol.
- **No "bake the server data" export tool**, i.e. no tool that turns a Kenshi install into a standalone, shareable
  server-data pack. A transformed package of Lo-Fi's data is sketchier than users uploading the original files. Hold
  `MULTIPLAYER.md` to this when the server design firms up.
