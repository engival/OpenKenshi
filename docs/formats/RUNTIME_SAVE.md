# Runtime save format (ours, v86)

The rewrite's own save: the simulation's state, nothing else.
Not Kenshi's `.platoon`/`quick.save` format (that is `SAVE_FORMAT.md`).
Code: `cpp/include/ksim/save/save.hpp`, `cpp/src/ksim/save/save.cpp`; tests
`cpp/tests/ksim_save.cpp`, `cpp/tests/ksim_continue.cpp` (save at tick N,
load, run M == uninterrupted N + M, at other thread counts) and
`cpp/tests/ksim_regions_determinism.cpp` (same, stand-in workload) and
`cpp/tests/ksim_weather.cpp` (the weather regions, v33).

## What is saved

- `state::Clock` (absolute tick since game day 0 00:00) and `state::Ids`
  (next instance Id).
- Every **instance entity**: any entity with a `state::Id`. Per entity, the
  components in `state::SavedComponents` it has.
- Every pair in `state::SavedRelations` whose source and target are both
  instance entities (`MemberOf` character -> squad; `Relation` faction ->
  faction with its value).
- The singletons in `state::SavedSingletons` that exist: `Scheduler` (world
  seed; per detail-level subsystem its RNG state, run count, checksum, last
  run tick), `StandInGroups` (the region engine's stand-in group states),
  `PlayerAreas` (player positions behind the full-detail areas),
  `WeatherRegions` (one weather region per BIOME_GROUP with seasons, v33),
  `GatesGone` (the town gates that no longer exist, v34), `BuildingLooks`
  (where the building test last looked at each fighter, v60), `WorldEventState`
  (the unique characters killed, and those the player touched, v72).
- The pending command queue (`ksim::Command`, command.hpp), as a trailer.

Faction layer state (`state::FactionAI` per faction once the faction layer has
stepped it, `state::FactionEvents`): see the v5 history entry below; the script's fields
and the extra event kinds, the v8 entry.

Squad AI state (`state::GroupAI`, one per squad once `Simulation::enableAi`
has stepped it; `cpp/include/ksim/ai/ai.hpp`): goal kind (u8: none, idle, go
home, travel), goal target (`EntityRef` to a `TownState`), tick the goal was
chosen, idle end tick, fallback reason (u8, `state::AiReason`), the coarse
route (vector of `GroundPoint`, walkable ground), route length, distance
travelled along it, last step tick, last tick with progress, last tick at
home, and the cooldown list (vector of {goal kind u8, target ref, until tick},
sorted). `TownState` also holds the town's position (x, z). The navigator and
behaviour registry that drive the AI are configuration, not state: load a save
with the same ones (`enableAi`).

Region-engine hot state: every instance with a `Position` is a region-engine
entity; its hot state is `Position` plus `Mover` (move order, stand-in
group/target/hits/steps, its own RNG state). The engine's arrays are
canonical; the save reads their flecs **mirror**, which `Simulation` writes
at every tick boundary (and when an entity is added), so between ticks the
mirror is complete. Saves are only taken between ticks. Loading rebuilds the
engine from the mirror (`RegionEngine::rebuild`, entities in Id order).

The type lists in `cpp/include/ksim/core/state.hpp` are the single source: each
type has `kName` and a `fields()` visitor; writer, reader and state hash all
go through them. Adding a saved type = one list entry + a version bump.

## What is not saved

- Templates (`ksim::tmpl`, flecs prefabs): rebuilt from game data on load.
  The header records the load order so a mismatch is caught.
- Derived state: the `Region` and `Detail` mirror components (from position,
  `PlayerAreas` and the region config), region membership, engine stats.
- The region config (worker count, region size, stand-in switch): runtime
  options, not state. The thread count never affects state.
- flecs entity ids. References are save-local indices instead.
- Orders in progress (`WorldRuntime`: arrest, smash, construction, lock, crop, standing run; limb play, limb cooldown, alarm board and wait are saved from v110). A save taken
  mid-order loads with the member free of the order, never stuck on it (kenshi-k16.6.73, test `ksim_save_mid_order`, reload and fresh load):

  | order | after a load |
  |---|---|
  | arrest (118, guard's) | resumes from v109 (`ArrestStates`, the 240 wait for a standing victim included); a v108 file: drops, the planner takes the member; under HUNT_BOUNTIES (213) the goal is saved and the arrest is re-issued or the goal ends |
  | smash 272, construction 2 / 71 / 95 / 96, lock 185 / 186 / 286 | drops: the order is the player's, the member is free (the building, the lock state and the materials are saved) |
  | EAT_CROPS 231 | drops: the Routine goal is saved, its order is not, so `stepAnimalGoals` ends a 231 goal with no crop order (it stood there for good before) |
  | standing goals 24 / 19 / 30 / 62 | drops: `stepStandingGoals` ends the goal when its place (`standingRun_`) is gone |
  | limb play 278, limb fetch cooldown 277 | resumes from v110 (`RunStates`: the destination and pick count, the tick the finder opens again); a v109 file: the destination is re-picked, the finder is open |
  | INVESTIGATE_ALARMS 209 / 210 | resumes from v110 (`RunStates`: the alarm board with each alarm's life left and the id counter, the wait's seconds left); a v109 file: the board is empty, the goal ends |
  | token squad member (`MemberDelta`) | carries no `walkHeading` / `walkFacing`: a stored member does not walk (the squad token moves as a whole), and the walk's velocity memory is a per-tick smoothing of a body on the mesh, so a promotion starts it unset exactly as a new spawn does |

  A goal that rides a Routine with no order behind it is the failure to test for (`Simulation::holdsOrder`).

## Layout

All integers little-endian, fixed width. `str` = u32 byte length + bytes
(UTF-8/raw, no terminator). `f64` = IEEE-754 bits as u64.

Header:

| field | type |
|---|---|
| magic `KSAV` | 4 bytes |
| version | u32 (the title's) |
| metadata block size | u32 (bytes of the block that follows; at most 65536) |
| metadata block (v36, below) | `size` bytes |
| load-order file count | u32 |
| file names (`gamedata.base`, `rebirth.mod`, ...) | str each |

Metadata block (v36; for listings: SIMLINK.md SAVES reads only this, never a world). It is not in the state hash. A reader that
wants more fields later appends them after `fileSize`; the size field lets old readers skip them.

| field | type |
|---|---|
| day | u32 (game day of the clock, the UI's "Day N") |
| hour | f32 (hour of day, as the CLOCK frame's `hourOfDay`), stored as its u32 bits |
| player faction name | str ("" = no player faction) |
| leader name | str (`state::Fighter::name` of the first member of the first squad of the player faction that has one; "" = none) |
| members | u16 (living members of the player faction's squads) |
| money | i64 (`state::PlayerFaction::money` of the player faction; 0 for a scenario's faction) |
| fileSize | u64 (the whole file's byte count, patched in last) |

"Player faction" is the lowest-Id `state::PlayerFaction`, as the PLAYER_SQUADS record picks it. `fileSize` is the integrity check a
listing can afford: a truncated or extended file no longer matches, so it lists as unreadable. It cannot see a damaged body behind an
intact head; loading does (`Simulation::reload` reads the whole save on a scratch world first, so a bad body changes nothing).
`ksim::readSaveInfo` (head only), `ksim::describeState` (from a live world).

**File name:** the save server (SIMLINK orders 80-83) stores a save under exactly the player's validated name, no extension (there is
no convention to follow; `ksim_run --save PATH` takes any path). Writes are atomic: a dot-prefixed temp file beside it, fsync, rename.

Body (also the exact bytes the state hash covers):

| field | type |
|---|---|
| clock tick | u64 |
| next Id | u64 |
| component kind count, names (`kName`, list order) | u32, str each |
| relationship kind count, names | u32, str each |
| singleton kind count, names | u32, str each |
| entity count N | u32 |
| per entity, ascending Id: Id, presence mask, present components' fields in list order | u64, u64 (u32 before v48), ... |
| pair count | u32 |
| per pair, sorted by (source, kind, target): source index, kind, target index, payload fields | u32, u8, u32, ... |
| singleton presence mask (bit = list position) | u32 |
| present singletons' fields, list order | ... |

Trailer (not hashed):

| field | type |
|---|---|
| pending command count | u32 |
| per command, enqueue order: tick, type, subject, object, value, x, z, points | u64, u8, u64, u64, i32, f64, f64, vector |

Entity index = position in the Id-sorted entity list. `EntityRef` fields are
saved as an index, `0xFFFFFFFF` = none. Field encodings: `std::string` str,
`double` f64, integers, `bool` and enums at their own width (`SpawnSource`
u8, `bool` 1 byte, `int32_t` 4 bytes); a nested record is its own fields in
order; `std::vector` is a u32 count then the elements. Tag relationships
(`MemberOf`) have no payload. Anything after the trailer is an error.

## Versioning rule

- Any change to the body or trailer (a list entry added/removed/reordered, a
  field added/changed, an encoding change) bumps `kSaveVersion`.
- The reader accepts only its own version (no migrations yet; v0 saves are
  refused). So a listing reports an older save as status 2 (unreadable), never 1 (migrates on load): status 1 is reserved until a
  migration exists. It also checks the component, relationship and singleton name
  lists match the build's, so a forgotten bump fails loudly rather than
  misreading.
- Load order: a save whose file list differs from the current load order is
  refused (`SaveError`) unless the caller forces it (`ksim_run --force`),
  in which case it loads and reports the mismatch. Template sids referenced
  by instances are not validated against the new data yet.

History: v0 had no `Mover`, no singletons, no trailer. v1 -> v2 (2026-10-31):
`GroupAI` added to the component list; `TownState` gained `x`, `z` (f64 each,
after `owner`). v2 -> v3 (2026-10-31): `SquadInstance` gained `hasDen`, `denX`,
`denZ` (after `source`; wilderness placement); `GroupAI` gained `goalX`, `goalZ`
(f64) and `leg` (u32) after `cooldowns`; `GoalKind` has Patrol = 4, Roam = 5. v3 -> v4 (2026-10-31): `SquadInstance`
gained `slot` (i32, -1 = none) and `nest` (u32) after `denZ`; singleton
`WildernessState` added (population f64, nests f64, rng u64, nextNest u32, per
slot two ascending u64 lists: homelessDue, nestDue); `CommandType::RemoveSquad` = 3
in the pending-command trailer.
v4 -> v5 (2026-10-31, faction layer): `SpawnSource::Campaign` = 7 (u8);
`GoalKind::Raid` = 6; `AiReason::Expired` = 10; `GroupAI` gained `order`
(a `RaidOrder`: status u8, failure u8, target ref, originX f64, originZ f64,
stay u64, deadline u64, issued u64, campaign str) after `leg`; component
`FactionAI` added last in the component list (intent u8, since u64, lastStep
u64, aggression/alarm/grief/confidence f64, `raids` [campaign str, target ref,
origin ref, planned u64, arrived u64, started bool, squads [u64 state::Id]],
`campaignCooldowns` and `targetCooldowns` [campaign str, town ref, until u64],
`triggers` [campaign str, due u64], then eight u32 counters: planned, started,
arrived, completed, failed, rejected, squadsLost, events); singleton
`FactionEvents` added last in the singleton list (pending [kind u8, faction
ref, town ref, tick u64]); `CommandType::SetTownOwner` = 4 in the trailer. Raid
squads are named by `state::Id`, not `EntityRef`, so a removed squad cannot
leave a dangling reference. The faction book (campaign data) is configuration,
not state: load with the same one (`AiSystem::Config::factions`).
v5 -> v6 (2026-10-31, town gates): `AiReason::DoorRefused` = 11 (u8, in `GroupAI`
fallback and cooldown-free failure counters only); component `TownDoors` added
last in the component list (posture u8: 0 Open, 1 Alert; since u64; holdUntil
u64; alerts u32), present on a `TownState` entity only once its gates left Open;
singleton `DoorOverrides` added last in the singleton list (entries [door key u64,
state u8: 0 Open, 1 Closed, 2 Locked], ascending by key; absent while empty);
`CommandType::SetDoorState` = 5 in the trailer (object = door key, value =
state or -1 to clear). The door book, the door-to-town binding and the data
(`is public`, gate buildings, `always locks`) are derived and configuration
(`AiSystem::Config::doors`), not state: the book is rebuilt from the saved
postures and overrides at the first squad step after a load.

v6 -> v7 (2026-10-31, faction layer follows the exe): `Relation.value` is an
f64 (was i32; reasons move it by fractions, clamped to [-100, 100], no decay;
`SetFactionRelation` still takes an integer); `OrderStatus::Retreating` = 7
(u8); `RaidOrder` gained `retreat` (u64 ticks) after `stay`; `Raid` gained
`squadsAtStart` and `squadsLost` (u32 each) after `started`; `FactionAI` gained
`nextRoll` (u64: the Clock tick of the faction's next 6-game-hour campaign roll,
0 = unset) after `lastStep`. `Command::RemoveSquad` uses `object` as the optional
killer faction (`state::Id`, 0 = none); no layout change in the trailer. Relations
were already saved state (the `Relation` pair list), they now also change at
runtime (killer-attributed `RemoveSquad`), so a save taken after a kill differs
from a fresh game's in the pair values. The areas map used for `biomes` /
`no-go zones` (`AreaLookup`) is configuration, not state.

v7 -> v8 (2026-10-31, Lua behaviour layer, first slice; `docs/reference/LUA_API.md`):
`FactionAI` gained `script` last (after `events`): the faction-intent script's declared
fields, a `std::vector` of {name str, type u8 (0 number, 1 integer, 2 boolean), integer
i64, number f64}, sorted by name; empty when the script declares none (the default script
declares none, so a scripted game's `FactionAI` differs from a native one's by nothing but
the empty list's count). `EventKind` gained `RaidOutcome` = 3 and `RelationCrossed` = 4;
`FactionEvent` gained `other` (ref) and `code` (i32) after `tick` (`RaidOutcome`: 0 victory,
1 loss, 2 defeated, `town` = the target; `RelationCrossed`: 1 became hostile, 2 no longer
hostile, 3 became ally, 4 no longer ally, `other` = the faction the opinion is about). Both
events are produced whether or not a script is loaded (`conclude`, `applyRelationReason`, the
`SetFactionRelation` command), so a native and a scripted run with the same inputs hold the
same pending events. **No Lua heap, bytecode or Lua value is saved**: the state is rebuilt
from the script text, and what a script remembers is its declared fields. The script text,
its limits and the choice of scripted or native are configuration (`AiSystem::Config::scripts`),
not state: load a save with the same script. A save taken with one script and loaded with
another is safe for the fields (kept by name and type, new ones at their default, dropped ones
discarded at the next call) but the game then diverges from the uninterrupted run, as with any
changed rules.

v8 -> v9 (2026-10-31, character layer and combat, first slice; `docs/reference/combat/IMPLEMENTATION.md`):
`state::Fighter` joined `SavedComponents` (last, bit 8): one per character at full detail,
fields in the order of `cpp/include/ksim/combat/fighter_state.hpp` (identity and team, kinematics
x z yaw speeds, the skill block, race multipliers and integer bonuses, heal and blood factors,
encumbrance, the wielded weapon, the armour list with part coverage, the body parts, blood,
wounds, KO timer and latch, unconscious/dead/arm/down flags, the CombatClass fields: state,
pending, technique and clip time, impact indices, recovery flag and timers, target Id (u64),
attack slots (holder Id, timer), engaged list, overflow, the fighter's RNG state). All numbers
are f64; enums are their underlying integers. The arena's array is canonical between steps;
the component is its one-way mirror written at the end of each tick. The combat data,
settings and `enableCombat` are configuration: load a save with the same ones, then call
`enableCombat` (fighters in the save rejoin the arena). Kind 8 of `kCombatStream` is the per-fighter
stream namespace (not a saved scheduler stream).

## State hash

`ksim::stateHash(ecs)` = FNV-1a 64 over the body bytes. It is independent
of flecs entity ids, iteration order and thread count, and covers every
saved value, so save -> load into a fresh world -> hash is an identity
(tested). `Simulation::stateHash()` is exactly this: the scheduler and
region-engine hot state are in the body through their mirrors. Pending
commands are not hashed (queued is not yet state); a command changes the
hash from the tick it applies.

v9 -> v10 (2026-10-31, SIMLINK player orders; `docs/formats/SIMLINK.md` section 10): `state::Fighter` gained, after
`attackXpPending`, `playerControlled` (bool), `hasMove` (bool), `moveX`, `moveZ` (f64), `orderTarget` (u64 Id, 0 none),
`hold` (bool), `orderStop` (bool, one-shot). New singleton `state::ViewFocus {x, z}` (f64), last in `SavedSingletons`
(bit 6), absent until the first `SetView` command. New `CommandType` values 6..10 (FighterMoveTo, FighterAttack,
FighterStop, FighterBlock, SetView) use the existing Command fields, so the pending-command encoding is unchanged.

v10 -> v11 (2026-10-31, navmesh movement; `docs/formats/SIMLINK.md` sections 6 and 12): `state::Fighter` gained `y` (f64)
after `z`, and replaced `hasMove`, `moveX`, `moveZ` (the straight-line walk, retired) with, in the same place (after
`playerControlled`): `route` (vector of `RoutePoint {x, y, z}` f64 x 3),
`routeNext` (u32), `routeVersion` (u32), `routeSeq` (u32), `drive` (u8: 0 stop, 1 forward, 2 backward),
`driveHeading` (f64), `driveSeq` (u32), `driveBlocked` (bool), then `orderTarget`, `hold`, `orderStop` as before.
`CommandType::FighterMoveTo` now carries the route in `points` (x, z) and the order's clientSeq in `object`;
new `CommandType::FighterDrive` = 11 (value = mode, x = heading, object = clientSeq). The navmesh is derived data,
never saved: load a save with the same `enableNavigation`; a save holding a route and loaded without one abandons it.

v11 -> v12 (2026-10-31, click approach): `state::Fighter` gained `routeApproach` (bool) right after `routeSeq`.
`CommandType::FighterMoveTo`'s `value` is now a bit set: bit 0 queue, bit 1 the route ends at an approach point.

v12 -> v13 (2026-10-31, engage rule): `state::Fighter` gained `cachedReach` (f64, -1 = not computed) right after
`previousTechnique` (CharStats+0x150, MEASURED item 18).

v13 -> v14 (2026-10-31, getEffectiveStat): `state::Fighter` gained `hunger` (f64, 3.0 full) right after `down`.

v14 -> v15 (2026-10-31, chases and reroutes): after `routeApproach`, `state::Fighter` gained `routeTries` (u32), `routeChase` (bool),
`chaseTx`, `chaseTz`, `chasePlanAge`, `chaseCooldown` (f64 x 4), `chaseDirect` (bool).

v15 -> v16 (2026-10-31, squad follow): after `orderStop`, `state::Fighter` gained `sneak` (bool), `followTarget` (u64), `followHx`,
`followHz` (f64), `followDirect` (bool), `followAge`, `followUnseen`, `followSeenX`, `followSeenZ` (f64 x 3), `velX`, `velZ` (f64 x 2).
`CommandType` gained 12 `FighterFollow` and 13 `FighterSneak`.

v16 -> v17 (2026-10-31, interactables): new saved components `Door`, `GroundItem`, `Inventory` (STUB) and `InteractTask`; `CommandType` 14 `FighterInteract`; `Command` gained `heights` (after `points`).

v17 -> v18 (2026-10-31, inventory, run, names): `state::Inventory` is real: `Entry` gained `slot` (u8, after `count`: 0 carried, 1 weapon, 2 + ARMOUR slot worn) and the component gained `version` (u32, after `items`); `Simulation::spawnFighter` gives every fighter one from its weapon and armour. `FighterWeapon` gained `sid` (string, after `name`); `FighterArmour` gained `sid` (string) and `slot` (i32), both after `name`. `state::Fighter` gained `run` (bool) and `name` (string) between `sneak` and `followTarget`. `Command` gained `text` (string, after `heights`); `CommandType` 15 `FighterDrop` and 16 `FighterRun`. Door paint on the navmesh is derived from the saved `Door` components (rebuilt on the first tick after a load); tile versions are runtime-only.

v18 -> v19 (2026-10-31, medical slice and hit recoil): `state::Fighter` gained, in field order: `recoilX`, `recoilZ` (f64 x2, after `stumbleTotal`); after `hunger`: `hungerRate` (f64), `hungerKo` (bool), `bleeding` (f64), `foodCharges` (f64), `eatTimer` (f64), `kitSid` (string), `kitCharges` (f64). `FighterSkills` gained `medic` (f64, last, after `poles`). `FighterPart` gained `pool` (f64) and `robotic` (bool), last (after `collapsed`). The component list is unchanged.

v19 -> v20 (2026-10-31, order queue): new component `state::OrderQueue` (`orders`: a vector of `QueuedOrder` {kind u8 (1 move, 2 interact), seq u32, target u64, action u8, x, y, z f64}), on a character's entity while it is non-empty; added to the component list after `InteractTask`. `Command` semantics only: `FighterMoveTo` value bit 0 and `FighterInteract` z bit 1 mean queued (no field changed).

v20 -> v21 (2026-10-31, doors as the original does them): `state::Door` gained, after `version`: `alwaysLocks` (bool), `moveSpeed` (f64), `ownerSid` (string); its default `health` / `maxHealth` are 16 (was 100); `state` 3 = broken. New components `state::Allegiance` (`factionSid` string) and `state::DoorStep` (`door` u64, `phase` u8, `remaining` f64, `thenOpen` bool, `waited` f64, `route` vector of RoutePoint, `routeNext` u32, `routeSeq` u32, `routeApproach` bool), appended to the component list after `OrderQueue`. `state::GroupAI` gained, last: `bashGate` (u64), `bashAt` (f64), `bashSeconds` (f64), `bashUntil` (u64). `state::DoorOverride::state` 3 = broken (SetDoorState accepts 3).

v21 -> v22 (2026-10-31, lockpicking, equip, containers): `state::FighterSkills` gained, last: `lockpicking` (f64, STATS `lockpicking`). `state::Door` gained, after `ownerSid`: `gate` (bool), `lockLevel` (i32). `state::DoorStep` gained `attempts` (u32) right after `phase` (phase 5 = pick the lock). New component `state::Container` (`recordSid` string, `x y z useX useY useZ` f64 x6, `ownerSid` string, `version` u32), appended to the component list after `DoorStep`; a container's contents are the `state::Inventory` on the same entity. `CommandType` 17 `FighterEquip` and 18 `ContainerTransfer` (Command semantics only).


v22 -> v23 (2026-10-31, perception): `state::FighterSkills` gained, last: `stealth` (f64, STATS `stealth`, or the CHARACTER's `stealth stats`). New struct `state::SightRecord` {`target` u64, `seen` bool, `sinceSeen` f64, `sighted` bool, `sinceRoll` f64, `rolls` u32, `meter` f64, `fill` f64, `active` bool, `state` u32}. `state::Fighter` gained, last, after `velZ`: `animal` (bool), `building` (u64), `crouched` (bool), `asleep` (bool), `attention` (f64, default 0.5), `senseClock` (f64, negative = the sensor is not staggered yet), `sight` (vector of SightRecord, sorted by target id). The time of day and the weather are not saved (the day fraction is the clock's; weather, line of sight and lamps are configuration).

v23 -> v24 (2026-10-31, door model of item 19): `state::Door` gained, last: `lockElapsed` (f64), `lockInterval` (f64, default 5), `lockAttempts` (u32), `reach` (f64: the xz radius of the slab polygons about `x`, `z`). `state::DoorStep` gained, last: `toggled` (bool). Door `maxHealth` / `health` and `lockLevel` now carry the record-derived values (data, not layout), and `moveSpeed` defaults to 0.5.

v24 -> v25 (2026-10-31, item instances): new struct `state::ItemQuality` {`maker` string (WEAPON_MANUFACTURER stringID, "" for armour), `model` string (MATERIAL_SPECS_WEAPON stringID), `level` f64 (-1 = no quality), `grade` u8 (0..5, 255 = none)}. It is appended, last, to: `state::Inventory::Entry` (after `slot`), `state::FighterWeapon` (after `defenceMod`), `state::FighterArmour` (after `coverage`); and inserted into `state::GroundItem` right after `count`. The wielded / worn quality lives twice (the Inventory entry and the Fighter piece); equipping rebuilds the Fighter's numbers from the Inventory entry.

v25 -> v26 (2026-11-01, carrying): `state::Fighter` gained, last (after `sight`): `carrying` (u64, the state::Id carried, 0 none), `carriedBy` (u64). `state::Door` gained, last: `building` (u64, the kworld::doorKey of the building's placement, 0 none), `inX` (f64), `inZ` (f64) (the slab's unit xz normal into the building; 0, 0 = no inside). New components appended to the list after `Container`: `state::CarryStep` (`target` u64, `phase` u8: 0 lifting, 1 carrying, 2 putting down, `elapsed` f64, `attached` bool) and `state::TakeOut` (`target` u64, `building` u64, `phase` u8: 0 fetch and lift, 1 walking out, `dropX dropY dropZ` f64 x3, `outside` f64 (-1 = inside), `elapsed` f64). `Door::ownerSid` now holds the building's faction (the placement's `owner faction ID`, else the nearest owned town's): data, not layout.

v26 -> v27 (2026-11-01, hit-reaction root motion): `state::Fighter` field `stumbleTotal` (f64, unused) is replaced in place by `reactClip` (i32, CombatData clip index of the playing hit reaction, -1 none) followed by `reactTime` (f64, seconds into that clip); the field order otherwise stays.

v27 -> v28 (2026-11-01, stagger escape roll, item 27): `state::Fighter` gained `escapeTimer` (f64, CombatClass+0x124, 1.2 s from the entry into state 8) and `escapeAsked` (bool, +0x120, set by `decideEngagement` while the fighter is in state 8), both inserted right after `stumbleTimer`. `stumbleTimer` now means the seconds left of the playing reaction clip (the heavy `mid blow` runs its whole clip unless the escape roll succeeds; it was the 1.2 s cut).

v28 -> v29 (2026-11-01, gate crews): the saved component `state::TownDoors` (and `state::Posture`) is removed from the component list (it sat after `FactionAI`): nothing closes a gate any more, so a town has no posture to save. New components appended after `TakeOut`: `state::GateCrew` on a gate-guard squad (`gate` u64 the gate's first door key, `postX postZ` f64 the outside point, `faceX faceZ` f64 the unit direction outside - inside) and `state::GateDuty` on a full-detail gate leader (`gate` u64 door key, `postX postY postZ` f64, `phase` u8: 0 post, 1 working the gate, 2 walking back). `state::GoalKind` gained `ManGate` = 7 (a crew holding its post: route empty, `goalX goalZ` the standing point). The crews' reopen timers are not saved. `DoorOverrides` unchanged.

v29 -> v30 (2026-11-01, crime foundation): new saved component `state::CrimeRecord` (appended after `GateDuty`), on a character's entity only while it holds a pending crime, a bounty or a sentence: `crime` u8 (CrimeEnum, 0 none), `victimFaction` string (FACTION sid), `victimChar` u64 (state::Id of the victim character, 0 none), `timer` f64 (seconds), `committed` bool (the exe's +0xA4), `sentenceStart` f64 and `sentenceHours` f64 (game hours; hours 0 = no sentence), `bounties` vector of `state::Bounty` sorted by faction sid: `factionSid` string, `amount` i32, `crimes` u32 (bit n = CrimeEnum n), `claim` bool, `stamp` f64 (game hours of the last commit). `state::Fighter` gained, appended after `carriedBy`: `npcClass` u8 (the CHARACTER's `NPC class`, 0 none), `homeBuilding` u64 (the resident squad's home, a BuildingSite key, 0 none) and `houseLocked` bool (the squad package byte +0xe8 of the trespass sensor). Stamps and the sentence start are saved at full precision: the original floors them to whole hours on save (CRIME_LIVE.md: 3369.357 -> 3369.0), which would make a loaded run differ from an uninterrupted one, so that rounding is deliberately not reproduced. The dialogue-event counters and the combat arena's attack notes are transient and not saved.

v30 -> v31 (2026-11-01, grid inventory): new struct `state::InventoryEntry` (`state::Inventory::Entry` is now an alias for it): `itemSid` string, `count` u32, `slot` u8, `quality` ItemQuality, then appended `section` u8 (0 main / the container's own grid, inside `contents` the backpack's grid; 254 = unplaced, 255 = equipped), `x` u8, `y` u8, `material` string (MATERIAL_SPECS_CLOTHING sid of an armour piece, container or item, "" = none; a weapon's material is `quality.model`), `colour` string (COLOR_DATA sid; always "" today, OPEN), `contents` vector of InventoryEntry (the backpack_content of a CONTAINER item instance; it moves with the item). `state::Inventory` gained, after `version`: `gridW` u8, `gridH` u8 (section 0's size: 8 x 10 for a character, a container's is its BUILDING `storage size`). `state::GroundItem` gained, after `quality`: `material` string, `colour` string, `contents` vector of InventoryEntry (a dropped backpack keeps its contents). Stacks are one entry each and capped (ITEM `stackable`, x the bag's bonus; weapons, armour, containers never stack), so a pre-v31 entry with a count above its cap is split on load. **Older saves**: the loader still accepts exactly the current version (as for every bump), but the state it reads is made whole by `grid::settle` (`Simulation::settledInventory`, run by every inventory order, the pick-up / loot / transfer paths and the INVENTORY writer): an entry with no position (section 254) is autoplaced row by row from the top left, an invalid or overlapping one likewise, an over-cap stack is split, a missing material is picked (seeded by the owner, the record and a running count; recorded on the entry). Test: `ksim_inventory_grid` (a save with all positions cleared is placed; save + load hash equal).

v31 -> v32 (2026-11-01, portraitSerial): `state::Fighter` gained, appended after `houseLocked`: `portraitSerial` u32 (SIMLINK.md CHAR_APPEARANCE, protocol 5: the portrait camera seed). Rolled once in `Simulation::spawnFighter` as `uniform01() x 4e9` truncated to u32 (the original rolls rand x 4e9 at creation, 1406223f0), from one draw of the pure stream `streamSeed(streamSeed(seed, kPortraitStream = 11), state::Id)`, so the same seed and spawn order give the same serials and no other stream moves. Part of the appearance: it never changes after spawn. A pre-v32 save is not read (as for every bump). Test: `ksim_carry` (save + load keeps it, same seed same serials, range), `ksim_simlink` (wire equals state).

v32 -> v33 (2026-11-01, weather cycle): new saved singleton `state::WeatherRegions` (appended last in `SavedSingletons`, after `ViewFocus`): a `vector<WeatherRegion>`, one per BIOME_GROUP that lists seasons, in BIOME_GROUP record order. `state::WeatherRegion` fields, in order: `group` string (BIOME_GROUP stringID), `season` u32 (index into the group's seasons sorted by their v0), `seasonEndDay` u64, `weather` string (WEATHER stringID of the current instance, "" = none), `previous` string, `startMinute` u64, `endMinute` u64 (game minutes since day 0 00:00), `strength` f64, `effectStrength` f64, `rng` u64 (the group's xorshift state; initial value `streamSeed(streamSeed(seed, kWeatherStream = 12), BIOME_GROUP index colour)`), `started` u8 (0 until the first step picks). Present only in a world with game data (`Simulation::newGame`, `enableWeather`); a `Simulation::load` world has none and its hash is unchanged. On load, `Simulation::restore` (with the game data) reconciles the list with the data: a region whose season index or weather id no longer exists falls back to season 0 and a fresh pick (stream kept), a missing group gets a fresh region, an unknown group is dropped. Design and rules: `reference/WEATHER_CYCLE_LIVE.md` ("Ours", "Implemented"). A pre-v33 save is not read (as for every bump). Test: `ksim_weather` (save, load and continue gives the straight run's regions, hash and bytes).

v33 -> v34 (2026-11-01, gate binding): `state::GateCrew` gained `home` (u64, appended after `faceZ`): the door key of the home building's gate (the exe's Ownerships+0x38), the spawn gate; 0 = none. `gate` stays the binding in force (the task's +0x10). New saved singleton `state::GatesGone` (appended last in `SavedSingletons`, after `WeatherRegions`): `keys` vector of u64 door keys, ascending, the gates removed by `Simulation::removeGate`; absent while there are none. A crew whose `gate` is gone or held by a lower-Id crew rebinds (own, `home`, the town's nearest free gateway) or loses its `GateCrew` (stands down). Reference: `reference/GATE_CREWS_LIVE.md` 5, `reference/DOORS.md` section 8. A pre-v34 save is not read (as for every bump). Test: `ksim_gates` (save, load and continue gives the same hash and bytes).

v34 -> v35 (2026-11-01, player squads): two new saved components, appended last in `SavedComponents`: `state::PlayerFaction` (on the FactionState entity of a faction a player owns: `name` str, `squadCounter` u32, the running N of the next default squad name `<name>_<N>`, saved and never reused, starts at 1 (UNSURE), `squads` vector of u64, the PlayerSquad Ids in list order = HUD tab order) and `state::PlayerSquad` (one instance entity per squad: `name` str, up to 32 characters, "" allowed; `members` vector of u64 fighter Ids, leader first). `SpawnSource` gained `Dismissed = 8` (the squad of a dismissed character; a SquadInstance with a Position that the squad AI then drives). New command type `SquadOrder = 20` in the pending-command trailer (orders 70-75). Reference: `reference/FACTION_SQUADS_LIVE.md`, SIMLINK.md PLAYER_SQUADS (31). A pre-v35 save is not read. Test: `ksim_squads` (save, load and continue gives the same hash).

v35 -> v36 (2026-11-01, save metadata): the header gained the metadata block (above) between the version and the load-order list; the body and
trailer are unchanged, so the state hash of a world is the same as under v35. SIMLINK SAVES (32) lists from it. A pre-v36 save is not read
(status 2 in the listing). Code: `ksim::describeState`, `readSaveInfo`, `writeSave`; the save server `cpp/src/ksim/net/simlink_saves.cpp`;
`Simulation::reload` (load in place). Test: `ksim_saves`.

v36 -> v37 (2026-11-01, new game): `state::PlayerFaction` gained, appended after `squads`: `money` i64 (the start-off's `money`; the original keeps it on the player faction's Ownerships) and `startoff` string (the NEW_GAME_STARTOFF stringID the faction began with, "" for a scenario's faction). The metadata block's `money` field is now that value. New command type `NewGame = 21` in the pending-command trailer (`text` = startoff sid, `\n`, the faction name; `object` = the asking session's ticket). Nothing else changed: the state of a world without a new game hashes as under v36 except that every PlayerFaction now hashes two more fields. A pre-v37 save is not read. Code: `cpp/src/ksim/save/newgame.cpp`; SIMLINK.md NEW_GAME / order 84 (the fresh-world mark is not saved: `Simulation::markPristine` is a server-side snapshot of the world at start-up). Test: `ksim_newgame` (a new game saved, loaded and continued gives the same hash; a NewGame command pending in a save applies after the load).

v37 -> v38 (2026-11-01, jobs V1): new saved component `state::Jobs` (appended last in `SavedComponents`, after `PlayerSquad`; on a character's entity, present once the character has had a job order or the JOBS toggle): `enabled` u8 (default 1), `nextJobId` u32 (the per-character counter, starts at 1, never reused), `rows` vector of `state::JobRow` {`jobId` u32, `type` u16 (the exe's TaskData number), `target` u64 (0 = none)} in priority order (order survives a load), then `running` u32 (the jobId whose goal the sim started, 0 none), `runType` u16 (58 first aid, 16 attack, 31 follow), `runTarget` u64. New command type `JobOrder = 22` in the pending-command trailer (`value` = order kind 76 / 77 / 78, or 10 for the JOBS toggle; `subject` the character; `object` the target of 76; `x` the type (76), the jobId (77, 78) or the enabled flag (10); `z` the index of 78). A world in which no job order was given has no `Jobs` component and hashes as under v37. A pre-v38 save is not read. Code: `cpp/src/ksim/ai/jobs.cpp`; SIMLINK.md JOBS (34), orders 76-78. Test: `ksim_jobs` (lists, order and the counter survive save and load; save, load and continue gives the straight run's hash).

v38 -> v39 (2026-11-01, unlocks): `state::PlayerFaction` gained, appended after `startoff`: `research`, a vector of strings (the completed RESEARCH records' stringIDs, in completion order; the unlocked buildings are derived from them, not saved). A new game fills it with the start-off's `research` list or `_Default Start` (5359-gamedata.base); a world that loads with a player faction whose set is empty (an old-style or scenario faction) gets `_Default Start` at the load (`Simulation::stepUnlocks`, also run each tick). The state hash covers it. A pre-v39 save is not read. Code: `cpp/src/ksim/save/newgame.cpp`; SIMLINK.md UNLOCKS (36). Test: `ksim_newgame` (save, load and continue gives the same hash; an emptied set in a save is refilled at the load).

v39 -> v40 (2026-11-01, prospecting): `state::FighterSkills` gained, last: `science` (f64, STATS `science`, CharStats+0xe0; 1 when the character has no STATS record). New saved component `state::ProspectTask` {`ticks` u32}, appended last in `SavedComponents` (after `Jobs`), on a character's entity only while its prospect order runs (20 ticks = 1.0 sim-second; a replacing order, a KO or death removes it). The state hash covers both. The derived data (zones, deposits, the results) is not saved; a finished result is sent and forgotten. A pre-v40 save is not read. Code: `cpp/src/ksim/world/prospect.cpp`; SIMLINK.md order 79 and PROSPECT (35). Test: `ksim_prospect` (a save in the middle of a task, load and continue ends it in the same tick with the same hash).

v40 -> v41 (2026-11-01, map discovery): `state::PlayerFaction` gained, appended after `research`: `discovered` and `visited`, vectors of strings (TOWN stringIDs, in discovery order; visited implies discovered; both only grow). A new game starts with both empty and its first check (at the start position) fills them; the check otherwise runs once per sim-second on the clock tick (`Simulation::stepDiscovery`). The derived data (the TOWN positions) is not saved. The state hash covers both. A pre-v41 save is not read. Code: `cpp/src/ksim/world/discovery.cpp`; SIMLINK.md DISCOVERED (37) and event 19. Test: `ksim_discovery` (save, load and continue gives the same hash and list).

v41 -> v42 (2026-11-01, character creation): `state::Fighter` gained, last (after `portraitSerial`): `look`, a `FighterLook` {`custom` bool, `female` u8, `headIndex` u16, `hair`, `beard`, `hairColour` strings (sids, "" = none), `skin` f64, `sliders` vector of {`name` string, `value` f64}, `stance` i32}. `custom` is true only for characters made by NEW_GAME with Specs; CHAR_APPEARANCE sends their look from it. The state hash covers it. A pre-v42 save is not read. Code: `cpp/src/ksim/save/creator.cpp`, `newgame.cpp`; SIMLINK.md NEW_GAME. Test: `ksim_newgame` (save, load and continue after a character-creation start: same hash and look).

v42 -> v43 (2026-11-01, faction relations): `state::PlayerFaction` gained, appended after `visited`: `known`, a vector of strings (the FACTION records' stringIDs the player faction knows, in the order they became known; only grows; the exe's PlayerFactionRelations "known" set, kinspect 49). A new game and `createPlayerFaction` seed it with every faction the player faction has a Relation pair toward; a relation change involving the player faction (either direction, `noteRelationChange`) and, once per sim-second, a character of another faction within `perception::kBaseRange` of a player character add to it. The state hash covers it. FACTION_RELATIONS 38 lists from it. A pre-v43 save is not read. Code: `cpp/src/ksim/world/player_squads.cpp` (`factionRelations`, `seedKnownFactions`, `stepKnownFactions`), `factions.cpp`; SIMLINK.md FACTION_RELATIONS (38), order 85. Test: `ksim_factionrel` (save, load and continue gives the same hash and list). The faction name (order 85) was already saved (`PlayerFaction::name`): no bump for it.

v43 -> v44 (2026-11-01, player buildings Phases 1-2): three new things. Two saved components appended last in `SavedComponents`: `state::PlayerBuilding` (an instance entity per placed building: `recordSid` str, `ownerFaction` u64 the player faction's Id, `town` u64 (0 none), `x y z` and `qw qx qy qz` f64, `floor` u8, the bools `built paused broken dismantling switchedOn runOffBattery repairWanted`, `hp` and `maxHp` f64, `mats` a vector of `state::BuildMaterial {itemSid str, need u32, have u32}` in the record's `construction` order (one list; the design's separate need / have lists were merged), `workers` u16 (the scripted builder count until Phase 3), `status` u8, `version` u32, `placedTick` u64) and `state::PlayerTown` (`name` str, `owner` u64, `x z radius` f64, `members` vector of u64 building Ids). Two new command types in the pending-command trailer: `BuildPlace = 24` (`text` the record, `x z`, `heights` = {y, qw, qx, qy, qz}, `value` the floor) and `BuildPause = 25` (`subject` the building, `value` 0 / 1). Not saved: the footprint boxes, the navmesh fraction, `Ka` (the build-speed knob is configuration like the combat settings; a loaded save needs the same value). The state hash covers both components. A pre-v44 save is not read. Code: `cpp/include/ksim/world/buildings.hpp`, `cpp/src/ksim/world/buildings.cpp`; SIMLINK.md BUILDING (47), orders 90 / 91 / 94; reference/BUILDINGS_SLICE.md. Test: `ksim_buildings_place`, `ksim_buildings_build` (save, load and continue gives the same hash, also mid-construction).

v44 -> v45 (2026-11-01, loot open): `state::InteractTask` gained `flags` (u8, last, after `seq`) and `state::QueuedOrder` gained `flags` (u8, last, after `z`): order 60's trailing flags byte (loot, bit 0 = open: the walk ends without taking anything). In the pending-command trailer a FighterInteract command carries them in `z >> 2` (bit 0 of z = approach, bit 1 = queued, as before). A pre-v45 save is not read. Code: `cpp/include/ksim/world/interact.hpp` (`kInteractFlagOpen`), `Simulation::stepInteractions`; test `ksim_interact` (loot open section).

v45 -> v46 (2026-11-01, merchant refresh, TOWN_LIFE_SLICE T1): `state::InventoryEntry` gained `unique` (bool, last, after `contents`; the exe's `Item+0x12a`; part of the merge key `sameKind`, so a unique stack never merges into a plain one). New saved component `state::TraderStock` appended last in `SavedComponents` (after `PlayerTown`), on a merchant squad's entity (SQUAD_TEMPLATE `is trader` with `vendors`) next to a `state::Inventory` (the shop's store, 20 x 20, UNSURE): `nextRefresh` f64 (absolute game hours, the exe's `inventory refresh time`), `refreshes` u32 (keys the dice), `money` i32 (the squad wallet), `seeded` bool (the spawn refresh ran), `destroyed` u64 (units destroyed because they did not fit, D2). The dice are a pure function of (world seed, squad Id, `refreshes`), nothing else is saved. A pre-v46 save is not read (`old_version`, SIMLINK 81). Code: `cpp/include/ksim/economy/trader.hpp`, `cpp/src/ksim/economy/trader.cpp`; test `ksim_trader_refresh` (save mid-timer, load and continue gives the straight run's hash).

v46 -> v47 (2026-11-01, wandering traders, TOWN_LIFE_SLICE T2): `state::FactionState` gained `prosperityLost` (f64, last, after `templateSid`; the loss below the FACTION's `max prosperity`, 0 = at the cap: a dead squad adds 10, every faction update gives back 0.1). New saved component `state::TraderTrip` appended last in `SavedComponents` (after `TraderStock`), on the squad entity of every squad that runs the `trade_circuit` behaviour (created at its first AI step): `town` EntityRef (the town it is in while staying, else the one it is heading for), `lastTown` EntityRef, `staying` bool, `arrivedAt` f64 (game hours, stamped only on the no-town -> town transition), `stayHours` f64 (the U(5, 8) rolled once per arrival), `arrivals` u32 (keys the dice). Pack animals are ordinary member entities that now carry a `state::Inventory` (their own `inventory w` x `h` grid with the worn backpack in the BACKPACK slot, its contents in the entry's `contents`); no new field. The dice are pure functions of (world seed, squad or member Id, `arrivals` / the refresh count). A pre-v47 save is not read (`old_version`, SIMLINK 81). Code: `cpp/include/ksim/economy/trader.hpp`, `cpp/src/ksim/economy/trader.cpp`, `cpp/src/ksim/ai/ai.cpp` (`circuitSelect`); test `ksim_trader_circuit` (save in the middle of a stay, load and continue gives the straight run's hash).

v47 -> v48 (2026-11-01, residents as Fighters, TOWN_LIFE_SLICE T3b1): the **component presence mask grew from u32 to u64** (the list reached 33 components; `save.cpp` asserts at most 64). Three saved components appended last in `SavedComponents` (after `TraderTrip`), in this order. `state::MemberRole` on every character a squad template spawned: `list` u8 (0 Squad, 1 Squad2, 2 Leader, 3 Slave; written by `SquadBuilder::addMember`; a character without it reads as 0). `state::Routine` on a promoted resident (a member at Full detail whose squad is in routine mode; removed on demotion and at death): `goal` u16 (the running AI_TASK enum, 0 = none), `since` u64, `holdUntil` u64, `target` `state::RoutineSite {kind u8, object u64, slot u16}` (kind 0 = none; kinds are `routines::SiteKind`), `lastSleep` f64 (game hours of the last sleep event; the one tiredness field, kinspect item 57, NOT `awakeSince`), `blocked` a vector of `{goal u16, target RoutineSite, until u64}` sorted, `nextScore` u64. `state::SquadRoutine` on the squad entity while its members are promoted: `homeLocked` u8, `barTable` u64 (0 = none). Promoted residents also carry `state::Fighter`, `Inventory` and `Allegiance` (existing components). Not saved, rebuilt at load by `Simulation::rebuildRoutineBook` (called from `enableCombat`): the reservation book, from every `Routine::target` (owner = the member's Id) and every `SquadRoutine::barTable` (a Table site, owner = the squad's Id); on a conflict the lower owner Id keeps the site. Also not saved: the compiled goal tables, the site index, the squad `phase`. A pre-v48 save is not read (`old_version`, SIMLINK 81). Code: `cpp/include/ksim/core/state.hpp`, `cpp/src/ksim/world/residents.cpp`, `cpp/src/ksim/world/spawn.cpp`, `cpp/src/ksim/save/save.cpp`; test `ksim_town_fighters` (a save with three members mid-routine, claims and a table; load gives the saved hash, the same fields and the same book; load and continue gives the straight run's hash).

v48 -> v49 (2026-11-02, producers, MINE_SELL_SLICE M1-M3): a new saved component `state::Producer` appended last in `SavedComponents` (after `SquadRoutine`): `recordSid` str, `ownerSid` str, `output` str, `cap` u32, `maxOperators` u8, `progressBar` bool, `useRange` f64, `x y z useX useY useZ` f64, `richness` f64, `progress` f64, `prodState` u8 (1 full, 2 working, 3 idle), `operators` vector u64 (arrival order), `version` u32. Its output is the entity's ordinary `state::Inventory`, which gained `stackLimit` u32 (last; 0 = the items' own caps, else the cap of every stack in `main`: a producer's one slot). `state::FighterSkills` gained `labouring` f64 (last, after `science`). A producer's Id is 2^62 + its site (a zone placement's index, or a zone cell's foliage deposit), not from the Ids counter. **A producer that holds no stock, no progress and no operator is not written** (`producerSaved`; it is made again from the data with the same Id when a player comes near), so the save and the state hash do not depend on which deposits happened to be near. A pre-v49 save is not read (`old_version`, SIMLINK 81). Code: `cpp/include/ksim/economy/producers.hpp`, `cpp/src/ksim/economy/producers.cpp`, `cpp/src/ksim/save/save.cpp`; test `ksim_producers` (a save mid-progress with two units, load, continue: the straight run's hash).

v49 -> v50 (2026-11-02, chase give-up monitor, BAR_FIGHT_LOOP step 2b/4): `state::SightRecord` gained `sinceSensed` f64, inserted after `sinceSeen` (seconds since a scan itself sensed the target; the attacker's `notice` never resets it; starts at 1e9). `state::Fighter` gained, appended after `look`: `chaseId` u64, `chaseAge`, `chaseUnseen`, `chaseX`, `chaseZ` f64 (the chase give-up monitor of the move-in task: target, sim seconds of chase, sim seconds unsensed, the sight record position). A pre-v50 save is not read (`old_version`, SIMLINK 81). Code: `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/src/ksim/ai/perception.cpp` (`sensedWithin`, `forget`), `cpp/src/ksim/combat/combat.cpp` (`chaseGivesUp`); test `ksim_chase_giveup`.

v50 -> v51 (2026-11-02, walker kinematics): `state::Fighter` gained `driveTurn` (bool) and `walkStuck` (f64), both after `driveBlocked`. `driveTurn`: a drive 0 in the drive hold is turning the body towards `driveHeading` (SIMLINK.md section 12, turn in place). `walkStuck`: seconds a walking character has barely moved (5 s re-paths). A pre-v51 save is not read (`old_version`). Code: `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/src/ksim/combat/combat.cpp`; test `ksim_walk_kinematics`. Design: ARCHITECTURE.md "Walker kinematics".

v51 -> v52 (2026-11-02, MQ14 path 2, squad goal ATTACK_ENEMIES_AND_NEUTRALS): `state::Fighter` gained, appended after the v51 fields (end of the record): `faction` u64 (the owning FACTION's `state::Id`, 0 none), `attacksNeutrals` bool, `aggroBlockId` u64, `aggroBlock` f64 (sim seconds left on a skipped target). The bond set (`CombatArena::bonded`) is derived, not saved. A pre-v52 save is not read. Code: `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/src/ksim/combat/combat.cpp`; test `ksim_aggressive`. Design: ARCHITECTURE.md "Aggressive squads", reference/BAR_FIGHT_LOOP.md "MQ14 path 2".

v52 -> v53 (2026-11-02, BQ10 goal 21 alignment, kinspect `bq10_goal21.md`): `state::Fighter` gained, appended after `aggroBlock`, in this order: `aggroStart` i32, `aggroEnd` i32 (the goal's hour window, default 0-24; per character, from its role list), `squad` u64 (the squad's `state::Id`; `1<<62 | id` for a squadless fighter, set in `CombatArena::add`), `uniqueSquad` bool, `hostileSquads` vector of u64 (the squad's personal type-4 hostile list, sorted squad keys). `attacksNeutrals` is now per character, not per squad. A pre-v53 save is not read. Code: `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/src/ksim/combat/combat.cpp` (`enemy`, `markHostile`), `cpp/src/ksim/world/residents.cpp` (`buildMember`, `attackGoalWindow`); test `ksim_aggressive`.

v53 -> v54 (2026-11-02, BQ15 corpse decay, kinspect `bq15_corpse_cleanup.md` a, `s79_static.md` 1.2): new saved component `state::CorpseDecay { f64 stamp }`, appended at the END of `state::SavedComponents` (after `Producer`; it is component bit 33 of the u64 presence mask, so no existing bit moves). It sits on a dead character after the first corpse evaluation (`Simulation::stepCorpses`): `stamp` is the game-hours time (Clock tick / `kTicksPerGameDay / 24`) of that evaluation, 0 = not yet stamped (the exe's `char+0xd0`, saved there as `decay`). The corpse is destroyed when `now - stamp > 12`; a carried corpse's stamp is clamped to `>= now - 6`. A dead character without the component has not been evaluated yet (an NPC corpse outside every player area stays unstamped). Nothing else changed: the squad-liveness and quota follow-ups of the same change (severity < 0.2, template nominal size, the float quota) are derived each step and save nothing. A pre-v54 save is not read. Code: `cpp/include/ksim/core/state.hpp`, `cpp/src/ksim/combat/corpses.cpp`, `cpp/include/ksim/combat/corpses.hpp`; test `ksim_corpses` (the stamp survives save and load), `ksim_ai` section 8. Design: ARCHITECTURE.md "Corpses".

v54 -> v55 (2026-11-03, producer arrival, kinspect `item59_mine_sell.md` MQ9): `state::Producer` gains `f64 boxMinX, boxMaxX, boxMinZ, boxMaxZ` after `useZ`: the xz bounds of the deposit's collision as the navmesh bake places it (all 0 = none; zone placements have none yet). An operator also counts as arrived inside them x 1.2 about their centre; the use range is measured to the origin (`x, z`), not the use point. A pre-v55 save is not read. Code: `cpp/include/ksim/core/state.hpp`, `cpp/src/ksim/economy/producers.cpp`, `cpp/include/ksim/world/interact.hpp` (`InteractTarget::within`); test `ksim_producers` ("reach from a distance").

v56 -> v57 (2026-11-03, DIALOGUE.md slice 1 step 5): component `state::Conversation` (str dialogue, str line, u64 voice, u64 target, u64 started, u64 until, u8 event) on a speaker with an open conversation, and singleton `state::DialogueShown` (stamps: str sid, u64 tick; ascending by sid), the shown-at ticks of the repetition limit. A pre-v57 save is not read. Test: `ksim_dialogue_events` (save / load mid-line).

v57 -> v58 (2026-11-03, DIALOGUE.md slice 1 step 6a): component `state::DialogueMemory` (locks: str sid, bool locked; ascending by sid), a character's dialogue lock map, and `state::Conversation` gains u64 follower (who DA_FOLLOW_WHILE_TALKING set following the target). A pre-v58 save is not read. Test: `ksim_dialogue_events` (effects: save / load keeps the lock map).

v58 -> v59 (2026-11-03, town populate, WORLD_MODEL.md "Who lives where"): `state::SquadInstance` gains u64 homeBuilding (the building key the populate gave the squad, 0 none), f64 homeX, f64 homeZ (its centre, the squad AI's home point) after `nest`. A pre-v59 save is not read. Tests: `ksim_save`, `ksim_producers`, `ksim_trade`.
v55 -> v56 (2026-11-03, faction-uniform armour, kinspect `s72_static.md` section 4): `state::ItemQuality` gains `str uniform` after `grade` (the FACTION sid whose uniform the piece is, "" none). Set at spawn on the armour of a CHARACTER with `wears uniform` (its `faction`, else the squad template's); trade prices it x0.3 (`trade.cpp`). A pre-v56 save is not read. Test: `ksim_trade`.

v59 -> v60 (2026-11-04, load determinism): new saved singleton `state::BuildingLooks` (appended last in `SavedSingletons`, after `DialogueShown`): `looks` vector of {u64 id, f64 x, y, z}, ascending by id, where `Simulation::stepBuildings` last looked at each fighter (it tests a fighter's building only once it has moved since). It was memory only, so a load restarted every look and missed a test the straight run made (ksim_newgame: a resident outside its door kept its building one tick longer, and a housemate sighted it). A pre-v60 save is not read. Test: `ksim_newgame` (save, load and continue after character creation).

v60 -> v61 (2026-11-04, DIALOGUE.md step 6b): `state::Fighter` gains `goal` (`state::IndividualGoal`: u8 kind 0 none / 1 Flee / 2 GoHome / 3 Summoned, u64 target, u64 until tick, f64 x, f64 z) after `hostileSquads`: the running individual goal a dialogue effect gave. A pre-v61 save is not read. Test: `ksim_dialogue_events` (save / load mid-goal).

v61 -> v62 (2026-11-04, SIMLINK INTERACTABLE ownership tail): `state::GroundItem` gains `ownerSid` (string, after `version`): the FACTION that owns a ground item, "" = nobody's; picking up an owned one is theft (event 14). A pre-v62 save is not read. Test: `ksim_interact` (owned pick-up).

v62 -> v63 (2026-11-04, placement item nodes, WORLD_LAYOUT.md "Placement item nodes"): new saved component `state::ItemNode` (u64 key, string groupSid, f64 x y z, f64 useX useY useZ, string ownerSid, u64 item Id (0 = empty), u64 refillTick (0 = not timed), u32 picks), appended to SavedComponents. A pre-v63 save is not read. Test: `ksim_item_nodes` (save while a refill waits).

v63 -> v64 (2026-11-04, dialogue reply menus, DIALOGUE.md 13): `state::Conversation` appends `replies` (vector of DIALOGUE_LINE sids offered to the target, "" = the "[...]" entry; empty = no menu open), u64 `menuTick` (the tick the menu opened; menuSeq = its low 32 bits; the option texts are rolled at it) and string `interrupt` (the DIALOGUE sid a spoken line's `interrupt` set). A pre-v64 save is not read. Test: `ksim_dialogue_events` (save / load with a menu open, the resend equal).

v64 -> v65 (2026-11-05, player speed modes, SIMLINK order 10 `which` 5): `state::Fighter`'s `run` bool becomes `moveMode` u8 + `speedGroup` u64 in the same place (after `sneak`, before `name`). `moveMode` 0 Walk, 1 Jog, 2 Run (the default), 3 Group; `speedGroup` the group key (the lowest id of the order's ids, 0 none). A v64 save is still read (`readSave` takes the previous version): its `run` 0 loads as Walk (0), 1 as Run (2), `speedGroup` 0. Code: `cpp/include/ksim/combat/fighter_state.hpp` (`fields`), `cpp/src/ksim/save/save.cpp`; test `ksim_speed_modes` (round trip, and a v64 file written and loaded as Walk).

v65 -> v66 (2026-11-06, a load mid-swing or mid-hold takes the same decisions): `state::Fighter` gains `hitList` (u32 count then u64 Ids: the fighters the impacts already passed of the running swing have hit, CombatClass +0x1d8) and `holdSteer` (`state::HoldSteer`: five bools + u8 `came`, the recovery hold's steer bookkeeping, +0x1f4 / CharMovement +0x378), after `hitListBuilt`. Both were `CombatArena::Member` transients, so a save dropped them and the loaded run made different engagement decisions from the straight one (`ksim_combat_sim`; task AA's `decideEngagement` step 2 reads the hit list). A v65 save is still read: both load empty / default. Code: `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/src/ksim/combat/combat.cpp`; test `ksim_combat_sim` (save at N, load, the hash equal to N + M).

v66 -> v67 (2026-11-06, the technique aim, ENGAGEMENT.md 9.4): `state::Fighter` gains `aimYaw` (f64, the look hand `mover+0x250` set at the technique start, after `holdSteer`). The swing's impact box and root motion read it, so a load mid-swing must resume with the same aim (`ksim_combat_sim`, `ksim_world_fighters`, `ksim_nav_regions`: the loaded run's hash differed). A v66 save is still read: `aimYaw` 0, then `CombatArena::reaim()` derives it from the saved target / threats. Code: `cpp/include/ksim/combat/fighter_state.hpp` (`fields`), `cpp/src/ksim/combat/combat.cpp`; test `ksim_combat_sim`.
v67 -> v68 (2026-11-06, the town slave trade, TOWN_LIFE_SLICE 5.19): `state::Fighter` gains `slaveState` u8 (0 not a slave, 1 a slave, 2 escaping, 3 an ex-slave: CRIME_NOTICE.md's enum), `captorSid` (string: the faction that chained it, "" = none) and `cageBuilding` u64 + `cageSlot` u16 (the cage site that holds it: the original's location type 2; 0 = free), after `goal`. `state::Routine` gains `chainTarget` u64 and `chainElapsed` f64 (the body a member is catching and the "Restraining" timer, negative until it reaches it), after `nextScore`; `state::SquadRoutine` gains `cagesLocked` u8 (195, one lock per squad), after `barTable`. No component was added, so a v67 save still loads: its characters are free (state 0, no owner, no cage), its members have no chain and its squads' cages are unlocked. Code: `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/include/ksim/core/state.hpp`, `cpp/src/ksim/combat/carry.cpp` (`stepSlaveTrade`), `cpp/src/ksim/ai/routine_scorer.cpp`; test `ksim_town_guards` (the chain and the cage across a run; `ksim_speed_modes` still writes and loads a v66 file).
v68 -> v69 (2026-11-06, the real-economy overlay, TOWN_LIFE_SLICE 7.7 / T4a): a new singleton `state::EconomySettings` (`mode` u8: 0 `original`, the V1 default, 1 `real`) and four new components, appended after `ItemNode`: `state::TownStock` (on a town: `lines` of `{itemSid, amount, target}`, `prosperity` f64, `lastTick` f64 game hours), `state::EconomyProducer` (`town` EntityRef, `itemSid`, `ratePerHour` f64, `buffer` f64, `consumes` of `{itemSid, perHour}`), `state::CaravanOrder` (`from`/`to` EntityRefs, `itemSid`, `amount` f64, `phase` u8 0 load / 1 go / 2 unload) and `state::SupplyBoard` (on a town: `entries` of `BoardEntry`). With `mode = original` no economy state exists at all (7.1), so a T4-off world's state hash is a T3 world's. A v68 save still loads (`readSave` takes the previous version): it has no economy singleton, so it is an `original` world. Creating the singleton at new game is `Simulation::seedEconomy`, the tick is `Simulation::stepEconomy` (once a game hour); code `cpp/include/ksim/economy/economy.hpp`, `cpp/src/ksim/economy/economy.cpp`, `cpp/include/ksim/core/state.hpp`; test `ksim_economy_flow`.

v69 -> v70 (2026-11-06, the dialogue economy, task AK): `state::FactionState` gains `cash` (i64, after `prosperityLost`): the faction's money, written today only by DA_TAKE_MONEY, which credits the speaker's faction instead of burning the cats (DIALOGUE.md D5 / decision 10). A plain counter, not the economy overlay's `state::TownStock`. A v69 save still loads: every faction's cash is 0. Code: `cpp/include/ksim/core/state.hpp` (`fields`), `cpp/src/ksim/world/player_squads.cpp` (`Simulation::adjustFactionCash`); test `ksim_dialogue_econ` (the credit and the save round trip).

v70 -> v71 (2026-11-06, containers: their site and lock, task AL): `state::Container` gained, before `recordSid`: `key` (u64, the site key: World `ContainerSite::key` for a placed storage building, `mix64(building.key ^ mix64(piece + 1))` for a storage piece of a building's interior layout; 0 = a scenario box with no site), and after `ownerSid`: `locked` (bool, the record's `has lock`), `lockLevel` (i32, the door formula from the owner faction), `lockElapsed` (f64), `lockInterval` (f64, 5 at spawn) and `lockAttempts` (u32) — the lock's picking timer, on the lock as a door's. Layout storage is a container now (spawned with its zone's navmesh, keyed so a second furnishing makes none); the owner is the placement's own owner faction, else the town's. A pre-v71 save is not read (`old_version`, SIMLINK 81; readSave takes only the current and the previous version, and v70 is another slice's). Code `cpp/include/ksim/core/state.hpp`, `cpp/include/ksim/world/world.hpp`, `cpp/src/ksim/world/interact.cpp`, `cpp/src/ksim/world/world.cpp`; test `ksim_containers` (the Hub Bar's layout boxes, a pick, an `itemtype limit`, save + load).

v71 -> v72 (2026-11-06, the live WORLD_EVENT_STATE set, task AN): a new singleton `state::WorldEventState` with `dead` (CHARACTER sids of the unique characters killed, sorted by insertion: the events remembered after the body is gone) and `playerTouched` (the unique characters the player killed or imprisoned, the involvement set — tracked, not used for the states' truth, WORLD_MODEL.md a). Both vectors of `str`, gated `ar.version >= 72`, so a v70 / v71 save loads with both empty. The statuses themselves are live from the fighters (dead / caged / held slave), so the dead set only remembers a character whose corpse has decayed. Code `cpp/include/ksim/core/state.hpp`, `cpp/include/ksim/world/world_state.hpp`, `cpp/src/ksim/core/simulation.cpp`, `cpp/src/ksim/combat/combat.cpp` (the transient `Member::lastAttacker` and `DeathNote`); test `ksim_world_state` (the fight, the dialogue gate before / after, save + load).

v72 -> v73 (2026-11-06, the delivery order's age, TOWN_LIFE_SLICE 7.4 / T4d): `state::CaravanOrder` gains `postedAt` (f64, game hours, after `phase`): the hour the match created the order. An order in load or go older than 48 game hours is released by the next economy tick: its goods return to the source line (clamped to the cap), or are lost if the source town is gone; the order is dropped and the destination's need re-posts, so a carrier that never arrives cannot hold a shortfall reserved forever. A v72 save carries orders without an age: they load at 0, and the first economy tick after the upgrade releases them (RUNTIME_SAVE's no-migration rule). Code: `cpp/include/ksim/core/state.hpp` (`fields`), `cpp/include/ksim/economy/economy.hpp` (`kOrderMaxHours`), `cpp/src/ksim/economy/economy.cpp` (`Simulation::stepEconomy`); test `ksim_economy_caravan` (the order's age across a save/load, and the stale release with and without its source).
v73 -> v74 (2026-11-06, the goal that started a fight, task AT): `state::Fighter` gains `combatGoal` (u16, after `targetAge`): the AI_TASK whose goal picked the current target — 5 a player's attack order (n_ATTACK), 32 SELF_PRESERVATION, 16 / 15 / 143 / 34 / 21 the join goals, 0 = none. Written where the target is set (`CombatArena::setTarget`'s goal argument: `pickTarget`, `joinFight`, the attack-order path) and cleared with the target; the Goal row of CHAR_STATS reads it (SIMLINK.md "Goal names"). A v73 save loads with 0: the row falls back to the plain attack goal (5 for a player character, 16 otherwise) until the next pick. Code `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/include/ksim/world/join_rules.hpp` (`goalOf`), `cpp/src/ksim/combat/combat.cpp`, `cpp/src/ksim/combat/char_stats.cpp`; test `ksim_goal_label`.

v74 -> v75 (2026-11-06, the dialogue memory, task BB): `state::Fighter` gains `met` (a vector of `state::MetRecord{target u64, tick u64}`, ascending by tick, at most 32 entries and the oldest evicted): who the character has seen and the sim tick it last saw it, written by the perception scan and DA_REMEMBER_CHARACTER. It backs DC_TARGET_LAST_SEEN_X_HOURS_AGO and DC_MET_TARGET_BEFORE (the exe's squad memory, per squad there; per character here, UNSURE). A v74 save still loads (`ar.version >= 75`): every character has met nobody, and the perception scan fills the entries again.

v75 -> v76 (2026-11-06, the ragdoll's lying pose, task BG): `state::Fighter` gains `restYaw` (f64), `restRolls` (u32) and `restYawValid` (bool), also gated `ar.version >= 76`. `restYaw` is the direction the body lies in while prone (CHAR_DYNAMIC flag bit 5; the character yaw convention, head to feet), rolled at every ragdoll start from the fighter's own `kRagdollStream` draw `streamSeed(streamSeed(streamSeed(seed, kRagdollStream), Id), restRolls)`; `restRolls` is the draw count, so a launch after a save/load takes the same flop seed and yaw, and `restYawValid` marks a yaw that belongs to the current prone episode (cleared when the body is up). A v75 save loads every fighter with no yaw: a body that is prone then rolls one on its first step. Code `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/include/ksim/core/rng.hpp`, `cpp/src/ksim/combat/combat.cpp` (`CombatArena::rollRagdoll`); test `ksim_simlink` (EVENT 48 and the rest tail), `ksim_saves`.

v76 -> v77 (2026-11-07, the new-game advanced options, task BT): a new singleton `state::WorldOptions` with `chanceOfDeath` (f64, cod, 0.5..4), `damageScale` (f64, gdm, 0.5..4) and `hungerTime` (f64, ht, 0.25..8), the values the world's NEW_GAME carried (the frame's optional trailing options block, SIMLINK.md) or a DEBUG_SET_OPTION made; the sim pushes them into the combat arena's `CombatSettings` copy, so the saved values are what the medical step and the damage rules read. A world without the singleton (every v76 save, and a world that never sent options) keeps the server's settings, which is the same behaviour. The medical clock is NOT saved: it is a session dev override (`COMBAT_OPTIONS.md` 4). Code `cpp/include/ksim/core/state.hpp`, `cpp/src/ksim/core/simulation.cpp` (`Simulation::setGameOption` / `applyGameOptions`), `cpp/src/ksim/save/newgame.cpp` (`parseNewGameOptions`, `runNewGame`); test `ksim_world_options` (the refusals, the round trip, cod x2 the bleed, ht x2 the hunger), `ksim_newgame` (the frame suffix, new_world clearing it), `ksim_debug_weather_time` (215 and QUERY kind 2).

v83 -> v84 (2026-11-07, the intruder eviction, task CH): `state::TakeOut` replaces its `phase` byte (u8: 0 the lift, 1 the way out) with the goal's G4 chain - `plan` (a vector of u16 task ids), `step` (u16), `liftOrdered` and `dropSet` (bools), all before the drop point, gated `ar.version >= 84`. An older save loads with no plan (the phase byte is consumed and dropped); on its first tick `stepIntruders` rebuilds the plan from the live hands - the body in the arms plans 224 outside the building, otherwise 68 LIFT_PERSON - the same rule `Routine::plan` follows. Code `cpp/include/ksim/core/state.hpp`, `cpp/src/ksim/combat/carry.cpp` (`stepIntruders`), `cpp/src/ksim/ai/goap.cpp`; test `ksim_carry` (the take-out's save/load round trip).

v84 -> v85 (2026-11-07, the indoors state, task indoors): `state::Fighter` gains `indoors` (bool, after `floor`), gated `ar.version >= 85`: in a building AND a ceiling above it (a storey surface above the floor, or the placement's roof within 30 u), so a roof deck is outdoors. `state::FighterWeapon` gains `indoorsMod` (f64, before `quality`, gated v85), the weapon's `indoors mod`, and `state::FighterArmour` gains `weatherTypes` (u32, a bit per weather affect type it protects) and `weatherProtection` (f64, `weather protection amount`), both gated v85. `indoors` and `cavernous`/`sheltered` are also recomputed from the placement on load (the step that sets the location); a v84 save loads every fighter with `indoors` false, which the first building step after the load rebuilds from where it stands. Weapon and armour fields derive from the equipped records, which the inventory already carries. Code `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/include/ksim/world/world.hpp`, `cpp/src/ksim/{world,combat_data,combat,carry,simulation}.cpp`; test `ksim_indoors`.

v81 -> v82 (2026-11-07, the GOAP planner, task CB): `state::Routine` gains `plan` (a vector of u16 task ids: the member's planned chain, execution order, empty = none) and `step` (u16, the step it runs now), after `chainElapsed`, gated `ar.version >= 82`. An older save loads with no plan; the executor plans the running goal once from the live world (`Simulation::planSlaveChain`), so a load mid-chain continues the same way. Code `cpp/include/ksim/core/state.hpp`, `cpp/include/ksim/ai/goap.hpp`, `cpp/src/ksim/combat/carry.cpp` (`stepSlaveTrade`), `cpp/src/ksim/ai/routine_scorer.cpp`, `cpp/src/ksim/world/world_tasks.cpp`; test `ksim_goap_plan` (pure) and `ksim_capture_live` / `ksim_town_guards` (the chain across a run).
v85 -> v86 (2026-11-07, the town shopping trip, task CH group 3): `state::Routine` gains the member's shopping list and the 119 transaction timer, after `searched`, gated `ar.version >= 86`: `shopping` (a vector of `state::ShoppingWant{function u16, want u16, taken u16}` — the ITEM `item function` it wants, how many the day's list wants and how many it has bought), `shoppingDay` (i32, the game day the list was rolled for, -1 none), `shoppingTimer` (f64, the 1 s re-roll the transaction waits), `shoppingLatch` (bool, one buy this trip), `shoppingShop` (u64 building) and `shoppingX` / `shoppingZ` (f64, the counter it stands at). A v85 save loads with no list: `Simulation::refreshShoppingList` rolls it from the character's template on the first evaluation. v85 itself is the indoors slice's (ds/indoors); this branch was cut before it. Also `kSaveVersion` 86. The list is rolled from CHARACTER `shopping` refs (ITEM, want, chance): a character without one never takes 118. Code: `cpp/include/ksim/core/templates.hpp` (`ShoppingRef`), `cpp/include/ksim/economy/shopping.hpp`, `cpp/src/ksim/{shopping,shopping_sim}.cpp`, `cpp/src/ksim/ai/routine_scorer.cpp`; test `ksim_shopping`.

v80 -> v81 (2026-11-07, the stable combat dice, task CC): `state::Fighter` gains `diceSeed` (u64), gated `ar.version >= 81`, written at the end of the fields. It is the character's stable dice identity: every combat stream of a fighter (the combat rng `kCombatStream`, the comfort radii `kAvoidStream`, the ragdoll `kRagdollStream`, the follow formation `kFormationStream`) and perception's scan stagger and noise roll are keyed on it instead of the Id, so an entity spawned earlier in the world (an interior-layout storage box) no longer re-rolls every fight. For a town or wilderness member it is its squad's `SquadInstance` content plus its slot in the squad (`Simulation::buildMember`); for a scenario-file character, the file's per-character spawn value. 0 = none: a save older than v81 loads every fighter at 0 and the arena keys on the Id, which is what the fighter's saved `rng` already started from, so nothing moves. v80 is unused (task CB's goap fields landed at v82); v79 task BQ's `attackType`. Code `cpp/include/ksim/combat/fighter_state.hpp`, `cpp/src/ksim/{combat,residents,scenario_file,perception}.cpp`; test `ksim_stable_dice`.

v86 -> v87 (2026-11-08, the lock state, task locks-save): a new singleton `state::LockStates` holds `entries`, a vector of `{shackles bool, id u64, elapsed f64, interval f64, attempts u32, hardness f64, open bool}` after the other singletons (gated `ar.version >= 87`). One entry per lock a character picked, cut or forced: `shackles` false = a cage building's lock (`id` the building object id), true = a wearer's worn shackles (`id` the member id); `open` the unlocked / broken flag, `elapsed` / `interval` / `attempts` the picking timer every picker shares, `hardness` the remaining work value a cut drains (< 0 = not read from the data yet). A lock nobody touched has no entry; a cage put-in and a removed pair of shackles erase theirs. A v86 save loads every lock untouched (cages with a prisoner locked, shackles locked). The queue of ordered lock tasks is derived and not saved. Code `cpp/include/ksim/core/state.hpp`, `cpp/src/ksim/nav/lock_tasks.cpp` (`lockFind` / `lockEnsure` / `lockErase`), `cpp/src/ksim/combat/carry.cpp`; test `ksim_lock_save`.
v88 -> v89 (2026-11-08, item provenance, task ds/provenance): `InventoryEntry` gets `planted` (a vector of `{units u32, planter u64, tick u64}`) and `stolen` (string, the victim faction) after `unique`; `GroundItem` gets `stolen` after `ownerSid` (all gated `ar.version >= 89`). A v88 save loads every item clean. Code `cpp/include/ksim/core/state.hpp`; test `ksim_provenance`.
v89 -> v90 (2026-11-09, farm crops, task ds/arrest-2): a new per-farm component `state::CropPlot` (on the farm building entity) holds the plot's crop growth and ripe food (see `state.hpp`, registered in `SavedComponents`), gated `ar.version >= 90`. A v89 save loads with no plots; `Simulation::ensureCrops` makes them (ripe) at the first query (finder, order, `cropPlots`), never by the tick, so a world nobody asked about carries none. Orders (`cropOrders_`) are derived and not saved. Code `cpp/src/ksim/economy/crops.cpp`; test `ksim_crops`.

v90 -> v91 (2026-11-09, task ds/kidnap): `Fighter` gets `captive` (bool, the captive-in-transit flag) after the captivity block (gated `ar.version >= 91`). A v90 save loads nobody captive. Code `cpp/include/ksim/combat/fighter_state.hpp`; tests `ksim_crime`, `ksim_carry`.

v91 -> v92 (2026-11-09, the G1 Board's runtime state, task ds/board-save): a new singleton `state::BoardStates` holds `entries`, a vector of `{squad u64 (the squad's Id), replacement string, contract string, contractSignal i32, contractExpiry f64, current string, currentPriority i32, reapply bool, runningAway bool, stickyCageBuilding u64, stickyCageSlot u16, hasSmell bool, smellX f64, smellZ f64}` after the other singletons (gated `ar.version >= 92`). The Board's priority table stays derived from the data and is rebuilt on load; only what the data cannot rebuild is saved: the dialogue `change AI` package (`replacement`, installed over the rebuilt table), the active contract and its expiry, the applied package, reapply, running away (dt19 "runningAwayMode"), the sticky cage hand and the blood smell. A v91 save loads every board rebuilt from the data, as before. Not modelled so nothing to save: the HiredAlly ended flag, current_town / town_time. Code `cpp/include/ksim/ai/blackboard.hpp` (`capture` / `restore`); test `ksim_board_save`.

v92 -> v93 (2026-11-09, task ds/town-port): one new saved component appended last in `SavedComponents`: `state::TalkHold` (`until` u64, the tick a character the player talked to stands still until; set by `Simulation::talkTo`, removed by the town scorer's loop when it runs out, a TTL because the sim does not see the Session's trade window). An older save loads nobody held. The hold is the same pattern as the container holds per client session (ROADMAP 7): the two should share one mechanism later; not built. Code `cpp/include/ksim/core/state.hpp`, `cpp/src/ksim/dialogue/dialogue_sim.cpp`, `cpp/src/ksim/ai/routine_scorer.cpp`; test `ksim_trade`.

v93 -> v94 (2026-11-09, task ds/board-pick): `state::BoardStates::Entry` gains `startedAt` f64, `traderState` i32 and `occupyHeld` f64 after `smellZ` (gated `ar.version >= 94`): the F package classes' per-package state (kinspect dt41 F) - when the applied package started (GoOutOnPatrol's 12.0 and PassingByTownAssault's 0.25 clocks), Package_WanderingTrader's 0 / 1 / 2 state and Package_OccupyTown's held time. A v93 save loads them as 0, i.e. restarted. Code `cpp/include/ksim/ai/blackboard.hpp` (`capture` / `restore`); test `ksim_board_save`.

No bump for task raid-stay (2026-11-11): `occupyHeld` keeps its slot but now accrues while NO member stands in the occupied town (4 s of play); the per-town occupancy record and `Board::occupyTown` / `assaultEngaged` are derived each second and never saved.

v98 -> v99 (2026-11-10, the member delta store, task ds/delta-store, kenshi-k16.6.51; part 1 of the demotion save, corpses k16.6.52, stragglers k16.6.53): one new saved component appended last in `SavedComponents`: `state::MemberDelta` on a character entity, the persistent subset of its Fighter while its squad is a token. Fields: `mask` (u8: 1 body, 2 slave, 4 weapon, 8 armour, 16 skills), then only the blocks the mask names. Body: `parts` (vector of FighterPart), `blood` f64, `wounds` (vector f64, the bleeding wound rates), `koTimer` f64, `unconscious`, `dead`, `rightArmOk`, `leftArmOk`, `down` (bools). Slave: `slaveState` u8, `captorSid` str. Weapon: a `FighterWeapon`. Armour: a vector of `FighterArmour`. Skills: a `FighterSkills`. `demoteWorldSquad` writes a record only for a member whose Fighter differs from `Simulation::templateFighter` (the makeFighter build `buildMember` makes, same dice) in a block, and only that block; an untouched member stores nothing and has any old record removed. `buildMember` applies the record over the fresh Fighter (at the token's ring, nothing transient: no route, target, task or combat timer) and removes it: the Fighter is then the state again, so a promoted squad saves with no records. A v98 save loads with the component absent (its kind list is one shorter: `componentKinds`): every member untouched, as before. A v98 write (`writeSaveFile(..., 98)`) leaves the component out. Tests `ksim_delta_store` (wound, KO, enslave, loot, demote, run, promote; the save and load of the demoted squad; a v98 file), `ksim_delta_untouched` (an untouched squad's members, hashed field by field after a demote and promote, equal what the base produces: pinned. The whole-world state hash cannot be compared with the base: it covers the component kind list, and other Hub members touched in the first ticks now keep their changed weapon instead of being rebuilt from the template).

Not stored, on purpose: `captorFaction` (derived from `captorSid` every tick, simulation.cpp), `koLatch` and `bleeding` (recomputed by the medical update from the parts and wounds), the route, target, task and combat timers (a promotion clears them), hunger, the carried crossbow (`ranged`, its ammo and reload), `carrying` / `carriedBy` (removeFighter frees them), the cage slot. Dead members keep the old behaviour: they stay on the map as bodies with their Fighter (corpses are k16.6.52). Members knocked out now demote with their squad and keep their `unconscious` and `koTimer` (before, a knocked-out body stayed in the arena when the squad was demoted).

Questions (answered with a default, deferred: playtesting): (1) a knocked-out member is promoted lying at the squad's ring, not where it fell; the timer does not run while the squad is a token (no off-screen healing, as the original; k16.6.52 changes that). (2) hunger is rebuilt from the template at a promotion (a starving member is fed again). (3) the crossbow's ammo and a caged slave's cage are rebuilt, not kept. (4) a scenario file's character, built by hand, differs from its template in many blocks, so it stores a record on its first demotion.

v99 -> v100 (2026-11-11, task ds/corpses, kenshi-k16.6.52; part 2 of the demotion save): no new component. `CorpseDecay` gains `deathTick` (u64) and `cause` (u8: 0 other, 1 bled out off-screen, 2 token fight, written by `stepTokenFights`, k16.6.46) after `stamp`: a body that died while its squad was a token carries them (`Simulation::leaveCorpse`), every other body 0 / 0. `MemberDelta` gains a block, mask bit 32 `kKit`: `kitSid` (string) and `kitCharges` (f64), the first-aid kit a stored medic has started (the off-screen first aid spends charges); written only for a member that differs from its template in them. An older save (v99) loads every corpse with no death tick and cause 0 and no member with a started kit; the reader accepts v99 and v100. The off-screen medical step itself is a process setting (`world.offscreen_medical`, COMBAT_OPTIONS.md 8) and is not saved. A corpse that died off-screen is an ordinary dead `Fighter` with its `CorpseDecay`, so it saves and loads as any body. Test `ksim_offscreen_medical`.

v100 -> v101 (2026-11-11, task ds/member-slot, kenshi-k16.6.55): one new saved component appended last in `SavedComponents`: `state::MemberSlot { u32 slot }` on a character entity, the build slot its template build is keyed on (`memberDiceKey(squad, slot)`). The spawner (`SquadBuilder::addMember`) writes it, the member's index in the squad at spawn; `Simulation::templateFighter` (so `buildMember`, `demoteWorldSquad`, `stepStoredMedical` and `leaveCorpse`) reads it instead of the member's place in the Id-sorted list, which shifts when an earlier squadmate is destroyed (a corpse decays, a capture) and so used to re-roll the later members. A member without one (a v99/v100 save, a scenario character, a recruit) takes its list index once at its first build and keeps it. The older kind-name list is one shorter (`componentKinds`): v99 and v100 end at `MemberDelta`. memberDiceKey's other consumers (combat stream, comfort radii, ragdoll, follow formation) read `Fighter::diceSeed`, which comes from the key, so they follow the slot. deferred: playtesting: a member that moves to another squad keeps its slot, so it can share a key with a member of the new squad of the same template (identical rolls). Test `ksim_member_slot`.

v101 -> v102 (2026-11-11, task ds/stragglers, kenshi-k16.6.53): one new saved component appended last in `SavedComponents`: `state::StragglerOf { EntityRef parent }` on a squad entity, the token a member was split into at demotion (`Simulation::splitStraggler`) and the squad it came from. The token carries a copy of the parent's `SquadInstance` (the member's template dice key on it), a Position, and the one member (`MemberOf` the token, with its `MemberSlot` and `MemberDelta`). Removed when the token merges back (`mergeStraggler`, which destroys the token) or when the parent is gone (the token stays a lone squad). `componentKinds`: v101 lists end at `MemberSlot`, v99 / v100 at `MemberDelta`. A v101 file loads with no stragglers; a straggler token written as v101 loses its link and loads as a lone squad. Test `ksim_stragglers`.


v102 -> v103 (2026-11-11, task ds/save-import2, kenshi-cym.2): no new component. `MemberDelta` gains a look block, mask bit 64 (`kLook`): `name` (string) and `female` (u8), written after the kit block, only when the member's name or sex differs from its template build. The original-save import (`ksim::saveimport::importSave`) needs it: a token member of an imported save keeps its own name and sex, which the template does not say. A v102 file loads unchanged (the bit is never set in it); a v102 build refuses a v103 file by the version. `MemberDelta::applyTo` sets `Fighter::name` and `look.female`. Test `ksim_save_import`.

v103 -> v104 (2026-11-11, task ds/limb-tint, kenshi-h2v.13.9): no new component. `GroundItem` gains `tint` (`LimbTint`: `bool set`, then nine f64: skin rgb, blood rgb, scale xyz), written after `stolen`, for every ground item (set false and the rest default except for a severed limb). A v103 file loads with every item untinted (its limbs draw untinted); a v103 build refuses a v104 file by the version. Test `ksim_limb_tint`.

v104 -> v105 (2026-11-12, task ds/save-import3b, kenshi-cym.6): one new component, `ExtraTown`, appended **last** to `SavedComponents` (after `StragglerOf`; `componentKinds(version)` is one shorter below 105). Fields: `instance`, `rootSid`, `activeSid`, `name` (strings), `owner` (`EntityRef`), `placed` (bool), `x`, `z` (f64), `info` (`TownInfo`): a town record of an imported save with no `TownState` of ours (a later placement of a TOWN record, the player's town). `TownInfo` is `known`, `isPublic`, `level` (i32), `material` (string), `started`, `explored`, `discovered`, `recentlyDiscovered`, `ded` (bools), `pop`, `popd`, `pop2`, `popd2` (f64), `factionsHere` (strings). Existing components gain fields, each gated `ar.version >= 105`: `TownState.info` (`TownInfo`, after its last field); `FactionState` gains `rank` (i32), `globalTrust` (f64) and `trust` (a vector of `FactionTrust{other string, trust f64, trustNeg f64}`, sorted by `other`); `WorldEventState` gains `used` (a vector of `UsedUnique{sid string, state i32, player bool, member u64}`, the save's `usedUniques`, kept apart from `dead` / `playerTouched`). A v104 file loads with no `ExtraTown`, default town info (`known` false), rank 0, no trust rows and no used uniques; a v104 build refuses a v105 file by the version. Written only by the import (`Simulation::importExtraTown`, `importTownInfo`, `importFactionStanding`, `importUniques`); nothing in the sim reads them yet. Test `ksim_save_import` (SkinHouse round trip: same state hash, 137 `ExtraTown`, 75 used uniques).

v105 -> v106 (2026-11-13, task ds/spectate-stagger, kenshi-k16.6.124): no new field or component. `InventoryEntry::slot` gains a value, `Inventory::kSheathed` (15, after `kBackpackSlot`): a weapon sheathed on the back by plan task 7 (`Simulation::sheatheWeapon`) stays equipped in this slot, no grid room needed, while its Fighter has the bare-hands weapon; plan task 6 (`drawWeapon`, also `stepRearm` at the first target) moves it back to `kWeapon`. Before, a sheathed weapon went to the pack as a carried entry, or stayed in hand when the pack had no room (the Berserkers' Fragment Axe). Every reader treats a slot other than `kCarried` as equipped, so the wire (`item_grid`) sends it as an equipped entry; `kFlagDrawn` still reads the Fighter's weapon type. A v105 save loads with no such entry; a v105 build reads a v106 file's slot 15 as an unknown equipped piece, hence the bump. Test `ksim_spectate_stagger`.

v106 -> v107 (2026-11-13, task ds/slave-timers, kenshi-k16.6.126): no new component. `Fighter` gains `slaveStamp` (f64, after `cageSlot` inside the slave block, gated `ar.version >= 107`) and `MemberDelta`'s slave block (mask `kSlave`) gains `slaveStamp` after `captorSid` (same gate): the world-clock game hour (`day * 24 + hour`) of the last change into slave state 2 or 3, -1 = none. The 2 -> 3 timer (12.0 h) and the 3 -> 0 timer (96.0 h) run from it (rwh.106), so a reload keeps the time left; the importer fills it from `ssct`. A v106 file loads every state 2 / 3 slave with -1, stamped at its first look by the slave step; a v106 build refuses a v107 file by the version.

v107 -> v108 (2026-11-13, task ds/container-piece, kenshi-k16.7.29): no new component. `Container` gains `building` (u64) and `piece` (u16, 0xFFFF = none) after `version`, gated `ar.version >= 108`: the wire's INTERACTABLE kind 3 tail (SIMLINK.md). A v107 file loads its boxes with 0 / 0xFFFF; the next furnish pass (`spawnMeshContainers`) finds each by its key, fills both from the data and bumps its version. The box key is unchanged (it keeps the position among the kept layout pieces), so saves are otherwise unaffected. A v107 build refuses a v108 file by the version.

v108 -> v109 (2026-11-14, task ds/save-walk-arrest, kenshi-k16.6.200): no new component. `Fighter` gains `walkHeading` and `walkFacing` (f64 each, after `slaveStamp`, gated `ar.version >= 109`): `walkOrder`'s velocity memory (the commanded heading, and the yaw it wrote with it), so a walker under a squadmate push resumes on the heading of an unbroken run. A new singleton `ArrestStates` (`SavedSingletons`, last entry; written by `syncBoardStates` just before an encode and removed after it and after a load, like `BoardStates`; absent with no order running) holds `Simulation::arrestOrders_`: a u32 count, then per guard in Id order `guard` (u64), `victim` (u64), `since` (u64, a clock tick), `lift` (u16), `started`, `snatched`, `gaveUp` (u8 each). `restoreBoards` rebuilds the map from it. A v108 file loads with the memory 0 / 0 (the walk then takes the yaw, as before) and no arrest order; a v108 build refuses a v109 file by the version. Tests `ksim_follow` (a save under a squadmate push continues tick-identical) and `ksim_save_mid_order` (`arrestWaits`).


v109 -> v110 (2026-11-14, task ds/unsaved-audit, kenshi-k16.6.202): no new component. A new singleton `RunStates` (`SavedSingletons`, last entry; written by `syncBoardStates` just before an encode and removed after it and after a load, like `ArrestStates`; absent when all four maps are empty) holds `limbCooldown_`, `limbPlay_`, `alarms_` and `alarmDwell_`: `cooldowns` (member u64, finder u64 clock tick), `plays` (member, x, z f64, picks u32), `alarms` (town u32 = the environment's town index, id u64, kind u8, owner, raiser, building u64 Id values, x, z, life f64), `lastAlarm` (u64, the board's id counter), `dwells` (member u64, left f64); each list in map / board order. `restoreBoards` rebuilds the four from it. A v109 file loads them empty. A v109 build refuses a v110 file by the version. Tests `ksim_save_mid_order` (`limbPlay`, `limbCooldown`, `alarmWait`: the value read after the load equals the one before the save).
v110 -> v111 (2026-11-14, task ds/drive-strafe, kenshi-k16.8.17): no new component. `Fighter` gains `driveStrafe` (i8: -1 left, 0 none, +1 right of the heading, the trailing strafe of SIMLINK order 20; after `driveTurn`, gated `ar.version >= 111`). A v110 file loads it 0. A v110 build refuses a v111 file by the version. Test `ksim_move` (strafe block: save, load, continue).

v111 -> v112 (2026-11-16, task ds/passive-hold, kenshi-k16.6.267): no new component. `Fighter` gains `passive` (bool, CharStats+0x12c: PROTECT_ALLIES at utility 0) and `holdPosition` (bool, the HOLD POSITION toggle: no ally join, no chase, fights only within 40 u), after `driveStrafe`, gated `ar.version >= 112`. A v111 file loads both off. A v111 build refuses a v112 file by the version. Tests `ksim_sneak_break` (passive / hold), `ksim_producers` and `ksim_trade` (save-version asserts).

The smash / build / crop / standing orders stay documented drops; every `WorldRuntime` member is now saved, a table row above, or derived / runtime-only and annotated so in `simulation.hpp` (the memos and occupancy maps, `raidLeaders_` / `raidHolds_` / `raidWon_`, `fightPairs_`, `buyback_`, the slave, eaten, medic and torture trackers, `announced_`, `buildPower_`, `buildersOn_`, `cropsReady_`).

y meaning (2026-11-18, kenshi-k16.8.31): no layout change and no `kSaveVersion` bump (ksim's call to revisit). `Fighter::y` is now always the navmesh point: the walker no longer snaps it to the terrain (it was within 5 u of the terrain on open ground), and the wire sends it as is. A save from before keeps its terrain-snapped y until the first walker step puts the character back on the mesh.
