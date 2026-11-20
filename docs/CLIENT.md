# Client (renderer) — decisions, open work

Running document for the client phase. The headless sim is coordinated
separately (`docs/STATE.md`, the sim coordinator). Keep this file current.

## Decisions (2026-10-30)

- **Graphics: sokol** (`sokol_gfx`, `sokol_app`, `sokol_glue`, `sokol_log`,
  later `sokol_imgui`). Code-only, CLI-driven, no editor. Godot rejected:
  editor-centric, its 4.x API churn, and the sim would sit behind a GDExtension.
  Ogre rejected: heavy, and Kenshi's real shading is D3D11 HLSL we can't reuse.
- **Backend:** `SOKOL_GLCORE` (GL 4.3 core, Mesa radeonsi) by default. sokol's
  Vulkan backend is marked experimental upstream; keep it buildable
  (`-DKENSHI_GFX=vulkan`) and switch when it behaves.
- **Shaders:** one annotated GLSL file per shader, compiled by `sokol-shdc`
  to `glsl430:spirv_vk`, output headers generated at build time.
- **Offscreen reads (kenshi-k16.2.79):** geometry is drawn with GL's NDC (y up) on every backend, but a render target's rows are stored
  bottom first on GL and top first on Vulkan / D3D / Metal (`sg_features.origin_top_left`). A shader that reads a render target, a depth
  texture or an atlas that geometry drew (a fullscreen pass, a projective lookup, the shadow atlas, the AO target) turns its NDC-derived
  (v up) uv into the target's with `offscreen_uv()` / `offscreen_texel()` / `offscreen_offset()` of `shaders/origin.inc`
  (`@include origin.inc`, then `@include_block origin` in the stage). It is the identity on GL (sokol-shdc defines `SOKOL_GLSL` for the GL
  target only): one GLSL source, no uniform, no hard-coded flip; `main.cpp` checks sokol's flag agrees. **New code never hard-codes a
  `1 - uv.y`** for this. Not flipped: a texture loaded from a file, `texelFetch` at `gl_FragCoord` (framebuffer coordinates, storage order
  on both), C++ uv rects into an atlas a pass drew (`Portraits::get` branches on `sg_query_features().origin_top_left`). GL-only readbacks
  (`glReadPixels`: screenshot, probe depth, the id buffer, luminance) are behind `SOKOL_GLCORE` and stay out of scope (k16.2.37).
- **Where characters stand (user 2026-11-18, kenshi-k16.8.29, review k16.2.338; solver k16.2.414.2):** the sim owns a character's navmesh
  point and floor (floors are their own navmesh islands, joined only by ramps; there is no jump), and the wire y is that navmesh point.
  The client lays every drawn placement from it onto the drawn geometry with ONE solver, `SimView::groundAt(x, z, navY, ...)`
  (bridge/simview.cpp; the rule is `solveGround`, k16.2.412.1's): the drawn **walkable** surface nearest the navmesh y at x, z, in a window
  (up 1.5, down 8), joined with the drawn terrain. Walkable = the navmesh bake's own set, a per-part bit decided at load
  (`BuildingPartDef::walkable`, world/buildings.cpp): a shell or interior part of a BUILDING whose `path mode` is WALKABLE (3; PROJECTED,
  OBSTACLE and IGNORE bake no walkable surface), not `is unwalkable roof`, `passable` or `no collision`; stairs included, furniture, masks
  and props never. So a roof, an awning or a table is never a candidate. Open ground (no walkable surface over x, z): the terrain at any
  distance (the coarse navmesh quad sits 12-33 u under the hill's dune). A walkable surface in reach but outside the window: the nearer
  to the navmesh y of it and the terrain (a body under a porch roof or on a low-navmesh bridge deck stays put). The terrain is excluded
  only inside an interior mask whose BUILDING has `interior terrain` 0 (`BuildingPartDef::carve`; the sunk dune). Callers: the
  body's feet (update, predict), the move marker, the route line; the ring follows the body. It may draw slightly off the sim
  (nudge: display only, bounded, eased, never fed back; k16.2.345). The client never re-snaps the y to the terrain heightmap by
  distance rules (retired with k16.2.339). User: "The client is just a presentation layer ... it has all the
  data it needs to place characters properly." Not done yet: the click through the same solver in reverse (k16.2.414.2 step 3).
- **One ground solver, both directions (user 2026-11-20, kenshi-k16.2.412; implemented in "Where characters stand" above):** the navmesh is approximate by design; the drawn
  geometry is the detail. Sim side: a position is 100% on the navmesh, the "true global position". Client side: every drawn
  placement from a sim position (body, ring, move marker, route line) corrects the navmesh point to the rendered geometry, per
  call. A screen click goes back the other way through **the same solver**: the drawn hit becomes a position the server
  understands (its navmesh point and floor), so the server never needs the detailed 3D geometry on screen. User: "you MUST
  translate a screen click by using the same solver as the standing height, and translate your coords into something server
  understands. Server shouldn't have to know about detailed 3d geometry on the screen."
  Then (2026-11-19 23:20): "Server side, navmesh *is* the world. Client side, real 3d geometry is the world. You just need to solve
  between them." Gotcha: filter the noise, so a body never lands on an awning, roof or prop overhead: the walkable drawn surface
  nearest the navmesh point and consistent with it, never the highest hit (walkable by the navmesh bake's own flags, NAVMESH_CACHE.md "Bake").
- **sokol API is newer than model training data** (2025 `sg_view` resource
  views). Agents read `client/third_party/sokol/*.h` and never write sokol
  calls from memory.
- **Language/build:** C++20, CMake + Ninja, same toolchain as `cpp/`. The
  client can link `kdata` later for records.
- **Verification without a human:** every milestone has a `--screenshot
  out.png` mode (render N frames offscreen, write PNG, exit) that agents use.
- **Agents never open windows on the user's desktop.** Run the client under
  `client/tools/headless.sh -- <cmd>`: stock `gamescope --backend headless`,
  full GPU (radeonsi verified), nested X display, child's exit code passed
  through. No synthetic input: this gamescope lacks libei, so no XTEST/xdotool.
  Next: a `--script FILE` mode in `kclient` (timed input events and
  screenshots), the only way agents drive the client; deterministic too.
- **Large world:** coordinates up to ±147456 units; render camera-relative
  (subtract the camera position on the CPU in double precision) to keep float
  precision.
- **Dev-only code is fenced (rule, user 2026-11-12, kenshi-k16.2.153; the sim's twin is ARCHITECTURE "Dev-only code is
  fenced").** Code that exists only for development, tests, playtests or scenarios (the playtest Notes / run files /
  verdict, `bd` calls, the SCENARIOS button and scenario loading, the animation lab, debug panels and dumps, the dev tool
  executables) lives in `client/src/dev/` (UI parts in `client/src/ui/dev/`) behind the CMake option
  `KCLIENT_DEV_TOOLS` (default ON). Game code reaches it only through thin hooks (`dev/devhooks.hpp`) that compile to
  nothing when it is OFF. Moved in so far: the playtest tooling's whole host side (`src/dev/devhost.cpp`: store,
  `--play`, `--bead`, Notes / Scenarios windows, scenario wire answers; it asks the game through the `dev::Env` main.cpp
  fills), `playtest.*`, `beadpost.*` (`src/dev/`), the animation lab (`animlab.*` and its host side in
  `src/dev/debughost.cpp`: the `lab` script op, its offscreen pass, its browser window, the debug panel's Animations
  row), the `dump` ops building, playerbuildings, light_bake, light_slice, floor, occluders, peek, windows, machine, trade
  and the `machine id=N` op (`dev::dumpOp` / `dev::machineOp` in `src/dev/debughost.cpp`, kenshi-k16.2.153.19), the `hints at X Y` /
  `hints kinds` ops (`dev::hintsOp`, `src/dev/hintsop.cpp`), the sky / weather / gpu / effects dumps and the `debug weather|weff|time` ops (`dev::debugOp`, kenshi-k16.2.153.21), the shot's debug sidecars (`src/dev/shotsidecars.cpp`: the `.txt` scene report, `.events.txt` and `.trails.tsv` beside a screenshot, and, with the debug view, the `.trees.txt`; the PNG itself is the game's, kenshi-k16.2.153.13), the vertical height probe (`src/dev/heightprobe.cpp`, Ctrl+F10 / `probe height X Z`: every surface the world-Y line crosses and the stand ray's window and pick, in the debug text, the scene and the shot sidecar; kenshi-k16.2.384.1), `kui_demo`, `kmeshdump`, `ksimlink_dump`, `kaudio_dump`
  (`src/dev/tools/`, executables built only when ON). An OFF build compiles and plays (title, new game, load); its check is
  `client/tools/dev_off_check.sh`. `KCLIENT_DEBUG_VIEW` (`src/debug/`) is the older model and stays. New dev code goes
  inside the fence from now on; what sits outside today moves under the children of kenshi-k16.2.153.
- **The launch guard (kenshi-k16.2.392, user 2026-11-19).** `kclient` and `kui_demo` refuse to open a window from a Claude session
  (any `CLAUDE*` variable in the environment) with no `gamescope` ancestor process — the tell of an agent run that skipped
  `client/tools/headless.sh`: one line to stderr and exit 1, before the game data resolve (its folder picker is a window) and any
  sokol setup. `KCLIENT_ALLOW_DESKTOP=1` skips it for a deliberate desktop run (it prints that it did). Dev-only: `src/dev/launchguard.cpp`,
  called from `sokol_main`; a release build (`KCLIENT_DEV_TOOLS` OFF) has no guard. Linux only (`/proc`); a no-op elsewhere.
- Physics later (Jolt Physics is the likely pick). No DeepSeek workers.
- **Format questions go to session `kenshi-b0`** (SendMessage), not
  re-investigated here. Formats already documented: `docs/formats/`.
  From 2026-10-30 `kenshi-b0` focuses on the server and batches pure-render exe questions at low priority.
  Render questions answerable from files on disk (shaders, .material, textures) are investigated by client agents directly, read-only; results go in `docs/reference/`.
  Anything the sim also needs still goes to `kenshi-b0`.

## Camera and control modes (user goal)

Kenshi is played from an angled top-down view; its free camera never follows a
character, so the world is mostly seen from above. We keep that and add a
close-up view of the world (`ARCHITECTURE.md` sim/client rule 4).

- **Command mode:** RTS / party-RPG style; select squads, issue orders.
- **Follow camera:** 3rd person only, on one character (user decision: no
  1st person; Kenshi needs a 3rd person view). Two modes (SIMLINK §12):
  1. Follow without driving: behind or in front of the character while its AI
     or its last click order moves it. Scenic only; sends no orders.
  2. Direct drive (below).
  Unlike the original, whose follow camera tracks position but never rotates,
  ours rotates with the character so the view stays its perspective. Smooth the
  CHAR_DYNAMIC yaw and look ahead along the CHAR_ROUTE waypoints, so the camera
  doesn't snap at every waypoint corner. If the yaw is too jittery, tell
  kenshi-b0 and the sim will smooth facing on its side.
  **Collision boom** (kenshi-k16.2.289.1; user 2026-11-15: a follow camera never hides the
  character; ours, all values MINE): a sphere of radius 3 u is swept from the head pivot (character
  + 14 u) back along the view to the wanted distance (`CollisionPick::sweep`, exact sphere-triangle
  contact). Solid world shortens the boom to just before the contact (0.5 u margin) at once, but never
  below the **minimum distance of 80 u** (kenshi-k16.2.289.4; user 2026-11-19: the pull-in left the
  character ~50 % of the screen. The body is 19.5 u, FOV 45 deg: 19.5 / (2 d tan 22.5) = 29 % at 80 u; a
  wanted distance below 80, the default 60 or the wheel, is the user's and is never cut): foliage shapes
  (rocks, trees) and building shapes that are not thin. What is still between the camera and the character
  at that minimum, walls included, fades in the occluder cone (16 u wide at 80 u, covers the body).
  **Terrain never shortens the boom** (user: the camera stands on the terrain): the eye rides at
  ground + 2 u at its own spot (rises at once, settles down at 30 u/s) and the view turns down to look at
  the pivot from there (`followPitchAdj`, undone each frame so the mouse pitch stays the user's). It eases back out at 30 u/s, only after it has held 0.6 s since the last
  blockage, so a post passed never makes it jitter. The ease-out is capped at what the sweep allows now.
  **Thin split** (`CollisionPick::thin`, by the shape's world box, extents sorted d0 <= d1 <= d2): a bar
  (d1 < 10 u: pole, rail, frame member) or a panel (d0 < 3 u and d2 < 60 u: a sign board) is thin; every
  other building shape is solid, and foliage shapes always are (the foliage pass has no occluder cone).
  Thin shapes never move the boom and never ghost a whole building (`lineHits` skips them while following);
  the occluder cone (mesh.glsl `xray`, the existing path, radius 16 u while following) dithers whatever
  building mesh is left on the line from the camera to the character, collision shape or not (a sign
  mesh with no collision shape included). Brief blockage: a pull-in is immediate (the camera is never
  inside a wall, not even for a moment); the ignore-and-hold rule applies to the ease-out (the hold).
  Scripted runs print `boom:` per frame (length, wanted, allowed, hold, camera, the blocking shape and
  its box). Test: `client/tests/drivecam/run.sh OUT spot|orbit|walk`.
- **Input while following** works like the original: a click or key is an
  ordinary immediate order at the front of the character's queue (e.g. a change
  of direction). When it completes, the AI re-evaluates and resumes its goals.
- **Per-character "AI off" toggle** (the original has a UI button): the
  character waits for orders instead of choosing goals. A non-terminal hold;
  AI on resumes normal behaviour.
- **Direct drive = autorun + steering**, as in the original (AI off, autorun
  on, rotate). Not a per-tick input stream. Decided in SIMLINK §12 (kenshi-b0,
  2026-10-31; `ARCHITECTURE.md` rule 4):
  - Order 20 `set drive`: `id u64, drive u8 (0 stop, 1 forward, 2 backward),
    heading f32`. No tick field: the server stamps every order, and event 9
    reports the applied tick.
  - Send it when the drive or the heading changes. The client maps W/S held to
    forward/backward and release to stop; a toggle key gives sticky forward like
    the original autorun. Mouse/A-D steering may send up to the tick rate
    (20 Hz). The server coalesces per character per tick, and the last one wins.
  - No resend loop, since TCP doesn't lose orders. Reconcile with
    `driveSeq u32` in CHAR_DYNAMIC (the last applied drive order). It arrives
    with the y/yValid layout change.
  - Forward/backward puts the character in the AI-off hold; stop stays in the
    hold. Any ordinary order clears drive. Backward = backpedal: facing
    unchanged, speed 0.5 for now (UNSURE).
- **Prediction:** the client predicts with the shared movement-step library
  (sim agent, SIMLINK §12): sliding along navmesh edges, plus "blocked" at a wall
  or cliff. It applies start/stop/turn immediately and smooths server
  corrections. A later navmesh tile stream for the full-detail area serves
  prediction and previews only; the server alone does pathfinding.

The M1 free fly camera is a debug tool. All input reaches the sim as commands.

**Camera modes (user, 2026-11-02, after a demo.sh run: "pretty much unplayable"):**
1. **Free look (debug only):** today's fly camera; a debug key, never a gameplay option.
2. **Floating (the game's default):** an RTS / god view tethered to a focus point on the ground near the squad.
   WASD pans the focus (speed scales with the distance), Q / E orbit, middle mouse drag (and LCtrl + mouse,
   controls.cfg `mouse_rotate`) orbits and tilts (user: Mouse3 changes the up / down rotation), `,` / `.` tilt,
   the wheel and Home / End zoom the distance. The camera floats over the terrain (never below it).
   Roof / floor slicing (kinspect camera_rts.md 6, the original's; user 2026-11-04): any player character inside a
   building (selected or not) slices it; the camera plays no part. One universal visible floor (the floor diamond, R / F)
   for every sliced building. **The floor view model** (`client/src/floorfollow.{hpp,cpp}`, `ff::FloorFollow`; the fable review kenshi-k16.2.236.9;
   `WorldBuildings::updateFocus` builds one sample of the camera subject per frame and shows the level it returns): one rule, no event fires once
   and is then trusted. While the camera follows a character (the V follow camera, or the RTS camera centred on one with F or a portrait double
   click: the original's follow, k16.2.236.1) the shown level is `manual` (a level R / F stepped to) if set, else the subject's *auto floor*;
   with the camera free it is the player's own level alone (h2v.24.6.2; the follow's end keeps the last shown level). The auto floor is resolved
   every frame, in this order, from the subject's SIM position (never the drawn, interpolated one; its y is the standing ray's ground since kenshi-k16.2.344): (1) on a flight of stairs: the stair latch's floor, in the
   flight's building whatever the wire says; (2) on a roof deck (the wire's indoors bit clear, `roofBuilding` names a building): that building's top
   floor; (3) indoors on the wire: the wire's floor; (4) outdoors: 0. The wire's bits are the sample at the *render tick* the position is interpolated
   at (`SimView::stateAt`), never the newest snapshot: the render clock runs a couple of ticks behind the sim and further behind after a stall, and the
   newest floor let a floor the feet had not yet reached settle the stair latch's origin (the climb's 0/1/0/1 flap under load, kenshi-k16.2.236.17).
   "On a flight" = the feet inside the flight's footprint and within 1.5 rises (`kOnFlightRises`) of its slope (the height interpolated from the foot
   tread to the top tread). The stair latch remembers `origin`, the floor the
   subject boarded from (first seen mid-flight: the wire floor): feet above top - 1 rise (`kDownLineRises`) show the upper floor, below top - 2
   rises (`kUpLineRises`) the lower, and between the two the floor that is not `origin`, so a climb flips two rises below the top and a descent
   one rise below it, and turning round mid-flight flips back; a flip holds until the feet are half a rise (`kHoldRises`) past its line. Events:
   the auto pair (building, floor) changing (a door, the latch, a roof, another building, a teleport), an engage (double click, portrait double
   click, F, V, Z) and a change of subject clear `manual`; R / F / the diamond set it (held until the next door, stair or roof crossing); a load
   restores the player's level and, with a restored subject, holds it as `manual`. The old rules (the snap on engage, the auto threshold written once
   per crossing, the stair hotspot and its quiet zone) are in `client/retired/floor_threshold_rules.cpp`, not compiled; `client/tests/floor_threshold/run.sh`
   (live, per-frame hidden check) and `kfloor_test` (the model alone) assert on the shown sequence. The diamond shows the player's level and only
   R / F / the diamond write it; a hovered building's peek and a click through it use that level clamped to the building's
   top (h2v.24.6), and a step while a building is sliced or hovered starts from the clamped floor. Nobody inside anything: the control is idle
   (no target, never a building nobody is in) and the next way in takes its floor afresh. Floors count from 0. No
   Hidden step any more (2026-11-04). Standing on a building's roof (over the interior bounds, in the parts' box xz,
   up to its top + 10) counts as inside, on the top floor (the Storm House's upstairs is its roof deck).
   Characters (and their shadows, name tags, the HUD's name labels) on a floor the cut hides are not drawn (Characters::culled,
   WorldBuildings::cutHides; user 2026-11-05: the guard on the roof at floor 0). One decision for the body, the debug tag and the
   HUD label, `main.cpp char_cut_hidden` (k16.2.264.1): the cut's geometric test AND the character's own wire floor over the
   shown level. The camera subject is the exception (k16.2.236.15): it asks the floor view model, never the wire (the wire's floor lags the
   model's view mid-flight on a descent), and is hidden only when the model's floor is above the shown level, which needs a manual R / F step
   above it; a followed subject is otherwise drawn on every frame (dev builds log `floor: subject hidden` per frame it is not; floor_threshold
   asserts none). Every other character keeps the wire rule — the client's derived floor table can put a storey's start below the deck the character stands on (the Outpost
   s-IV: floor 1 at local 123.93 under a deck at 139.35), and then the geometric test alone hid the body of a character on a floor
   the renderer draws while its label stayed (`dump cut id=N` prints both). A roof deck keeps the geometric test alone, and only with the
   wire's indoors bit clear: the wire's floor says nothing up there (h2v.24.1 a), while a single-storey building's box makes
   `roofBuilding` match its interior too. Peeks and occluder glass hide nobody.
   Occluder fade (ours; the original has none, camera_rts.md 7; a config key until the options handler exists): in
   auto-center and hover / drive, a building between the camera and the followed character fades whole to glass
   (base too, 0.3 s in / 0.4 s out, the peek's renderer) while it blocks (the segment crosses its parts' box; camera or chest
   inside skips it: by the interior bounds for a building with floors, none while ours are in it; `dump occluders`); the click ray passes it. `camera.occluder_ghost`
   0 = the old dithered cone (the user found it odd; A/B until the glass is confirmed). Terrain can't fade, so a hill in
   the way pulls the camera in. Foliage never fades (user 2026-11-20, kenshi-k16.2.411.4.1: a palm between the camera and the
   Wanderer ghosted): the fade is for big buildings, trees are not occluders in the original's logic either; a part with
   `BuildingPartDef::plant` (plants::isPlant) skips the cone. Rocks, ruins and structures keep the fade; the floor peek is separate (an interior plant hides with its floor like any furniture).
3. **Auto-center:** as floating, but the focus tracks one character (Z, or the number key twice). WASD drops back
   to floating at the current focus; rotation and zoom keep working.
4. **Hover / direct drive (ours):** as auto-center, but the camera turns with the character's movement; when the
   character isn't auto-moving, the rotation keys turn the character (direct drive, SIMLINK §12). Today's
   `debug.follow` mode is its start. Entered by V, Ctrl+C, or a double right click on one of your own characters (the
   second click drives instead of ordering, k16.2.238.1: the first keeps its normal order); left by V, Esc, or a double
   right click on the driven character. Every entry starts at the default close camera (60 u, -15 deg) whatever the RTS
   zoom was, leave and re-enter alike (k16.2.237 part 5), and a host-drawn `DRIVING` tag over the floor diamond shows
   the mode (the diamond has no drive half).
Defaults (MINE until kinspect measures the original): zoom 10..2000, start distance 400, pitch -45 deg, pan
speed from settings.cfg `camera speed=500` at `camera zoom=125`.
**The camera across a save (kenshi-k16.2.134.1):** a save holds no camera, so the client keeps one beside its other per-user files,
`$XDG_DATA_HOME/kclient/cameras/<server name>-<server data hash>/<save name>.camera` (`%LOCALAPPDATA%\OpenKenshi\data\kclient\cameras\` on Windows;
the per-user roots are app/userdirs.hpp, kenshi-379.31.3): mode, eye, focus, yaw / pitch, the distances and the
followed / auto-centered character. It is written when the client issues a save (the UI panel's SAVE, F5) and applied on that save's load at its
first snapshot after the reset (at once when the sim is paused: no snapshots come); a save without one loads as before. Deleting a save deletes
its sidecar. The client's tests/camerasave/ drives all four paths headless. Keys still to add (client/src/ui/keys.cpp, the UI
session's): `mouse_rotate` (LCtrl, Mouse3), a debug free-camera toggle, a gameplay key for the hover mode.
**The world map's view (kenshi-k16.2.435; a deliberate deviation from the original, below):** the same sidecar carries the MAP page's view as
`map_zoom` (pixels a map texel, 0 = fitted) / `map_cx` / `map_cz` (the world point at the view's centre), from `Overview::mapView()` at the save
and back through `Overview::setMapView()` with the camera at the load. A new game, or a load of a sidecar without them (an older client's), starts
at the default overview: `map_view_default` (main.cpp) computes the zoom so the Hub is at the view's centre and Stack just inside its top edge
from the two towns' TOWN records (not the discovered list) and `mapFrame().h`, applied by `map_view_frame` on the first frame the MAP page has a
rect; a missing town (a mod) falls back to the fitted whole world with one stderr line, and a save taken before the default applied writes no map
keys. `client/tests/mapsave/` drives it headless; `dump map` prints the view.

## Goals

Moved to `CLIENT_GOALS.md` (one section per feature): M1 — walk the bare terrain (current); M1.5 — orientation (where am I in Kenshi?); Supported mods (user-facing list: `client/MODS.md`); M2 — static world (current); M3 — sky, sun, day/night, lighting (2026-10-30; see Status); Regional weather (stages 1 and 2, 2026-10-30); Fog volumes (2026-10-30); Particles (2026-10-31); Dev UI and scene tests (user request 2026-10-30); Building interiors (agreed with kenshi-b0, 2026-10-30); Sim alignment follow-ups (ARCHITECTURE "Runtime state", 24aaf42); Characters (2026-10-31); UI layer (user decision 2026-10-31; owner kenshi-10); Construction (2026-10-31).

## Open work (the todo list; keep current)

- [ ] **Render thread** (user 2026-11-14, kenshi-k16.2.205.12): draw submission off the main thread; options, shadow caching, Vulkan, OpenXR and the start-now-vs-later cost are in `docs/RENDER_THREAD.md`.
- [ ] **Linear HDR pipeline** (user 2026-11-07): merged 0b9bf8c5 (render-tone: linear radiance, exposure in composite; baseline blessed). 2026-11-10 (k16.2.82): the original's display is the one base (tone clip, exposure key 0.55, no output encode); the AgX / Neutral curves, post.contrast and the post presets are retired (client/retired/), `--vanilla` is gone. Follow-up render-linear-2: particle/rain colour decode DONE (k16.2.80, 2026-11-10: the particle records/materials and their textures, and the character hair/vest/blood/armour tints, were display-authored tints on a now display-referred buffer; they are used raw, as the game's particles.hlsl, and `srgb.hpp` is retired — Fable 5.2 review items 1+2), display-colour audit of the other shaders, vanilla haze/flatness, albedo decode (rejected 2026-11-10, k16.2.82: the game never decodes; recorded in `CLIENT_RENDER_NEXT.md` 3.4).
- [ ] **Wind-aimed parts** (user 2026-11-08, shobattai_2258: the turbine head 180° off): aiming DONE 1d9eebaa (kinspect fdb6b8a live: local +Z = -wind in
  the building frame; nested parts turn with the head; scene `wind=DEG`; renders at 4 winds look right offline). Open: live check against the sim's windYaw
  (the sim's direction is its own seeded pick). Turn and ramp DONE (p4-carry, worldbuildings.cpp `advanceMotion`, mesh.glsl inst_vs): a wind change turns
  the part at a constant yaw rate, `rotation speed max` x a per-instance 0.95..1.05 (offline `set wind`: the ShoBattai heads turn over a few seconds,
  findings/wind-turn/sheet.png); function 3 spins at (W - min) / (max - min) of its speed from the live wind (MINE: that the factor scales the drawn spin).
- [ ] **Foliage wind** (kenshi-k16.2.420.1, user 2026-11-20): one live wind (`foliagewind.hpp`), smoothed in `advanceMotion` from the same `windSpeed` / `wind[]` as the blade spin
  and the aimed parts: strength = speed / `render.foliage_wind_ref` (default 30 u/s: the weather records' `wind speed max`, 30 for most, 60-100 in storms; the windmills ramp 10..50),
  direction = the wind's unit x, z, a phase integrated at a rate that rises with the strength. Unknown speed (an offline scene): strength 1, direction (0.8, 0.6); `set windspeed N|live` forces
  it. Leaves (`render.leaf_sway`, on by default, user 2026-11-20; off is the original's static trees): **plants only** — `plants::isPlant` (`src/world/plants.hpp`: a `leaves mesh` canopy, or the record's own mesh drawn with the
  vegetation FOLIAGE material (`material type` / `texture mode` 4) and no `collision` mesh; both trees and the scattered FOLIAGE_MESH rocks are FOLIAGE_MESH records,
  so the material is the only FCS field that separates them, kenshi-k16.2.420.6.1) — bend along the wind, trunk and leaves by one tree height (height^2), fronds and a
  small flutter weighted by the distance from the trunk axis; still at no wind. The records the rule gets wrong are data, not code: `client/data/foliage_plants.txt`
  (one line per FCS string id, `1` a plant / `0` static, and the reason; read once at startup, the dead trees listed `0`). Rocks, wrecks, ruins and props
  (`Foliage_1K_RockStack01`, the boulders, `LandCables01`, `Building_Ruins_Newer01`) never bend: the part's `plant` flag (set from the same predicate where the
  parts are made) is 0, which the occluder fade's foliage exemption will read too (kenshi-k16.2.411.4.1); `dump foliage` prints `sway 0|1 src material|data "<reason>"`.
  The diffuse texture's alpha cannot decide this: it is the gloss mask (a rock's texture is as non-opaque as a palm's), and the FOLIAGE alpha test cuts on the NORMAL
  map's alpha — the measured 748-record table is in the task's findings. Grass: the original's fixed +X, sin(0.3 pi rad/s x real time + 2.0 rad/unit x world x) (foliage_wind.md §2-3; the 2.0 is `frequency` times the vertex's x, so rad per world
  unit); `render.grass_live_wind` (off) makes it follow the same wind with our two-sine field. Dev: `KCLIENT_WINDLOG=1` prints one `windlog:` line a second (wind speed, foliage strength, direction, blade ramp).
- [ ] **Greenlands bridge** (user 2026-11-08, scene greenlands_bridge): our deck spans are not level with each other (each tilts, they don't join) where the
  original's are flat and continuous: geometry (placement rotation or sub-mesh transform), not texture. The original also has water under it; ours a dry bed.
  Compare: kenshi-inspect/findings/albedo-ab/bridge_compare.png.
- [ ] **Sunrise/sunset sky + horizon haze** (user's saves, scenes obsidian_sunrise / canyonsunset / riveroverlook1, weathers pinned to the game's live
  records; sheets kenshi-inspect/findings/albedo-ab/clouds_weather_sheet.png, dome_ray_zoom.png): inverted sun fixed 6f8039e8 (hue_clamp); dome ray length
  ~1 as SkyX_Skydome + the SkyX cloud layer with live uniforms 560771c7 (canyonsunset yellow sky, riveroverlook1 haze instead of a black band). Sun path
  matches the live uLightDir. The display is the game's clip 4a5a54af: at auto exposure obsidian's sky top #caf3e9 vs the game's #c8ebd7 and the white
  disc (user 2026-11-08 "looks nice now"; sheet obsidian_auto_vs_user.png). Cliff normal mirroring c962ee8e; scene shots deterministic 972dba04. Still:
  the sky is slightly cyan. Water sun = water.hlsl GGX with the raw sunColour f4f66ac1 (dawn glitter column back; raw sunColour confirmed live, kenshi-inspect findings/refs_shadow2.md).
- [x] **Spectator mode** (user 2026-11-07, via sim): no player faction (derived from the wire), follow any NPC (click = watch, Ctrl+C / CAST double click / `--follow` / `follow char` = follow), Watching panel goal; task waits on the CHAR_STATS AI tail. Merged 317bed09 + 1cb9f7ea.
- [x] **Playtest UI** (user 2026-11-07, priority; spec docs/reference/PLAYTEST.md; format CLIENT_GOALS "Playtest"): windows, store, run files,
  git hash, Ctrl+N, scenario wire (list / load / end / metrics) verified live 5510e5a8. Metrics on quit 7b31bbff. Open: the sim's tower exit + targeted_player fix, then the user's first playtest. Verified live f4ffb698 (bar_lure: targeted_player=0 unprovoked, run file + metrics written).
- [x] **Debug gate** merged 2e34a157: `--debug` gates F8, the debug keys, scenes and the debug script ops.
- [ ] **World particles** (user 2026-11-05, no fire in the lamps; plan CLIENT_GOALS "World effects plan"): slice 1 merged 10c62be9
  (effect data, .pu parser gaps, PNG textures, `dump effects`; nothing drawn). Slice 2 + 2b merged e02df5c0: flames (particleworld driver,
  PU Scale affector, sizes from data, no made-up constants) + building-record lights; `set particles N` for verification shots. Slice 3 c950461c: every
  placed script, sparks hidden at night, no fire view range (kinspect pu2). Blister Hill pot matches at the same hour (its rebirth.mod building LIGHT). The Hub street torch is lit in the original, day and night
  (kenshi-rwh.16, world_particles.md 11: FlameStarter_7, 4 techniques, live 3-5 billboards, flickering; the old "dark" captures were a screenshot side effect); ours
  draws it the same (kenshi-k16.2.11.4: a `set particles 0.0167` frame series at the Hub torch, flame up to ~85 px against the oracle's 89 px, flickering).
  Lightning (EFFECT type 7) draws (kenshi-k16.2.11.1): it was dropped by the soft-particle depth test because the peeked-glass layer (renderGlass) writes its nearest
  surface into the scene depth; while glass draws, the opaque depth is copied first (Hdr::copyOpaqueDepth) and particles, fog volumes, AO and heat haze sample the
  copy (Hdr::sceneDepthTex). The 1 s EFFECT handler is removed at its TTL but its psys is not: emission stops and the system runs its particles out
  (PlacedSystem::outlive), so the bolt column's flash lasts ~1.6 s spawn-to-gone and its Flames fireball and SPARKS spray are the techniques of the same script at
  the foot (rwh.120, checked by client/tests/lightning, kenshi-k16.2.177.1).
  Venge's sky beams (EFFECT type 9, kenshi-h2v.14.2.1) are drawn from the sim's BEAMS schedule (beamfield.cpp): the beam's ground point at the render tick from the
  wire's path (x, z, speed, segments; y snapped to the terrain), as the EFFECT's particle system — SkyBeam.pu's 200 x 16 000 u additive `oriented_common` strip
  (`light_beam.png`, interior-masked), the base flame, the dust blast and the mist — played as an event (no pre-warm). The 3 s advance notice is the wind-up: the
  beam is registered before it strikes, nothing shows during the lead (the original has no pre-strike visual), and the client drops it at endTick (its particles
  finish on their own, so the decoder forgets a beam `simlink::kBeamParticleTailTicks` after its end — the sim never reports an ended beam again and only a
  weather change clears a group, which would otherwise grow for the weather's whole life; the kMaxSize cap is 20 000 so the 16 000 u strip is not cut). The
  burst is one shot: an emitter with a `duration` and no `repeat_delay` runs once (kenshi-h2v.14.9.1), so the Flame1 foot fire is its 9 s of emission plus
  its 2 s particles (~11 s, rwh.175 2.6) and never burns under a beam whose strip is gone, while the strip's `keep_local` particles ride the placed system so
  the column stays a column at the wandering foot instead of a smear left behind it. The
  sim's scripts are suppressed from WeatherInstances' own placement, so a beam is drawn once; `dump beams`, `dump beamfeet` (each beam's foot against its
  system's techniques, the foot-fire check: `client/tools/beam_foot_check.sh`) and `camera beam [id=N]` are the debug hooks. Sound
  (`AMBIENT_SOUND Inferno`) is not played (no effect sounds yet).
- [ ] **Locomotion fidelity** (user 2026-11-06: no sliding or floating; run stops mid-stride then morphs to idle): plan in CLIENT_GOALS
  "Locomotion fidelity": 1 foot log, 2 legs from wire speed, 3 strafe timing, 4 stop on a planted foot, 5 inertialization; foot IK slice 1 (foot locking) in progress (user 2026-11-07).
- [x] **Screenshot debug trees** (user 2026-11-06; verified headless, client 089b8913: duel 2 chars + 1 squad, none missing): F12 also writes <shot>.trees.txt, the DEBUG_TREE of every on-screen character and squad
  (nearest first, cap 64, 5 s timeout, missing ids listed). Sim asked: N queries per tick answered? combat fields in the char tree.
- [ ] **Fight feel** (user 2026-11-06): every fighter floaty and sliding; KO snaps standing -> lying with no fall. Foot log for all
  fighters (worker-08). KO/death (kinspect ko_transitions.md): the original ragdolls, no fall clip; full ragdoll parked
  (CLIENT_GOALS "Ragdolls"); interim done: `anim.fall_blend` into lying, `standing up 3` on recovery (53cc37a5; a KO never wakes in the sim yet, reported). Segfault found: locomote null layer info, guarded 729661b1 (root open).
- [x] **Animal KO / death** (plans/animal-down): animals ragdoll their RACE record's `male`/`female ragdoll` .phs (no hard-coded race table), laid down from the standing pose, get-up blends into the live pose (riseLive). Merged f695b33a, verified live f695b33a (beak + crab dead/KO, wake; duel human hu_m_ragdoll_5 settles 0.06 u). Open: foreleg hit parts kick the pelvis (logged); animals settle 5-11 u off the rest point.
- [x] **Carried humanoid: pose + bone pin** (kenshi-k16.2.161.2, oracle rwh.98; user 2026-11-13 "carrying a person is HORRIBLY bad. The physics are way too loose"): the original never simulates a carried person. The body plays its `carry me` record (play speed 0.006, `reverse looping`, `delete weapons`) and its node is set from the carrier's upper arm each frame; a KO ragdoll is dropped at the lift. `Ragdolls` starts the carried-ragdoll path for animals only. Verified headless in slavers_downed (lift, walk, stop; findings/kenshi-k16.2.161/).
- [ ] **Playtest nitpicks** (user 2026-11-06): loading tip font/colour done (ui 9b0f0f52, master 23243f16); load, pause and speed sounds done (bbcbc2a7, 1ece5640:
  --simlink is an instant join with no title or menu music; menu pauses silent; the pause bus duck applied, AUDIO_ENGINE.md "Pause duck"); playtest glide fixed, client 6700d28f (a sim-cut swing held its full-body clip up to 1.2 s; it now ends on the wire's 0; foot log `log feet combat|all`); crawl (sim BF) verified live 6700d28f; a backing fighter skipped locomotion (frozen stride and action fade, the 5017 hold) fixed cc16eb6c (Spec::loco); roof cull fixed, client 4acf0f25: props above the shown floor hidden whole, a character on a stair counts by the stair's low face; a prop over a PEEK turns to glass with that building (db80aa08) and one on a stair counts by the stair's low face like a character.
  Done: sessions start auto-centred on the character 91c1388a.
- [ ] **Roof fight across floors?** (user playtest 2026-11-06, filed for later): hiding on the bar roof, the player engaged someone on the
  floor below. The sim's engage range is horizontal only (combat.cpp inEngageRange, MEASURED item 18). Check the original, then the sim.
  2nd playtest: hits land across floors (Storm House roof Ninja Guard, kclient-20261106-163058.png); sent to the sim 2026-11-06.
  Sim: inImpactBox/inThreatReach ignore height (box half-height 25, MEASURED 17); engage stays horizontal (original); task AQ. Verify live.
- [ ] **Client segfault after a bar brawl** (user playtest 2026-11-06, `client/tools/demo.sh` 156 = kclient): no core (ulimit -c 0), no
  log kept. Next repro: `ulimit -c unlimited` first.
- [ ] **Catch-up, legs from the drawn motion** (user 2026-11-05: nothing repositions without walking): 36da4fb4. Remote chars run to close
  a gap (max(100, 1.5x wire speed) u/s; snap past 400 u); legs follow the drawn velocity. Duel: backward shuffles step. Pure nudges unconfirmed.
- [x] **Exterior signs** (user 2026-11-05, Hub BAR sign): GAMESTATE_BUILDING `exterior layout name` joins `EXT-<building sid>-<name>`, drawn as shell
  parts at the building's range (325 placements, 257 joined); verified live on the Hub bar, client bf53e978.
- [x] **Ruined buildings** (user 2026-11-05, Hub rendered intact): squad template `building ruined` (276 placements) -> BUILDING_PART `destroyed mesh`;
  floor >= 1 parts without one dropped; no interior parts and all parts on floor 0 (6ba1b8a6: entering no longer un-ruins it). Rubble navmesh: sim (asked 2026-11-05). Verified live in the Hub, client cd2c46a7. Runtime destruction later needs SIMLINK section 9.
- [ ] **Inventory stats box** (`GearState.info`, UI 6934882f): rows only from wire fields, none sent yet (a5: pack weight + capacity, disguise,
  slave state). Order: free = Slave of above Looks like slave; slave state >= 1 = Looks like slave above Slave of (kinspect info_panel 7d6d1c8).
  Tips on Disguise and both slave rows; free = both dim "-".
- [ ] **Discovery radius too large?** (user 2026-11-05, filed for later): at a new game Stack is on our map, not in the original's. Ours is
  the sim's (discovery.cpp; kinspect map24_static.md: 8500, 17000 for type 2, 4250 camps/ruins; the camera counts too). Compare from the same
  start, then raise with the sim if it differs.
- [ ] **Map: NewDiscovered badge** (161404f3 dropped it from walk-by discovery; the original sets it only for map items, kinspect
  map24_static.md 14075f550): needs a sim event for map-item reveals. Ours (user 2026-11-05): the badge stays a few minutes after the first
  view, not until the map closes (a config key). Labels at zoom level >= 1 with `unexplored name` still to check live.
- [x] **RTS pivot on floors** (kinspect 2026-11-04, bar side-by-side): the focus rests on the highest drawn surface under it (down-ray from
  +3000 through the collision shapes, cut-away floors / roofs pass; camera_rts.md 3), else the terrain. Bar floor 1581.4, eye 1680 = the
  original's (3c40857). Anchors match the original (user's Steam shots 2026-11-04): follow = mid-body (feet + 10.8), detach = the floor
  (ours eases at 8/s, the original in 0.1 s). User finds the detach drop ugly: candidate (deferred, playtest): keep the eye, re-aim the
  pivot at the view ray's floor hit. Navmesh overlay on v9 (e5670ea): flags 0 skipped, Area::Edge magenta. The pitch clamp -66.9..11.5 is in update_rts since f2c6e13f (mouse drag included: it applies the next frame, before the draw).
- [ ] **Session front end** (user 2026-11-04; CLIENT_GOALS "Session front end"). Slice 1 done (89ee059): single player starts its own
  OpenKenshi-server (`LocalServer`: child process, `--listen 127.0.0.1:0`, the stdout listen line = ready, SIGTERM on quit, PR_SET_PDEATHSIG;
  `session.server`, `session.server_args` default `--scenario newgame` until a5's empty start); the title draws and streams no world;
  CONTINUE = the game on screen, else the newest save. A CONTINUE / NEW GAME / LOAD GAME / IMPORT GAME click while the server is still
  starting is held, one at a time (the newest wins), its button drawn pushed (`MainMenu::setPending`), and runs once the link is up and
  greeted; EXIT and OPTIONS run at once (k16.2.210.4; dev check `client/tools/title_pending_check.sh`, `serverhold` + `dump title`).
  Verified headless (`--local-server`, scripts/session/newgame.txt): title, NEW GAME,
  BEGIN, creator CONFIRM, Citizen at the Hub. Slice 2 done (af67771): the session loading screen (LoadScreen over the world frame,
  input but releases swallowed) from the camera's first placement or a reset until `world_loading()` (the scene test's predicate) and the
  new game have been quiet 10 frames, cap 120 s (loud); its own sim pause, sent only after NEW_GAME applied (the server applies it on a later
  tick). `session.loading_screen` 0 / 1 (not scripted) / 2. Verified headless with =2 (scripts/session/loading.txt); the streaming progress
  (peak pending) shows only interactively: scripted runs load synchronously. No world frame before a camera target (user 2026-11-12,
  k16.2.112.1: "loading into deadlands should be considered a spoiler"): the camera's default position is the Hub (-50619,-1086), never the
  world origin, and a joined or title-started session with no `--pos` / `--at` / `--camera-pos` streams and draws nothing (black, or the
  session loading screen a load put up) until the squad's first placement, a new game, a scenario's centre or a load places it; a local
  session's pre-game world does not count. No target after 10 s: the Hub default, logged (`kclient: no camera target after N s; Hub default`).
  `first_world_frame: x= z= reason=` leads the first drawn world frame in every mode (reason: default, option, squad, spectator, load,
  newgame, scenario, timeout or viewer). Next, after a5's runtime navmesh (a fresh server re-bakes the
  navmesh now, ~16 s; until then EXIT GAME keeps the server, and the title draws no world): 3. EXIT GAME ends the session (server stopped, sim view and characters reset; NEW / LOAD start a fresh one). Join / host menus deferred. The polished front end (title scene, server started on CONTINUE / NEW / LOAD) waits until past alpha (user 2026-11-07).
  Runtime navmesh (a5, sim af9c5ce, NAVMESH_CACHE.md "Runtime mesh"; supersedes the first-run world bake and its progress lines): the base is
  the game's shipped navtiles converted to Detour at load from the install; recipe bakes patch only touched tiles. Then the spawned server
  drops the nav args. A small patch cache may come (a5 sends its layout).
- [ ] **Mine-and-sell demo polish** (user playtest 2026-11-02, ROADMAP current milestone; for the user to playtest):
  - Run stutter / speed (4, 5): motion was drawn in whole-tick steps (render 2 ticks behind, but only the newest pair interpolated); every interest change rebuilt all characters and restarted their walk cycles. Wire 70.0 u/s, drawn 68-72 per frame (047d56e).
  - Feet (1): on the drawn terrain; the sim's y only more than 4 u above it (floors, decks), blended from 2 u (047d56e).
    A character the wire puts INDOORS is placed at the sim's y outright, like one inside an interior mask hull (k16.2.264.1):
    the Outpost s-IV's outer deck is no interior, and where the hill runs above it the blend clamps at 0 and draws the
    character up to the heightmap -- the user's 2026-11-15 dip under the outside walkway, id 943 drawn 38.4 u over its wire.
  - Selection never empties (2) (f569839).
  - Deposit hover (6): deposits are foliage rocks with no placement: the rock's own collision picks (a hit shape whose footprint holds the producer's origin), the whole drawn rock down to the ground (eefff28; server 6aa7aa4 puts them on the wire from the Hub gate).
  - Gate cursor (9): the door picks on its leaf's own collision, moved to the open pose while open (kinspect door_pick.md; `door axis` 2 = up by
    `door move dist`): the open Hub gate's opening walks, only the raised leaf takes the door cursor; closed+locked, the doorway shows lockpick
    (b82e2c9). House doors: slide along the shape's local `door axis` after `door comes out` along the leaf normal (door_pick.md door2,
    kworld shape frame 1010375; cc36012, Hub house door 36754 traced).
  - Foliage shadow flicker (8): the shadow light holds the sun until it moves 0.25 deg; unpaused static frames identical (d80f52e).
  - Selection marker (3): none under selected characters (the original's is the name tag only), no route line (debug keys markers.selection_ring / markers.route); the move marker is the game's position.mesh pin in its flat green, popping in and fading over 0.75 s as the original live (kenshi-rwh.5, kenshi-k16.1.1).
  - Tree crowns (7): FOLIAGE parts cut on the normal map alpha at int/255, none without one (kinspect foliage_alpha_threshold.md; the diffuse alpha is gloss); Yucca and ThinTree crowns drawn (85dc237). Open: trunks once wrongly clipped now show (a big pale tree, reddish shrubs): compare against the original from one pose; FOLIAGE lighting is still foliage.hlsl's up-normal, which kinspect says is unused (StaticObject).

- [x] Naruto run (user 2026-11-03): the sneak run, clip `ninjarun` (stealth mode 1) over crouch walk at 30-45 u/s (kinspect
  sprint_anim.md). Verified live ba64f17: Stealth 60 sneaker at 52 u/s plays ninjarun lower + upper at full weight.

- [ ] Inspect focus (user 2026-11-03; kinspect inspect_focus.md 8026a75): LMB a non-player (or a building) makes it the left panels' focus;
  orders stay on the squad. No range check: an unloaded NPC leaves empty frames, refilled when it reloads. Replaced by another NPC / building,
  a portrait or own character, a number key, select-all, a box catching a member; ground clicks keep it. NPC fields as for players (state,
  goal or "Aimless", encumbrance, stats rows, extended, medical, bounty): the sim must send them for the focus (ask sim). Client done 30c5395,
  CHAR_STATS rows 99e2c32 (verified live). RUN SPEED per kinspect run_speed_mph.md (8ec2698). Open: the skill-in-use row, a building as focus.
- [ ] Normal maps on statics: done for UV-mapped parts (render.normal_maps, see commit); gaps: DUAL's `normal map 2` not loaded (that side flat),
  script-material submeshes flat (their script normal unread). Subtle at RTS height; a paired close-up capture would confirm strength.
- [ ] Speed diamond walk / sync-to-slowest (f3d980c wires run only, via the RUN toggle): needs a sim order; queued for a5 (not a loop blocker).
- [ ] Occluder glass (user 2026-11-04; A/B `camera.occluder_ghost` 1 = whole-building glass, 0 = cone): verified headless behind the Hub Storm
  House (scripts/hidden/storm.txt, findings/client_focus/storm_glass.png; the centre pick reaches the Wanderer). User playtest pending.
- [x] Inside test from the wire: CHAR_DYNAMIC bit 19 drives the roof cut-away and the occluder inside test; verified live e9299ca1 (bar floor 0 in, deck out, NPC inside ghosts from outside)
  replaces our interior bounds (a porch counts as inside today) for the roof cut-away of the followed / selected character. Waiting on the sim hash.
- [x] Loading screen freezes the window during long load steps (user 2026-11-07; k16.2.26): the `characters` game data merge, the colour
  cache read, the detail uploads, the water PNG decodes and the simlink data hash run on workers or one part a frame; worst startup frame
  62-76 ms (was a 3.3 s step), session-load worst 64-87 ms, and a frame gap over 250 ms is an evlog `stall`. Numbers and the remaining
  named frames: CLIENT_GOALS "Loading screen responsiveness".
- [ ] Peek (user idea 2026-11-04, ours; deferred: playtest): `camera.peek` (default on). The cursor on a building with floors that no player
  character is in (not sliced, not a ghosted occluder) for 0.2 s cuts it at the player's floor level clamped to the building's top (the floor
  diamond's value, never written back: level 3 on a two-level house cuts at the roof deck, uncut), the cut parts as one glass layer (depth prepass, blend at EQUAL; renderGlass after water)
  fading to 25 % opacity over 0.3 s; the glass keeps its shadow (user 2026-11-05). Fades out over 0.4 s. The surface comes from the cursor ID pass
  (idbuffer.hpp, user 2026-11-05): the buildings the cursor ray's box test lists are drawn again over the scene depth (EQUAL) into an RGBA32F
  target (id, camera-relative position); one pixel read back through a PBO, 2 frames late. So only what is drawn counts (not terrain, a buried base
  or a collision box), and a hit under the ground floor (the base, outside steps; `belowFloors`) peeks nothing. Holds while the cursor stays on
  the building, its glass included. The click ray passes the cut parts (cutAway); a right press holds the peek (only a look drag drops it), so the
  release clicks through, and it holds while the cursor is on the UI (the floor diamond, always shown, greys with no target). R / F step the
  level from the clamped floor (0..top), which the peek then cuts at; nobody is inside anything while the peek is on. Black caps are left out of the glass. Verified headless (scripts/peek/idbuffer.txt + `dump
  peek`; roof: scripts/floors/roof.txt). Open: user playtest; height slicing of shell meshes that span floors (the Storm House's outer wall).
- [x] Lamp lights (a5 / user 2026-11-03; kenshi-inspect light_record.md): LIGHT instances on BUILDING_PARTs (`lights`) become point / spot lights
  in the shared light block (lighting.inc light_points: the game's piecewise falloff, cone, sun-style diffuse + GGX; buildings, terrain, grass,
  characters); indoor (interior parts, furniture) always on, outdoor only 22:48-05:12; nearest 16 (MINE cap; the game has none). Hub bar at
  23:50 matches the original's cone pools (findings/bar_items/ours_lamps_2350.png vs light_record/bar_d140_2330.jpg). BUILDING-record lights
  (campfire, Holy torch) since 1d30f903; no lit part is meshless in the merged data; Rebirth's City Lamp Posts light at 23:30 (findings/lamp-posts/).
  The hiver lantern (character light) is kenshi-k16.2.56.
- [ ] Owned items and objects (user 2026-11-03, findings/bar_items/): ALT labels of items we would steal get a red border; cursors over owned
  things: red magnifier (an owned container: search = steal), red lock (locked), red loot bag (an owned item). LMB on a building / container
  fills the left panels: name + owner faction, Town, Lock state / Lock complexity, Workers n/5, Efficiency, Max efficiency, a Condition bar.
  Needs ownership on the wire (ask the sim) and a building focus panel (UI).
- [x] Sitting / sleeping pose (SIMLINK flags 15 kSeated, 16 kAsleep): drawn from the CHAR_DYNAMIC pose tail (sim e7340dc, client 2d3c7a8d):
  the body at poseX/Y/Z with poseYaw and poseClip, snapped every frame to the furniture (no interpolation, no ground fit), weapons hidden, no
  posture layers; an older server (poseClip 0) plays sitting chair / sleepinbed at the sim's own position and yaw. Spec: kinspect
  findings/bed_seat_pose.md (ec9036d): seat = the furniture origin facing its nearest table or local +Z, bed = the use node with yaw = node
  yaw - 90, clip from BUILDING_FUNCTIONALITY `animation` (sleepinbed, sleeponfloor, sitting chair, sitting idle); the name tag anchors at the
  same point. No client-side nearest-furniture guess: the sim sends the snap point and the clip. Verified live 2026-11-07 (seed 1, Hub bar
  and Heng `--regions 9 4 11 6`, `dump bones`): Pacifier on chair 7718 at (-51186,1581,2630) is drawn at (-51186.31, 1580.77, 2629.94) = the
  chair origin, pelvis +6.33 above it, feet +1.26, head +13.08, yaw 2.089 against 2.078 to table 7710, clip sitting chair;
  four other sitters within 0.4 u of their chair origins and 2 deg of their tables. Heng: a Slaver Guard in a bed draws at (49342.75,
  1428.66, -42744.26), clip sleepinbed, Bip01 +6.73 and head +7.36 above the node (the spec's clip root 6.73 / head 7.36); yard sleepers
  (sleeponfloor) at their own spot and yaw, pelvis +1.46, head +1.11, feet +0.36..+1.16. Shots: findings/sit_sleep/bar_bones.png,
  h2690.png, h8735.png, h_yard_sleepers_top.png. Standing up moves the body from the seat point to the wire's stand point, at the bar 2 u
  up and 0.3 u across in one frame (no smoothing; not seen as a pop live).
  kAsleep is live since sim b505109 (seen 2026-11-04 in Heng, camera at 48030,-41953,
  server `--regions 9 4 11 6`): sleepers within 4 u of the bed's use node, on its floor.
- [x] Ground item meshes (a5 2026-11-03; ffee2fb, live headless): ITEM / NEST_ITEM records draw `ground mesh` else `mesh` at the wire pos, unsnapped,
  in the mesh's own orientation (BuildingSet::items via setProps); the sphere pick stays. Open: weapons / crossbows (texture + scale are the
  instance's MATERIAL_SPECS_WEAPON model, rest pose the sim's: asked a5), armour / backpacks (worn skinned mesh only: ring), item rest pose.
- [x] Tree leaves sway: the original's leaves are static (kinspect foliage_wind.md); ours `render.leaf_sway`, on by default by the user's decision 2026-11-20 (kenshi-k16.2.420; off is the original's static trees), merged.
- [x] Health panel to the default layout (user 2026-11-03): UI fd9ba8e (Blood / parts / Hunger rows, the original's bar layers) + host
  part order Head .. Right Leg; live 720p. CHAR_MEDICAL panel tail (sim 3c4b53a) decoded and wired: bloodMax, bleed arrows, hunger + fed / in bed,
  per-part max (wear) / bandage / splint / stump-crushed / robot; HealthText gains Starving, Bleeding, Hungry, Malnourished. Verified live
  (newgame, Dust Bandit fight: full Blood bar, "Bleeding", 1 '<'; `findings/scripts/med/tail.txt`). Not yet seen live:
  bandage, splint, a stump, robot = Oil, the in-bed and digesting flags.
- [x] Navmesh overlay (user 2026-11-03, debug): `debug.navmesh` / Ctrl+F5, 2e1f4ec navoverlay.cpp: poly edges
  within 900 u of the focus, cyan grid, rims and unlinked tile portals yellow, seams faint, doors green / red. Lines draw over everything
  (no depth); a real glow pass is a later nicety. Height bands (user 2026-11-04, 169b1e6): the selected character's level (else the focus
  ground) within `debug.navmesh_band` (20 u) full strength and its polygons filled; below dimmed, greyed, gone 4 bands down; above faint, hidden
  fainter where cut away (never hidden). Stairs / ramps (slope >= `debug.navmesh_stair`, 30 deg) orange within 4 bands whatever the cut; the
  surface under the selection flood-filled across neighbours < 10 deg apart; the hover polygon (the cursor's walkable test) white.
  **Its vertices are capped (k16.19.6.1)**: every polygon and edge within the 900 u fade used to go out with no bound, so a dense town could
  fill the default sgl context (then sokol's 65,536 vertices; 262,144 now) and silently drop the HUD, minimap and windows drawn after it. The overlay now stops at 56,000
  vertices (`kMaxVertices`, navoverlay.cpp) and reports the skipped polygons once a run (`sgl: navmesh overlay capped at 56000 vertices`), a
  `dump navdraw` trace line giving the frame's count. Measured with the overlay on: Heng 21,822 vertices, Blister Hill 20,365, the Hub
  12,240-12,430, Mourn 4,585 (every level in the fade); the cap is for the worst case, not a normal frame.
- [x] Collision overlay (user 2026-11-18, debug; kenshi-k16.2.383.1, wording kenshi-k16.2.382.4): `debug.collision` / Ctrl+F7, dev/colloverlay.cpp:
  the building collision shapes and the drawn terrain, round the focus with the navmesh overlay's 900 u fade and its own key beside Ctrl+F5 so the two
  flip over the same spot. White wire: the building collision shapes the CollisionPick gather holds — the barricades that block movement and line of
  sight, which bodies do not stand on since kenshi-k16.2.382.2; yellow: doorways (NavUse::Door); grey: the thin props (CollisionPick::thin); green:
  the drawn terrain bodies stand on, Heightmap::terrainHeight's drawn triangles on the 18 u lattice (each cell's two edges and its split diagonal), the
  ground k16.2.382.1 put the stand ray and the yValid-clear fallback on. What a body stands on at a point is the Ctrl+F10 height probe / `probe height X Z`. A debug-text line counts the
  shapes, triangles and cells drawn; capped at 90,000 vertices (the navmesh overlay's k16.19.6.1 rule). No depth test, like the navmesh overlay:
  indoors the terrain grid shows through the floor (read the white wires there).
- [x] **Whole-fight trails** (user 2026-11-10, debug; kenshi-k16.2.73): every drawn character's two foot contacts (the animation viewer's sole
  points, the lower of heel and ball a foot; not `dump feet`'s lowest skinned vertex), its sim centre (`SimView::simPos`, dashed) and its drawn
  centre (solid) at half the click box above the feet, recorded every frame and drawn floating in 3D over the scene for the whole fight (user
  2026-11-10: the paths are not laid on the ground), one colour a character (green-cyan-blue-violet; red and magenta are the warning's), feet
  dimmer than the centres. A frame move over 2.5x the faster of the track's own smoothed speed and the sim's, x dt, plus a floor (4 u for the
  centres, 15 u for the feet, just above the ~12 u a clip switch snaps a foot: measured 4-12.4 u frozen, 5-9.9 u animating) draws in the warning
  colour, three lines thick (a teleport or a snap; a foot's stride swing is its own speed and stays quiet); the sim and drawn centres are
  separate lines so the display slop can never hide a sim bug. `debug.trails` / Ctrl+F6; script `trails all | id=N | name NAME... | window
  SECONDS | clear | off`; `dump trails` prints the state and the warning count a track. Every shot taken while the trails record (script `shot`,
  F12) also writes `<shot>.trails.tsv` beside the .png: `id name t tick kind x y z warn`, one row a kept point, kind lfoot / rfoot / sim /
  drawn, the whole kept history (not the view window). Kept when a track moved over 0.5 u (at most 20 Hz) or 0.25 s passed, 640 points a track
  (the widest draw window, 30 s x 20 Hz = 600, plus a margin; ~5 MB with the 64-character cap; the points past it are `debug.trails_dump`'s), 64
  characters (`src/frame/trails.cpp`). **A player character keeps more (kenshi-k16.2.231.1):** 60 s, or the whole time since its newest move_to order
  when that is longer, capped at 120 s (2400 points a track at the top rate; the whole span goes in the shot's `.trails.tsv`), so a walk out of a
  building that took a minute and a half is still in the shot. Beside it a `--debug` shot writes `<shot>.route.tsv` (`shot_route` in
  `src/main.cpp`, `KCLIENT_DEV_TOOLS`): per selected character — every player character when none is selected — the wire's current CHAR_ROUTE
  (`route` / `noroute`: version, clientSeq, flags truncated / approach, arrival tick, every waypoint, the last as `goal`), the character's wire
  position, building and floor, and its last 8 move_to orders (`order`: seq, tick, goal, yValid, the click's building and floor, `outcome` = the
  route that arrived with that clientSeq, event 9 / 10, or nothing). What the wire does **not** carry for a stuck-at-a-door diagnosis, and the
  sidecar cannot show: the sim's nav / task status (walking, waiting, blocked), the reason a walk re-planned or was cleared, the snapped goal when
  the route is truncated (flags bit 0), and the door or portal a walk waits on (only the door's own INTERACTABLE state is on the wire).
  **The draw is bounded (k16.19.6.1)**: only the newest `debug.trails_seconds` seconds (1-30, default 30) of a track are drawn,
  at most 30 x 20 = 600 points a track, in the trails' own sgl context, so a full history can never fill the default context's 262,144 vertices
  and silently drop the HUD, minimap, labels and windows with it (the k16.19.6 bug). The context is sized 64 x 4 x (cap x 20) x 6 + 64 x 4 x 4
  vertices (922,624 at the 30 s default, ~25 MB) and reports "last draw N of M vertices" in the status; `trails window 0` now means the cap (not
  the whole fight) and a larger N clamps to it. After the frame's draws both contexts are checked and an overflow is logged once per change
  (`sgl: vertices full in the default context`). `debug.trails_dump` (off; needs --debug) writes the points that drop out of memory to segment
  files under `$XDG_DATA_HOME/kclient/trails/`: `trails-<date>-<time>-<pid>-NNNN.tsv`, one point a line (`id name track t tick x y z pop`),
  a new segment every 250,000 points; ~11 MB/min at 26 characters and ~28 MB/min at 64 at the top decimation rate (~90-byte lines), a quarter of
  that standing still. A "show all / one / last X s" panel is the UI session's (k16.3) — its WHOLE FIGHT button sets 0, which is the cap now.
- [x] **Click through visible stairs** (user 2026-11-04, found with the overlay): `cutAway` cuts by height, the renderer by part floor; a
  `WorldShape::solid` hit (stairs) now counts by its low face (the floor it starts on), walls still by height (one wall shape spans both
  floors, only the lower half drawn). Storm House grid: only the stair ray changed. Floors count from 0 (the original's ground floor is 0).
- [x] **The click picks the first drawn surface** (user 2026-11-15, kenshi-k16.2.259: "I clicked at the far end of the room on a valid navmesh
  tile … the click landed on something in the foreground that we can't see"): the click ray starts at the camera's near clip plane, not the eye,
  as the original's does (its `getCameraToViewportRay` unprojects z = -1; kenshi-rwh.142), and EVERY pick layer — world shapes, doors, producers,
  player buildings, items, containers, beds, characters — passes what is not drawn there (`see_through` → `WorldBuildings::cutAway`: a floor above
  the floor cut of every building, not only the one ours stand in; the occluder glass; a peek), so the click takes the first drawn surface along it.
  A collision shape the camera is inside is passed when it is not drawn at the camera either (MINE: the original reports such a shape at distance 0).
  A character is passed only where `Characters::culled` hides it. The F12 pick line names each skipped shape or thing and why
  (`pick skipped "container \"Water Tank\"" at … : above the level cut of …`), and its `pick:` hit is the picked thing's own hit.
  Every click (kenshi-k16.2.259.3, user 2026-11-17) is recorded at its own frame: the exact camera (mode, eye, yaw, pitch, dist, pivot, viewport,
  letterbox), the screen x/y, the pick ray, the full pick (kind, id, hit, building / floor, the terrain under the ray, every skipped shape and why,
  the buildings ghosted with their fade, the xray cone) and the sim's route end for a move. It prints to the run log as `click record:` (the
  first line is key=value words) and the F12 sidecar's `clicks:` block holds the last 6, newest first, before the cursor's own pick (F12 comes
  after the fact; the cursor is not the click). Script `click replay KEY=VALUE...` (the record's words) repeats the pick from that camera and
  prints MATCH / DIFFERS against the recorded pick.
- [ ] **Camera modes and controls** (user, 2026-11-02; design under "Camera and control modes"). Coded (main.cpp
  `update_rts` / `set_rts` / `free_camera`): the floating camera is the default on connect, with WASD pan by distance,
  a focus tether at 800 u from the nearest bar character, Q/E orbit, `,` / `.` tilt (-85..-5°), wheel / Home / End zoom
  and terrain clearance. MMB drag, LCtrl + mouse or RMB drag rotate. Auto-center: Z, a number key twice, the HUD focus or
  a new game; WASD ends it. V = hover / drive, and V again returns to auto-center. Ctrl+F = free look. All numbers are
  kinspect's camera_rts.md numbers since: pitch -66.9..11.5 (default -30), Q/E 54°/s, tilt 12°/s, pan 500 d / 600,
  wheel x0.75 / x1.25 a notch below 450 and +-112.5 above, 150 after a load, clearance min(0.2 d, 20) + 10 (by lifting
  the focus), leash 2250 from the nearest selected character, auto-center = the character + 10.8 with no lag. Roof
  slicing is done (WorldBuildings::occupants, above). Occluder fade done: `camera.occluder_fade`
  (default on), a dithered cone in mesh.glsl inst_fs from the followed character (+10.8) to the camera. It is widest at
  40 u and narrows as 0.35 x the distance back from the character, so the floor around them stays solid. Open: in drive
  mode, A/D turning the character while it stands. Main-menu zone banner fixed (b2f5394).
- [ ] **First playable demo: mine and sell** (user, 2026-11-02; sim design `docs/reference/MINE_SELL_SLICE.md`, b0 after T3b1; SIMLINK rows come to the client coordinator for approval). The client half, which waits on those rows:
  - C1 deposit pick (coded 7dff80a, not yet live): INTERACTABLE kind 4 (MINE_SELL 3.1, approved) with its tail; `Pick::Producer` = a non-door collision hit within kProducerMatch 50 (XZ, MINE) of the placement; cursor `mine` (kinspect item 59); RMB / hold menu "Mine" = order 60 action 4 (`use_producer`); hover label "<output> count/cap" (MINE). No bar for TASK_PROGRESS 9: the original shows none (item 59; its Goal line reads "Operating machine", not shown yet).
  - C2 deposit window (coded 7dff80a): `use_producer` sets `app.container`, so the output window opens when the operator arrives (MINE); the menu's Open does it without mining. Taking is 68 (drag) / 63. Money: PLAYER_SQUADS' trailing i64 feeds `HudState::money`.
  - C3 talk: RMB on a trader = action 6 `talk`, cursor `talk` (kinspect cursors.md C3 / C17); the TRADE frame opens the Gear's trade mode (UI done: `GearState::trade`, docs/UI.md trade.*).
  - C4 trade: Gear moves to / from the trader's grid become buy / sell; prices go in the tooltips; the trader's money in the window, ours in the HUD money box; refusals go to `ui::message` with the original's texts; the trade closes as the sim says.
  - C1 / C2 verified live (server ff4017c, 2026-11-02):
    - The run: new game → `move -52740 1910` → RMB on the Copper Resource (cursor `mine`) → `use … applied` → at sim speed 10 the ore shows in the 3x2 output window → a drag into the character: "take entry 0 x1 applied". Copper 4 kg c.180. 0 `[sg][error]`.
    - Fix: a producer's window opens within its `use range` (100) of its origin (`kProducerOpen`; MQ9), not the containers' 16 from the use point.
  - **The full slice verified live (server 048e3cc, 2026-11-02):** new game → mine 1 Copper → `interact id=1689 6` (the Barman) → the trade window (BARMAN, Money c.5,057, 41 entries packed) → the Copper tooltip (Value / Sell value c.163 muted, Avg. Price c.180, mark-up 91% green) → right-click sells: "put entry 4 x1 applied", HUD money c.0 → c.163, the trader's c.5,057 → c.4,894 → Escape closes: order 86, event 40 "client". 0 `[sg][error]`. The first talk from the deposit came back `unreachable` (b0's known west-gate walker stop); the second went through.
  - ITEM_STATS (SIMLINK, approved, b0 building): the client half is on master (cf2524d). It decodes type 29 into `decoder().itemStats[{id, version, index}]`, including the (g) fields; on a world reset it clears. `SimView::askItemStats` sends order 69 once per key, with a quiet applied. `gear_state` asks for every weapon and armour entry it shows. A refusal (stale version, or a server without 69: `unknown_order`) is not shown. Still to do: verify the layout against b0's build, then the tooltip rows (armour resistances, weapon damage), proposed with the UI session.
  - Verified live on server bc1ab82 (2026-11-02), 0 `[sg][error]`:
    - The walk back from the deposit takes one talk (7186aeb).
    - ITEM_STATS answers decode: the katana is kind 2 (cut 13, bleed 1.2, ap -0.3), the armour kind 1 (blunt / cut / harpoon / parts / class / material), trader stock kind 0.
    - The duel servant has kHostile: cursor `attk`, RMB sends attack. The barman talks.
    - The full slice sells at the top-anchored window coordinates: ore 845,78 → 682,264, then a right-click there to sell. Money c.0 → c.163. Script op `dump stats`.
  - ITEM_STATS tooltip rows (ui e317c57 + bf22e9b, merged) verified live on bc1ab82: Catun katana level 0.3, Cutting damage 0.81 (cutMult), Blood loss 1.20x, AP -30%, +4 / -4; Samurai Clothpants Blunt 6%, Cut 9%, Cut efficiency 50%, Harpoon 7 pts.
  - C3 / C4 coded against MINE_SELL 3.2, not yet verified live (the sim's trade is unbuilt; a server at 26016c9 answers talk `unsupported`):
    - simlink: TRADE type **53** (3.2's 43 was NAV_PARAMS; b0 confirmed 53) → `decoder().trade`. Event 40 clears it and drops `inventories[stock]`. Order 86 is `SimView::tradeClose`.
    - Talk: RMB on someone else standing = talk (6), Ctrl+RMB = attack, and the hold menu has a Talk row. This is a stopgap until the sim's hostile-to-player bit (b0, queued after trade); the original attacks hostiles on a plain RMB.
    - main.cpp: `app.tradeStock` makes the stock the container (no reach rule). Closing the window or opening another container sends 86, and `tradeClosing` stops the reopen until event 40.
    - `gear_state`: the caption is the trader's name in capitals, plus `trade{true, traderMoney}`.
    - Tooltips: TRADE's per-unit Value / Sell value (muted), Avg. Price and Price mark-up, only for entries whose INVENTORY version matches the TRADE.
    - The refusals go to `ui::message` with 3.2's texts.
    - Script op `interact id=N ACTION`.
  - The end-to-end check is the slice's step list, run headless by the coordinator.
- [ ] Playable loop, first end-to-end run (2026-11-02, client ecc6817, build-perf server; scripts in the client coordinator's scratchpad, recipe in `findings/HANDOFF.md`). Session 1 (duel): fight, RMB loot, quick move to ARMOR / WEAPON I, Escape (closes the loot window) + Escape menu SAVE GAME "Loop one", F5 quicksave, quit. Session 2 (a fresh server on the same save dir): Escape NEW GAME, confirm YES (a dialog before the panel), BEGIN / CONFIRM, the HUD with the new squad, a move_to applied, Ctrl+F9 quickload: Engival back with the looted robe and club worn; 0 [sg][error]. Gaps: a new game at the Hub has no one to fight (1 character on the wire) until world squads become Fighters at Full detail (b0's top sim item after T3b1; T3b1's residents are neutral townsfolk; meanwhile `--scenario squad` covers the fight); the load panel's LOAD path was not driven in this run (verified 2026-11-01); a drag-equip from main was not driven (quick move equips). First side-by-side demo candidate: the duel half (run it with `--vanilla`, like every comparison against the original).
  - **New-game fight verified live (server b90edca, client bf00530, 2026-11-02):** `--navworld` (demo.sh default), new Wanderer game (the picker no longer starts on a Hub beginning: 5 clicks right), `move -48881 -823` to the Dust Bandit squad. The tile swap works (2897 → 4833 tiles). On arrival the 15 bandits carry kHostile and attack on their own. The Wanderer goes combat → KO → dead, with blood. Barman trade unchanged. Gaps: loot after a win (one Wanderer can't beat 15; the duel covers it); world Fighters don't wander to the Hub yet (b0 queue). Script ops `dump chars` and `attack id=N` added.
  - **Loop re-run on b90edca + --navworld (client 2d9f8d1, 2026-11-02):** duel: `attack id=37401`, KO after about 2500 frames, `interact id=37401 5` (now through open_body) → loot window, `transfer take 0` x2 → robe worn, Escape (one window per press) → menu SAVE GAME "Loop one", F5. A fresh server on the same save dir: new Wanderer game (28 chars), Ctrl+F9 → Engival with the robe, the KO'd servant and the blood restored. 0 [sg][error]. Scripts: the coordinator's scratchpad nw/e (s1c, sv, s2b).
- [ ] Ownership (SIMLINK INTERACTABLE tail, sim a7a8704): owner / lockLevel / model / yaw decoded; `SimView::foreign` = owner set and not
  `ownFaction()` (SQUADS faction of a PLAYER_SQUADS squad), the theft-event set only for an older server; steal cursor on containers, the
  stealing item label on hover, ground items at their rest yaw. Live (newgame, `findings/scripts/own/dump.txt`): own faction
  204-gamedata.base, 24 Hub doors foreign with lock levels 12-88. Placement items (sim f4303e6): the Hub bar counter's cups / bowls / sake
  owned by 40551-rebirth.mod, on the counter (y 1591); hovering the Sake shows the stealing label (`scripts/own/cups.txt`). Open: the door
  lock level in the building panel, the ground weapon `model` mesh, the original's cursor over an owned item (ours: pick_item).
- [ ] Speech bubbles (SIMLINK event 41 speech / 42 speech_end, sim dd116d2, DIALOGUE slice 1): `SimView::speech()` keeps each speaker's line (text strid, duration, target, flags) until the duration runs out, event 42 or the speaker leaves; `speech:` / `speech_end:` log lines. `speech_frame` feeds the UI session's `ui::SpeechBubbles` (ui f62b978) per kinspect findings/speech_bubble.md: bottom centre on Bip01 Head (idle-stance height, cached per speaker) + 4 up, alpha saturate((2500 - dist) / 1500), the speaker's name tag hidden while it talks, not hidden by F7 (M). Verified live (2026-11-03, server master 26054f7, bar_crowd + --bake, `findings/scripts/speech/bar.txt`): "Somebody do something!" then "Guards!" over 36787, gone after their durations. Flags (sim d40102e): bit 3 large (large font, range x2, screen clamp), bit 4 important (range x1.33, clamp; a5 to set it from the important event list); the timer is now the exe's (4.7 / 2.8 s seen live on d40102e). Open: a large or important line, event 42 and the SHOO chain not yet seen live.
- [ ] Portraits (first cut done 2026-11-01, `src/portraits.*`, `shaders/portrait.glsl`; exe: `RTT_Portrait` 14084b890). Done:
  - **Atlas:** each character's head goes into a 128 x 128 cell of a 2048 x 2048 RGBA8 atlas (the original's `portraits_texture`, 140414380). It is rendered once and kept; a changed APPEARANCE version (or serial) re-renders 1 s after the last change (queuePortrait's timer, 140787550, re-armed by each change; the old image shows meanwhile). When all 256 cells are taken, the least recently shown cell is reused (MINE).
  - **Render:** into a 128 x 128 RGBA16F scratch with the character pipelines, then copied into the cell. Up to one portrait per frame; a scripted run renders all of them at once.
  - **The copy** (`Characters::portraitPose` via `portraitFrame`, in the logic half: docs/RENDER_THREAD.md, kenshi-k16.2.205.12.67; as 14084b890 does): the character unturned, no weapons (the original copies only the node's entities: body, armour, hair), posed in its `idle stance` clip at time 0 (FUN_140845b60) plus its posture clips.
  - **Camera:** looks at Bip01 Head + the RACE `portrait offset x/y/z` (Human 0, 0.5, 0; the exe scales the offset by a vf390 value, taken as 1). The eye is the target + Ry(yaw) x Rx(pitch) x (0, 0, RACE `portrait distance`, 10 for most races). FOV y 25 deg, near 0.1, far 400, aspect 1 (FUN_140845f30; kinspect item 9).
  - **Angle:** set by `portrait_serial` (u32, rand x 4e9 at creation, 1406223f0): yaw = ((serial % 500) / 1000 - 0.25) x pi, pitch = ((serial % 252) / 1000 - 0.083) x pi. The save also keeps `portrait_index` (u16, the atlas cell).
  - **HUD:** the squad bar shows them (HudMember `portrait` / `portraitUv` / `portraitSmp`, kenshi-10 abc2573). Verified live (squad scenario).
  - **Lighting** (kinspect item 9, findings/portrait9_static.md + portrait9_live.md; 2026-11-01): fixed, the same at noon and at night. The portrait scene has no lights, ambient, fog or shadows; rtt.hlsl lights it with deferred.material's lightDirection (1, 1, 2), lightColour 0.7, exposure 0.5 and the static IBL (mp_irradiance / mp_specularity). `Portraits::light_` feeds those to the character shader's lighting.inc terms; `renderPortrait` draws unshadowed. Transparent background. Verified live (squad): faces lit from the upper right, like the reference pair.
  - **Checking it:** the script command `set portraits 1` draws the first 4 cells top-left. Verified live (squad scenario): two heads at different angles, framed like the game's.
  Not yet:
  - The sim's own serial: b0 accepted it as CHAR_APPEARANCE `portraitSerial` (u32, last field, protocol 5, SIMLINK ac20299). Done 2026-11-01: `kProtocol` is 5 (server ee54f9a), the decoder reads it; verified live (squad): the HUD heads turn by the sim's serials. A hash of the id stands in only when the serial is 0 (MINE).
- [x] Reply menus (dialogue slice 2, sim 6e8ae61 / 1084e32; SIMLINK 43 / 44, orders 88 / 89, CLOCK paused bits): the decoder keeps open menus per (speaker, char), replaced on resend, closed by event 44 or the char's answer (41 bit 2), cleared on a reset; `dialogue_frame` shows the newest in ui::Dialogue (speaker's newest line, options, "[...]" for line 0, money, portraits); the pause key toggles bit 0 only. Verified live 2026-11-04, client 59f439d, `findings/scripts/dialogue/mug.txt` (forced starver mugging): menu, freeze held, reply 0 spoken, next "[...]" menu, end_talk = 44 reason 4. The dialogue freeze shows no PAUSED banner (765c5f6; the original's look for it is untraced, deferred). Reply target = the NPC since sim a718dad (verified).
  - Dialogue portraits: the UI side is ready (ui c93dfb7: DialogueState `npcPortrait` / `playerPortrait`, a ui::PortraitRef of view, uv, sampler). kclient does not open the dialogue screen yet (that waits on the sim's dialogue); when it does, fill both from `Portraits::get` like the squad bar.
  - Our version bumps on any appearance change; the original's trigger list is narrower (no re-render for a hip katana; findings/portrait9_static.md). Harmless: the image is the same.
  - Not checked live by kinspect: KO / limb / hair triggers, the yaw / pitch signs.
Fidelity, found against the live references in `scenes.txt` (`ref=`):
- [ ] Weather stage 3: time-varying weather. The static regional pick shows rain where the game had clear/dust (deadlands_border, foliage_dense, swamp_*). Biggest remaining mismatch.
- [x] Hub ruin state: done by the ruined-buildings item above (cd2c46a7: squad template `building ruined` -> `destroyed mesh`).
- [ ] Noon exposure: game sits at the MAX_LUMINANCE 1.2 clamp, ours ~0.94 (fog_dome). Not the luminance chain (exact port); near-ground snow is ~1.28x too dark in ours (LIGHTING.md 15). Needs a live capture without the HUD.
- [ ] Stars ~7x too sparse/dim (night_moons), with the traced UV and sRGB decode. Unresolved (LIGHTING.md 15).
- [ ] dust_red_dawn red-lit faces: not particles (ruled out); LIGHTING.md 12.
- [ ] Obsidian glassiness: highlight energy ~2x; not gloss (LIGHTING.md obsidian trace).
- [ ] Swamp: the tall yellow-green stalks and orange fronds are missing; foreground clumps a bit sparse after the grass fix.
- [ ] foliage_dense: left hill in shadow (boulder shadow from inside the rock?); game shows it lit.
- [ ] Shark town: grass on the town mud (no clearing rule found in the exe yet).
- [ ] Weather-driven dust is off (its size now follows the PU Scale affector, e02df5c0; kGrowth removed).
- [ ] Grass: our noise paint vs the baked overlay R byte differ (`--grass-map disk`); which one the game ends with is unknown.
- [ ] Water reflection near the bridge; `kReflection` 0.2 is a guess.
- [ ] Deliberate deviation, keep: a minimap (the original has none; user 2026-11-20, kenshi-k16.2.436): a frameless draggable square window at the top right, no text, five wheel zoom levels (default ~0.9 km across), on by default (`ui.minimap`); place and level kept as `minimap.x/.y/.level` in kclient.cfg. `ui::Minimap` (ui/minimap.*), wired in host/uihost.cpp; the old overlay minimap (`WorldMap::drawMinimap`, `Ctrl+=` / `Ctrl+-`) is deleted, not retired (nothing used it).
- [ ] Deliberate deviation, keep: the world map (M) opens at an area overview and remembers its view (user 2026-11-20, kenshi-k16.2.435). The original's map opens fitted to the whole world and keeps no view in a save; ours starts centred on the Hub with Stack just inside the top edge, keeps the zoom and centre across close / reopen, and writes them into the save's camera sidecar (`map_zoom` / `map_cx` / `map_cz`). The minimap's level (`ui::Minimap`) is separate.
- [ ] Deliberate deviation, keep: foliage drawn 6x the game range (no pop-in), so e.g. the RefineryHeads show in deadlands_border where the game culls them at 1000.
Features:
- [ ] Live sim link (`docs/formats/SIMLINK.md`, client half `src/simlink.*` + `src/simview.*`). Done (2026-10-31):
  - The decoder and order writer; `build/ksimlink_dump FILE.bin | --selftest`.
  - `kclient --simlink FILE.bin|HOST:PORT [--simlink-tick N]`: a replay at 20 ticks/s, or a TCP connection to `OpenKenshi-server`. The hello carries `kworld::dataHash` (it matches the server's).
  - One-command demo: `client/tools/demo.sh [--scenario duel|squad] [--port N] [--build] [-- KCLIENT_ARGS]` starts `OpenKenshi-server` (log in a `/tmp/kdemo.*` dir, waits for `listening`), runs `kclient --simlink` against it in the foreground, and kills only that server on exit / Ctrl-C. `--build` builds both targets first (`nice -n 10`, `-j8`).
  - The server binary the tools start (kenshi-k16.2.306.1): `demo.sh`, `live.sh`, `play.sh` and `client/tests/*/run.sh` take `KSIM_SERVER=<path>`; without it they ask `client/tools/shared_server.sh`. `client/tools/shared_server.sh` looks up `$HOME/git/kenshi/bin/OpenKenshi-server-cpp-<first 12 hex of git rev-parse HEAD:cpp>` first, from any branch or base, when the worktree has no uncommitted or untracked change under `cpp/` (the coordinator's deploy hard-links one per master deploy and prunes entries untouched for 3 days; a hit is `touch -h`ed so trees in use stay; `--explain` names the tree and the rule that answered); the merge-base lookup below stays as rule 2 for deploys that predate the cpp- links, and the wire-number rule (kenshi-k16.2.306.5.1, rule 3) sits after rule 2's cpp/ check: when this branch's commits since the merge base touch no `cpp/` file (rule (a) stays first: additive wire changes are only safe newer-server / older-client, so a branch that changes `cpp/` builds its own server), N = `kProtocol` from this branch's `client/src/net/simlink.hpp` and the newest (by mtime) `$HOME/git/kenshi/bin/OpenKenshi-server-w<N>-<hash8>` is handed out, `--wire-version` confirmed as N when the binary has it (`--explain`: `shared PATH (wN: ...)`, or `built (no wN binary, ...)`), before the merge-base `<hash8>` name. Bump `kProtocol` on any wire change, server and client together: rule in SIMLINK.md (docs/formats/SIMLINK.md) section "Protocol version"; the client refuses a server with another number at HELLO. Auto-cache (kenshi-k16.2.306.6.1): after `demo.sh` / `dev_off_check.sh` build ksim_server in a worktree whose `cpp/` is clean, `shared_server.sh --cache BIN` copies it to that cpp-<tree12> name (unique .tmp name, chmod 755, `mv -n`; skipped when the name exists; never for a build with uncommitted or untracked `cpp/` changes). Each entry is ~300 MB. Failing the tree key it answers with the shared master build the coordinator deploys to `$HOME/git/kenshi/bin/OpenKenshi-server-<merge base hash8>` when this branch's diff against its merge base with master touches no file under `cpp/` (all the server is built from) and that deploy exists (deployed under `$HOME` on both the desktop and ds; a host without one, such as dev, falls back to its own build); with no answer they build and use `client/build/cpp/OpenKenshi-server` as before. A shared server skips the `ksim_server` build and `demo.sh`'s stale-source rebuild of it; the run prints `server: shared PATH` or `server: built (REASON)` (`. client/tools/server_bin.sh`, `pick_server`). The title's own server (`play.sh title`, a run's `--local-server`) gets it as `--set session.server=PATH`. The deploy is the client lead's build, so its baked game data, sim data and `KSIM_SCENARIOS_DIR` are that machine's paths: the tools pass `--data` (the build's `KENSHI_DATA`) and `--sim-data cpp/data` with a shared server (`pick_server`'s `SRV_ARGS`; `session.server_args` for a `--local-server` run).
  - Characters come from CHAR_APPEARANCE (record sids -> names through `Characters::nameOf`; WEAPON items by section: hand = drawn, hip, back). The set is rebuilt when it, or an appearance version, changes.
  - CHAR_DYNAMIC between the two newest samples: position, yaw (shortest arc), fwd / lateral speed, combat / stealth, legs. Rendered about 100 ms of wall time behind the newest when live (2 ticks at 1x, 10 at 5x: the delay scales with the game speed, kenshi-k16.2.181.1). The y is ground-snapped while `yValid` is clear.
  - Actions start at their age (`Characters::action`).
  - The camera is placed once behind the focus (first player) character.
  - PING (type 4, SIMLINK 11) after 5 s with nothing sent. Soak: 60 s live, still connected (the server kicks after 30 s silent).
  - Verified: the sample replays mid-fight, and a live duel draws (headless shots).
  Next:
  - KO / dead / prone poses (done, from the CHAR_DYNAMIC flags): the game ragdolls (PhysX); we have no physics. MINE: KO or dead lies in the skeleton's face-down `crawl idle down`, frozen. Prone and moving plays `crawl loop` (the game's `crawling move`, locomotion category 5, is not modelled). The character is rebuilt on a change (`Characters::replace`), snapped with no fall, weapon sheathed. Checked on a 2400-tick duel dump (enemy KO at tick 74431). Later: a fall (dodgefall / stealthKO clips?), and a real ragdoll if physics ever comes.
  - Events: event 9 / 10 answers; hit splashes (done: event 1 with result 0 paints a terrain splash per BLOOD_DECALS.md 140793b20: hit direction +-50 deg, offset 20 x rand (UNSURE), size from cut + blunt + pierce, the victim race's blood colour; rand is a hash of tick and victim, so a replay paints the same; verified live in `--scenario squad`). Bleed drips (event 2), pools (16) and smears (17): decoded and painted (see Medical). Hits that happened before the client connected are not painted (the server keeps no blood state).
  - Orders by mouse (done): left click selects a player character (Shift adds; empty ground clears). A right click with no drag (< 6 px of mouse travel) attacks the character under the cursor when it isn't ours, else sends move_to on the terrain hit (Shift queues). Right drag still turns the camera. The first snapshot selects the player's characters. Picking: a vertical segment per character (17 units, 4 while down), reach 3. Ground: 4-unit march plus bisection on the heightmap. Events 9/10 answer by clientSeq, and the HUD's sim line shows the last answer; rejections go to stderr. Verified live: the duel player walks to the click and the server's order log shows the move_to.
  - Markers (done, `draw_sim_markers`, overlay, no depth test): a green ring under each selected character, its CHAR_ROUTE as a white line through the waypoints, and a 1 s fading ring at the last move_to click. When the route is an approach (CHAR_ROUTE bit1: the click was unreachable and the server walks as close as it can), an amber ring at the route's end replaces the click ring. Ground-snapped (+0.5) and split every 8 units to follow the terrain. Lines are 1 px (sgl); thicker quads later. The move marker (position.mesh) stands at the click point until the sim's route for that order (`clientSeq`) arrives, then at the route's own last waypoint — the target the sim walks to, never a client-side snap (kenshi-k16.2.259); the click and the answer are logged in ticks (`move marker:` line).
  - CHAR_ROUTE (type 23) is decoded into `Character::route` and drawn for the selection. Protocol 2 (b0 6f0e903): event 12 position_corrected decoded, `OrderWriter::drive` (order 20) written but not wired to keys yet. The markers still ground-snap; use the route's y (navmesh, valid with yValid) once decks and stairs matter. A rejected order (event 10, e.g. `no_route`) flashes the click ring red (verified live: a click on the Storm House wall got no_route). The amber approach ring waits on b0's next slice to be seen live. Direct drive (above, SIMLINK §12): driveSeq is decoded; drive waits on the movement-step library. Terrain y with yValid is coming from the server.
  - `set_view` / `set_interest` (done, `SimView::view`, once the camera has gone to the sim's characters): interest radius 2000 round the camera, re-sent after 250 units of travel; set_view after 1000 units (the server quantizes to zones with hysteresis). Their event 9 answers stay off the HUD. MINE: distance triggers rather than the client-side zone check. Still to do: the camera leash.
  - Follow camera and direct drive, first cut (done; `set_follow` / `update_follow` in main.cpp). C follows the first selected character; C again returns to the free camera. The camera sits behind it at 60 units (wheel zoom 10..2000, the original's clamp), pitch -15 deg, mouse look orbits. Its yaw is smoothed (2.5/s, MINE) towards the character's yaw, or towards the first route point 15 units ahead, so waypoint corners don't snap it. On a player character: W / S drive forward / back along the camera, A / D (2 rad/s, MINE) and mouse look steer, Q toggles autorun (UNSURE: the original's key). Order 20 goes out on a change of drive, and on heading changes over 0.01 rad at most at the tick rate. A move or attack click ends autorun (the server clears drive on any ordinary order). Verified live (duel): the server applies the drive (CHAR_DYNAMIC drive 1, heading follows), the character speeds up and moves; event 8 shows "drive blocked" on the HUD. Not yet: client prediction (knav::Walker needs navmesh tiles on the client), smoothing server corrections, a gait toggle.
  - The general picker (SIMLINK §13; the original's rule, measured (docs/reference/CURSORS_LIVE.md): one ray, the nearest hit wins, no layer priority; cursor and RMB default action from one decision). Done so far:
    - Bottom layer: `CollisionPick` (collisionpick.cpp) gathers the building and foliage collision shapes (`kworld::AreaGatherer`, the shapes the navmesh bakes from) for the camera's zone cell and its neighbours, on a worker thread (Hub: 22k shapes, ~170 ms). It skips doorway shapes, culls by AABB, then tests triangles. `ray_hit` takes the nearer of that and the heightmap march. A click on a wall now sends move_to at the wall's foot, not a point inside the building.
    - **The drawn body's y eases off the stair treads** (user 2026-11-19 after kenshi-k16.2.389, kenshi-k16.2.395.1: "Walking up the stairs is a jumpy
      jittery mess, because every new stair rung makes the character teleport to a new height. It needs to interpolate between current height and new
      requested height."). The sim's y and the stand ray's result stay exact (the floor model reads the wire y, k16.2.236.8.4.3); the DRAWN body's y
      eases toward the stand height (`anim.stand_ease_tau`, 0.03 s, rate-capped at 150 u/s), so a tread becomes a ramp of a few frames; a step past 8 u
      (twice a flight's own 3.9 u rise) is a fall, a ledge or a spawn and snaps. The follow camera's boom pivot has its own light vertical ease (`look.follow_pivot_tau`, 0.12 s, snapping
      past 4 u; horizontal follow and the look-around stay immediate). Picks, the selection ring and the name labels read the drawn y (they draw with it);
      foot IK's ground is the terrain's own height, so a slope's lag is the leg reach's to take.
    - **What the collision mesh is and isn't** (user 2026-11-19, verdict on kenshi-k16.2.389: "100% unusable for stairs"; it disagrees with the navmesh and a
      character can stand inside it, which is fine). It is the original's barricade set: it blocks movement sideways (`depenetrate`) and line of sight, and
      it feeds the picker, the camera boom and the occluder fade. It is **never** a stand height or a foot surface. Bodies stand on the drawn floors, stairs
      and ramps (`WorldBuildings::groundDrawn`, the stand ray, kenshi-k16.2.382.2), else the drawn terrain. Foot IK reads only the drawn terrain
      (`Characters::ground` = `terrainHeight`) and does nothing off it. Never add collision back to a stand or foot query. The rule, in the user's words (2026-11-19): "that stairway collision mesh is unusable
      garbage for any of this. It's floating above the stair. It's a great representation of 'crossbow shots shouldn't pass through here', but
      nothing else." Whether picks move off it too: kenshi-k16.2.396.
    - Characters (`SimView::pick`): a segment per character, vertical while standing, lying along the yaw (18 long) while down, 3 reach. `pick_nearest` (main.cpp) compares it with the world hit; the character wins up to 3 u past it (a click at its feet). MINE: segment sizes; the original's PhysX pick shapes are asked of kenshi-inspect.
    - Interactables (SIMLINK 6 INTERACTABLE, order 60, event 13; 2026-10-31): `pick_at` (main.cpp) is one nearest-wins pick over the world, characters, ground items (`SimView::pickItem`, a 4 u sphere 1 u above the item, MINE) and doors (the doorway collision shapes, `CollisionPick::ray(..., doors)`, matched to the nearest door interactable within 150 u, MINE). A container that a building's interior layout draws is picked by that piece's own oriented mesh boxes (the wire's kind 3 tail names the building and the piece, `WorldBuildings::setProps`): any visible part of a shop counter picks it, including a face the piece's collision proxy hugs a hair nearer than its mesh boxes (the click lands on the drawn box, `WorldBuildings::propBox`, so a wall really in front still wins), and a container on a floor the cut hides never picks, as a bed (k16.2.239.1). Cursors: pick_item; door / lock (locked) / spanner (damaged). RMB: pick up (7); open (1) a closed door, close (2) an open one; a locked door sends nothing (lockpick later). Ground items are drawn as a small ring (no meshes yet) with the name label (`Bread x3`) while ALT is held or hovered (MINE until kenshi-inspect measures the labels). The HUD line counts things and nav tiles. Verified live (squad): 24 things and 324 nav tiles received; labels show; pick_item cursor over the Katana, RMB pick up applied and both selected walk to it. Not yet seen: the pick-up completing, a door click. Event 13 goes to the HUD reply.
    - X toggles sneak for the selection (order 10 toggle 1; on unless all of it already sneaks; MINE key until the UI's sneak button). Verified live (squad): applied on / off, the follower walks with the sneak animation (CHAR_DYNAMIC stealth flag picks the anims).
    - RMB on one of ours while someone else is selected: follow (order 3, `SimView::follow`, the rest of the selection follows it), cursor `green`. Verified live (`--scenario squad`, b0 4a99964): green cursor over the leader, follow sent, then the leader's move_to: the follower trails it to the spot. Script note: releasing RMB unlocks the mouse and headless X reports the pointer at (0, 0); re-send `mouse pos` before the next click.
    - Hover cursor (cursors.cpp): the original's MyGUI pointers cut from Kenshi_UI.png, drawn in software as the last overlay draw (the original draws its own pointer too), sampling the UI layer's atlas (`ui::gfx::atlas()`, ~8 MB saved) when the UI is up, else its own copy (the loading screen keeps its own: it runs before ui::init and is freed after loading), the system cursor hidden while it shows. sokol's X11 custom cursor (`sapp_bind_mouse_cursor_image`) segfaulted in XFlush under the headless gamescope, so it is not used. For now: `attk` over a character not ours, `green` over ours (follow), `arrow` otherwise; this matches what RMB does.
    - Character cursors and RMB (2026-11-01): `char_act` (main.cpp) is one decision for both, after kinspect findings/cursors.md rows C1-C22. Ours: dead search / loot (5), hurt med / first aid (10), KO'd and not carried lift / carry (8) when someone else is selected, carried by the selection lift / put down (13). Anyone else: down or dead search / loot (5; C22's scavenge vs search is UNSURE), else attk / attack. MINE: ours otherwise green / follow while someone else is selected. Verified live (duel_katana): search over the KO'd servant, med over the hurt Engival; RMB on the servant sent loot, the server replied done. Loot open (kenshi-10 ui 807f184, merged 2026-11-02; sim b0 3748429): the loot cursor / menu Loot sends 60 action 5 flags 1; the body's INVENTORY opens as a second Gear (`app.lootGear`, grids kGearOther + n, the full character layout) docked right of the looter's; worn pieces strip with 68 take 1 (section 255). Verified live 2026-11-02 (duel): RMB on the KO'd servant opened ESCAPED SERVANT beside ENGIVAL; dragging its worn armour and Iron Club into our main grid applied ("take entry 0 x1 applied"), both left its slots, 0 [sg][error]. Take-all (flags 0) stays as the container menu's "Take all" and strips too (b0's row, pending kinspect's loot capture).
    - Still to do: building cursors (the building's own cursor, use/door/…; build when unbuilt), the owned-item steal cursor (I2: the owner is not on the wire), talk / knockout / cage rows (wait on the sim), invalidmove (done: no client navmesh on the hit's vertical line +-5000, never over a building shape, per kinspect cursors.md "Wall hover"; scripts/navmesh/bar_hover.txt), dropped-item name labels (the only hover-recoloured world label; the original has no outline or hover tooltip, names show in the hold-RMB menu), and the hold-RMB menu. Small objects are hard to hit in the original too: the user zooms in, or holds ALT to show the item labels and clicks a label. Labels are click targets that queue pickups (user, confident: ALT mode exists for that); plan them as a pick layer. Asked of kenshi-inspect: the label hit area, whether labels show only with ALT, Shift queueing on labels. Future extension (user, 2026-10-31; MINE, off by default): an options checkbox to show name labels for characters and/or machines too, clickable like item labels. These wait on the server's entities (§13 `interact`). move_to sends the click's y with yValid (protocol 3, done).
  - The HUD line (done, `SimView::status`: tick, characters, selection, last answer).
  - A drawn weapon's sheath looked blade-like in the live shot (check).
  Client duties beyond decoding:
  - ~~leash the camera~~: dropped. kenshi-inspect measured no roam leash in the original (a free camera parked 16000 u away stays, streaming follows it; SIMLINK §5, d6a7c50). Detail follows the camera via set_view;
  - send `set_view` (order 51) only when the camera's focus zone (4147-unit zones, zone + 8 neighbours, the original's re-centre hysteresis) changes. It is a logged sim command: small moves send nothing;
  - draw `lowDetail` characters as distant figures or squad markers, never with full-detail actions; respect the server's full-detail bounds;
  - send `set_interest` as a pure send filter (default 1 km).
- [x] **Building loading stand-in** (kenshi-k16.2.258.4; the character pattern of kenshi-k16.2.258.1, user 2026-11-19: "If the load delay is just textures and lighting, then we can
  use the same pure-black body trick as the character loader. It shows that SOMETHING is there."): per instance, all or nothing (`WorldBuildings::update`, the pack loop). Its
  shell meshes not all through (Failed and fileless meshes never hold it): nothing drawn, no footprint box. Meshes through but a texture still streaming (the 298.1 per-part
  test `partTexturesReady` stays the completion test): every part draws flat black, unlit, no texture, no shadow (`Group::standin`, mesh.glsl `inst_fs` `misc2.y` = 1, the
  character silhouette's look). The last texture ready: the whole instance dithers into its real look over 0.2 s (`kStandinFade`; the real parts in `Group::fade` where
  `ign < cover`, the black where `ign >= cover`, so one pixel is never both); never a frame of black and textured parts together outside that switch (`Stats::mixed`, `dump popin`'s
  MIXED line: 0). `render.loading_standin` is the character's switch: silhouette = black; double has no building meaning (a neutral lit body is a character's identity-hiding
  trick, a wall has none), so it is black too; off = per instance, nothing until every texture is ready, no mixing. An instance that cannot stand in (peeked / ghosted glass,
  under construction) waits whole like `off`. TERRAIN-mode feature parts (stretch w is their biome row) switch without the dither. `dev.building_standin_hold` (dev
  tools) keeps every building black until that many seconds after the first update (a script sets it to 0 to release them). Trees and leaves (foliage.cpp) keep the 298.2 per-part wait.
- [ ] Interiors: lighting and doors (`docs/reference/INTERIORS.md`).
- [ ] Construction, later (polish, after a working game): progress from the sim (needs a SIMLINK building record), placement ghost, per-piece assembly, 3D scaffold. See "Construction" above.
- [ ] Rain splashes, mist, ash, lightning; rain clipped indoors.
- [ ] Vulkan backend untested.

## Light terms (the scene light debugger, kenshi-jj3.27.1)

Every light contribution of the frame has one switch: a bit mask of the terms that are OFF, dev state of the run (never saved, not a cfg option). It rides in the shared
light block (`lighting.inc` `pl_head.w`, an integer in a float; 0 = all on, so a zeroed block and every release build read all-on) and in `FramePacket::lightOff`: logic
sets both at the end of `logic_frame` (`dev::lightOffMask()`), the draw reads only the packet. Code: `src/dev/lightterms.{hpp,cpp}` (KCLIENT_DEV_TOOLS; a release build
compiles the mask to 0 and the lamp list to nothing). API: `dev::lightTerms()` -> `{name, group, on}` rows (then one `lamp:<instance>.<l|o><index>` row per lamp of the last
frame's view, nearest 64), `dev::setLight(name, on)`, `dev::soloLight(name)`, `dev::allLights()`. Script / console: `light <name>|all on|off|solo`, `dump light` (the table),
and the F12 / `--screenshot` sidecar line `light: all on` or `light: off: a, b` (`.txt`, after `weather:`).

Off = the term adds nothing, the rest of the frame is unchanged. Solo = every term off except this one and what it needs to show (the `needs` column); post terms need the whole lit scene.

| term (group) | needs for solo | where it is added (file:line) | off means |
|---|---|---|---|
| `sun_direct` (sun) | - | `lighting.inc:86` `light_shade` sun term (also `:168` `_f0`), sun specular `:122` / `:197` `light_spec_sun*` | no sun diffuse, no sun glint |
| `sun_shadows` (sun) | `sun_direct` | shadow factor in the same two functions; the lookup is `shadow.inc:95` `shadow_visibility`. The cut-roof casters are NOT a separate caster set (one cascade set), so no separate row | the sun lights unshadowed |
| `sky_ambient` (ambient) | - | `lighting.inc:87` / `:169` the irradiance cube / SH (`light_irradiance` `:71`), x `amb_mult`, x the daylight clamp | no ambient irradiance (`post_indirect` and `sun_bounce` stay) |
| `ao` (ambient) | `sky_ambient` | `lighting.inc:352` `light_ao` (`.a`), the previous frame's `ao.glsl` AO, reprojected | AO factor 1 |
| `post_indirect` (ambient) | - | `lighting.inc:352` `light_ao` (`.rgb`), `ao.glsl` il target, added into the ambient bracket `:87` | no screen-space indirect |
| `sun_bounce` (ambient) | - | `lighting.inc:400` `light_vol` bounce out, `:438` `light_bounce`; called from `mesh.glsl:412`, `character.glsl:221` / `:294` / `:350` | no baked sun bounce |
| `interior_light` (ambient) | `sky_ambient` | `lighting.inc:400` `light_vol` factor, applied as `ao.a *= vol.x` (`mesh.glsl:415`, `character.glsl:224`) and `ao_spec` | factor 1: no probe darkening indoors. `render.interior_light` (default 0.25) is the amount of original lighting kept indoors; the factor's strength is 1 - its value (1 = the factor off, no bake) |
| `env_specular` (ambient) | - | `lighting.inc:104` / `:182` `light_spec` / `light_spec_f0`, the specularity cube | no environment glint |
| `lamps` (lamps) | - | `lighting.inc:129` `light_points` (terrain `terrain.glsl:185`, mesh `:419`, character, grass `:88`); the list is `worldbuildings.cpp` `packLights` | no building light; `lamp:<id>` rows drop one lamp from the packed list (the next-nearest takes its place) |
| `emissive` (emissive) | - | `emissive_on()` `lighting.inc:30`: mesh glow `mesh.glsl:418`, glowy eyes `character.glsl:226`, water glow `water.glsl:171` | no self-lit glow |
| `fog_atmosphere` (fog) | - | `lighting.inc:288` `light_finish`, `atm_fog` (also `sky.glsl:221` below the horizon) | no distance atmosphere fog on surfaces |
| `fog_weather` (fog) | - | `lighting.inc:288` `light_finish`, the weather fog / fog map | no weather fog |
| `sky` (sky) | - | `sky.glsl:221` above the horizon (dome, sun, moons, stars, clouds) | black sky; the below-horizon fog colour stays |
| `cut_band` (marker) | - | `mesh.glsl:421` the warm band in the 3 units under a roof cut: a marker the cut adds, not a light of the game (the band at the top of the Storm House walls, jj3.26.6) | no band |
| `sun_shafts` (air) | - | `sunshafts.glsl` pass (`renderframe.cpp` after the AO) | pass skipped |
| `dust_motes` (air) | - | `dust.glsl` pass | pass skipped |
| `fog_volumes` (air) | - | `fogvolume.glsl` pass | pass skipped |
| `bloom` (post) | the lit scene | `post.glsl` bloom_* / `scene_comp` (`Post::settings`) | bloom strength 0 (nothing to remove while `post.bloom` is 0, the default) |
| `exposure` (post) | the lit scene | `hdr.glsl` composite (`renderframe.cpp` `hf.fixedExposure`) | fixed gain 1: no auto exposure / eye adaptation (not "zero": the scene is shown unexposed) |
| `post_grade` (post) | the lit scene | `post.glsl:45` `grade()` (shoulder, black point, vibrance, warmth, clarity; `Post::settings`) | grade neutral; the display clip (`tone.inc`) stays, it is the original's display (nothing to remove while the `post.*` grade options are 0, the default) |
| `lamp_beams` (air) | `render.lamp_beams` > 0 | `sunshafts.glsl` pass, the lamp loop (`renderframe.cpp` after the AO; own switch, so a solo of another term hides it) | no cones (the sun shafts of the same pass stay) |

Not terms: wetness / dust albedo (`light_wet`, a material), the particles' own colour, SMAA / sharpen / vignette / letterbox (image effects), the water reflection pass (it
stores albedo luma, no light).

## Critical-events log

kenshi-k16.2.75 (user 2026-11-10: "all these logs get lost mostly. It should be dumped with the screenshot metadata"). `eventlog.*`: one line
per notable runtime event (wall time, seconds since start, render sim tick, kind, character wire id, numbers), held in a ring of 4096.

- Written three ways: beside every shot (F12 or the script's `screenshot`) as `<shot>.events.txt` (the last 10 min, with an `events:` line in
  `<shot>.txt`); by the script op `dump events [SECONDS]` (stdout); and appended to the run's own file `kclient/events/events-<date>-<pid>.txt`
  next to the screenshot folder, flushed per line so a crash or a kill keeps it (the newest 20 run files are kept).
- Kinds: `teleport` (a drawn character snapped to the sim past the 400 u limit), `catchup` (a catch-up / display-slop episode ended: its length in
  ticks and largest error, over 4 u only), `drawjump` (a drawn character moved further in one frame than 2x its sim speed and over 1.5 u: a pop, the
  kenshi-k16.2.74 measurement), `clock` (the render clock snapped over 10 ticks to the newest snapshot, or eased the startup backlog),
  `warp` (a stride-warp clamp, once per clip), `build` / `pose` / `action` / `fallback` (a character's rebuild, down pose or action failed, or a
  ragdoll fallback), `ko` (a stealth KO attempt: event 50's result — landed, roll failed or aborted because the victim was aware — and its
  chance, k16.2.70), `link` (the simlink failed), `rejected` (an order the sim rejected), `stall` (a frame gap over 250 ms, named with the
  load step / session load / world frame that blocked, k16.2.26), and the three character-state cases of k16.2.264.1 (the user's
  2026-11-15 walk into the Outpost s-IV: "sim delayed? character kept running in place", a dip under the deck, then the body gone
  indoors with its tag still drawn — the run file had none of it): `walkstall` (a drawn character walking at 0.5 u/s or more while
  the wire's own position has not moved for `anim.wire_still_ticks` ticks, default 10 = 0.5 s: the sim stalled it, not our easing),
  `drawnoff` (the drawn feet more than `anim.draw_y_off` units off the wire's y, default 2 u, with the terrain and the blend fraction:
  the ground blend under a deck or a missed interior), `nobody` (a character the frame places with no body in its set, naming
  Characters::DrawState — no model built, a floor cut hides it, the draw buffers ran out, a loading stand-in (render.loading_standin,
  kenshi-k16.2.258.1), not in the frame's set — one frame after
  the state holds). One line per character per 5 s; the cfg keys force any of them in a check.
- Forcing events: `debug nudge DX DZ NAME...` shifts the named characters' drawn position (60 u: an eased catch-up, 600 u: a teleport).
  Live check: agent repo `scripts/events/run.sh fight`.
- Display slop (kenshi-k16.2.74, user 2026-11-10: "it might be okay for the display location to be *slightly* off from the sim location, as long as
  it doesn't change gameplay. The client should 'ease into correction'"): the drawn position may sit off the sim's interpolated position and eases
  to it (`anim.slop_tau`, default 0.25 s, speed-capped so the legs walk it, with a 1 u tolerance where it rides; 0 = the capped catch-up chase).
  Picking, selection and hit tests read the sim (`Slot::sim`), never the drawn position; `placed()` (camera, popups, ragdoll range) stays drawn.

## Code map (`client/src/`)

One subsystem per directory, one level deep (kenshi-k16.2.369, 2026-11-18). Includes name the path from `client/src`
(`#include "render/sky.hpp"`), never a bare name or `../`. A new file goes in the directory of the subsystem it serves;
a new directory only for a new subsystem (put it in this table). Platform-specific code lives only under `platform/<os>/`
(windowing, Metal/.mm, input), picked by CMake; shared non-graphics platform code is the server's too and goes to
`cpp/` kutil (agreed with the sim lead on kenshi-k16.6.387).

| dir | holds |
|---|---|
| main.cpp | app, frame loop, load steps (`app.loadSteps`), camera, HUD, scenes/scripts (being split: kenshi-k16.2.369.2) |
| host/ | the host: App state and the glue main.cpp hands out (orders, picking, HUD, UI wiring, logic frame); kenshi-k16.2.369.2 slices |
| dev/, debug/ | dev-only code behind `KCLIENT_DEV_TOOLS`; the debug view |
| frame/ | the frame level: `render_frame`, trails, beam field, load screen, last-frame capture, the animation lab's frame (`labframe.hpp`) |
| input/ | picking, cursors, control hints, input queue |
| bridge/ | `SimView` (the wire -> Characters bridge: snapshots, prediction) and the ragdoll driver that reads it |
| characters/ | skinned characters, ragdoll, physics, portraits, character creator |
| ui/, audio/ | the UI library (UI worker's); Wwise bank/vorbis decode, mixer, music, sound events, the audio hook |
| world/ | buildings (records and world pass), towns, world map, weather instances, nav, floor follow, squads, foliage, grass, world particles |
| render/ | terrain, sky, water, shadows, HDR, post/AO, particles, weather map, draw lists, render queue, GPU timers, frame stats |
| assets/ | DDS and Ogre mesh loaders, materials, resource paths, mods, stb |
| platform/ | sokol implementation unit, window geometry and gate, data-dir discovery, the local server child, the simlink socket, default game installs, the GL loader; `linux/`, `macos/`, `windows/` the per-OS halves, `posix/` the Linux + macOS one (kenshi-379.31.1) |
| net/ | the SIMLINK wire: decoder and I/O, AI task names, progress feed |
| knobs/ | dev knobs read low in the stack (`ablate`, `lightterms`): headers that read as "off" in a release build |
| app/ | config, data-dir picker, event log, logic thread, work pool, `threadsplit`, git hash: the core nothing else is below |
| retired/ | code kept for reference, not built |

### Libraries (kenshi-k16.2.369.15.1)

Each directory is one CMake library (`kc_<dir>`, plus `kui` for `ui/` and `kaudio` for `audio/`), listed above from the top of the stack
to the bottom. A library declares (`kc_layer(... DEPS ...)` in `client/CMakeLists.txt`) the libraries below it and nothing else: an
upward `#include` fails the build in `tools/layer_check.py` (run first by every build), an upward symbol fails the link. Edges
(`DEPS`): `kc_host` -> everything; `kc_dev` -> everything below `host`; `kc_debug` -> net; `kc_frame` -> input, bridge, characters, world, ui, render,
assets, net, platform, knobs, app; `kc_input` -> bridge, characters, render, net, app; `kc_bridge` -> characters, net, platform, app;
`kc_characters` -> render, assets, knobs, app; `kui` and `kaudio` -> render, app; `kc_world` -> render, assets, net, platform, knobs, app; `kc_render` -> assets,
net, platform, knobs, app; `kc_platform` and `kc_assets` -> app; `kc_net`, `kc_knobs`, `kc_app`: none (`client/build/layers.txt` is the generated list).
Where a new file goes: the lowest directory whose library already has everything it includes (a file that needs a library above its directory
belongs in the higher directory). Higher layers reach down only; a lower layer that must call up takes a hook set at start (`framehooks` in
`frame/renderframe.hpp`: the dev lab and view draws), a forward declaration, or a split header. `ui/` is the UI worker's; its target is declared in
the same file. A new directory: add its `kc_layer` call and its row here.

Shaders in `client/shaders/`: `lighting.inc` (the shared deferred light model), `biome.inc` (terrain splat), `terrain`, `mesh` (buildings/features/foliage), `sky`, `hdr`, `post`, `ao`, `particles`, `fogvolume`, `character` (body / armour / hair / shadow). Formulas and their game sources: `docs/reference/LIGHTING.md` (sections 1-15).

## Working method (how the coordinator session runs)

- Coordinator delegates each problem to one Sonnet agent in a worktree (`isolation: worktree`), one scene problem per agent. Prompt must carry: CLAUDE.md rules, `-DKENSHI_SHDC=<main>/client/bin/sokol-shdc`, `-j6`, never `find /`, never touch `cpp/`, commit by path, Status line, no bless.
- Merge: in the agent worktree `git rebase master` (CLIENT.md conflicts: keep both sides' lines), build, `tools/scene_test.sh`, Read the sheets, then `git merge --ff-only` in the main tree and `--bless` from the worktree build. Then rebuild the main tree's `client/build` (`nice -n 10 cmake --build client/build -j8 --target kclient`): the user runs `./build/kclient` there, and an old binary rejects new `scenes.txt` keys (`bad field`).
- If master's `cpp/CMakeLists.txt` lists files kenshi-b0 has not committed (2026-10-31: `cpp/src/ksim/wilderness.*`), copy them untracked into the worktree to configure; never commit them.
- Live references come from the kenshi-inspect session (reads the running game: camera presets via `tools/kview.py`, shader uniforms, TreeLoader instances). Its findings are read-only in `findings/`; each becomes a `ref=` scene with the exact live pose, time and weather. Trace fixes to game shaders/data/exe; no tuning to the picture.

## Open questions

- Units per metre: **10, inferred from mesh heights** (human_male 19.46, female
  19.30 → 1.9 m). No exe constant found yet (`kenshi-b0`, 2026-10-30); b0 adds
  proof if one turns up.
- Building materials: answered in `docs/reference/BUILDING_MATERIALS.md`
  (part → `material match` → BUILDING → TOWN; MATERIAL_SPEC collections pick
  by weight; `texture map` alpha = shininess; `material type` modes). Still
  unverified: spec replaces `StaticObject` on every submesh. Report meshes with
  mixed submesh material names to `kenshi-b0`.
- Which biome map the game uses: unknown. Use `land/biomemap.png` (the one
  consistent with blendmap/blendinfo).

## Status

The dated log to 2026-11-02 is `history/CLIENT_STATUS_2026-10.md`. Verified-live notes now go in "Open work" (one line + client hash).
