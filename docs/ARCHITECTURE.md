# Architecture

A from-scratch runtime for Kenshi's world, built simulation-first. It reads
Kenshi's data files to bootstrap content, but owns its own simulation, AI,
pathfinding, streaming and save format. Rendering is a later client, not the core.

## Decided vs intended

Every section below carries one of two labels. Build the decided parts; treat
the intended parts as direction whose details are still open. Don't build an
intended feature ahead of the decided floor under it.

- **Decided**: the design rule is settled. It may not be built yet.
- **Intended**: where we want to go. The mechanism is open and may change.

| Area | Status |
|---|---|
| Server boundary, commands in, snapshots out | Decided |
| Region-owned two-phase tick | Decided |
| Runtime state model, clock, save format v0 | Decided (2026-10-30, after the architecture review) |
| Levels of detail; honest coarse levels | Decided |
| What has a position at Strategic level; encounter resolution | Open (per encounter type) |
| AI packages: faithful native runtime + optional Lua override (2026-11-07) | Decided |
| Three AI layers and their techniques | Decided in outline; techniques intended |
| Per-traveller path cost (squad min-stat profile) | Decided |
| Direct drive protocol (drive state, heading, resend) | Intended |
| Gate portals with access policies | Mechanism decided and implemented (nav); first town rules implemented (PROVISIONAL, "Town gates, first slice"); guards, queues, opening state intended |
| Living towns: posts, escalation | Intended |
| News carried by couriers and rumour | Intended |
| Town economy and job board | Intended (proposed) |
| Multiplayer replication, interest management | Intended |

## Goals

- **Living world.** Factions, towns and populations change state through
  simulation, not through mod-style boolean swaps of one town version for another.
- **A real economy.** Goods actually exist and move. Faction production camps
  (farms, mines, stone camps) really produce; the faction spawns real delivery
  caravans; deliveries (or their loss to raiders) change the state of the towns
  they supply: stock, prices, mood, garrison strength, recruitment. Shortages and
  surpluses come from the simulation, not from scripts. It may need heavy
  balancing. Any "cheat" that keeps the economy from collapsing (for example
  imports from off the map) is an explicit, named, logged valve, so soak runs
  measure how much the world depends on it.
- **Robust AI.** No NPC ever runs out of things to do. No freezing, no loops.
- **Better pathfinding and streaming.** The simulation never blocks on disk I/O.
- **Mostly Kenshi's rules** for mechanics: stats, combat, needs, economy.
  Extract formulas from the original exe one at a time as each system needs them.
- **Combat is the exception: match the original as close to 100% as we can**
  (user, 2026-10-31: "That combat *is* Kenshi"). No improvements, no
  approximations by choice: engagement, attack slots, block/dodge, damage,
  knockdown/KO, animation timing. Every formula comes from the exe or a
  measurement in the running game (kinspect), and fights are checked against it.
  Original bugs we may fix are named settings with a config-screen write-up of the
  original behaviour: `docs/COMBAT_OPTIONS.md`.

## Version 1: the game runs as it does now, in intent

The first playable target is Kenshi's own world behaving the way its data intends,
on our engine. Guards guard, raiders raid, caravans travel, towns trade, with the
original's stalls and loops gone. New systems (real economy, news, job board)
come in gradually on top of that.

- **The world is the data; the game people love is that world.** Factions,
  squads, towns, relations, dialogue and AI package *assignments* all come from the
  game files. The original engine adds little that's worth keeping.
- **AI packages: the original runtime first, Lua override second (user 2026-11-07; replaces "the package name is
  the contract; the steps are ours", 2026-10-30).** Same split as dialogue (DIALOGUE.md section 11 #15, section 12):
  - **Native runtime, faithful.** A C++ re-implementation of the original's generic AI systems runs the packages as
    data: the package stack (priorities, inheritance, signal funcs, dialogue `change AI` / `AI contract`, campaign
    phases), goal selection (windows, weights, tiers), goal resolvers over the sensor list (include / exclude masks),
    and the GOAP planner over the task registry (preconditions, effects). Target: near 100% faithful; most of the
    effort goes here. Systems and their status: `docs/reference/RUNTIME_SYSTEMS.md`.
  - **Lua override, optional.** A package can be replaced by a Lua unit (a mod's, or ours); "Behaviour layer: Lua"
    below. Our own design changes ship there, never in the native runtime.
  - Why: the original decision (our behaviour per package, goal lists as hints) was too ambitious. Get the game
    working and faithful first, improve it after.
  - **Stuck states (DEFAULT, playtest-able):** the faithful runtime keeps a watchdog. A member stuck in one goal or
    task past a limit is logged and its goal is re-planned. That is a FEATURES "deliberate difference" row, not a
    change to the package data.
  - Existing per-package behaviours (behaviours.hpp TSV map, `stepWorldTasks`, the town scorer cases) stay until the
    native runtime covers their packages, then move to a retired directory.
- **Coverage is measured.** A mapping table from package (and package family,
  given the inheritance) to our behaviour, plus a report: packages mapped, in
  progress or missing, weighted by how many squads use them. An unmapped
  package falls back to a defined generic behaviour and is logged, never
  silently dropped.
- **Our changes are an overlay "mod".** It sits on top of the game data and holds:
  - direct replacements for named AI packages, mapped to a generic behaviour of ours;
  - overrides for *specific* squads, adding behaviour;
  - new records, such as delivery-caravan squads for the economy;
  - the economy's balancing valves.

  Where it can, it uses Kenshi's own record format, so `kdata`'s overlay rules load
  it and FCS can edit it; anything that format can't express goes in a small
  side file of our own. Start small: override only what the engine needs.

## Non-goals

- Writing the original's save format. We use our own format. Reading it is one-way only: `tools/ksave_scenario.py` turns a save into a scenario file within a radius (`reference/SCENARIO_IMPORT.md`), and a full save import (load an original save as our whole world, to run side by side with the real game) is planned under kenshi-cym (user 2026-11-11: "save in the real game, load in our client, and run them both at the same time from that point, and observe the differences"). No round trip: we never write the original's files.
- A 1:1 reimplementation of Kenshi's AI packages and task classes. We keep their
  *intent* (see "Version 1"), not their mechanics.
- Behaving identically to the original engine.
- Baking the whole world on the player's machine. The runtime mesh is the shipped Havok navmesh (`navtiles/*.hkt`, converted
  to Detour); our bake, to the original's recipe, recomputes only what changes (player buildings, placed objects, zones a
  mod changes). Our full-world bake stays as the validation of that recipe against the shipped tiles
  (`formats/NAVMESH_CACHE.md` "Runtime mesh", "The original's recipe").

## Language and stack

- C++20.
- ECS: **flecs** (v4). Chosen for built-in relationships (membership, ownership,
  hostility), prefabs (map to game-data templates), observers (events flowing
  upwards), per-system tick rates, and Flecs Explorer for inspecting the headless world.
  Keep fast-changing AI state in component values, not tag add/remove,
  so entities aren't constantly moving between archetypes.
  **Per-tick queries (rule, 2026-11-02; WORLD_SIM_BASELINE §4-5):** `ecs.each` / `world.each` / a
  `query_builder().build()` makes an uncached query that walks every table holding its first term on each call
  (~0.2-0.3 ms in the whole world, whatever the match count). It is allowed only at load / save / snapshot /
  command / event time, and in tools and tests. A pass that runs per tick, per AI pass or once per sim-second
  returns early first (`if (ecs.count<T>() == 0) return;` on the rarest term, as in `stepConstruction`) or keys
  off a dirty flag. Never call `each` inside a per-member loop: hoist it or index it. A pass that needs the match
  set every time builds one cached `flecs::query` member once, in `Simulation`'s constructor, with a comment on
  how it is rebuilt when the world reloads. `World::each(fn)` is the shortcut: `ecs.each` over a query cached per lambda signature (`eachMember` for a squad's members); docs/PROFILING.md "Cached queries". A new per-tick system is checked with a 2-day soak before it merges.
- Navmesh: Recast/Detour (planned).
- Behaviour scripts: embedded Lua 5.5.0 (vendored, `third_party/lua-5.5.0`), sandboxed; high-level decisions only
  ("Behaviour layer: Lua" under AI).
- Code style: tabs; braces on their own lines, aligned (see the user's global style).
- **Dev-only code is fenced (rule, user 2026-11-12, kenshi-379.9).** Code that exists only for development, tests or
  scenarios (the .kscen / fixture loader and its overrides, test-only orders and SIMLINK frames, soak / CSV tooling,
  debug views) lives in a dev directory (`cpp/src/ksim/dev/`, `cpp/include/ksim/dev/`) behind a CMake option (default
  ON, like `KSIM_DEBUG_VIEW`), reached from the sim through thin hooks. An OFF build drops it, compiles, and its server
  runs a new game; tests that need it skip themselves (77) when it is OFF. New dev code goes there from now on; what
  already sits outside is moved under kenshi-379.9.
- **Source layout (decided 2026-11-18, user via kenshi-k16.6.387; shared-code terms agreed by the sim and client leads
  on that item).**
  - Libraries and their layering are CMake deps, lowest first: `kutil` <- `kdata` <- `kworld` <- `knav` <- `ksim`. A
    library includes only those below it; the client links on top. Include root: `cpp/include`.
  - `kdata`, `kworld`, `knav`, `kutil` stay flat (`<lib>/<file>.hpp`).
  - `ksim` is split by subsystem, header and source at matching paths: `cpp/include/ksim/<subsys>/x.hpp` and
    `cpp/src/ksim/<subsys>/x.cpp`. The subsystems: `core` (Simulation, state, clock, rng, script, simdata), `ai`
    (goal selection, GOAP, routines, behaviours, perception), `combat` (arena, fighters, medical, carry), `law` (crime,
    arrest, slaves), `nav` (navigator, doors, placement, LOS), `world` (world state and tasks, regions, residents,
    factions, buildings, interaction), `economy` (trade, shops, producers, inventory), `dialogue`, `save` (save, import,
    new game, scenarios), `net` (SIMLINK and the server). A new file goes in the subsystem it serves; a new subsystem
    is a lead's call, recorded here.
  - The top of `include/ksim/` is the embedding API the client uses: `server_main.hpp` and `debugview/enums.hpp` keep
    their paths. `dev/` (the fence above), `debugview/` and `src/ksim/retired/` stay.
  - Private headers sit beside their sources and are included as `"<subsys>/x.hpp"` (`src/ksim` is a private include
    dir of `ksim`); one private header including another uses a file-relative path, since tests include them by path.
  - Platform code: shared non-graphics OS code (executable path, crash log, sockets, threads, file mapping) lives once
    in `kutil`, with implementations in `cpp/src/kutil/platform/<os>/` chosen by CMake; neither the sim nor the client
    keeps its own copy. Graphics / windowing platform code is the client's.
  - The data-dir lookup (`datadir`, the Steam library scan, the macOS bundle path) moves to `kdata`; the client keeps
    the picker UI, the API returns the candidates and why none was found (kenshi-k16.6.388).
  - A later move of a shared path is announced on the leads' items first and lands in the same master batch. Move
    commits are move-only (renames plus include / build paths) and are listed in `.git-blame-ignore-revs`.

## Layering (decided)

```
   ┌──────────────┐     commands      ┌───────────────────────────┐
   │ client/view  │ ───────────────▶  │ simulation (headless)     │
   │ 2D debug map │ ◀───────────────  │  world, AI, pathing, rules│
   │ later: 3D    │   state/events    └─────────────┬─────────────┘
   └──────────────┘                                 │ reads once
                                          ┌─────────▼─────────┐
                                          │ data layer        │
                                          │ Kenshi files →    │
                                          │ world model       │
                                          └───────────────────┘
```

**The simulation is a server**, even in single player (like an integrated
server). Multiplayer is designed for now and built later.

**One player faction per user** (decided 2026-11-01). Several factions carry the
"player faction" flag; each is also its client's control group (what that client
may command). Relations, bounties and money are per player; co-op is allied
player factions. Each user picks their own start (NEW_GAME_STARTOFF). Wherever
the exe tests "the player faction", we test the flag, not identity.
Open: persistent world (MMO-like, players come and go) vs a session that runs
once all players have joined. Disconnect handling depends on it. Keep both
possible; don't design either out. Proposal and open decisions: `docs/MULTIPLAYER.md`.

1. **The simulation never calls into the view.** It publishes state and events.
2. **All player input enters as commands** through a queue. Nothing outside the
   simulation writes simulation state directly.
3. **The boundary is a message protocol, not function calls.** Commands go in;
   snapshots and events come out, filtered per client by what that client can
   see (interest management). In-process and networked clients use the same
   messages. Replication and interest management are client-side work and can
   wait; the boundary can't.
4. **Following a character is a camera, not a control mode.** Watching one character (3rd person follow; no 1st person, decided 2026-10-31; `docs/CLIENT.md`) changes nothing in the simulation. Control works the way the original game does:
   - **Orders while following** are ordinary immediate orders at the front of the character's queue, e.g. a click or key changes direction. When the order completes, the AI re-evaluates and resumes its own goals.
   - **A per-character "AI off" toggle** (the game has a UI button for it) makes the character player-driven. Its individual AI waits for orders instead of choosing goals. That is a deliberate hold, not a terminal state, and turning AI back on resumes normal goal selection. Squad and group layers treat it like a player-led member.
   - **Direct drive: the decided protocol is SIMLINK.md section 12 (2026-10-31), which overrides the speculative notes below where they differ (one order with drive + heading, coalesced per tick, no resend loop over TCP).** Direct drive is autorun plus steering, sent as state changes, not per-tick input. The original game does it this way: AI off, autorun on, then rotate and adjust. Ours works like MMO movement:
     - **`set drive {stop | forward | backward}`** is one sticky state. The client maps held W/S to forward/backward and key release to stop; a toggle key gives the original sticky autorun.
     - **`set heading h`**: A/D turning sends absolute headings with tick stamps, a few per second while a key is held.
     - Commands are sent only when something changes. They are idempotent (sending one twice does no harm).
     - **Lost commands:** the snapshot for a driven character carries its drive state and heading. That serves as the acknowledgement: the client resends its current state until the snapshot matches, so a lost `stop` can't leave a runaway.
     - **Disconnect:** when a client disconnects, the sim sets that client's driven characters to `stop`.
     - **Backward is a backpedal:** facing is unchanged, at a reduced speed. The speed factor and animation are to be decided (check what the game data offers).
     - With (start tick, position, heading, speed), both sides can extrapolate the same path. The server is authoritative.
     - The client predicts starts, stops and turns immediately; it does not wait for the server's acknowledgement. Server corrections are smoothed.
     - The server reports "blocked" when a wall or cliff stops the character.
     - This needs the driven character's movement and collision to be deterministic and cheap.

The server owns all game state. Lockstep determinism is **not** a goal.

### Why not the whole world at full detail

Several thousand NPCs at full detail (navmesh steering, per-attack combat,
continuous perception) would cost hundreds of milliseconds per tick (estimate).
A server has a limited number of player slots (8–16), so there are at most that many
full-detail areas, fewer when players group up. Everything else runs at squad or
strategic level. That is affordable only if the coarse levels are **honest**: the same
data and rules, aggregated, so promotion never reveals an inconsistency.

### Threading: regions with one owner each

- The world is split into **regions** (zone-aligned, 4,608 units or a multiple;
  the size is configuration, not a 1:1 mapping to zones. Most zones are empty,
  so larger regions are expected).
  At every tick each entity is owned by exactly one region, and a region is
  processed by exactly one thread.
- **Two-phase tick.** Phase 1: every region reads the previous tick's state, frozen
  for the whole tick, and computes its updates. Phase 2: each region writes only
  its own entities. Cross-border effects (a hit, a perception event, an entity
  moving into the next region) are messages applied at the tick boundary.
- **Ownership decides who writes, not who can be seen.** The frozen previous-tick
  state is readable from every thread for the whole tick. A region never writes
  into a neighbour. Border strips only make spatial queries ("who is near me")
  cheap. The strip is at least the longest interaction range (bows, crossbows,
  turrets); beyond it, a query falls back to a global lookup.
- **References are entity IDs, never region-local pointers.** An archer keeps
  their target when the runner crosses a region line: only the runner's owner
  changes. Effects such as a hit go as messages to whichever region owns the
  target that tick (one tick of delay).
- **Squads span regions.** A squad's group-level step is its own work unit,
  assigned to a worker by squad ID rather than by region. It reads its members
  from the frozen state wherever they are and sends them orders as messages.
  Later, optionally: while a fight is active, the regions involved merge into one
  work unit, removing the one-tick delay for hits.
  - **Built squads move by their Fighters (implemented 2026-11-02).** While a
    world squad is built (Full detail), the group step still chooses the
    destination, but the Fighters carry the movement: the leader walks a carrot
    on the group's route, the others follow the leader, and the squad token's
    position is the leader's (written once a second, not advanced by the
    strategic mover). The route progress waits while a member fights or the
    leader lags. `docs/reference/TOWN_LIFE_SLICE.md` 5.14.
- Results must not depend on the thread count, so replays and debugging survive
  parallelism.
- **Per level:**

  | Level | Parallel unit |
  |---|---|
  | Full detail | One area per player; areas that overlap merge into one |
  | Squad | One region, at ~1 s ticks |
  | Faction | Global, about once per game-hour, run in the background on a copy of the world |

  Factions act on beliefs delivered by news, not on truth (see Living towns),
  so a slightly stale copy is the correct model, not a shortcut.
- flecs's built-in multithreading splits work by entity count, not by space,
  and its deferred writes only buffer structural changes; they don't give
  threads a frozen previous tick. So the region scheduler is ours, and hot
  state lives outside flecs (see "Runtime state").

### Server loop and pause policy (decided, user 2026-11-01)

`SimLinkServer` ticks the sim in real time (20 Hz at speed 1) only while at least one session has
completed HELLO: it starts paused, resumes at the first ready session, and pauses again when the last
one leaves, keeping the speed. An admin's explicit pause is separate and wins. `--run-without-clients`
overrides it (a server config replaces the flag later). This is the single-server default; who may
pause is MULTIPLAYER 6.2 (`pause_policy`). Details: `docs/formats/SIMLINK.md` "Pause when nobody is
connected". Tools without the server loop (`ksim_soak`, `--dump-snapshot`) always run.

## Runtime state (decided)

What the simulation's live state is, where it lives, and how it is saved.
Everything else (AI, promotion, snapshots, saves, soak metrics) is a function
over this.

- **Templates vs instances.** Game-data records (FACTION, SQUAD_TEMPLATE, TOWN,
  CHARACTER, ...) load as read-only templates. Live state is instance
  components that reference a template by string ID: `SquadInstance`,
  `CharacterInstance`, `TownState` (owner, population, prosperity, posts),
  `FactionState` (relations), and one global `Clock`. Template and instance
  components live in separate headers and namespaces.
- **Two stores, one owner each.**
  - **flecs is canonical** for templates, instances, relationships
    (membership, ownership, faction) and all slow-changing state. It is the
    source for saves and snapshots.
  - **Hot kinematic state** (position, heading, move target, current order)
    lives in the region engine's double-buffered arrays, which give every
    thread the frozen previous tick. It is mirrored one way into flecs at the
    tick boundary. Nothing writes the mirror.
- **Open: what has a position at Strategic level.** Most of the time a squad
  just travels or patrols and meets nobody; then one squad position on the
  road/region graph, with members as data on it, is enough (and matches the
  squad min-stat travel profile, Pathfinding). Encounters are where it
  matters, and they differ by kind (user, 2026-10-30):
  - Some clashes are where the interesting world dynamics come from. A squad
    can take prisoners, so resolving combat and capture properly (per
    character, possibly at a finer level with no player near) may give the
    best result.
  - Others are a pure stats check with nothing worth resolving (a patrol
    meets a pack of bonedogs). A statistical roll by squad type is fine.
  - So the resolution rule is per encounter type, not one global choice.
  - Must avoid: the original's raid that waits for the player (user,
    2026-10-30). A world event sends a raid to the player's base or town; it
    arrives and shows on the minimap, but nothing happens until the player's
    loaded area reaches the town, so the player can run a squad across the
    world and arrive in time to start it. Ours must resolve the raid when it
    arrives, watched or not: either at a finer level with no player present
    (the player's own characters and turrets there fight as normal NPCs) or
    as an honest roll over the same data. A player arriving later finds the
    result. This is a scenario test (Validation).
  - Candidate (user, 2026-10-30): **encounter slots**. Encounters that
    deserve per-character resolution get a small full-detail area of their
    own, anywhere on the map, with a fixed maximum in flight. When the
    slots are full, an NPC-only encounter waits in a queue with its
    participants frozen in place; nobody notices a caravan's fight with
    bonedogs resolving five minutes late. Open details:
    - Priority: anything touching the player's people or property (the
      raid above) never waits behind NPC-only encounters.
    - The queue ages entries so none starves (no terminal states).
    - A squad that walks into a frozen encounter joins it rather than
      acting on the frozen participants.
    - A player arriving at a queued encounter promotes it at once.
    - Unwatched slots may run faster than real time when workers have
      spare time, so the backlog drains.
  Until this is decided, code must not assume either way: characters are
  always instances of their own, and a squad's position is the only one
  that coarse travel moves.
- **A spawner** turns templates into instances, using the default-world rules
  derived in `docs/reference/WORLD_MODEL.md` (town residents, roaming squads,
  variants by world state). Its counts are checked against
  `docs/reference/world_model/data/`.
- **The sim owns the clock.** Fixed tick of 0.05 sim-seconds. One game day is
  2618.18 sim-seconds (the exe's 1x day, `docs/reference/SKY_EXE.md`), so a
  game-hour is 109.09 s ≈ 2,182 ticks. Game speed changes ticks per wall
  second, never the tick length. The calendar is derived from the tick
  count. The client is sent the clock; it doesn't keep its own.
- **The sim owns the load order.** The record stack is the shipped one
  (gamedata.base → Newwworld → Dialogue → rebirth) plus our own overlay.
  Third-party Kenshi mods are never overlaid as records at runtime: a
  supported mod (`docs/reference/SUPPORTED_MODS.md`) is detected and handled
  natively, as "load this asset file" or "apply this behaviour change".
  Preprocessing on the player's machine may read a supported mod's records to derive that
  handling, through the same classify-and-replace pass as the base data's AI
  packages. The sim publishes what it loaded. The install's own
  `data/mods.cfg` is never read (it is the player's setup for the original
  game; 2026-11-01 the user enabled a UI mod there for screenshots and four
  tests broke). The load order is vanilla unless a mod list is passed
  explicitly (`--mods FILE`, `kworld/loadorder.hpp`). Static world content (terrain, zone
  placements, foliage) is a pure function of (game data, load order) in
  `kworld`, and the client may compute it locally with the same functions
  and the sim's list. Anything the simulation changes (town variant, ruins,
  gate state, populations) reaches the client only through snapshots.
- **No build step reads game data** (user, 2026-11-03). A CI build (e.g. a GitHub
  Windows runner) has no Kenshi install, and converted game data is never
  redistributed. Every conversion (dialogue to Lua, `DIALOGUE.md` §12.1; asset
  and record preprocessing) is engine or tool code that runs on the player's
  machine against their install. Whether to cache the output is decided per case, after measuring its size.
- **Interiors are always simulated.** Furniture (storage, beds, benches)
  are ordinary instances; their contents and production never depend on
  what a client draws. In the original the furniture objects also always
  exist and only their visuals unload when no player character is inside
  (`docs/reference/INTERIORS.md`; which building a character is in comes from
  its position, "Building membership" there). How much interior the client draws is a
  rendering policy (by camera distance, never a doorway snap).
- **Player buildings and outposts** (proposed, draft): a player-placed building is an instance entity with its own saved component, and a player's outpost is a runtime-created `PlayerTown` instance beside the record-derived `TownState` towns. Power is one derived grid per `PlayerTown`. Design: `docs/reference/BUILDINGS_SLICE.md`.
- **One tick driver.** `Simulation` and the region engine merge into one tick
  with one command queue, one RNG seeding scheme and one state hash.
- **Save format.** Our own, versioned (`docs/formats/RUNTIME_SAVE.md`): the
  clock, all instances and relationship values. A round trip must reproduce
  the state hash. The same serialiser, filtered per client, is the snapshot
  message. Kenshi `.save` files are import-only (`docs/formats/SAVE_FORMAT.md`).

## Simulation (decided)

- **Fixed tick.** Seeded random numbers, one stream per subsystem.
  The goal is reproducible debugging and replays, not exact sync over the network.
- **Levels of detail instead of loaded/unloaded chunks.** An entity's data looks
  the same at every level; only its update rate and resolution change.

| Level | Where | Movement | Combat | Tick |
|---|---|---|---|---|
| Full | Near any player | Navmesh, per-tick steering | Per-attack | Every tick |
| Squad | Rest of the active region | Coarse region/portal graph | Statistical, over seconds | ~1 s |
| Strategic | Everywhere else | Road/region graph | Outcome rolls | ~1 game-hour |

  **Promotion** (moving to a more detailed level) must place entities somewhere
  believable. A squad travelling on a road at a coarse level appears on that road.
  Coarse movement follows a walkable region/portal graph derived from the baked
  navmesh, never a straight line, so a squad's position is always on walkable ground.
  On promotion each character is snapped to the nearest valid navmesh point near
  that position. This fixes the original's "stuck in a wall or on a cliff after the
  zone loads" (its off-map `UnloadedPlatoon` token slides in a straight line and
  characters spawn wherever it stopped; `docs/reference/tasks/` platoon reports).
  Promotion and demotion are spread over several ticks and never block one.
- **Navmesh and collision are baked offline** for the whole world (at import or
  first run) and cached in our own format. Nothing is cooked or generated during play.
- **Streaming belongs to the client.** The simulation needs only navmesh and
  collision for regions at full detail. It builds or loads these off the tick thread
  and runs at squad level until they're ready. The tick never waits for disk.

## AI: three layers, no terminal states (decided in outline)

The core rule: **no layer may reach a state with no next step.** Every goal
can end, fail or become impossible, and the layer above must always be able
to pick a successor. The bottom layer always falls back to ordinary life:
needs, work, socialising, going home.

Kenshi's NPCs clustering at gates is the failure this rule prevents. The
gate is a spawn point, and the NPCs' script ended there with nothing next.

| Layer | Decides | Re-evaluates | Technique |
|---|---|---|---|
| Faction | Policy and temperament: expand, defend, raid, consolidate, mourn, purge | Game-hours, or on major events | Utility scoring over faction state |
| Group (squad, town population) | Scenario goal: patrol, escort, hunt, regroup, rout, siege, rejoin | Seconds, or on perception events | Scenario assessment → utility |
| Individual | Current action: move, fight, loot, heal an ally, flee | Every tick, or on interrupt | HTN plan / behaviour tree per goal |

- **Orders flow down as intent, not commands.** A faction sets temperament and
  priorities ("at war with X, aggression 0.8"); each group reads them against
  its own situation.
- **Information flows up as events.** For example, "leader killed" changes
  faction variables (succession, aggression, tolerance), and every group
  re-scores its goals.
- **Scenario assessment** at the group layer weighs: whether we're in combat,
  our strength relative to the enemy, nearby allies in danger, how fast we're
  taking losses compared with dealing them, distance to friendly territory, and
  the faction's current intent. Routing → fleeing → regrouping → rejoining
  should fall out of the scores without special-case code.
- **No special-case AI code** (user 2026-11-15, kenshi-k16.6.286). Behaviour the generic runner (package data,
  goal priorities, selector, goal end / fallback) should produce is fixed in the runner, never by code for one
  character, faction, goal or scenario. Special-case code for such behaviour is flagged in the merge request
  ("special case: <what>, why") and tracked by an item (label `special-case`) until it is folded back. Special by
  design: the campaign / raid dispatch rewrite (FACTION_CAMPAIGN data). The live list is the `special-cased` rows of
  `docs/reference/AI_COVERAGE.md`, tracked under kenshi-k16.6.12.
- **Commitment and hysteresis.** A chosen goal gets a bonus for sticking with it
  that fades over time. A goal that was abandoned or failed gets a cooldown.
  Failed targets go on a temporary blacklist. These prevent flip-flopping
  between goals and retry loops.
- **Stuck detection is an input to the AI, not just a debug metric.** A lack of
  progress towards a goal feeds back into goal selection.

**First slice (2026-10-31; code in `cpp/include/ksim/ai/ai.hpp`, `behaviours.hpp`).**
Provisional unless marked; the numbers are guesses (STATUS.md lists them).

- **Behaviour registry (the mapping is data).** `cpp/data/ksim_ai_behaviours.tsv`
  maps a package to a behaviour by exact name, then by the package's own
  `unloaded func` (the original's abstract-behaviour field, from the package or
  its nearest ancestor), then by a name keyword (own name, then ancestors').
  A squad runs its default package: the lowest `AI packages` priority.
  Dialogue swaps, contracts and campaign phases are not modelled yet. A rule
  may name a behaviour not implemented yet (reported as *planned*); an unmatched
  package is *unmapped*. Both run the generic fallback and log the reason once.
  `ksim_ai_coverage` prints the counts. Baseline with the fidelity gaps: `docs/reference/WORLD_SIM_BASELINE_2026-11-01.md`.
- **Fallback = ordinary life.** Go home if away, idle at home for a few
  game-hours, occasionally visit a town within 8,000 units and come back. A
  squad with no position (wilderness, the known gap), outside the navigation
  window, or with no navigator can only idle, still with a goal and a logged
  reason.
- **Group layer.** Goals (idle / go home / travel to a town) are scored by
  utility; the goal in hand keeps a bonus that fades over 12 game-hours; a goal
  that fails (no route, off mesh, no progress for 6 game-hours) is blacklisted
  for 24 game-hours and the next best is tried in the same step; Idle cannot
  fail, so a step always ends with a goal. The 12 h bonus, 6 h no-progress and
  24 h blacklist are ours, not the original's. The Faction layer is real since
  the faction first slice below. There is no Individual-layer interface
  (removed with kenshi-k16.6.43); where a later Lua override plugs in is
  kenshi-k16.6.40's registration design (a fixed, ordered table of decision
  sources registered at `Simulation` construction; selection stays `goalsel`;
  design only, not built). Per-character AI runs in `Simulation`: the
  squad's package stack (`blackboard.cpp`) gives goal rows, the goal selector
  (`goalsel.cpp`) scores them, the town scorer (`routine_scorer.cpp`, from
  `stepResidents`) and the world-squad tasks (`world_tasks.cpp`, from
  `stepWorldSquads`) carry the winner out with GOAP chains (`goap.cpp`,
  `goap_sim.cpp`); combat, dialogue orders (`individual_goals.cpp`) and crime
  goals (`crime_goals.cpp`) run beside them.
- **Decided by the implementation.** The group step is serial, in squad Id
  order, with randomness hashed from (seed, squad Id, tick), so results do not
  depend on worker threads. The squad's goal, route and cooldowns are flecs
  state and are saved (`GroupAI`, save v2); the position is still hot state, and
  the group step moves it through `RegionEngine::place` between ticks. The
  route is the coarse graph's stored path (positions stay on walkable ground);
  y is not stored. This does not decide the open "what has a position at
  Strategic level" question: only the squad's position moves.

**Second slice (2026-10-31): local behaviours and wilderness placement.**
PROVISIONAL unless marked; code `cpp/include/ksim/{placement,ai}.hpp`.

- **Patrol (`patrol_local`) and territory (`animal_territory`)** are goals of
  the same utility layer (`GoalKind::Patrol`, `Roam`). The anchor is the home
  town, else the squad's wilderness den. Patrol: a loop of 6 points (the anchor,
  then points on an inner and an outer ring, each moved to the nearest walkable
  ground, so the loop follows the navmesh round the buildings), walked in
  order with random rests. Territory: random points of the disc round the
  anchor, with rests. Radii (guesses): 250 round a town, 1,200 (patrol) / 1,500
  (animals) round a den. A route that strays past 1.5 radii is refused, so a
  loop or territory never leaves far. Both end in the usual re-selection;
  Idle always exists, so neither is terminal. Save v3.
- **Guard mapping (decided from the data, 2026-10-31).** "Guard town" and its
  variants carry `unloaded func` TRAVEL_TARGET but their goals are bed, turret,
  alarms, trouble makers: shift posts. `Task_PatrolTown` has no loop of its own
  (it follows the package target). They are pinned to `stay_home`;
  "Guard military patrol base" (has PATROL_TOWN) to `patrol_local`.
- **Wilderness spawns (decided, user 2026-10-31): density with respawn.**
  The data: BIOME_GROUP `homeless spawns` (squad, chance), `homeless spawn
  amount` ("approximately squads per zone"), `nests` + `num nests`; the areas
  map (`areasmap.tga`, colour == BIOME_GROUP `index`) gives zones. These are
  filler spawns, not major players: a refresh cycle is wanted ("you kill the
  bonedogs, and that road is forever safe?"). The original does it as a
  continuous top-up against a per-zone density quota over the *loaded* zones,
  no stored timers, and one new-game option, the nest multiplier
  (`docs/reference/WILDERNESS_SPAWNS.md`). Ours (`ksim/wilderness.*`, code
  provisional where marked):
  - **Instancing: the whole world from day 0.** There is no player-centred
    loading in our sim, so the quota is the whole map: per BIOME_GROUP slot,
    homeless squads = round(zones x amount x population), nests = round(`num
    nests` x nest multiplier), all instanced by `newGame` (about 4,400
    squads and 500 nests at 1.0, ~5,700 squads in all with the town
    squads; the soak numbers are in STATUS.md). The 175 template squads are
    no longer spawned one-per-template; faction `roaming squads` stay
    persistent. Cost is the price of a living world; measured, not cut.
  - **Respawn.** A strategic step (one game-hour) counts each slot's live
    squads (nests: distinct nest instances, alive while any squad lives).
    Every hole in a quota gets a due tick, `now` + uniform(36..84) game-hours
    for a squad, 120..240 for a nest (guesses: a refresh cycle of days; the
    original has no timer). When due, a new squad is drawn from the slot's
    list by chance and placed at a dry point of the area (`WildGround`,
    shared with `placement.*`); a nest draws a nest TOWN, one point, and all
    its resident squads at it. Removal (`Command::RemoveSquad`) and a wipe (no member left at all: the
    last corpse destroyed, see "Corpses" below; a squad whose members are all dead or KO'd stays, non-intact at 0.5: see "Squad liveness" below) free quota; the next step queues its replacement. The timers, the stream
    and the nest counter are state (`WildernessState`, save v4); quotas are
    recomputed from data and the two multipliers.
  - **Squad liveness** (kinspect s71_static.md 5, BQ4, s79_static.md 2 / bq15_corpse_cleanup.md b, c; `ksim/squad_alive.*`, `squadVitals`; no
    save field). The exe's "intact" test (`1407ac7b0` -> `14079acf0`), as of 2026-11-02 (BQ15):
    - **Effective member** = not dead, and (not down, or injury severity strictly `< 0.2`). Down (`1405c9e10`) = dead, carried, ragdoll
      (`Fighter::down`) or KO'd (posture >= 3, M). Severity (`injurySeverity`, MedicalSystem `+0xac`) = `max(0.5 (1 - min part), 1 - min
      vital part)` over the natural parts, health as a fraction of max (above 0.999 snaps to 1), 0 unhurt, about 1 with a vital part at 0 HP;
      the bleeding term `(1 - blood / maxBlood) max(0, bleeding - 1.5 leak) 10` replaces it when larger and > 0.2. So a KO'd but unhurt
      member (hunger, a drug) still counts as effective, and a carried one too.
    - **Nominal size** (`platoon+0xa4`, `nominalSquadSize`) = the template's: 1 for a non-empty `leader` list + per `squad`, `squad2`,
      `animals`, `animals2` entry `(min + max) / 2` (integer division; `min` when max is 0 or 100), x the squad size multiplier only when
      < 1 (we have no such option: 1.0), floor 1. Not the live count. A squad with no `SquadInstance` template (a synthetic one) falls back
      to the live count. A squad of fewer than 4 is intact while its leader is effective; of 4 or more, while `effective >= 0.2 x nominal`
      (`0.4 x` with the leader down).
    - **Quota** (`WildernessSpawner`, `topUp`): single-precision float as in the exe: `cur = |S2| + 0.5 |S3|` (S2 intact homeless squads
      of the slot, S3 non-intact ones, which include the all-dead and the all-KO'd), a spawn happens only while `target > cur`, and the
      sector cap skips it when `target <= |S2|`. `target` stays our rounded `zones x amount x population`. Nests count as all S2. One
      respawn timer is queued per hole, `ceil(target - cur)`; the per-template density cap counts intact squads only (UNSURE), so a
      wiped squad's corpses do not block its replacement. A replacement that spawns below 0.2 x nominal (the draw is `between(min, max)`,
      the nominal is the average) is non-intact from birth and opens a new hole: the squad count in a slot can exceed its quota.
    - A non-intact squad is not deleted: in the exe it sits in S3 and is erased when its zone unloads. Ours: nothing retires it by
      itself. An all-KO'd squad stays until a member wakes or dies. An **all-dead squad stays too, until its corpses are destroyed**
      (see "Corpses" below); the faction tick's "no members left" rule (`retireWipedSquads`, `members == 0`) then retires it. (This replaces
      the 7dd2710 "leaves at once"; code in `src/ksim/retired/`.) Consequence: with no player nearby a wiped squad keeps 0.5 of a quota
      slot for ever, so one extra squad is spawned beside it (target 3, cur 2.5 -> spawn).
    - UNSURE: (1) the tied-with-host 0.8 threshold is not modelled; (2) a squad with no marked leader uses `effective > 0`; (3) `leak`
      (`ms+0x74`) is the raw severed-limb leak without the data's extra-blood-loss factor; (4) posture 3/4 = KO is M, `Fighter::asleep`
      is not "down"; (5) on intact -> not intact the exe removes the squad from its home town / nest group (`140796b30`): not modelled,
      our nest resident budgets are not adopted (WILDERNESS_SPAWNS.md); (6) the per-sector cap is per sector in the exe, per slot here.
  - **Corpses (default decided by b0 2026-11-02, deferred and playtest-able; kinspect BQ15 `bq15_corpse_cleanup.md` a, `s79_static.md` 1;
    `Simulation::stepCorpses` in `src/ksim/combat/corpses.cpp`, constants in `include/ksim/combat/corpses.hpp`; `state::CorpseDecay`, save v54).**
    What the exe does (`Character::vf29`, `1405ccd90`, dead branch): death sets nothing; the **first evaluation after death stamps the
    time** (game hours, `char+0xd0`, saved as `decay`); later evaluations destroy the corpse when `now - stamp > 12` game hours ("corpse
    decayed", strict). While the corpse is **carried** (or in a building slot) the stamp is clamped to `>= now - 6`: its age freezes at 6 h
    and it cannot decay. An NPC corpse is **evaluated only on screen** (within "NPC View Range", 3250, of the camera) or carried; player
    faction corpses always. A corpse whose zone is not loaded is destroyed ("corpse unloaded"). Other removals: furnace / cooker
    ("incinerated or cooked"), "eaten" (prisoner), squad destroy. No count cap, distance rule, looted-and-empty rule or animal-eats-corpse.
    The 5 s in `140799af0` is a deferred destructor; we destroy at once. 12 game hours = 1309 s at speed 1.
    Our mapping (all zones are loaded, so "on screen" and "unloaded" are re-expressed):
    - **On screen = inside a player's full-detail area** (`RegionEngine::areas()`, the 3x3 regions around each player character; with the
      default 4608-unit regions that is the same block as the original's loaded zones). An NPC corpse ages only there (or while carried).
    - **Zone unloaded = no player area covers it and no player is within the original's load range**: the 3x3 zones around the camera zone,
      plus 340 units around a player-faction platoon (`s71_static.md` 5.1, `140a0f590` radius 1, `140a0f640`, `14174a100` = 340.0). The
      range is measured from the `PlayerAreas` centres (the living player characters; we have no camera). Such a corpse is destroyed at the
      next pass, whether or not it was ever evaluated. UNSURE: the exe tests this inside the same on-screen gate (a never-seen corpse is
      dropped only when its zone unloads), and the camera is not the player character. When the detail area is smaller than the load
      range (`areaRadiusRegions` 0) a corpse in the load range but outside the area is neither aged nor destroyed.
    - **Player corpses always age** (a `playerControlled` fighter, or one whose faction is a `PlayerFaction`), wherever they lie, and are
      never "unloaded" (UNSURE: the exe would drop a far one).
    - **While no player area exists nothing is evaluated** (no unload either): a headless scenario or soak keeps its bodies. UNSURE: the
      exe always has a camera.
    - **Carried**: the stamp is clamped to `>= now - 6 h` (never destroyed while carried, `Fighter::carriedBy`); a body in a building slot
      is not modelled (our beds are released at death, kinspect item 58).
    - **Destroying drops nothing extra**: the entity goes with its `Inventory` (the exe's destroy releases the inventory too). Looting
      before the 12 h is the player's window. A destroyed member leaves its squad (`MemberOf` goes with the entity); a spawner squad left
      with no member is retired by `retireWipedSquads` ("A wiped squad ends when its last corpse goes").
    - **Consequence to playtest**: a player who walks more than one zone away (outside the 3x3 load block) finds the dead bodies gone
      on return, with their gear; KO'd characters are alive and stay. The original does the same (zone unload). Tests that pinned
      "a body stays when its squad is demoted / the player leaves" (`ksim_town_fighters` death test, `ksim_bar_fight` step 5) now pin this.
    - **Setting `world.offscreen_medical` (k16.6.52, COMBAT_OPTIONS.md 8; default off).** On: every corpse is evaluated everywhere (it ages 12 h
      from its death wherever it lies) and none is destroyed as "unloaded"; a stored member of a demoted world squad that bleeds out (the
      off-screen medical step, `stored_medical.cpp`) is built as a dead `Fighter` at the token's position with a `CorpseDecay` stamped at the
      death (`deathTick`, `cause`: `Simulation::leaveCorpse`, which a token fight's death (k16.6.46) calls too). Off: the rules above.
      deferred: playtesting: (1) the 12 h counts from the death off-screen but from the first evaluation in detail, as before; (2) a body is a
      full arena member while it lies (cost: one per corpse for 12 h); (3) a corpse outside the detail area is not carried or eaten by anything.
    - **Setting `world.offscreen_fights` (k16.6.46, COMBAT_OPTIONS.md 9; default off).** On: two hostile tokens (neither built) within
      the encounter radius fight a strength-squared roll once a game hour per pair (`token_fights.cpp`); the loser's weakest half falls. A fallen
      member is `leaveCorpse(..., cause 2)` with `world.offscreen_medical` off, else KO'd with a bleeding wound in its `MemberDelta` (a later death
      is cause 1). Ours; the original resolves nothing off-screen.
    - Not implemented: the flies effect after 4 h and the smell source (wolves hunt by smell), furnace / "eaten" removals.
    - Pass cadence `kCorpseIntervalTicks` = 40 (the exe evaluates one character per frame; the decay is in game hours, so any fine
      cadence gives the same outcome). The stamp is the only new state: `state::CorpseDecay { f64 stamp }` on a dead character (0 =
      not yet stamped), save v54 (`formats/RUNTIME_SAVE.md`). Test: `ksim_corpses`; `ksim_ai` section 8 (a wiped squad stays at 0.5 until
      its corpses decay). FEATURES.md "Wilderness squad liveness and corpse removal".
  - **Settings.** `SpawnOptions::wildernessPopulation` (default 1.0) scales the
    homeless squads, `nestMultiplier` (1.0) the nests; 0 switches a kind off.
    The exe exposes only the nest multiplier; the population one is ours.
  - **Populate once.** Towns and buildings are populated once per world and
    never again from camera or zone state. The original re-populates a fresh
    zone when the camera jumps during its load step, so whole towns and gate
    crews end up doubled (kinspect 35, `reference/GATE_CREWS_LIVE.md` 4a, FEATURES
    "Original bug" row). It also re-populates any zone whose zone file is missing from
    a save, beside the saved residents (kinspect 38, 4b). Ours: a saved per-Town /
    per-building populated flag, never inferred from save file presence; a damaged
    save fails loudly.
  - **Exe facts adopted (2026-11-02, kinspect `s71_static.md`, ccf1940).**
    Only BIOME_GROUP carries `homeless spawns`; the BIOMES lists are dead data
    in the exe and not read by us (the placer's legacy path is retired,
    `ksim/retired/placement_biomes_homeless_lists.txt`). The Hub block (zones
    x20-22, z30-32) is areasmap colour 0xFF0000 = BIOME_GROUP `42262-rebirth.mod`
    "Border Zone" (not "The Desert"; pinned by `ksim_wilderness`). A squad is drawn
    by weight = w - liveCopies x totalW / (zones x pop x amount), at least 0,
    liveCopies = the slot's live squads of that template (whole world instanced,
    so "loaded" is "live"); all-zero weights spawn nothing that pass. Our
    quota and refresh timers stay as decided above.
  - **Not adopted, by design.** Despawn / destruction on unload (our world is
    persistent), the player-distance rule (spawn >= 1382 u from the player),
    nest resident budgets (`nest resident population`, regrowth 0.218 per game
    hour), re-rolls on reload, the 2 s pass cadence. See `reference/WILDERNESS_SPAWNS.md`
    "Exe facts (kinspect s71)". Squads of one template spread by distance is
    still undecided.
  - **Cost and local goals (2026-10-31).** With ~4,000 squads
    patrolling/roaming, routing every goal on the world coarse graph made the
    whole world unrunnable (82 ms per route; 250+ s per game-day). Local goals
    (patrol leg, territory walk, within ~1,500 units of the anchor) are
    routed by one bounded Detour search (`Navigator::localRoute`): on the
    mesh of the region when both ends are in it, over a region border on a
    window mesh of the tiles round both ends copied from the region meshes
    (Detour links them across the seams; a dtNavMesh holds 8,192 tiles, a
    window ~200), with one wider retry when a search closes. Goals are
    picked on the squad's own connected set of ground (`nearestReachable`,
    sets labelled at load), so they cannot be islands; what still fails is
    ~0.8% of walks (pool exhaustion on very long ways round, snaps) plus
    paths beyond the leash. Resting squads cost one comparison per step;
    `RegionEngine::tick` does nothing when no workload runs. Whole world
    9-10 s/game-day (STATUS "Squad AI cost", "Local goals: borders and
    reachability"). Whether squads far from every
    player should instead get token movement (UnloadedPlatoon) stays OPEN.

**Faction layer, first slice (2026-10-31): campaigns as raids.** PROVISIONAL
unless marked; code `cpp/include/ksim/world/factions.hpp`, data and exe notes in
`docs/reference/FACTIONS.md`, test `ksim_faction`. Save v5, v7 after the exe update.

**Exe-grounded now (2026-10-31, FACTIONS.md 2.1, 2.2, 3.1, section 4) vs still
PROVISIONAL.** Grounded: the hostile (-30) and ally (50) thresholds, in raid targeting and
the gate traveller classes; the campaign roll (every 6 game-hours per faction, only with
no running campaign and no queued follow-up, `p = chance per day x 0.25 x falloff x
raidFrequencyMult`, `repeat limit` / `world state` / distance gates, one picked by weight);
`range near/far` in metres x 10 units; force `int(num forces x raidSizeMult)`; the
lifecycle (on site 4 / 12 / 1 h by `key`, a 3 h retreat state, defeated under 0.2 alive);
the relation reason table (floors, one-delta overshoot, clamp, no decay) with the
`RemoveSquad` killer producer; `biomes` / `no-go zones` on targets. PROVISIONAL (ours):
targets among the other factions' towns, the intent/temperament layer on top of the roll
(a faction rolls only while its intent is Raid), cooldowns after failures, follow-up
patience, squad-granularity alive fraction, the player-only gates (no player base;
`target tech level` has no readable town level), every number in the bullets below not
named here. The bullets below are the first-slice text; where they conflict, this
paragraph and FACTIONS.md section 4 win.

**Decided (user, 2026-10-31): raids are the player's problem, as in the original.**
Campaigns harass the player faction only: a base built too close to a faction's towns
draws its campaigns (United Cities tax collectors, Holy Nation prayer-day inspections).
Each raid has its own goal, and the goal is the content: the Shek test your strength
(beat your people, leave the fallen, go home); bandits may stay and occupy your town
(ownership does not transfer; you have to drive them out); collectors and inspectors
demand and check. The faction-versus-faction targets of the first slice are a stand-in
to exercise the machinery until a player faction exists, not the design.
**Parked:** a "living world" where factions war on each other and towns are captured
and change faction. Interesting, but it needs major design decisions; not V1.

- **What the data gives.** Relations (`relations` + `default relation`, live as
  `state::Relation`), FACTION `campaigns` -> FACTION_CAMPAIGN (`chance per day`,
  `range near/far`, `repeat limit`, `num forces`, `squads to use`, `victory` /
  `loss trigger` follow-ups, `world state`). The original runs these only
  against the *player's* bases (territorial daily roll within range of the
  faction's towns); there is no faction-versus-faction campaign in the data.
  `FactionBook` (built from the merged data, configuration like the behaviour
  registry) resolves 179 campaigns: 117 eligible at the default world state,
  29 runnable (eligible, owned, territorial, ranged, with a force list).
- **State** (saved): `FactionAI` per faction: intent (Consolidate / Defend /
  Raid / Mourn), temperament (`aggression` = 0.4 + 0.6 x share of the
  town-owning factions it is hostile to, recomputed from the live relations;
  `alarm`, `grief`, `confidence` from events, decaying with half-lives of about
  a day), its raids, cooldowns on campaigns (the `repeat limit`) and target
  towns, pending follow-up campaigns, counters. `FactionEvents` singleton:
  events waiting for the next step.
- **Information up.** `SquadLost` (pushed by `Command::RemoveSquad`, the kill
  stand-in), `TownLost` / `TownGained` (pushed by the new `SetTownOwner`
  command), consumed at the next faction step. A raid squad that vanishes
  produces its event through whatever removed it. Few producers exist until
  combat and conquest do; the hooks and the scoring that reads them are real.
- **Faction step (every game-hour, serial, faction Id order).** Absorb events;
  decay; follow raids (squads Done / Failed are retired, a raid with no squad
  left concludes: arrived = completed, else failed with a 48 h target and 12 h
  campaign cooldown, and the campaign's follow-ups roll as absolute chances; a
  force with under 0.2 of its squads alive is defeated: the rest is retired and
  it concludes as a loss);
  utility over `Consolidate` (0.35 + 0.5 confidence), `Defend` (0.2 + 1.3
  alarm), `Mourn` (1.2 grief), `Raid` (0.9 x aggression x (1 - grief) for a
  faction that has campaigns and a navigator); the intent in hand keeps a bonus
  fading over 6 h. **The roll** is every 6 game-hours per faction (clock
  `FactionAI::nextRoll`), when the intent is Raid, nothing runs and no follow-up
  is queued: each territorial campaign draws `rand < chance per day x 0.25 x
  falloff x raidFrequencyMult` and one of those that pass is commissioned,
  picked in proportion to the chance; at most one campaign per faction at a
  time. Due follow-up campaigns fire whatever the intent (when nothing runs).
  Expand and Purge are named and not implemented.
  **Since the Lua first slice (2026-10-31) the temperament decay, the event
  weights and the utility scoring above are the default script
  `cpp/data/behaviour/default/faction_intent.lua`**; the C++ copy
  (`UtilityFactionLayer::nativeDecide`) is the engine fallback and the reference the
  two are tested bit-identical against (see "Behaviour layer: Lua"). The roll, the
  lifecycle and the relation reasons are C++. Direction (user, 2026-10-31, not
  scheduled): no silent duplicate fallback; broken scripts fail loudly, like broken
  mods; the native copy moves to a retired directory, not deleted (LUA_API.md 8).
- **Targets (data silent: a design choice, see questions in the report).**
  Hostile campaigns (`is hostile`) take towns owned by factions at or below
  -30 in the raider's view (the exe's hostile threshold); the others (visits,
  tributes, traders) towns above it; towns outside the faction's `biomes` or
  inside its `no-go zones` (unless `ignores nogo zones`) are refused. Distance is
  from the faction's nearest town; beyond `range far` (metres x 10 units) is out,
  the chance falls linearly from `range near` to `range far`. Follow-up
  campaigns ignore the range. A faction with no hostile neighbour in range
  does not raid. The player's bases are not special-cased (there are none yet).
- **Commission.** The force is drawn from `squads to use` (own, else the
  nearest `inherits from` ancestor's, else the owner's roaming squads) until
  `int(num forces x raidSizeMult)` men or 12 squads, spawned positioned at the faction's town
  nearest the target (`SpawnSource::Campaign`), each with a `RaidOrder`: target,
  origin, stay (the exe's on-site time by campaign `key`: 4 / 12 / 1 h), retreat
  (3 h), deadline (1.5 x the walk there and back plus the stay, the retreat and a day). A route check (the coarse graph)
  comes first; if it fails or no force can be raised, the try is rejected with
  short cooldowns on campaign and target, and the next candidate is tried in
  the same step (3 tries).
- **Group layer: `Behaviour::Raid` and `GoalKind::Raid`.** Intent flows down as
  a `RaidOrder` in `GroupAI`. An Issued order outranks every ordinary goal
  (even a rest or a walk in progress); the squad routes to the target town,
  stays until `until`, holds a 3 h `Retreating` state, routes back to the origin, and the order is Done. A route
  that cannot be found, a deadline, no progress for 6 game-hours, or a squad
  that cannot act (no position / outside the window) mark the order Failed
  with the reason and the squad falls back to ordinary life in the same
  step. The faction reads Done / Failed back from the squads, retires the
  campaign squads (they exist for the raid only) and picks a successor on a
  later step: no terminal state at either layer. Squads of raid-keyword
  packages in the default world (slaver residents, ...) now map to
  `Behaviour::Raid` and live ordinary life; they are not commissioned.
- **Faction intent flows down** to ordinary life too: a Defending faction's
  squads weigh town visits at 0.4, a Mourning one's at 0.7.
- **Cost.** A faction step scans the towns for each faction that has
  campaigns (24 of 103); a raid is one coarse route and a handful of spawns. See
  STATUS for the whole-world numbers.
- **Not decided / OPEN.** (Targets are decided above: the player only;
  faction-versus-faction war is parked.) The per-raid goals (the exe's phase
  packages: `Package_WarAssault`, `Package_OccupyTown`, ...); the player-only gates (population, town level),
  per-phase AI packages; relation producers other than a killer-attributed
  `RemoveSquad` (the reason table is implemented; hits, treatment, capture,
  war declarations await combat); members counted for the defeat fraction;
  what a raid *does* at the target
  (nothing yet: no combat); Expand / Purge; faction replacement of wiped-out
  `UNIQUE_SQUAD_TEMPLATE` squads (`replacement time`).

**Town gates, first slice (2026-10-31): who may pass.** PROVISIONAL unless marked;
code `cpp/include/ksim/nav/doors.hpp`, data and exe notes in `docs/reference/DOORS.md`, test
`ksim_doors`. Save v6.

- **What the data gives.** Doors are child BUILDINGs; `always locks` (gate doors), `is wall gate` /
  `is gateway` (the parent), TOWN `is public`. There is *no* gate schedule, curfew, lockdown or
  alarm rule in the data or the exe (DOORS.md): the original's gates are opened and closed by
  characters running door tasks, and a locked one needs the faction's gate code. So everything
  beyond `is public` and `always locks` below is our design.
- **Owner.** `DoorSystem` (in `AiSystem`, run at the end of the strategic step and synced before
  each squad step) owns the `knav::DoorBook`. Saved: `state::TownDoors` (posture Open / Alert,
  since, hold, alert count; on a town only once it left Open) and `state::DoorOverrides` (the
  `SetDoorState` command: one door Open / Closed / Locked). Everything else is derived and
  rebuilt after a load: the gate-to-town binding (nearest town centre by xz, no cutoff as the exe,
  `docs/reference/GATE_CREWS_LIVE.md` 3; one function, `ksim/ai/gatebind.hpp`, used by the spawn rule and `DoorSystem::bind`; a crew's own binding, `GateCrew::gate` / `home`, is saved and rebinds through `resolveGate` in the same header: own gate, home gate, town's nearest free gateway, else the crew stands down; `GATE_CREWS_LIVE.md` 5), the book, the navigator's door view.
  House doors are not operated (open to all, as baked).
- **Rules.** Default Open. A town with `is public` false (36 records) refuses neutral and
  hostile travellers always. **Alert** = the owner's intent is Defend, or a hostile raid of
  another faction has `started` towards the town: the gates go Closed (travellers with access
  wait: the DoorClosed cost) and refuse hostile travellers; `always locks` gates also refuse
  neutral ones (nobody without the gate code can open them); held 6 game-hours after the
  cause ends, then Open. No night rule (no data; `public night` is about homes).
- **Traveller class.** knav's 8 classes, four used: Resident (same faction as the town's owner),
  Ally (the owner's relation to the traveller's faction >= 50), Neutral, Hostile (<= -30; both are the exe's global thresholds); a
  factionless squad is Hostile (animals do not open gates); an ownerless town treats all as
  Neutral. The class is the stance towards the town the squad is **going to** (nearest gate
  town centre to the goal within 1,500); a trip to no town is Neutral; **a squad standing inside
  a gate town (700 of its centre) travels as Resident, so leaving is never refused**. A route
  through other towns' gates en route uses the same class (leak, below).
- **Navigator.** `Navigator::applyDoors(book)` (tick boundary: the end of the strategic step, or the
  next squad step after a command / a load) repaints every region mesh's door polygons and the
  windows built later (`NavMesh::applyDoors`, no tile rebuilt); `routeFor` / `localRouteFor` /
  `nearestReachableFor` take a `Traveller` class and pass it to the coarse graph's
  `DoorQuery` and to Detour's `TravelFilter::doorClass`. **Connected sets and doors**: the
  polygon sets are now labelled *without* door polygons; each door lists the sets beside it, and
  a per-class table (built on first use after a book change) unions the sets of the doors that
  class may pass. A goal pick is "same set as the squad as this class sees it", so a squad never
  picks a goal behind a gate it cannot pass; a coarse route whose ends are in different sets
  is answered at once (no graph search; DoorRefused when the sets are one with every door open),
  which also removed the whole-graph proof searches for plain unreachable pairs.
- **No terminal state.** Refusal only removes goals: a squad outside a closed town picks goals on
  its own side; a squad inside leaves (Resident); an override Locked traps both sides in their own
  connected sets, where Patrol / Roam / Idle always exist. A DoorRefused goal fails like NoRoute
  (24 h cooldown for travel goals) and the next candidate is picked in the same step.
- **Not decided / OPEN.** Guards operating gates (request, check, open, wait, close) and the
  "opening" state; queues and capacity; the original's gate code (who holds it); house doors and
  `public night`; raiders bashing or lockpicking a closed gate (now: refused, the raid order fails
  with a cooldown); rechecking a route in progress when a gate closes behind it (squads keep
  their route); class per door owner on a route through several towns (one class per trip).

**Aggressive squads (MQ14 path 2 + BQ10, 2026-11-02; `docs/reference/BAR_FIGHT_LOOP.md` "MQ14 path 2", kinspect `bq10_goal21.md`, FEATURES row "Squad goal ATTACK_ENEMIES_AND_NEUTRALS").**
- Hostility stays "different team" (`worldTeam`) plus: the squad's personal hostile list (`Fighter::hostileSquads`, read first) and goal 21. A world-squad character gets `Fighter::attacksNeutrals` and its hour window from its OWN role list of the squad's package (`Simulation::attackGoalWindow`; leader / squad / squad2 / slave); the window is checked every evaluation.
- `CombatArena::enemy` then holds for any target that is not own squad, not a unique squad, not bonded (same faction or relation >= 50, `Fighter::faction` = the faction's `state::Id`), not a `coexistence` faction, and within 6517 u (or in the aggressor's home building). The sets are derived every 10 s or when the present factions change (`Simulation::stepCombat`, never saved).
- Answering is on targeting: `setTarget` adds the attacker's squad to every member of the victim's squad (type-4 entry; not when the factions are allied; other squads of the faction are not flipped). Allies still join through `joinFight` (PROTECT_ALLIES).
- No terminal state: the chase monitor (all chasers, kinspect s73_static.md section 3) clears a chase after 66 game minutes (60 U) even while the target is sensed; ours drops the target and skips it for 20 s (`kChaseClearOrders`, `kBlockedTarget`; the original's 20 s block is for path failures, which approach does not report: UNSURE). The unsensed-target give-up (13.2 min) is unchanged.
- Persisted (save v53): `faction`, `attacksNeutrals`, `aggroStart`, `aggroEnd`, `aggroBlockId`, `aggroBlock`, `squad`, `uniqueSquad`, `hostileSquads`. `ksim_ai_coverage` counts the packages and squads; `--aggressive` lists the nearest to The Hub. Path 1 (dialogue muggings) is DIALOGUE slice 1.

**Combat notes: KO, chase and notice (bar-fight audit, 2026-11-02; `docs/reference/BAR_FIGHT_LOOP.md` findings 0, 0b, 0c; kinspect `~/git/kenshi/kenshi-inspect/findings/s73_static.md`, `ko_death_model.md`, `item51_ko_causes.md`, `perception.md`).**
- **The downed are never struck and the untreated die slowly, as in the original.** Targeting requires a usable (awake, uncarried) enemy, so
  nobody hits a KO'd body. A KO'd character with a vital part past its KO point never wakes, and the original's degeneration sink (`dt x 0.05 x 1.5 x 1.2 x T5` per
  medical second, medical clock 2) takes it to death in 2-5 game hours unless it is treated or carried to care. No rate or rule was changed
  (`ksim_ko_care`; the live shallow / deep fates, blood regeneration and the 1/4 sink are pinned in `ksim_ko_fate`, BAR_FIGHT_LOOP 0d). Limb leak and dismemberment (BQ11): only a severed limb leaks (`H + R`), a hit past -M severs a humanoid's limb (`Dismemberment` 0/1/2, default 1), the severed state is the limb's max forced to 5 (saved as is), the sink stops at -3M; `CombatRules::trySever`, RESOLUTION 5.4. A medical KO (the sink, blood or hunger, no hit, also the re-KO of a woken shallow KO) emits the same `Knockout` event as a hit's KO, once per KO transition, `other` (attacker) 0; the wire event 3 is `id` only, so no layout change (SIMLINK EVENT table; test `ksim_ko_fate` 4, `ksim_combat_armed` event count). The intruder carry-out drops outside the building's footprint hull (the Bar's reaches ~50 u past its door; `stepIntruders` steps the drop point out). Care that the original has and we lack: NPC medics on allies with a kit (goal 58), and the town variants of the intruder carry-out (273 / 274).
- **Chase layers.** Goal layer: a target is held while it is sensed or the attacker `learn`s of it every tick. Task layer (`chaseGivesUp`): in Approach on a target no scan senses,
  after 5 game minutes of chase the fighter gives up at more than 13.2 game minutes unsensed or within 60 u of the record's position (EV_GIVE_UP_CHASE, then the squad AI's
  go-home). Game time is 1 game minute = kGameDaySeconds / 1440 sim seconds. Test `ksim_chase_giveup`.
- **Notice range.** `perception::visionRange`: 500 only inside a walled (gated) town, else 1500 x the race's `vision range mult`; x (0.5 + 0.5 daylight), so x0.5 at night.
  The original keys on the navmesh gate-region id (`~/git/kenshi/kenshi-inspect/findings/bq7_sight_region.md`): the unwalled Hub is 1500 even inside the Bar. The sim approximates the
  walled region from the gate sites (`Environment::walled`, set by the Simulation; UNSURE, a region partition would be faithful). See BAR_FIGHT_LOOP finding 0c. A town extent pass and sight across big towns (the original splits a sprawling town
  into blind pockets) is parked as FUTURE_IDEAS.md §9.
- **Crowd spacing** (2026-11-02, BAR_FIGHT_LOOP finding 0e, kinspect `bq9_crowd_spacing.md` / `s74_static.md`): a melee crowd is spread by `CombatSteer`'s two avoidance terms (per-character random comfort radii and dead zones, `CombatArena::comfort`, a pure function of seed and id, nothing saved), a 7.07 u depenetration between characters in combat (UNSURE), KO'd bodies skipped by both, the 5-slot engaged list sending the sixth attacker on to Waiting (state 6), and an incoming-threat list that also counts attackers with me in their hit list. Never a flat push-apart. Pinned by `ksim_crowd_spacing`; fixture gates `ksim_fight_crowd` / `ksim_fight_1v1`.

### Behaviour layer: Lua (decided in outline, user 2026-10-31)

High-level AI decisions are Lua scripts in a sandbox; the engine is C++. Lua is a
**behaviour layer, not a combat computation replacement**: it decides *what* to do, the
engine computes *how* and *whether it works*. Since 2026-11-07 it is the **override** path: vanilla packages run on the faithful native runtime ("AI packages"
above); a Lua unit replaces a package when a mod (or our default set) ships one. Our own design choices ship as the **default Lua set** (raid policy, wilderness
respawn, gate rules, town life, later the parked "living world"); mods override or extend it.

- **What Lua decides.** Faction intent and campaigns, group scenario goals, an individual's
  goal choice (not its execution), reactions to noticed events, dialog conditions and
  choices. Hundreds to thousands of decisions per game-hour, not per frame.
- **What stays C++.** Combat (it matches the original near 100%; mods tune it through data
  constants only, as slot mods do now), movement and steering, pathfinding and the navmesh,
  animation timing, perception and detection, damage and medical state, the economy's
  arithmetic, the region threads, saving.
- **Event subscription, not polling.** A script declares what it cares about as a subscription
  the engine evaluates natively: "a character within N carries an item of category X", "a
  stranger enters my building", "relation to F drops below -30", "my squad lost a member", a
  timer. C++ does the detection cheaply each tick (the same perception the engine needs anyway);
  Lua is woken only when a subscription fires, to make the decision. Subscriptions are data
  (filter + radius + rate), so the engine can batch, index and budget them.
- **Read a view, return commands.** A script gets a read-only view of the relevant state and
  returns commands (set goal, start dialog, change relation, issue order), applied by the
  engine like any other command. Scripts never write the world directly.
- **Determinism.** One Lua state per region thread; no shared mutable state between them.
  Scripts get the sim's seeded RNG (no `math.random`, no clock); Lua's string-hash seed is
  fixed so `pairs` order is reproducible. The 1 = 4 workers and save/continue tests cover
  scripted behaviour too.
- **Saves.** No Lua heap dumps. Script state lives in declared fields on entities (typed,
  versioned, in the save format like any other field); scripts are reloadable.
- **No terminal state, enforced by the engine.** A script error, an exceeded instruction or
  memory budget, or an unusable result falls back to the C++ default (ordinary life). A broken
  mod can't freeze an NPC; the failure is logged with the script and subscription.
- **Sandbox.** No `io`, `os`, file `require` or `debug`; instruction-count and memory limits
  per call; mods load through a manifest with an explicit load order (the sim publishes it, as
  it does the record stack).
- **Runtime: Lua 5.5.0 (user, 2026-10-31)**, vendored pristine in `cpp/third_party/lua-5.5.0`
  (MIT). Not LuaJIT: its trace compiler falls back to the interpreter at ordinary C-function
  calls (our whole API), its speed needs the FFI (which breaks the sandbox), and it is stuck at
  5.1 semantics. 5.5's `global` declarations turn mod typos into compile errors. The engine
  creates states with `lua_newstate(alloc, ud, seed)`: our allocator enforces the memory budget,
  the seed is fixed. Table and userdata keys hash by address, so `pairs` over them is not
  reproducible: the API hands scripts arrays or sorted keys, never object-keyed maps.
- **Stable API.** Every decision goes through a documented API (views, subscriptions,
  commands). The faction scoring of the first slice moves behind it first.
- **First slice (2026-10-31, `docs/reference/LUA_API.md`): SETTLED.** The faction intent and
  temperament scoring (Raid / Defend / Mourn / Consolidate, alarm, grief, confidence) is a default
  script, `cpp/data/behaviour/default/faction_intent.lua`; the exe-grounded campaign roll, the
  lifecycle and the relation-reason table stay in C++. What the slice fixes:
  - *Host* (`ksim/core/script.hpp`): Lua 5.5.0 as a static C library (no `lua.c`; io, os, debug,
    package and coroutine not even compiled), `lua_newstate` with our allocator (memory budget,
    4 MiB) and the fixed seed, a count-hook instruction budget (50,000; sticky, so `pcall` cannot
    swallow it), a whitelisted environment per script (base without load/require/getmetatable/print,
    `tostring` restricted to non-address values, `collectgarbage("count")` only, string without dump,
    table, math without random, utf8, and `ksim.random/random_int/log`), text-mode loading only,
    errors caught and returned. A host is one `lua_State`, not thread safe, one per thread.
    Data crosses as `ScriptValue` trees (sorted maps, arrays), never Lua references, so a Lua error
    only ever unwinds Lua frames.
  - *Shape*: a script returns a module `{ api = 0, subscriptions, fields, decide }`. Subscriptions are
    data (`timer` by clock phase, `squad_lost`, `town_lost`, `town_gained`, `raid_outcome`,
    `relation_crossed` at the exe's -30 / 50 thresholds); C++ queues what fired and `decide(view)`
    runs only for those. The view is read-only data; the reply is a map of validated commands
    (`set_intent`, `set_temperament`, `set_state`) applied together or not at all.
  - *State*: declared, typed fields on `FactionAI` in the save (v8); no Lua heap is saved.
  - *No terminal state*: error, budget, memory or an unusable result runs the native C++ scoring
    for that faction-hour from the untouched state. The native code is the reference the default
    script was transcribed from operation by operation: **scripted and native runs are
    bit-identical** (state hash, `ksim_behaviour`), and so is a run whose script always fails.
  - Selected by `AiSystem::Config::scripts` / `KSIM_BEHAVIOUR=native`; scripted is the default.
  - Cost (whole world, 10 days, 1 thread): see STATUS.
- **Not decided / OPEN.** Commands beyond intent and temperament (targets, group goals, an
  individual's goal choice: how much is scripted vs engine defaults); the rest of the subscription
  catalogue (the native perception subscriptions: items within N, a stranger in a building); mod
  manifest and load order; hot reload in a running server; how dialog (the data's DIALOGUE records)
  and scripts meet; a watchdog for time spent inside one C library call (pathological patterns);
  running scripts on region threads (the host is ready for it, the faction step is still serial);
  an installed data directory for the shipped scripts.

## Item provenance (intended, user 2026-11-08)

Every item stack silently records where its units came from: who **planted** it
(put it into another character's inventory or equipment through the loot / trade
screen, or fitted it, e.g. a robotic limb) and whom it was **stolen** from (the
victim faction, for Fencing). Nothing in the base rules reads "planted by"; it is
there so mods can (planted evidence, framing, the robot-limb crime of
FUTURE_IDEAS 14). User: "maybe we should silently start tracking 'planted by'.
Modders could have a field day with 'planted evidence' type of data."

- **Stolen is visible, as in the original** (user 2026-11-08: "the game already forbids
  combining normal first aid kits with stolen first aid kits. You can't smuggle it by
  combining items"; kinspect dt43 D, `findings/dt43_stolen.md`). The mark is `InventoryEntry::stolen`, the victim's faction sid
  ("" = clean), per stack:
  - **the first victim sticks**: a theft, a pick-up or a drop stamps the mark only on a clean stack; a stack never changes victim
    (the original stamps only an empty owner hand, dt43 D section 3);
  - **a stack merges with the same victim only**: the mark is part of the merge key, so stolen never joins clean, and stolen from
    A never joins stolen from B (dt43 D section 5);
  - **the mark clears** when the stack goes into a box of its victim faction, into the pack or body of a character of that faction
    (a give to the owner, or the owner taking it back), and into shop stock (a fence launders); a nested container's contents go with
    it. Nothing else clears it: a plain pick-up, a trade buy, a give to another faction and crafting keep it (dt43 D section 4);
  - **saved** in the stack (`stolen`, save v89), and a GroundItem keeps it too, so dropping is no way to smuggle. The original saves a
    victim hand under `ownedbyTYPE / C / CS / I / S` (dt43 D section 7); there is no import of it yet (SAVE_IMPORT `ownedby*`).
  - UNSURE: the original marks the victim *hand* (a character or a building), ours the victim's faction, so two victims of one faction
    merge here and not there; the exact predicate of the give-to-owner clear is also UNSURE in dt43 D 4.3 (ours: the receiving
    character's faction is the victim's).
- **Planted is silent**: it never changes stacking, the merge key or anything the
  player sees. A stack keeps a short list of {units, planter, time}; a merge
  concatenates, a split takes units first in, first out, so nothing is lost.
- Saved (a save version bump), readable from Lua; a "planted" event fires with
  (planter, holder, item).
- The stolen half follows the original (dt43 D, above).

## Weather (decided 2026-11-01)

- The sim owns the weather: one saved region per BIOME_GROUP (season, end day, current instance with its strength), ticking whether or not anyone is there, as the original does (kinspect 37, `reference/WEATHER_CYCLE_LIVE.md`).
- The rolls come from seeded per-group streams, so a save, load and continue is identical; the original uses the CRT rand.
- Gameplay reads the region at a character's position (perception sight, later weather damage). The client gets every group's state (SIMLINK WEATHER 41) and blends by camera itself.
- The two original bugs are not copied (FEATURES "Original bug" rows).
- Built (2026-11-01): `ksim/world/weather.hpp`, save v33 (`formats/RUNTIME_SAVE.md`), SIMLINK 41 sent; what differs and what is UNSURE: `reference/WEATHER_CYCLE_LIVE.md` "Implemented".

## Living towns (intended)

Towns are state, not versions: owner faction, population, prosperity, damage,
garrison strength, unrest. Spawns, shops, guards and building variants are
derived from that state. Kenshi's existing town variants serve as the set of
visual states to choose from.

- **Posts.** A town has posts that need staffing: gate, guards, shops, bar,
  patrols. An empty post is town state the faction layer can see; refilling it
  is an ordinary faction goal, competing with the faction's other priorities.
- **Requests nobody fulfils escalate.** A request that goes unanswered (open
  the gate, serve me) never vanishes silently. It becomes an event the layer
  above re-scores on. Example: a caravan finds an unmanned gate → its squad
  goal becomes "report to a friendly town" → its faction weighs reclaiming
  the town against abandoning it.
- **News travels through the world.** Factions learn of events through carriers
  (survivors, caravans, patrols), not instantly. This gives delays, messengers
  that can be intercepted, and factions acting on different beliefs about the
  same events.
  - Knowledge has a location. An event starts with its witnesses and spreads
    when they travel and talk, along roads at travel speed.
  - Major news gets a dedicated courier, a real squad that can be killed,
    captured or delayed. Minor news spreads as rumour through caravans and
    travellers, and can be garbled.
  - Away from players, a courier is a squad on the road graph at strategic
    level carrying an "event #n" tag, so news in transit costs almost nothing.
  - Factions and towns act on what they believe, not on what is true.
- **Guards notice what you carry.** The original scans only the walking
  character, so a player can pick up a jailed ally and carry them out of
  town unchallenged (user, 2026-10-30). Carrying is also how bounties are
  legitimately delivered. Rule (user): the **prisoner flag** decides, not
  the carrier's faction. A bounty target or an unconscious stranger carried
  into town has no prisoner flag, so carrying them is not noticed.
  Delivery (to the police, a jail, a slaver) is what sets the flag. From
  then on, anyone but the holding faction carrying that character is
  noticed, and leaving with them is a crime.
  A wanted character carried in has no prisoner flag but is **wanted
  here** (the town's faction has a bounty on them), so it gets the
  intent check (user): carrying them in is allowed, since that may be a
  delivery; carrying them out of town is aiding a fugitive. So guards
  judge a carried character by their status *relative to the town's
  faction*:

  | Carried character | Bring in | Take out |
  |---|---|---|
  | No status here (stranger, a bounty from elsewhere) | free | free |
  | Wanted here | allowed (delivery) | crime |
  | Prisoner held here | n/a | crime, unless the holding faction moves them |

  One rule for every faction and every town type. Noticing uses the
  ordinary notice mechanics (range, line of sight, recognition), but a
  character carried over the shoulder gets -100% stealth (user): anyone
  who sees the carrier sees who is being carried. Seeing the carrier is
  the game: sneak in at night, keep your distance from guards and away
  from lights. That is the sandbox play this rule exists for, so
  perception must model distance, line of sight, light level (time of
  day, lamps and fires placed in town) and the carrier's sneak skill.
  Original mechanics: `docs/reference/CRIME_NOTICE.md`. The original
  does inspect the carrier's load, but its crime writer refuses a victim
  of the carrier's own faction, which is the whole exploit; its sight
  range already has a day/night factor and an awareness accumulator,
  and each character has a light level (time of day plus nearby
  building lights and lanterns) that scales who sees whom
  (`docs/reference/LIGHTING_EXE.md` Q10). We keep that model.
  Related: witnesses who are friends of the carried (kidnapping event) in
  `docs/DIALOGUE.md` D1; the gap row in `docs/FEATURES.md` ("Original gap");
  where the original's chain breaks in play: `docs/reference/CARRY_KIDNAP_LIVE.md`.
- **Town life: residents become Fighters; the resident door routine is part of it** (user, 2026-11-01, REPORT_2026-11-01 §4). Town residents are promoted to full `Fighter`s (they act, not just stand in state), and the residents' own **door close / lock routine** (the house-locked byte of TRESPASS_LIVE; FEATURES "Trespassing" row) folds into the town-life / world-sim work: design and build it there, not as a separate crime or dialogue slice.
- **Kenshi's world-state flags trigger nothing.** In the original, a global
  flag (for example "Phoenix dead") instantly swaps the version of towns
  anywhere in the world. We use those town versions only as visual states and
  starting conditions. Our simulation decides when a town actually changes.

## Town economy and jobs (intended, proposed)

The phased design (merchant refresh, wandering traders, in-town routines as a per-member goal scorer (§5: hour windows, utility × weight × crowd decay, role lists, a derived reservation book; T3b2 runs it for residents at Full detail, `routine_scorer.cpp`, TOWN_LIFE_SLICE 5.15; T3c adds beds, the resident door routine and shop closing, 5.16; T3d adds the bar hangout, a claimed table per squad, 5.17), the real-economy overlay and its coarse model): `docs/reference/TOWN_LIFE_SLICE.md`.

The original's automation is the part of the game closest to something great
and the most broken. Evidence and hypotheses: `docs/reference/ORIGINAL_AI.md` §8b.
Direction (RimWorld, Dwarf Fortress or Factorio-logistics style), not final:

- **A job board per town or base**, not per character. The town posts jobs (mine,
  haul, craft, build, staff the gate). Workers claim them; each claim **expires**,
  so a dead or distracted worker never takes a job down with them. Priorities and
  worker preferences decide who takes what.
- **Supply and demand, not searches.** Producers advertise output ("50 stone
  ready"); storage advertises capacity ("room for 200, accepts stone"). The board
  matches them and posts complete hauling jobs with **both ends reserved**. A
  worker never picks something up without a reserved destination.
- **The search covers the whole town, never what's loaded.** The world model is
  always in memory, so a huge base can't hide machines.
- **No deadlocks.** "All storage full" or "input empty" becomes a request that
  escalates through the layers (build storage, sell surplus to a caravan,
  throttle production), like the unmanned gate.
- **Same system for the player and NPC towns.** Only who sets the priorities
  differs. NPC towns really produce, which feeds real trade, caravans and prices.
- **Honest at every detail level.** Away from players a town's economy runs as
  aggregate flows over the same board data (rates per game-hour); on promotion,
  stockpiles match what the flows produced.

### V1: the original's per-character jobs (proposed 2026-11-01, first slice implemented 2026-11-01, kenshi-b0)

The original's player-facing jobs list stays as the V1 surface. Source: kinspect
`~/git/kenshi/kenshi-inspect/findings/jobs17_static.md`, and the live half in `findings/ui_captures/jobs.md`.
The job board above comes later, behind the same executors. Wire: SIMLINK JOBS (34) and orders 76-78
(implemented; choices listed in SIMLINK). UI: `docs/UI.md` `jobs.*`. Checklist: `docs/FEATURES.md` "Jobs list per character".

- **Data.** A character holds the `state::Jobs` component:
  - `enabled`: the JOBS toggle. Default on.
  - `rows`: a list of `{u32 jobId, u16 type, u64 target}`.
  - It is saved, and order survives a load.
  - `type` is the exe's TaskData number (jobs17 Appendix A), so the wire and the save never need a mapping.
  - `jobId` is per character, never reused. Remove and move name it, so a client's stale row can never hit the wrong job.
- **Adding** follows the exe's `addTask`:
  - Only types with `kind != 0` become rows.
  - A replacement type applies: Repairing becomes Engineer, First aid becomes Medic.
  - Duplicates are dropped: the same type is a duplicate when it has no target, and the same type and target otherwise.
  - Companion rows follow directly: Operate automatic machine adds Hauling to, Bodyguard adds Staying close, and so on.
  - Adding a job turns `enabled` back on.
  - Adding fans out over every character the order names. A plain order never touches the list.
- **Running.** Goal selection considers the rows below the URGENT tier (combat, fleeing) and above the idle tier.
  - Row order is priority.
  - A row that can't be planned falls through to the next one; it never stalls.
  - A row whose goal is already met is skipped.
  - With `enabled` off, only the exe's six still run: Attacking enemies, Staying close (both), Bodyguard, Robotics, Medic and Splint Rigging.
  - With `enabled` off, the idle behaviours are off too: ditch items, sleep when injured, sit when idle.
- **Executors** are shared with the board later. One executor per type plans a goal from the character's world view:
  - The first slice: Medic (58, FirstAid on the nearest wounded ally), Attacking enemies (16), Staying close (31 / 289) and Bodyguard (45).
  - These four use behaviours the sim already has: first aid, combat, follow.
  - Hauling to (124), Find and rescue / Find and put in bed (148 / 106) and Corpse disposal (137 / 138) come next, on carry and containers.
  - Operating machine, Engineer, Training and Turret wait for machines, building, dummies and turrets.
- **Implemented (2026-11-01, kenshi-b0 sim worker; save v38).** Code: `cpp/include/ksim/ai/jobs.hpp`, `cpp/src/ksim/ai/jobs.cpp`, test `ksim_jobs`.
  - Data, adding (replacement, dedup, companions, `enabled` on), the drag rule and remove are pure functions (`addJob`, `moveJob`, `removeJob`) over `state::Jobs`, which is saved in order.
  - The orders are the logged command `JobOrder` (CommandType 22), checked when given and again when applied.
  - The four executors of the first slice run in `Simulation::stepJobs` (every 10 ticks, after the order queues), reusing the first aid interact, the attack order and the follow task.
  - The exact rules and the choices (ranges, what counts as busy, the Bodyguard defence) are in SIMLINK "JOBS (type 34)", Implemented.
  - Still to build: the other executors listed above, and the idle behaviours that JOBS off disables (the sim has no ditch / sleep / sit yet).
- **No silent rows.** A type with no executor yet is refused with `unsupported` when it is added. It never sits in the list doing nothing. A target that is gone (dead, destroyed) leaves the row in the list, as the exe does, and the row's planner fails through.
- **Not in V1** (exe behaviour noted for later):
  - The enslave paths that wipe the list and add Obedient Slave (187).
  - Turret kind 2's separate scoring pass.
  - "Jobs dropped far from their outpost" (FEATURES).
- **The board relation.** A board claim will be just another source of a goal for the same executor. The per-character list stays as the player's override, ranked above the board's claims. Open: whether board-assigned work shows up as rows.

Open: how the coarse economy model is built and kept honest. One candidate is a
**background sampler**: periodically run one town at full detail for a short
window on a *copy* of its region (cheap with the frozen-state two-phase tick;
the copy is discarded, so sampling never disturbs the world), measure real
throughput, and use the measured rates to calibrate that town's coarse model.
The same comparison doubles as an automated honesty check in soak runs. Risks:
samples must be representative (time of day, raids, shortages), and the CPU
budget must stay inside spare worker time.

## Pathfinding (decided, except gate access policies: intended)

- **Two levels.** A coarse region/portal graph for long distances (HPA*-style);
  a detailed navmesh only for entities at full detail.
- **Our own navmesh** from Recast, built from terrain plus building geometry
  (render meshes are fine if the collision data is locked in Havok formats).
  Implemented first slice (2026-10-30): the collision is PhysX XML, not Havok, so the bake
  uses it directly (`COLLISION_FORMAT.md`). Library `knav` is separate from `ksim`; tiles are
  256 units, baked offline per zone rectangle into a cache file
  (`docs/formats/NAVMESH_CACHE.md`), queried with Detour. Agent size and climb are guesses
  (STATUS.md).
  Decided by the implementation: BUILDING `path mode` is honoured (IGNORE / PROJECTED /
  OBSTACLE / WALKABLE); dynamic changes (building placed, finished, destroyed) rebuild the
  affected tiles (`knav::NavWorld`, a pure function of terrain plus the active buildings,
  applied at the tick boundary), not DetourTileCache; the navmesh is derived and never saved
  at runtime (the cache is the static bake). Doors and gates are links, not geometry (below).
  Decided by the world bake (2026-10-31): one cache file per region of 4 x 4 zones, tile
  coordinates shared across the world grid; a `dtNavMesh` is a *resident* set of at most 8,192 tiles
  (32-bit poly refs), so full-detail regions are loaded with a margin and long routes use the
  coarse graph. The graph is derived from the baked tiles: clusters of 3 x 3 tiles (768 units),
  nodes = connected walkable regions of a cluster, portals on tile edges of the cluster boundary,
  edge costs = Detour path lengths inside the cluster. Squads at Squad/Strategic level move along
  its stored paths, so they stand on walkable ground (x, z; y is snapped on promotion). Tile
  rebuilds update only the cluster and its four neighbours.
  Implemented (2026-11-02): the server's full-detail mesh streams from the world bake, the zones of the
  players' areas resident (`ksim_server --navworld`), in place, derived and never saved
  (`docs/formats/NAVMESH_CACHE.md` "Streaming coverage").
- **Gates and doors are explicit portals** with a capacity and a queue.
  Crowds heading to a shared destination use flow fields.
- **(Decided, implemented 2026-10-31.) Door and gate links.** Every door (a child BUILDING of the
  `doors` list, placed with its parent's transform) is baked as walkable passage with its own
  polygons (areas DoorOpen / DoorClosed); tiles never depend on door state. A `knav::DoorBook`
  holds per-door state {open, closed, locked} and a denied-traveller-class mask (8 classes, chosen
  by the sim); `NavMesh::applyDoors` repaints the door polygons, no tile is rebuilt. Per traveller
  (`TravelFilter::doorClass`): allowed and open = normal cost, allowed and closed = a wait cost
  (DoorClosed, about 200 units of walking), denied or locked = excluded, so the search routes
  around or fails cleanly. The coarse graph keeps the doors each portal pair's path crosses and a
  door-avoiding bypass, and reads the book at query time (no cluster recomputed). Detail:
  `docs/formats/NAVMESH_CACHE.md` "Doors and gates". Capacity, queues and who sets the state
  stay intended, below.
- **(Intended.) Gates are operated, with access control.** The original game never closes
  gates (a closed gate would break its pathfinding), and high run speed lets
  anyone bypass town security. Ours:
  - Portal state (open / closed / opening) plus an access policy derived from
    town state: time of day, alert level, security, siege. (The state and the per-class policy
    are implemented in the nav layer; the first town rules that set them are in ksim/doors.*
    (alert from the owner's Defend intent or an incoming hostile raid, `is public`; no time of
    day, the data has none), see "Town gates, first slice". There is no "opening" state yet:
    closed has the wait cost.)
  - **Pathing cost depends on who is travelling.** Permitted travellers see a
    small delay (queue plus opening time). Others see the gate as blocked, or
    as a long wait, and route around or wait somewhere sensible, never
    against the gate. (Implemented: blocked, excluded from the search.)
  - Guards serve requests: an ally approaches, a guard checks, opens, waits
    for them to pass, closes. With no guard, the gate stays as it is, and path
    costs reflect that.
  - Events change the policy. An alarm or a known hostile closes gates and
    switches guards to intercepting, so sprinting through stops working.
- **(Decided.) Terrain cost is also per traveller.** The original appears to give water
  a fixed cost, so it routes a character with no swimming skill through deep
  water at 0.1x speed, where it gets eaten. That is unverified in the exe, but
  the user has observed it. Ours:
  - Path cost is travel time from the traveller's own speed in each medium
    (swimming skill, encumbrance, injuries), plus a simple exposure penalty
    for acid (WATER.md) and hostile weather.
  - A squad moving at sync squad speed uses one profile: the minimum of each
    relevant stat over its members (lowest swimming skill, lowest resist,
    and so on). There is no per-member micromanagement.
  - Acid water: armour resists don't protect while swimming. Only races
    immune by nature do (Robots, Hivers; per the user, not yet checked
    against RACE data).
  - A route that is slow but reachable is never refused outright (no
    terminal states). It simply loses to any reasonable dry detour.
  - Later, possibly: faction squads learning routes from experience. Out of
    scope for now.

### Walker kinematics (decided, implemented 2026-11-02)

How a walking character moves along its route, after the original's Pather (kinspect `s70_static.md`, `move_kinematics.md`;
`knav/walk.hpp`, `combat.cpp` `walkOrder` / `driveStep`; test `ksim_walk_kinematics`). Retired: the fitted 15 u/s^2 ramp and the
half-turn-per-second yaw.
- **Acceleration** is the RACE `pathfind acceleration` x 10 (Human 200 u/s^2; `knav::Gait::acceleration`), the same rate for braking: no separate
  deceleration. Athletics, encumbrance and injuries act on the top speed only (`runSpeed`).
- **Braking.** `Walker::speedLimit`: for the end of the route and every corner within braking reach the speed is limited to
  `sqrt(vCorner^2 + 2 a distance)`; the end's `vCorner` is 0, so a straight approach brakes at exactly `a` and the walk ends by the
  stop snap (`follow` lands on the last point, speed 0), no overshoot. A corner's `vCorner = sqrt(2 a L)`, `L = (footprint + 5) cos(turn / 2)`:
  the lateral offset of the funnel's pushed-out corner (radius + 0.5 Havok = 7.6 units for a human). A bend under 0.1 rad is no corner.
- **Facing** turns towards the next waypoint at 20 rad/s (`kTurnRate`; 4 in the combat-ready stance, `kTurnRateCombat`: while a
  technique plays the body turns towards its opponent at it, `integrate`, test `ksim_combat_facing`; the swing's box and root
  motion follow the technique's *aim* instead, `Fighter::aimYaw`, the look hand set at the technique start, while perception and
  the block behind tests follow the body — ENGAGEMENT.md 9.4; chases / follow in combat keep the old half turn per second,
  UNSURE: no source says whether they hold the flag; a swing does not wait for the facing, UNSURE: no source). Translation
  follows the route, not the facing, so the start moves at once while the body pivots, as in the original.
- **Re-path** after 5 s of barely moving (`Fighter::walkStuck`), like a block (bounded by `kMaxReroutes`).
- **UNSURE / not done.** The corner term `L` is a model of the funnel offset, not the exe's exact lateral distance (M, s70 section 2);
  no real funnel (string-pulling of the portal corridor) runs each frame, the route's corners are Detour's. Routes are still replaced
  at once (the original keeps the old corridor until the new path arrives). Attack chases and the follow task's direct steering keep the
  old half-turn-per-second yaw (combat.cpp, `chaseStep`, `follow`); they share the new acceleration through `rampSpeed`.
- **Carried leaders** (`stepCarriedLeaders`, residents.cpp) walk to a carrot on the group's route with this walker. The carrot's height is the
  mesh surface at the leader's level (not the terrain), an unreachable carrot (or one whose nearest mesh point is where the leader
  stands) is replaced by one further along the route, and 10 s with no routable carrot places the leader at the route's end, a last
  resort against a terminal stall: counted in `ResidentStats::leaderWarps`, logged to stderr, and printed by `ksim_soak` (should be 0).
- **Turn in place** (drive 0 in the drive hold) and **operator facing** use the same turn: SIMLINK.md section 12,
  `docs/reference/MINE_SELL_SLICE.md` MQ16.

### Talk approach goal (decided 2026-11-02)

A Talk (interact action 6, trade) to a character picks the goal first: `routeToInteract` (interact.cpp) asks
`MeshNavigator::routeToStand` for navmesh points on three rings (0.5 / 0.75 / 0.9) of 16 angles round the target within the
stopped `characterReach`, keeps those with a clear line (`lineClear`), paths to each and walks the shortest; the target's own position
is only the fallback, and every other action routes as before. Order 60 and the queued order both go through it, so the wire is
unchanged and players and AI share the rule. `stepInteractions` picks again when the route end (or the stopped actor) is out of reach
of a target that moved. The line test has no wall / furniture classes to read: a thin gap (<= 40 u) whose walk round is short
(<= 3 x the line + 60) is a counter and is talked across; a longer way round is a wall. UNSURE, a stand-in. Test `ksim_talk_approach`
(hand-built meshes). Feature row: `docs/FEATURES.md` "Talk / trade approach stops on the near side of a counter"; retired code:
`cpp/src/ksim/retired/interact_talk_route_to_target.txt`.

## Data layer (decided)

The data layer reads Kenshi's files once and builds our world model.

| Source | Contents | Status |
|---|---|---|
| `data/gamedata.base`, `data/*.mod` | Game records (items, characters, factions, towns, dialogue) | Known: `docs/formats/MOD_FORMAT.md` |
| `data/newland/leveldata/**` (672 `zone.X.Y.zone`, `*.level`) | Placed buildings per cell, towns, roads, interiors | Known: `docs/formats/WORLD_LAYOUT.md` |
| `data/newland/land/` | 16-bit heightmap, biome/blend maps, features | Mostly known: `docs/formats/TERRAIN_FORMAT.md` |
| `data/globalPathing.path` | Original world-scale portal graph | Layout known, semantics partly: `docs/formats/GLOBAL_PATHING_FORMAT.md`. Reference only |
| `data/newland/land/navtiles/*.hkt` | Baked Havok navmesh | Runtime navmesh base (read at runtime, never at build): `docs/formats/NAVMESH_CACHE.md` "Runtime mesh" |

`KENSHI_MOD_FORMAT.md` in the install directory is **unreliable**. It's partly
right and partly invented, and a byte-for-byte round trip was never achieved.
Treat it as a set of guesses to test. The parser passes when every byte of
`gamedata.base` and the shipped `.mod` files is consumed, with nothing left over,
and the record counts are consistent. Compare against community parsers where they exist.

### Coordinate system

Verified against zone files, the heightmap and `globalPathing.path` (see the format docs).

| Item | Value |
|---|---|
| Units | Kenshi world units (FCS `KenshiZoneSize = 4608`) |
| Axes | x, z = ground plane; **y = up** (height). Instance/feature positions are `(x, y, z)`; a zone instance's `y` is **relative to the terrain height** (`docs/formats/COLLISION_FORMAT.md`) |
| World extent | x, z ∈ [-147456, +147456) = 294,912 units per side; heightmap y 0..9,800 |
| Zones | 64 × 64 cells of 4,608 units. `zone.X.Y.zone`: `X = floor((x + 147456) / 4608)`, `Y = floor((z + 147456) / 4608)` |
| Zone index | `X + 64·Y` (`features.dat`) |
| Heightmap | 16385² samples, 18 units apart; col = x, row = z, row 0 = z min; `y = raw · 9800 / 65535` |
| Coarser grids | blend/biome cells 32 × 32 (2 × 2 zones); `globalPathing` blocks 16 × 16 (4 × 4 zones, 18,432 units) |

Parsers and validators live in `tools/` (standard-library Python).

## Validation (decided)

There's no original behaviour to match, so the headless simulation is its own test suite:

- **Long soak runs** (thousands of game-hours, as fast as possible), measuring:
  idle or goal-less NPCs (target: zero), stuck time, goal flip-flop rate,
  path success rate, gate throughput, and the cost of each tick at each level.
  **Measure these as trends over game time, not snapshots.** The original
  world decays: players periodically "import" (reset the world but keep their
  characters) to repair broken NPCs. A soak run fails if any of these metrics
  keeps drifting; day 500 must look like day 1.
- **Scenario tests**, for example "500 NPCs, one gate, all through in < N s",
  "a raid reaches a town no player is near and resolves on arrival",
  "a squad routs and rejoins its faction", "leader killed → faction temperament shifts".
- **Fast-runner test** (guards against the original's world-pause while zones load):
  a character at maximum run speed crosses the whole map, plus a second player
  character ordered across the map at the same time. Pass: no tick exceeds its
  budget (record the maximum and 99.9th-percentile tick time), the world never
  pauses, and every promotion places both runners on walkable ground. A tick-time
  spike at a promotion boundary is a failure, not a tuning issue.

These measurements are the score every AI or pathing change is judged by,
including changes written by agents.

## Milestones

The live list is docs/ROADMAP.md ("Milestones").

## Open questions

**What counts as a blocker (user, 2026-11-01):** a question blocks work only if its answer shifts the foundation (state model, wire, save layout, threading, system ownership). Game-design choices that playtesting can settle get a default (the exe's behaviour or our lean, switchable where cheap), are tagged "deferred: decide in playtesting", and do not hold up code. Example: gate guards' forced search dialogue and pausing the game for it. STATUS "Waiting on the user" lists only foundational ones.

- **Direction (decided (user, 2026-11-01, REPORT_2026-11-01 §4), REPORT_2026-11-01 §4):** sim: **world sim first**: a whole-world soak with today's systems, the AI package coverage report, then town life / economy (caravans, production camps; residents as Fighters folded in). Client-requested frames (buildings, benches) queue behind it (FEATURES "Base building"). The client does a playable loop first; combat cadence is deferred behind a side-by-side demo. Dialogue is unblocked but queues behind the world-sim work unless judged part of town life (DIALOGUE.md §11, §12.8).

- Town economy: the coarse model and the sampler idea (above); the job board design is proposed, not final.
- Multiplayer: designed for (server boundary, region threading), not built. Replication protocol and interest management are open.
- Terrain blend payload, trailing blocks in zone files and other unknowns listed in `docs/formats/`.
- Faction layer: relations and the `FACTION_CAMPAIGN` data seed it (done, first slice); targets when the data is silent, the player-base trigger and war between factions are open (see "Faction layer, first slice").
- What the client draws from kworld itself versus from snapshots, case by case (the rule is in "Runtime state"; the snapshot protocol is not designed yet).
