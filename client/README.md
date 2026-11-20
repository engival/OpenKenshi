# OpenKenshi: play Kenshi on a free, open-source engine

**Not affiliated with or endorsed by Lo-Fi Games. "Kenshi" is a trademark of Lo-Fi Games Ltd. This project will be renamed at Lo-Fi Games' request.**

(`kclient` below is the client target; a dev build writes `client/build/kclient`, a release build `OpenKenshi-client`.)

C++20 / sokol rendering client for the Kenshi rewrite. M1: fly/walk over the bare terrain.
Decisions and open work: `../docs/CLIENT.md`; feature notes: `../docs/CLIENT_GOALS.md`.
Rule: a merge that adds or changes something the player can see or press updates `../docs/PLAYER_GUIDE.md` in the same merge.

## Build

```bash
client/tools/get_shdc.sh                  # once: fetches the prebuilt sokol-shdc into client/bin/
cmake -S client -B client/build -G Ninja  # -DKENSHI_GFX=vulkan builds the (experimental, untested) Vulkan backend
nice -n 10 cmake --build client/build -j8
# configure uses the shared ccache (CCACHE_DIR=/var/cache/kenshi-ccache) by itself when /var/cache/kenshi-ccache/ccache.conf exists and no CMAKE_CXX_COMPILER_LAUNCHER is set
# git worktree (client/bin is gitignored, so sokol-shdc is missing): add -DKENSHI_SHDC=<main checkout>/client/bin/sokol-shdc to the configure line
```

**Build lock.** Every build in `client/build` (sessions, workers, the coordinator, scripts) takes `flock client/build/.build.lock`, e.g.
`flock client/build/.build.lock nice -n 10 cmake --build client/build -j8`, so two ninjas never share the dir (a second one waits, then finds the
targets fresh). The playtest launcher's build (`demo.sh --build`, or its stale-binary rebuild) is the player's and uses all cores (`-j$(nproc)`, no
nice), printing a wait line when it has to queue; every other build is `nice -n 10`, at most `-j8`.

Needs X11/Xi/Xcursor and GL dev packages (Vulkan SDK for the vulkan backend).

**Shared server.** `ksim_server` (the `cpp/` target `OpenKenshi-server`, the slow one) is usually identical across client worktrees, so a
worktree with no `cpp/` change reuses master's build instead of making its own: the coordinator deploys it after every merge as
`$HOME/git/kenshi/bin/OpenKenshi-server-<hash8>` (the newest 4 kept, `OpenKenshi-server-master` a link to the newest). The run scripts
(`client/tools/play.sh`, `demo.sh`, `live.sh`, `client/tests/*/run.sh`) take `KSIM_SERVER=<path>` for an explicit binary; otherwise they ask
`client/tools/shared_server.sh` looks up `$HOME/git/kenshi/bin/OpenKenshi-server-cpp-<first 12 hex of git rev-parse HEAD:cpp>` first, from any branch or base, when the worktree has no uncommitted or untracked change under `cpp/` (the coordinator's deploy hard-links one per master deploy and prunes entries untouched for 3 days; a hit is `touch -h`ed so trees in use stay; `--explain` names the tree and the rule that answered); the merge-base lookup below stays as rule 2 for deploys that predate the cpp- links, and the wire-number rule (kenshi-k16.2.306.5.1, rule 3) sits after rule 2's cpp/ check: when this branch's commits since the merge base touch no `cpp/` file (rule (a) stays first: additive wire changes are only safe newer-server / older-client, so a branch that changes `cpp/` builds its own server), N = `kProtocol` from this branch's `client/src/net/simlink.hpp` and the newest (by mtime) `$HOME/git/kenshi/bin/OpenKenshi-server-w<N>-<hash8>` is handed out, `--wire-version` confirmed as N when the binary has it (`--explain`: `shared PATH (wN: ...)`, or `built (no wN binary, ...)`), before the merge-base `<hash8>` name. Bump `kProtocol` on any wire change, server and client together: rule in SIMLINK.md (docs/formats/SIMLINK.md) section "Protocol version"; the client refuses a server with another number at HELLO. Auto-cache (kenshi-k16.2.306.6.1): after `demo.sh` / `dev_off_check.sh` build ksim_server in a worktree whose `cpp/` is clean, `shared_server.sh --cache BIN` copies it to that cpp-<tree12> name (unique .tmp name, chmod 755, `mv -n`; skipped when the name exists; never for a build with uncommitted or untracked `cpp/` changes). Each entry is ~300 MB. Failing that, `client/tools/shared_server.sh`, which answers only when this branch's diff against its merge base with master touches no file under `cpp/`
and the deploy for that merge base exists; with no answer they build and use `client/build/cpp/OpenKenshi-server` as before. Every run prints
which it is, `server: shared PATH` or `server: built (REASON)` (`. client/tools/server_bin.sh`; `pick_server`), and a shared server skips
the `ksim_server` build and `demo.sh`'s stale-source rebuild of it, so a worktree needs `--target kclient` only. Where there is no
`$HOME/git/kenshi/bin` (a host without a deploy, such as dev) the reason is `no shared binary for <hash8>` and the run falls back to its own build. The coordinator deploys to `$HOME` on both the desktop and ds. A shared server is master's
build, made by the client lead's account: its baked game data, sim data and `KSIM_SCENARIOS_DIR` are that machine's paths (the scripts pass
absolute `--scenario-file` paths), so the tools pass `--data` (this build's `KENSHI_DATA`) and `--sim-data cpp/data` with it
(`pick_server`'s `SRV_ARGS`; a `--local-server` run gets the latter through `session.server_args`), and it reports master's git hash.

**Debug build (sokol validation).** The release build logs only a message-less `[sg][error]`; a Debug build defines `SOKOL_DEBUG`, so sokol
validates every call and logs the reason (on the line after the `[sg][error]`), then panics (SIGABRT, the crash report has the backtrace).
Keep it in its own directory, `client/build-debug` (git-ignored):

```bash
cmake -S client -B client/build-debug -DCMAKE_BUILD_TYPE=Debug   # + the same -DKENSHI_DATA / -DKENSHI_SHDC as the release build
nice -n 10 cmake --build client/build-debug -j4 --target kclient ksim_server
KCLIENT=$PWD/client/build-debug/kclient client/tools/scene_test.sh OUT   # the scenes; a validation message is a failure (never --bless from it)
client/tools/dev_off_check.sh   # the dev fence: builds kclient with KCLIENT_DEV_TOOLS=OFF in client/build-devoff, plays title + new game, checks no playtest symbols
client/tools/title_pending_check.sh OUT   # a NEW GAME click while the title's server starts is held, then opens the new-game panel (dev builds; serverhold + dump title)
client/tools/multi_cut_check.sh OUT   # two squads in two buildings, floor 0: both buildings cut whichever squad is selected (dump cuts, dump pick; kenshi-k16.2.332.1)
BUILD=$PWD/client/build-debug client/tests/sellburst/run.sh smoke OUT   # a live headless run against OpenKenshi-server
```

The unoptimised Debug build also aborts on libstdc++'s `operator[]` range checks (seen with GCC 15), where release reads past the end.

**ThreadSanitizer build (the render thread's hand-off).** `render.thread=lockstep` puts `logic_frame` on its own thread
(`src/app/logicthread.hpp`); `-DKCLIENT_TSAN=ON` builds every target with `-fsanitize=thread` for the races between the two. Keep it in its own
directory, `client/build-tsan` (git-ignored). `client/tests/tsan/run.sh` configures and builds that directory, then runs the scene test and a
scripted lockstep scenario under it and writes the reports' summary to `OUT/summary.txt`:

```bash
client/tests/tsan/run.sh [--mode lockstep|overlap] [--scenes NAME,NAME] [--only both|scenes|scenario] OUT   # OUT default client/build-tsan/tsan-run
```

The first build takes ~20 min at -j4, the scene test ~6 min and the scenario ~8 min under TSan (the shared desktop: `nice -n 10`, at most -j4).
`--mode overlap` is accepted for when overlap lands; until then the client prints its loud "overlap IS NOT IMPLEMENTED" line and runs lockstep.
The reports are written by TSan's `log_path` into `OUT/tsan/`, one file per process; a race is the script's result, not its failure (exit 0).

## Release targets (kenshi-379.6)

Three binaries from the same libraries: `kclient` (client only; single player forks `OpenKenshi-server` from the build dir's `cpp/`), `OpenKenshi-server`
(dedicated) and `OpenKenshi` (full: the same client with the server in its process). `OpenKenshi` is not in the default build (it shares the client objects with
kclient; only main.cpp and localserver.cpp compile a second time): `nice -n 10 cmake --build client/build -j4 --target OpenKenshi`. Its `LocalServer` (`src/platform/localserver.cpp`, `KENSHI_EMBED_SERVER`)
runs `ksim::runServer` on a `std::thread` and stops it with the server's stop flag; the client still joins over loopback TCP, the same path as
multiplayer. The server's globals are shared with the client (crash recorder tick, process-wide state): the audit is kenshi-379.7. The smoke run
(title, New Game, Wanderer, BEGIN, CONFIRM, a few seconds in the Hub, quit) is `tests/localserver/run.sh BINARY OUT` (use `OpenKenshi` or `kclient`).

**No flags for a player (kenshi-379.13).** The game data is resolved at runtime (`src/platform/datadir.cpp`): `--data DIR`, then `$KENSHI_DATA`, then a
saved `data=PATH` line in `$XDG_CONFIG_HOME/kenshi-client/data.conf` (`~/.config/kenshi-client/data.conf`; `~/Library/Application Support/kenshi-client/`
on macOS, `%APPDATA%\OpenKenshi\kenshi-client\` on Windows), then the Steam/GOG default installs (Linux: `~/.steam/steam` and `~/.local/share/Steam`, plus
their `libraryfolders.vdf` libraries; macOS: Steam's `common/Kenshi` and a `Kenshi` folder beside the binary; a copied Windows install works
there, the game has no macOS build). A hit is saved; nothing found opens a native folder picker ("Where is Kenshi installed?", `src/app/datapicker.cpp`, tinyfiledialogs: zenity/kdialog/qarma on Linux, the shell dialog on Windows, osascript on macOS) before the window exists; the install folder or its `data/` folder is accepted and saved as `data=PATH` (a folder without `gamedata.base` reopens it with the reason in its title). Cancel, no dialog tool, no display (Linux: no `DISPLAY`/`WAYLAND_DISPLAY`), `KCLIENT_NO_DIALOG=1` or `KCLIENT_CONFIG=none` (headless.sh sets both) print every place it looked and the three ways to set one, then exit 1. A
dev build searches the configure-time `KENSHI_DATA` first among the installs; a release build (dev tools OFF) carries no path, in the client or the
in-process server (`KSIM_BAKE_DATA_PATH` follows `KCLIENT_DEV_TOOLS`, and the client passes the resolved `--data` to its server). `kenshi` with
no options behaves as `play.sh title` — the title and its own server, also for a scripted/headless run; `--debug` keeps the offline fly-around
and `--simlink` keeps joining a given server.

**Windows log (kenshi-379.31.9.1).** `OpenKenshi.exe` is a GUI-subsystem program: no console window opens. Its stdout and stderr (client, embedded server, crash report) go to `%LOCALAPPDATA%\OpenKenshi\data\logs\openkenshi.log`; the previous run is kept as `openkenshi.log.1`. `--console` (or `KCLIENT_CONSOLE=1`) opts in to a console instead: the one it was started from (cmd, a terminal), else a new window (`src/platform/console.hpp`). Linux and macOS print to their terminal as before.

## Play

`client/tools/play.sh` is the launcher: the named modes, so the flags do not have to be remembered. No arguments (or `--help`)
prints the list:

```bash
client/tools/play.sh                          # the modes, one line each, with an example
client/tools/play.sh fly --at Hub             # the debug free camera over a place, no title screen (default: the Hub)
client/tools/play.sh title                    # the title screen with its own OpenKenshi-server; New Game starts the game
client/tools/play.sh game                     # a new game: demo.sh's newgame at the Hub, the OpenKenshi-server behind it
client/tools/play.sh scenario                 # the scenario fixtures in scenarios/ (add a NAME to run one)
client/tools/play.sh scenario bar_lure        # one fixture: the server swaps the world to it
client/tools/play.sh spectate spectate_fight  # a player-less spectator fixture (the spectate_* files)
```

Arguments after `--` go to kclient (`play.sh fly --at Hub -- --alt 200`). `game`, `scenario` and `spectate` run
`tools/demo.sh` (server + client); `title` and `fly` run kclient alone (`title` starts its own OpenKenshi-server when the
title shows, also in a scripted/headless run: `--local-server`). Builds are not run by play.sh: a missing binary prints the
command that builds it. `KCLIENT` overrides the client command in every mode (`demo.sh` reads it too), word-split (e.g. a headless wrapper:
`KCLIENT="client/tools/headless.sh -- $PWD/client/build/kclient" client/tools/play.sh fly --at Hub -- --script shot.txt`).

`--at PLACE` (also a kclient flag) takes a town name from the game data, case-insensitive and by prefix, a leading "The"
ignored (`Hub`, `squin`, `The Hub`), or an x,z pair. A name frames the place: the eye goes back along the view by
`alt / tan(|pitch|)`, so the sight line at the given yaw and pitch meets the town's centre; an x,z pair (or a `--pos`
with a name) puts the eye exactly there. An unknown name lists the closest names and exits 2.

Without the launcher: `client/build/kclient --debug` by itself is an offline fly-around over the world at `--at`/`--pos`, with
no title screen and no game server (a `--debug` run does not show the title). A plain `client/build/kclient` shows the title,
which starts its own local server behind it. A joined session needs `--simlink HOST:PORT` (`--debug` for the development keys
and windows).

## Run

```bash
client/build/kclient [--data DIR] [--pos x,z] [--at PLACE|x,z] [--alt H] [--yaw DEG] [--pitch DEG] [--walk]
                     [--fog DIST] [--time HH.H] [--day N] [--timescale N] [--weather NAME|auto] [--weather-blur KM] [--front-blur KM] [--scene-file FILE] [--size WxH] [--map GUI_Map.dds] [--screenshot out.png] [--script FILE] [--bradius CELLS] [--no-features] [--no-interiors] [--fradius CELLS] [--foliage-range F] [--no-foliage] [--no-grass] [--grass-range F] [--grass-density F] [--grass-map disk|paint] [--no-water] [--no-shadows] [--squads FILE] [--mat-report FILE]
                     [--scene-test OUTDIR [--scenes NAME,NAME]]
                     [--list-mods] [--mod KEY=on|off]... [--save-mods]
                     [--view-mesh NAME_OR_PATH [--tex TEXTURE.dds] [--dist D]]
```

- `--data DIR` is optional: without it the data folder is found by the search in "Release targets" (kenshi-379.13).
- `--pos` world x,z (range +-147456); `--alt` height above ground; yaw 0 looks toward -z, 90 toward +x.
- `--map FILE` overrides the map image (8192^2 DXT1 DDS). Default: the `nicemap` mod's `GUI_Map.dds` if found and enabled, else `data/gui/gfx/GUI_Map.dds`.
- Mods: `--list-mods` prints key, name, id, enabled, used/planned and where each supported mod was found, then exits;
  `--mod KEY=on|off` (repeatable) overrides for this run; `--save-mods` also writes the result to
  `$XDG_CONFIG_HOME/kclient/kclient.cfg` (`~/.config/kclient/kclient.cfg`; `%APPDATA%\OpenKenshi\kclient\kclient.cfg` on Windows). Details: `MODS.md`.
- Ground colour: `data/newland/land/overlaymaps/colour.X.Y.png` (X = column, Y = row, no flips). The first run builds
  a BC1 + mips cache (170.7 MiB, ~2 s with 8 threads) in `$XDG_CACHE_HOME/kclient` (`~/.cache/kclient`; `%LOCALAPPDATA%\OpenKenshi\cache\kclient` on Windows), keyed by
  the tiles' size+mtime; later runs only read it. Uploaded as one 16384^2 BC1 texture (drops top mips if the GPU
  limit is lower).
- Water (M4): one plane at y = 100 over every terrain chunk that dips below it (colour `watercolourmap.png`, flow/scum `flowmap.png`, per biome from BIOMES: gloss, visibility, strength, glow, water/scum scales, distortion, the water normal map (default `materials/water.png`), turbulence and scum maps, none where the biome has none). `--no-water` skips it; `--water-flat` draws the unlit map colour only (debug).
- `--screenshot` writes a PNG and exits (glcore only): 4 frames in an offline run. In a `--simlink` session with no `--script` the first frames
  have no world yet (nothing is drawn before the camera has a target), so the shot is taken 3 frames after the first world frame instead — the
  squad's placement, a new game or scenario, or the Hub default after the 10 s no-target fallback (kenshi-k16.2.124.1).
- `--script FILE` runs a text script (below); `--screenshot` is sugar for a script (normally `3 screenshot X`, `3 quit`; with `--simlink` and no
  `--script`, `first world frame + 3 screenshot X`, `first world frame + 3 quit`).
- Agents and scripted runs MUST go through the headless wrapper so no window appears:
  `client/tools/headless.sh -- /abs/path/client/build/kclient --screenshot /abs/out.png --pos 0,0`
  A join to a running server, the shot at the first world frame (no script needed):
  `client/tools/headless.sh -- client/build/kclient --debug --simlink 127.0.0.1:47811 --screenshot /abs/out.png`
  Runs are bounded by default: at `HEADLESS_TIMEOUT` seconds (default 600; 0 disables the bound) the wrapper kills
  gamescope and the client it started together and exits 124 with a `headless.sh: TIMEOUT` line, so a hung run ends
  the command instead of the job (kenshi-k16.2.219). A `kui_demo` with no `--screenshot` never exits on its own
  (nothing can press its Q inside gamescope), and the wrapper says so at start; give it `--screenshot` or expect the timeout.
- **Log noise (kenshi-sb3.1).** A walking squad used to log ~3 audio / notify lines per footstep. They are behind `debug.audio_trace` (default off;
  `--set debug.audio_trace=1`, a script line `set debug.audio_trace 1`, or the config file, like the other `debug.*` keys): every world sound post (distance, gated, voices), every
  object switch (`Armor_Worn = ...`) and main.cpp's `notify:` animation-event lines. With it off the log keeps one line per distinct (event, reason) for
  `not in the loaded banks` / `object not registered`. What plays is unchanged.

## Mesh viewer (M2a)

`--view-mesh NAME_OR_PATH` renders one Ogre `.mesh` on a plain background (no terrain) with an orbit camera fitted to the vertex
bounds; `--yaw/--pitch` set the view direction (yaw 0 looks toward -z; to see a flat sign face try yaw 90 or -90), `--dist` the
orbit distance, `set dist D` works in scripts, wheel zooms, right mouse orbits. NAME is a bare mesh name (looked up over the
`resources.cfg` directories) or a path as the records write it (`.\data\buildings\x.mesh`, relative to the install root).
It prints the bounds in world units, and per submesh the material, the texture and where the texture came from.

Textures: Kenshi's mesh materials are mostly `StaticObject`, whose script texture is a placeholder (`black.dds`); the real texture is
bound at runtime from game data (MATERIAL_SPEC `texture map`), which the viewer does not read yet. Order used: `--tex NAME` for
every submesh, else the material script's diffuse texture, else a view-only guess (the `.dds` in the mesh's directory with the
longest common name prefix, normal/specular maps skipped), else flat grey. The guess is often wrong; use `--tex`. Opaque only:
alpha-tested materials (foliage leaves) are not handled.

Sources: `ogremesh` (reader, v1.41/1.8/1.100), `resources` (name index over `resources.cfg` `FileSystem=` dirs: non-recursive,
case-insensitive, first listing wins, mod dirs not indexed), `materials` (`.material` scripts, inheritance), `dds` (BC1/2/3 incl.
DX10 headers, mips, any size, uploaded as BC; uncompressed DDS rejected), `meshview`, `shaders/mesh.glsl`.

`build/kmeshdump [--resolve INSTALL_ROOT] FILE.mesh...` prints per mesh in `tools/ogre_mesh.py`'s format plus one line per submesh
(counts, vertex elements present, and with `--resolve` the material's texture). Parity check against the Python reader:
all 3,396 game meshes, 0 mismatches in version, submesh count, vertices, triangles, vertex bounds, and per-submesh material and counts
(script: compare `kmeshdump` output with `ogre_mesh.parse()`).

## Buildings (M2b)

Placed buildings render in the world by default (`--bradius N` = load radius in zone cells, default 3, 0 = off; past it the far ring keeps
the exteriors only, no interiors, lights or effects, out to `render.building_far` (default 60000 u, capped by `--fog`), dithered out over its last
15 %, so a town has no pop-in and no stand-in model (kenshi-k16.2.65; ~+0.5 ms GPU at 1440p, ~2x resident meshes); `--mat-report FILE`
writes at exit the loaded meshes whose submesh materials are not `StaticObject`). Sources: `buildings` (CPU: kworld `Zones::place` ->
layer overlay by cell + instance id (14,614 raw -> 11,714) -> BUILDING `parts` -> BUILDING_PART mesh + MATERIAL_SPEC `texture map`, tile, shader type),
`worldbuildings` (streaming + instanced draw), `building` program in `shaders/mesh.glsl`. Loaded on a background thread (~2.6 s;
scripted runs wait). Part groups (`parts` v0 group, v1 chance) and material collections are rolled with a hash of the instance id.
Meshes/textures decode on 4 worker threads, upload on the main thread (8 per frame; scripted runs block until everything near is loaded);
caches are keyed by file and evicted LRU. One instanced draw per (part, submesh); distance, size and frustum culling per part per
instance (camera-relative instance data rebuilt each frame). The texture rule is one function, `WorldBuildings::textureFile`.
M2c: part groups per BUILDING_MATERIALS.md (group 0 = independent absolute-% roll per part index, group N = one weighted pick); part `offset X/Y/Z`
applied in the mesh frame (x, y, z; verified on windmills, `--offset-frame 1` tries the Z-up mapping) and summed down nested parts;
`rotation function` 1-3 parts spin about `rotation axis` at `rotation speed` (scripted time = frame/60); parts flagged `is for position marker`
are drawn un-offset (camp beds, dummies; `--hide-markers` hides them). Materials: ALPHA = alpha test (normal map alpha < 0.5), EMISSIVE = texture
x normal-map alpha added unlit, DUAL = texture map 2 blended by vertex alpha (only when the mesh has vertex colours), FOLIAGE = alpha test on the
texture alpha. Startup prints `GL_RENDERER`.
Not done: mods (kworld applies only the 4 shipped data files), parent rotation for nested part offsets, wind/turret aiming (functions 4/5 stay
still), doors-as-children, true alpha blending, destroyed meshes.

## Building interiors

On by default (`--no-interiors` = the game's look with every interior unloaded). Data: `docs/reference/INTERIORS.md`. `buildings.cpp` adds, per placed building, its BUILDING `interior`
parts (kind 1) and `interior mask` parts (kind 2, never drawn, only their bounds), and, for the 708 of 772 placements whose GAMESTATE_BUILDING `interior layout name` joins an
`interiors.level` collection `<building sid>-<name>` (the 64 that do not are counted and skipped), one instance per layout item (kind 3 furniture; the instance transform is composed
with the building's; `nodemarker.mesh` editor dummies are not drawn). Each part carries `building floor` (0..3). Drawing is by camera distance, not by "a character is inside":
parts and furniture within `WorldBuildings::interiorRadius` (2500 units = 250 m) load and draw, the last 20 % fades out with the dithered distance fade (`shaders/fade.inc`).
Interior parts cull back faces so a ceiling does not hide the room from above.
- `R` / `F` step the shown floor of the building with floors under or nearest the camera (within 1500 units); parts and furniture on higher floors are hidden, floor 0 never. HUD line:
  `building NAME  floor n/N`. Scene field `keys=F` / `keys=R` sends the presses after loading.
- Camera inside the building (its mask bounds): the shown floor is also capped at the camera's floor, which cuts the roof and upper floors away. A floor starts at its interior slab
  (`interior` parts of that floor) or, when it has none, at an even split of the interior height.
- Furniture floor: the layout state's `floor` int, else the floor whose start height is below it.
Not done: terrain clip with the mask (CLIP_INTERIOR), `shares interiors with`, doors, sim state (contents, damage), interior ambience lighting.

## Hand-placed features (M2d)

`--no-features` skips them (default: drawn with the buildings, same `--bradius` zone radius). Source: `kworld::Features` (`cpp/include/kworld/features.hpp`): `data/newland/land/features.dat`
(MF01, 1,821 placements) resolved against the merged MAP_FEATURES records (mesh, `texture map`, `texture mode` = MapFeatureMode, tile, alpha threshold, `local coordinates`;
`hidden` markers are skipped; FOLIAGE_MESH `leaves mesh` would become a second, alpha-tested part). Placement y is absolute, scale per axis, rotation w,x,y,z (a non-finite
quaternion draws unrotated). Each record mesh becomes a building part (`BuildingPartDef::featureMode >= 0`), so the instanced path, culling and texture cache are shared; the
instance buffer now carries a per-axis stretch (48 bytes per instance). Shader modes in `shaders/mesh.glsl`: TRIPLANAR (three-axis projection, world space or object space with
`local coordinates`, uv = position / 1000 x tile), TERRAIN (the terrain's computeBiome with the one biome under the instance, `shaders/biome.inc`; the record texture is ignored), DUAL_TEXTURE / DUAL_TRIPLANAR (second texture by vertex alpha), FOLIAGE and `alpha threshold` > 0 (alpha test), EMISSIVE. HUD line: `buildings N drawn, features M`.
Road foliage (`5-roadfoliage`, 2,725 points after the layer overlay) names no record, so it is counted at startup but not drawn.

## Procedural foliage (M2e)

On by default: trees, bushes, rocks, skeletons, wrecks from `kworld::generateFoliage` (FOLIAGE_EXE.md). `--fradius N` = load radius in zone cells (default 4; the last 20 % of it is the dithered fade, so a distant landmark is drawn solid and the fade sits beyond the usual fog), `--foliage-range F` (default 6; the game ships 1) multiplies every layer's draw distance, `PgUp`/`PgDn` step it live, `--no-foliage` skips it

Grass (GRASS records, `src/grass.*`, `shaders/grass.glsl`, docs/CLIENT.md "Grass"): on by default with the foliage (it shares its game data, heightmap and biome map; `--no-foliage` also drops it). `--grass-range F` = multiplier on the game's per-class grass distance (500 / 1000 / 8000 units at 1, from the layer's `visibility range`; every shipped grass layer is class 1 = 1000; default 2 = 2000 units for the GPU budget, 4 = 4000 costs ~0.75 ms; the dithered fade covers each class's last 20 %), `--grass-density F` multiplies the blades per area (the game's `grass density` setting, default 1), `--grass-map disk|paint` picks the density map (disk: the overlay R byte as loaded, as the game, default; paint: the exe's noise paint, which the game runs only when a save-fixup rebuilds the foliage), `--no-grass` skips it. `KGRASSLOG=1` prints each biome's grass layers at start.
(HUD line 6: `foliage N drawn of M, D draws, cells, cpu ms`). Code: `foliage.{hpp,cpp}`; it reuses `WorldBuildings` for meshes, textures, materials and the instanced pipeline
(`addPart` / `beginDraws` / `draw`; the instance buffer is positioned relative to a `base` uniform so a cell's instances can be a static buffer).
- A context (merged game data, heightmap, `biomemap.png`, features, towns, every FOLIAGE_MESH resolved by `resolveFeatureRecord`) loads on a background thread (~3.3 s; scripted runs wait).
- Cells within the radius generate on 3 worker threads (nearest first, 6 requested at a time; scripted runs request all and wait), are bucketed by record and uploaded once as one
  static vertex buffer per cell (12 floats per instance, cell-local x/z, absolute y); a cell is freed one cell beyond the radius.
- A FOLIAGE_MESH record is one part for `mesh` (its `material type` = MapFeatureMode, `alpha threshold` for the FOLIAGE mode) plus a second two-sided alpha-tested part for `leaves mesh`
  (`leaves texture`, `leaves alpha threshold`, default 80). Instance rotation is the generator's yaw quaternion; scale uniform; `slope align` / `keep upright` are ignored.
- Culling is per (cell, record) group, not per instance: an instance-bounds sphere against the frustum and against a distance limit of min(fog, radius cells, 400 x the smallest
  instance's mesh radius). FOLIAGE_LAYER `visibility range` is 1 on all 82 layers, so it cannot drive distance; there is no per-mesh draw distance on FOLIAGE_MESH.
- Screenshot lines print resident / drawn instances, draws, worker generation ms per cell, CPU ms for cull + draw list, cell upload and draw submission, whole-frame CPU, and
  the instance counts (children and top records) of the camera's cell.
- The link adds `-Wl,--allow-multiple-definition` because `libkworld.a` (`biomemap.cpp`) and `stb_impl.cpp` both define stb_image.

## Venge beams

The sim runs every sky beam (SIMLINK "BEAMS"); the client draws only the `render.venge_beams_max` nearest the camera whose own zone cell is
loaded (default 7; 0 = none, a large number = all the loaded ones; kenshi-k16.2.409.1). Distance is horizontal, from the `Camera` position. A
drawn beam stays until it is 10% farther than the last one in the set; a beam joining or leaving fades over one second (the particle system's
`fade`), it does not pop. The beam's particles follow the same set.

The loaded-cell gate is the original's weather-handler gate (`140100ba0`, kenshi-rwh.211; kenshi-k16.2.409.3): a beam is drawn only if the zone
cell under it (64x64 cells of 4608 u, `cell = clamp(int((x + 147465) / 4608), 0, 63)` per axis) is loaded. Loaded is the 3x3 block round the
focus cell -- the cell under the camera, re-centred only past 0.9 cell (4147.2 u) from the focus cell's centre per axis -- plus every cell
touched by a +-340 u box round each player-faction character (the "Fast zone hopping" radius is 0 in the default config). The cell's biome
group == the handler's owner group half of the original's gate is the sim's population rule already (a beam starts in a random zone cell of its
own group), so the client tests only the cell. A beam the gate shuts out leaves through the same one-second fade as one the cap drops.
`debug.venge_beams_gate 0` (KCLIENT_DEV_TOOLS) bypasses the gate, the pre-409.3 look, for comparisons.

`KBEAMSLOG=1` prints a line per second to stderr: `beamlog tick N wire W burst B drawn D inview V fading F ... focus X,Z cells C`
(beams sent / inside their burst / in the drawn set / of the drawn set whose column, its foot to 16 000 u top, crosses the camera frustum /
fading out of it; the gate's focus cell and its loaded-cell count).

## Sky, time and lighting (M3)

Clock, sun, atmosphere, clouds, stars, moons, fog and the shared lighting block live in `sky.{hpp,cpp}`, `shaders/sky.glsl` and `shaders/lighting.inc` (included by `terrain.glsl`
and `mesh.glsl`, so terrain, buildings and features shade identically). Formulas and constants: `../docs/reference/SKY_EXE.md`; what is exact and what is approximated: `../docs/history/CLIENT_STATUS_2026-10.md`.

- `--time HH.H` start hour (default 12.0; the sun is highest at 14.0, day = 5..23 from the merged GLOBAL CONSTANTS), `--timescale N` game speed: 0 pauses, 1 / 2 / 5 are the game's speeds
  (24 h = 2618 s at 1; other values work for testing).
- Keys: `P` pause, `T` cycle speed 1 / 2 / 5, `[` / `]` hour -0.5 / +0.5 (Shift: 3 h). HUD line 5 shows time, day, speed and sun altitude.
- Script: `<f> set time H`, `<f> set timescale N` (the client's own clock; with a sim link `<f> set simspeed N` sends the sim's SET_SPEED order, 0 < N <= 64), keys `P`, `T`, `LBRACKET`, `RBRACKET`. Every screenshot line prints a `sky:` line (sun direction, alpha, exposure, light colours, weather).
  For repeatable shots use `--timescale 0` and `set time`.
- `--exposure auto|N`: HDR auto exposure (default `auto`): the scene is drawn into an RGBA16F target, a GPU luminance chain and 1x1 adapted-luminance texture (0.5 / s, `clamp(MIN, MAX)`) set the exposure in the composite, no CPU readback; `N` (e.g. 0.6875) pins a fixed multiplier. Scripted runs (`--screenshot`, `--script`, scene test) jump to the current luminance each frame, so shots are deterministic; `--adapt` makes them adapt at the real rate (1/60 s per frame). Script `set exposure auto|N`; screenshot lines print a `hdr:` line, the scene JSON has `hdr_scene_lum`, `hdr_adapted`, `exposure`, `gpu_exposure_ms`. `../docs/reference/LIGHTING.md` section 10.
- The display is the original's (k16.2.82): the composite scales by the game's `EXPOSURE_KEY` 0.55 and the frame is hard-clipped to 0..1, no curve and no output encode. There is no display-curve, contrast or preset switch; the `post.*` effects below are each their own option, all off by default.
- `--day N` start day (default 1; the moons spin with the total game minutes, script `set day N`). `--weather NAME` loads a WEATHER record from the merged data (name or stringID, case-insensitive) for comparison shots only: `sky color mult`, `clouds density`, `fog enabled` / `fog color` / `fog distance min` (e.g. `--weather "Desert Blaster"`: sky mult fcac7c, fog off in the merged data; `"shek desert storm"`: fog on, 9000). That pins the weather. Without `--weather` (or with `--weather auto`) the weather is regional: areasmap -> BIOME_GROUP -> SEASON -> WEATHER (one representative weather per region; the rule is in `../docs/CLIENT_GOALS.md` "Regional weather"), blended over a Gaussian around the camera (`--weather-blur KM`, full width, default 1) and drawn as a per-pixel fog from a blurred map (`--front-blur KM`, sigma, default 2; 0 = hard region edges). The HUD shows the mix (`weather auto: Heavy_Rain deadlands 78% / ...`). `KWEATHERLOG=1` prints each group's chosen season and weather at startup, and each live change from the sim's WEATHER frames (then each group shows the sim's current weather, `../docs/CLIENT_GOALS.md` "Live weather"). `default` is the fixed pinned look (no weather fog, mult 1, clouds 0.25). Textures are looked up in the resource directories:
  `SkyX_Starfield.dds`, `CloudsTile.dds`, `moon_HI.dds`, `moon2_HI.dds` (a missing one is reported and drawn black).

## Sun shadows

Cascaded shadow maps (4 cascades, one 4096 x 4096 depth atlas, PCF), `shadow.{hpp,cpp}` and `shaders/shadow.inc`; terrain, buildings, features and foliage cast (leaves keep their alpha test) and receive; only the sun term of `light_shade()` is shadowed. `F3` toggles, `--no-shadows` starts with them off. The HUD's last line shows the state and the CPU ms of the shadow pass; `scene_test` JSON has `shadow_cpu_ms`. Technique, ranges and the game's own shaders: `../docs/reference/LIGHTING.md` section 7.

## Controls

Key summary (`F1` shows the same list on screen): WASD / Space / Ctrl / Shift move, `1`-`9` jump to scene N, `,` / `.` previous / next weather (auto, default, records; a manual one pins the regional blend off), `[` `]` / `P` / `T` time, `M` map, `F2` terrain view, `F3` sun shadows, `F4` sim squads (hidden by default), `+`/`-` minimap zoom, `G` walk, `PgUp`/`PgDn` foliage range, `R`/`F` building floor up/down, `F1` help. HUD last line and scene JSON `gpu_ms` / `gpu_<pass>_ms` are GL_TIME_ELAPSED GPU times (GLCORE only).

WASD move, Space/Ctrl up/down (fly), Shift fast, right mouse (hold) or Tab = mouse look,
wheel = fly speed, G = walk mode (clamped to terrain + eye height), Esc quits.
`M` toggles the full-screen world map (left click teleports there, keeping height above ground);
`F2` cycles the terrain view: ground colour / biome map / plain. `+`/`-` zoom the corner minimap (1 zone .. whole world). HUD top-left: position, ground, altitude, zone,
heading, nearest town (distance assumes 10 units/m, approximate). The corner minimap is a development view (`--debug` only); that top-left text needs `--debug` and the `debug.hud_text` option (Debug menu, default off; the F12 `.txt` keeps every line as `hud.<key>:`)
(the player-visible map is the Overview's MAP page; `../docs/PLAYER_GUIDE.md`).

The HUD's **Sim block** (a live `--debug` session only, `KCLIENT_PROFILE` on) polls the KSIM_PROFILE server about once a second while it is shown (order 120 `profile_query`, no query while the UI is hidden with F7 or the title is up; the `debug.hud_text` option only hides the drawing, the F12 `.txt` still has the lines): the tick mean / p50 / p99 / max against the server's tick budget, the per-tick characters / squads / path-query counts, a sparkline of the recent tick means and one row per `../docs/PROFILING.md` section, sorted by mean (`SIMLINK.md` "PROFILE"). A server built without `KSIM_PROFILE`, or a non-admin session, answers with one refusal word (`unsupported` / `not_allowed`) on a single line and no further queries are sent. `cmake -DKCLIENT_PROFILE=OFF` compiles the block and its polling out.

## Scripts (`--script FILE`)

Deterministic: fixed dt = 1/60 s per frame, unlimited chunk loads per frame, so runs are reproducible.
One command per line, `<frame> <op> args`, `#` comments. Input commands run at the start of the
frame and go through the real `event()` handler; `screenshot` and `quit` run after rendering.
Each screenshot prints one line: frame, camera pos, ground height, yaw, pitch.

```
<f> key W|A|S|D|G|SPACE|SHIFT|CTRL|TAB|ESCAPE|<letter>|F2|F3|F4|PLUS|MINUS down|up
<f> mouse DX DY            # relative move; only turns the camera while look is active
<f> mouse down|up right|left   # right button = mouse look, left = click (teleport on the full map); with --simlink: left selects, a right click without drag orders
<f> mouse pos X Y          # absolute pointer position, framebuffer pixels
<f> scroll DY              # fly speed
<f> set pos X,Z | alt H | yaw DEG | pitch DEG   # pos keeps the height above ground
<f> set particles N        # scripted particle step (s) after the pre-warm: 0 = frozen shot (default), > 0 advances it (verification shots)
<f> screenshot /abs/out.png
<f> quit
```

Sample: `scripts/smoke.txt` (replace `SHOTDIR` with a directory before running).

## Scenes and the scene test

`scenes.txt` (next to this README; `--scene-file FILE` overrides) holds named camera bookmarks. One scene per line, fields separated by TAB, `#` starts a comment:

```
name<TAB>pos=x,y,z<TAB>yaw=DEG<TAB>pitch=DEG<TAB>day=N<TAB>time=HH:MM<TAB>weather=NAME<TAB>ref=screenshot.jpg
```

- `pos=x,y,z` absolute camera position, or `pos=x,z` plus `alt=H` (height above ground). `yaw=DEG pitch=DEG` (yaw 0 looks toward -z, 90 toward +x), or `look=x,y,z` to aim at a point.
- `time` is `HH:MM` or decimal hours, `day` defaults to 1, `weather` is a WEATHER record name, `default` (the fixed pinned look) or `auto` (regional weather, below), `ref` an in-game screenshot for the contact sheets. `keys=F` / `keys=R` / `keys=FF`: floor steps (down / up) sent once everything has loaded, before the shot (the building under or nearest the camera). `build=P`: construction progress 0..1 of the building nearest the look point (else the camera), set the same way. Everything but the name is optional.
- Save-camera scenes (hub_stormhouse, shobattai_*) are the save's CAMERA pivot - forward x zoom (`tools/ksave.py dump`; the quaternion is w,x,y,z and the camera looks along its local +Z).
- Hotkeys `1`-`9` jump to the Nth scene (position, day, time, weather); scripts use `set scene NAME` and `set weather NAME`. `,` / `.` step through `auto` + `default` + every WEATHER record of the merged game data sorted by name (the list loads on first use); the HUD shows the weather and scene names.

### Scene test

```bash
client/tools/scene_test.sh [--bless] [--scenes NAME,NAME] [OUTDIR]
```

Runs `kclient --scene-test OUTDIR [--scenes ...]` through `headless.sh`: per scene it jumps, sets time/day/weather, renders until nothing is loading (buildings, features, foliage; at least 3 frames; 1500 frames = load failure), and writes `OUTDIR/NAME.png` and `NAME.json`
(drawn and resident counts: buildings, features, foliage, foliage cells, water chunks drawn, terrain chunks drawn/resident; CPU ms, load wait ms, failed mesh/texture counts, `sg_errors`, `hud_box`). Animation time and the clock are frozen, so repeated runs are pixel-identical.
With `KCLIENT_PROFILE` on (the default; `cmake -DKCLIENT_PROFILE=OFF` compiles the GPU timers, the submit counters and their JSON fields out of the client and the UI library), the JSON also carries `gpu_characters_ms`, `gpu_character_shadows_ms`, `gpu_items_ms`, `gpu_ui_ms` and `passes`: per GPU section (the pass timers of `src/render/gputimer.hpp`), means over the measured frames of draw calls, instances, triangles and pipeline / binding changes, counted at the client's submit wrappers (`src/render/drawstats.hpp`) and at the sokol_gl batches of the UI layer, whose pipelines and bindings are internal to sgl and read 0. A section with no submits that frame is left out; `ui` draws only with the interface up, which a scene shot hides, and a scene of 3 frames often ends before the GL timer's results are collected (`gpu_*_ms` 0.00, the counters still exact).
kclient exits non-zero on any `[sg][error]` or a scene that never finished loading. The wrapper then compares each PNG with the baseline in `~/.cache/kclient/scene-baseline/` (outside git; `KCLIENT_SCENE_BASELINE` overrides), ignoring the HUD box:
RMSE and the percentage of pixels with a channel difference above 16. `pass` = at most 0.5 % changed, otherwise `changed` (reported, exit code stays 0); `new` = no baseline yet. `--bless` copies the current output over the baseline.
It prints one line per scene and writes `NAME.sheet.png` (reference | baseline | current) and `overview.png` (all scenes) into OUTDIR (default: a fresh `/tmp/scene-test.*`). A full run takes about 10 s.

### Vsync and present pacing (kenshi-k16.2.418.1)

Every run prints one startup line (all builds): `kclient: swap interval asked 1 (gfx.vsync on, ...), in effect 1 via GLX_EXT_swap_control ...; render.thread=overlap, ...`.
"In effect" is what the GLX driver reports for the window, so `0` there with `gfx.vsync` on means the driver or a layer overrode it.

`KCLIENT_PACELOG=1` (dev builds) adds one `pace:` line a second, with no hook of libc, libGL or an overlay, so it reads the same with or without MangoHud:
`cb/s` frame callbacks (sokol swaps after each, so a callback is a present), `drew/s` those that ran a pass, the interval between callback starts
(mean, p50, p95, p99, max; steady clock), how many fell under 0.75 x or over 1.5 x the refresh period, and the vblank advance per callback from
`glXGetSyncValuesOML` as `0:n 1:n 2:n 3+:n` (`n/a` without GLX_OML_sync_control). `0` = two presents inside one refresh: the over-refresh case.
`1` everywhere = paced. To check on the real display:

    KCLIENT_PACELOG=1 client/build/kclient --debug 2> pace.log                 # without an overlay
    KCLIENT_PACELOG=1 mangohud client/build/kclient --debug 2> pace-mango.log  # with it

Set `render.thread=overlap` in the config for the overlap case; read the startup line first, then the `pace:` lines after loading. The HUD's fps is an
exponential average (0.95 / 0.05), so it hides spikes: judge by p50 / p99 / max and the `0:` count. `HEADLESS_REFRESH=N` (tools/headless.sh) pins
the nested gamescope's refresh for a headless run.

While `render.thread` runs a logic thread (lockstep or overlap) the line ends with two more sections (kenshi-k16.2.437.1):
`lockstep repeats N (runs N, longest L frames / M ms, P over 50 ms printed)` and `logic ms L vs bound B`. `repeats` = callbacks that found no packet
in time and blitted the kept frame back (`cb/s` minus `drew/s` is the same count), `runs` = runs of them, `longest` = the longest run and its wall
time (a run still going at the report counts its length so far), `printed` = the runs that printed a `kclient: lockstep: no packet for ...` line.
Only a run lasting over 50 ms prints, once, when it ends
(`logicthread::kMissLogMs`; at 144 Hz one frame is 6.9 ms, so a shorter run is one or two repeated frames, which is why the log used to spam).
`logic ms` is the mean of the world frames' logic half (`FramePacket::logicMs`, published by `logicthread::noteLogicMs`) against the bound the
render thread waits for a packet (`lockstep_bound_ms`: the refresh period, or the cap's step under `gfx.fps_cap` with vsync off): logic time at or
over the bound is why a packet arrives late.

### Per-frame profiling (dev builds)

`KCLIENT_STAGES=<ms>` prints one line per world frame to stderr (`src/app/cpustage.hpp`): the wall ms between the frame's stage marks,
`gap` = the time outside `frame()` (present, sokol events), `total`, and the HUD's `cpu`; frames under the threshold are not printed
(0 = every frame). `KCLIENT_GPULOG=1` appends each GPU section's ms and its submit counters (draws, instances, triangles) to the
same line — the GPU values are collected two frames late, so a timer never stalls the frame it measures. `KCLIENT_ABL=a,b` removes
work for upper-bound measurements (`src/knobs/ablate.hpp`: `charshadow`, `nosample`, `noprops`; `onedraw` measured the retired per-part draw and went with it in kenshi-k16.2.205.8; the picture is wrong by
design and a run with one set is a measurement, never a playtest). `--simspeed N` (with `--debug`) sets the sim's game speed.
kenshi-k16.2.205.7's report is the worked example: `kenshi-inspect/findings/kenshi-k16.2.205.7/REPORT.md`.
