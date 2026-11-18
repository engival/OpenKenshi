# kdata — milestone 1, step 1 (game-data record layer)

Small C++20 library (`kdata`) plus `kdump` and `kmerge`. Standard library only;
no third-party code, no network. Reference: `tools/kmod.py` and `docs/formats/MOD_FORMAT.md` in the repo.

## Layout

```
CMakeLists.txt
include/kdata/format.hpp     data model + parse/write + ParseError
include/kdata/gamedata.hpp   overlay loader
src/format.cpp               all version branches (v1..v17)
src/gamedata.cpp             field/reference/instance/REMOVED overlays
tools/kdump.cpp              kmod.py-compatible summary
tools/kmerge.cpp             merged-record TSV
tests/{roundtrip,parity,overlay,versions,malformed}.cpp
tests/test_util.hpp
```

See README.md for build/test. Tests: roundtrip (all 686 record files, byte compare), parity (runs kmod.py, compares record and per-type counts), overlay and versions (synthetic), malformed (truncation/fuzz, also under ASan+UBSan).

## UNSURE items (also marked `// UNSURE:` in the code)

All are in `src/gamedata.cpp` unless noted. "Simplest rule" as instructed.

1. **:38** Pre-v8 reference identity. The 8 opaque bytes have no target string,
   so overlay identity is the raw 8-byte payload.
2. **:56** v8/v9 carry only `v0`. The spec defines removal only as
   `v0=v1=v2=0x7FFFFFFF`; a lone `v0==0x7FFFFFFF` is treated as removal there.
3. **:114** Instance key across versions. v15+ uses the string ID, earlier
   versions an i32; keys get an `s:`/`i:` prefix so the domains cannot collide.
4. **:132** Instance deletion (empty target) is only applied for file version
   >= 8, since earlier versions have no target field.
5. **:154** Name merging before v15, where there is no flags word: a non-empty
   incoming name wins. For v15+ the documented rule is used instead
   (`MODIFIED|RENAMED` required to rename an existing item).
6. **:159** The merged record's flags word: the newest record's word replaces
   the previous one when present.
7. **:163** `type`/`id` of an overlay record are ignored on merge; identity is
   the stringID and the first definition establishes them.
8. **:167** Fields are merged independently per section. MOD_FORMAT.md's single
   `modData` dictionary implies the "string does not overwrite a non-string"
   rule could matter across sections; no shipped record has a cross-section key
   collision (checked: 0 within each of the four files and 0 between each mod
   and base).
9. **:181** v11–v14 tag tables are per-record application filters, not data;
   the required overlay set does not define their merge, so the target keeps
   its table.
10. **:190** The v17 header `deletes` block (`':'`-separated stringIDs) is not
    mentioned by the load-semantics section and is ignored by GameData.
11. **:205** Editor error states (`MODIFIED` on a missing item, or a
    non-modifying record colliding with an existing item) are not part of the
    required overlay rules; an unknown stringID is always inserted.

Additional judgment calls worth disclosing (not marked UNSURE because the
source is explicit or because kmod.py settles it):

- `parse` synthesises a pre-v7 stringID from a filename, but the required
  one-argument API has no filename; the second parameter defaults to `""`, so a
  bare `parse(span)` yields `"{id}-"`. Tools/tests pass the basename.
- `write` appends `File::leftover`, unlike kmod.py, so v15 zone/level files are
  byte-exact. This is required by the roundtrip test.
- A bool byte > 1 throws `ParseError` (kmod.py behaviour), although MOD_FORMAT.md
  says `ReadBoolean` accepts any non-zero.
- Negative/oversized counts throw `ParseError`; Python's `range(negative)` in
  kmod.py silently yields an empty list.
- Strings are stored as raw bytes and are not UTF-8 validated; kmod.py's
  `.decode('utf-8')` would raise on invalid UTF-8. Valid input matches.
- Reference categories are kept in first-appearance order; a category emptied
  by removals is retained. Field/instance replacement keeps the original
  position; new keys/entries append.
- `kmerge` writes raw TSV fields (no escaping of tab/newline in names).
- `.save` is out of scope (as required); there are no `.save`-specific branches
  in kmod.py to mirror.

## MOD_FORMAT.md: wrong or ambiguous, with evidence

1. **"No trailing data" is wrong for v15 zone/level files.** The same document
   and `WORLD_LAYOUT.md` describe a trailing `i32 n + n*i32` block.
   `zone.35.20.zone` has `leftover=4`; `rebirth/zone.20.55.zone` has
   `leftover=5732` (1432 values after the leading count). kmod.py reports the
   leftover but does not write it, so its own roundtrip prints `False` for these
   files. Our `File::leftover` stores and re-emits it, giving `True`. (v16
   `rebirth/zone.12.37.zone` has a header and `leftover=0`, matching the doc's
   version table.)
2. **Header extra bytes.** The doc says the reader seeks to `headerEnd`, so
   unknown bytes are skipped. kmod.py instead raises `header end mismatch` when
   bytes remain after the optional blocks. No shipped file exercises this
   (all headers end exactly after a known block: `Dialogue.mod` size 67 ends
   after `mergedCount`; `rebirth.mod` size 70 has the full tail). We follow the
   spec and preserve such bytes in `Header::extra`, so they round-trip.
3. **`bool` primitive.** The table says any non-zero byte is true; kmod.py
   raises for values > 1. All shipped bytes are 0/1, so the files cannot settle
   it. We chose kmod.py parity.
4. **Merged-block guard.** The doc describes peeking the next `i32` and stopping
   when it is > 256; kmod.py reads a `u8` count and never peeks. All shipped
   `mergedCount` values are 0, so neither behaviour is observable. We mirror
   kmod.py.
5. **Removal marker width.** The doc defines removal as all three ints equal to
   `0x7FFFFFFF`; kmod.py's summary classifies on `v0` alone. No shipped file has
   any removed reference (`refs: {'normal': N}` for all four), so this is only
   exercised by hand-built tests. GameData uses all three for v10+ and `v0`
   alone for v8/v9.
6. **`CONSTANTS` field discard (version < 14).** The doc says FCS discards field
   data for type 27 on load, but kmod.py parses it normally and round-trips it.
   All three files that contain one `CONSTANTS` record
   (`gamedata.base`, `Newwworld.mod`, `rebirth.mod`) round-trip only if the
   fields are kept, which we do; whether an FCS-level merged view should hide
   them is a later-milestone question.
7. **Pre-v7/pre-v15 ID synthesis** (`"{id}-{filename}"`,
   `"{n}-{filename}-INGAME"`) describes FCS load behaviour; kmod.py keeps the
   raw i32 and writes it back. We mirror kmod.py (round-trip requires it); the
   filename-dependent string form described by the doc is only implementable
   with a filename, which the required `parse(span)` API does not carry.
8. **Version table wording "v1–6" / "v7+" etc.** is correct as matched against
   the `if ver >= N` branches in kmod.py; the synthetic v1..v17 test pins every
   boundary (`> 8` vs `>= 10`, `> 6`, `>= 15`, `>= 16`, `>= 17`).

## Notes on the two tools

- `kdump <file>...`: prints the same lines as `kmod.py` (header dict, type
  counts with Python dict/`repr` formatting, flags, size field, field totals,
  reference counters, ordinal-order counters, instance/REMOVED counters). Exit
  status is non-zero if a file cannot be parsed. Formatting was verified
  identical over all 686 files except the intentional roundtrip-line difference
  above.
- `kmerge <out.tsv> <file>...`: loads files in order through `GameData` and
  writes `type \t stringID \t name \t fieldCount \t referenceCount \t
  instanceCount`, sorted by stringID (byte/UTF-8 ordinal). Prints the merged
  record count to stdout.

# kworld — milestone 1, step 2 (world readers)

Small C++20 library (`kworld`) beside `kdata`, two tools and six tests. Standard
library only. 

## Layout

```
include/kworld/coords.hpp    world/zone constants, zone cell/index helpers
include/kworld/heightmap.hpp TERRAIN_FORMAT.md reader + bilinear height()
include/kworld/zones.hpp     zone containers, instances, placed buildings
include/kworld/gamedata.hpp  merged-GameData helpers, BUILDING float lookup
include/kworld/towns.hpp     TOWN records + townlist placements
include/kworld/pathing.hpp   GLOBAL_PATHING_FORMAT.md reader
src/kworld/*.cpp             the five readers; io.hpp/le.hpp are internal
tools/kworld_survey.cpp      world_survey.py-compatible summary
tools/kheight.cpp            height at a world (x, z)
tests/kworld_{terrain,zones,pathing,placement,towns,malformed}.cpp
tests/kworld_py/*            expectations generated by the reference tools
```

All game-data paths come from one cache variable:
`KENSHI_DATA`, default the local install's `data/`. The tests additionally use
`KWORLD_REF_TOOLS` (default: the repo's `tools/`), to run the Python
references.

## Implemented

- **Heightmap** (`TERRAIN_FORMAT.md`): memory-maps `newland/land/fullmap.tif`,
  validates the classic little-endian TIFF directory and the single
  uncompressed 16-bit strip, and interpolates bilinearly. Sample `i` is at
  `-147456 + 18i`; `height = raw * 9800 / 65535`. Edge samples clamp.
- **Zones** (`WORLD_LAYOUT.md`): recursively loads all `zone.X.Y.zone` files
  through `kdata`, parses the cell strictly from the filename, and
  classifies each instance: empty target = deletion/tombstone, all-zero
  x/y/z with a target = inventory row, otherwise a placement. Placements kept
  the raw 28-byte transform, decompose the 3x3 basis into a quaternion + scale,
  and set `y = bilinear terrain(x,z) + instance y` (`COLLISION_FORMAT.md`
  section 4). The scale is resolved from the merged BUILDING record named by
  the target. The v15 trailing `i32 n + n*i32` block is validated (ascending,
  unique) and preserved.
- **Towns** (`WORLD_LAYOUT.md`): merges `gamedata.base`, `Newwworld.mod`,
  `Dialogue.mod`, `rebirth.mod`, takes TOWN records after REMOVED overlays, and
  joins them to `0-townlist` instances in each layer's `leveldata.level` by
  state `-S5e` (`GAMESTATE_TOWN`). Declared zone cells come from `zones_count`
  and the `zone_x_k`/`zone_y_k` pairs. TOWN records carry no position, so the
  townlist instance is the positional source.
- **Global pathing** (`GLOBAL_PATHING_FORMAT.md`): parses the 12-byte header,
  2,587 nodes (40-byte header + `E*12` edges each) and the four-u32 trailer,
  rejecting trailing bytes. `validate()` reproduces the reference invariants
  (ids 1..N, count2 == count, H/V portal geometry, symmetry, shared cell,
  duplicate targets).

## Results (clean build, 2026-10-30)

Toolchain: GCC 15.2.0, CMake 4.1.1, Linux x86-64. Clean build (`rm -rf build`,
Release, `nice -n 10 ... -j8`) took **31.0 s** wall with no warnings. Full
suite: **13/13 passed**, 11.08 s (the five kdata tests still pass).

```
kworld_terrain:   heightmap 16385x16385; 20 sample heights match terrain.py (max |diff| 0)
kworld_zones:     672 files, 14614 placements, 19603 deletes, 1361 inventory rows;
                  per-zone counts match kmod (672 files); index round trips ok;
                  0 placements outside their cell
kworld_pathing:   2587 nodes, 93492 directed edges (H 1263, V 1324,
                  class20 33044, class27 60448); validate PASS
kworld_placement: 14614 placed, 14600 with scale, 14 missing targets (3 ids);
                  all in their file cell; quaternions unit;
                  example 2-rebirth-INGAME terrain 1948.94 abs y 1961.67
kworld_towns:     346 TOWN records, 383 townlist placements (169 targets,
                  177 towns without a position); positions/cells match kmod
kworld_malformed: TIFF, zone and pathing truncation/fuzz checks ok
                  (also kworld_malformed_asan, ASan+UBSan)
```

`kworld_survey > cpp.txt` and
`python3 -I tools/world_survey.py <install> > py.txt` are
byte-identical (`diff` empty). Survey numbers: 672 zone files
(Dialogue 28, Newwworld 329, rebirth 285 v15 + 29 v16, base 1), 500 distinct
cells, X 2..62, Y 0..62, 14,614/19,603/1,361 instances, 643 trailing blocks
present and 29 absent. Townlist: 383 entries in 169 target towns; all 274
entries that declare cells have their position inside a declared cell (the
survey's per-layer checks are 17/17, 49/49 and 208/208).

## Load timings (warm cache, Release)

```
heightmap open    < 0.0001 s  (mmap; 16385^2 samples)
zones load          0.036 s   (672 files, 14,614 placements)
gamedata merge      1.70-1.90 s (four files in documented order)
towns load          1.83 s    (merge + 3 leveldata.level files)
place()             0.007 s   (14,614 instances + terrain sample each)
pathing load        0.0005 s  (2,587 nodes, 93,492 edges)
kworld_survey       0.041 s   (zones 0.037, leveldata 0.004)
```

`kheight -122592.46875 55659.91015625` prints `1948.941608` (the
COLLISION_FORMAT.md example); the four world corners print `0` because their
border samples are zero in the TIFF, not because of clamping.

## Tests

Expected values are produced at test time by importing the Python references
under `python3 -I`; nothing is hard-coded. `kworld_terrain` uses 20 points
(world corners, cell borders/origins and interiors). `kworld_zones` compares
record/version/placement/deletion/inventory counts for every file, checks the
coordinate helpers over all 4096 cells and both round trips, and asserts no
placement is outside its file's cell. `kworld_pathing` compares every count,
the trailer, node/edge geometry and H/V/class totals, and requires
`validate()` to be empty. `kworld_placement` compares placement/resolved/
missing counts and the worked example, checks unit quaternions and recomputes
every absolute y. `kworld_towns` compares TOWN ids and all 383 placement
records (layer, id, target, state, position, cell, declared cells).
`kworld_malformed` builds synthetic TIFF/zone/pathing files, truncates at every
prefix, corrupts fields, and applies 500 random mutations per format; all must
throw `std::exception` and never crash. The same binary runs against an
ASan+UBSan build of the kworld/kdata parsers.

## UNSURE / spec gaps (all `// UNSURE:` markers are listed here)

1. **Heightmap vertex vs cell centre** (`src/kworld/heightmap.cpp`): the doc
   gives the sample spacing but not whether samples sit on vertices or cell
   centres. Vertex sampling + bilinear is used; it matches terrain.py and the
   COLLISION_FORMAT.md section 4 example (1,948.94 against the documented
   1,948.9), while centre sampling would shift every value by 9 units.
2. **Missing BUILDING scale** (`src/kworld/zones.cpp`): no rule is given for a
   target with no merged BUILDING record. It defaults to 1.0; 14 placements
   (3 distinct ids) are affected.
3. **Layer precedence** (`src/kworld/zones.cpp`, `include/kworld/towns.hpp`):
   the docs do not say which layer wins when several contain the same zone cell
   or the same town target. Every file/entry is kept and placed independently;
   672 files cover 500 distinct cells.
4. **v15 trailing block** (`include/kworld/zones.hpp`): the meaning of
   `i32 n + n ascending unique i32` is unknown. Its bytes and values are
   preserved; only the documented shape is validated.
5. **Global pathing** (`include/kworld/pathing.hpp`): `count2`, the four-u32
   trailer, edge class 20/27 and the edge-cost formula are undocumented. They
   are stored verbatim; only geometric/count invariants are checked.
6. **Town positions and duplicates** (`include/kworld/towns.hpp`): TOWN records
   have no position field, so positions come from the `0-townlist` instances.
   WORLD_LAYOUT.md says one instance per town, but the shipped data has 383
   entries for 169 targets; all entries are listed rather than merged, and 177
   of 346 towns have no placement.
7. **Declared town cells** (`src/kworld/towns.cpp`): a `zone_x_k` without a
   `zone_y_k` (or vice versa) has no defined meaning; only complete pairs are
   recorded.
8. **REMOVED TOWN records**: applying the merge's REMOVED overlays yields 346
   live TOWN records (two are removed in `rebirth.mod`); a raw count would be
   348. We follow the documented merge semantics.
9. **building_geometry.py** was not visible to the DeepSeek job; placement semantics come from COLLISION_FORMAT.md section 4 and terrain.py (the repo now has `tools/building_geometry.py`; not yet cross-checked).

# ksim — milestone 1, step 3 (world model + headless tick skeleton)

Library `ksim` (namespace `ksim`) builds a flecs v4.0.4 world from `kdata` +
`kworld` and drives a fixed-rate headless tick. `ksim_stats` prints model
counts, load time and RSS; `ksim_run <ticks>` prints tick timings and the state
hash. The game install is read-only; no `kdata`/`kworld` files were changed.

## Layout

```
include/ksim/core/templates.hpp    template components + relation types (ksim::tmpl)
include/ksim/core/state.hpp        instance components, Id, Clock (ksim::state)
include/ksim/core/detail.hpp       DetailLevel / Detail
include/ksim/core/clock.hpp        tick length, game day/hour, new-game start
include/ksim/core/rng.hpp          RngStream, streamSeed, stream numbers
include/ksim/world/spawn.hpp        spawnDefaultWorld (default-world rules)
include/ksim/save/save.hpp         runtime save v0, stateHash
include/ksim/save/snapshot.hpp     squad JSON snapshot
include/ksim/world/world.hpp        World::load, stringID lookup, counts
include/ksim/core/simulation.hpp   fixed tick, detail systems, RNG, commands, hash
src/ksim/world/world.cpp            prefab/relationship construction, placement
src/ksim/core/simulation.cpp       scheduler, command queue, per-subsystem RNG
tools/ksim_stats.cpp          counts + load time + VmRSS
tools/ksim_run.cpp            new game / --load, date, squads per faction,
                              --save, --snapshot, --ticks
tests/ksim_world.cpp          reference overlay spot checks, lookup, placements
tests/ksim_commands.cpp       command queue is the only writer
tests/ksim_schedule.cpp       Full/Squad/Strategic tick boundaries
tests/ksim_determinism.cpp    same seed + commands -> identical hashes
tests/ksim_py/ksim_expected.py kmod.py overlay reference (run under -I)
```

## World model

- One flecs **prefab per live record** for FACTION, TOWN, SQUAD_TEMPLATE,
  CHARACTER, HUMAN_CHARACTER, ANIMAL_CHARACTER, BUILDING, RACE and
  AI_PACKAGE. `Source{stringID, name, type}` is set on every prefab and a
  `stringID -> prefab` map provides lookup. The shipped merge has no duplicate
  stringIDs: the nine per-type counts sum to the 2,965 prefabs.
- Components hold only schema fields useful to the simulation, with the
  schema's documented defaults when a field is absent. Floats are decoded with
  `kworld::decodeFloat`; missing scalar fields keep the component default.
- Relationships are flecs pairs. Membership is `MemberOf`, town ownership is
  `OwnedBy` (TOWN -> FACTION), and each list has its own relation type because
  flecs stores a pair value under the relationship's component. `ListEntry`
  keeps the record's `(v0, v1, v2)`; `AiPackageLink` keeps priority/v1/v2.
  Faction starting relations are `FactionRelation{value}` copied from
  `FACTION.relations` v0. An unlisted faction pair is left absent; callers fall
  back to `Faction::defaultRelation` as GAMEDATA_SCHEMA.md says.
- Modelled references: FACTION -> relations, residents, default resident, bar
  squads, squads, squad default, roaming squads; TOWN -> faction, residents,
  bar squads, roaming squads, default resident, override town; SQUAD_TEMPLATE
  -> faction, squad, squad2, leader, choosefrom list, animals, animals2,
  AI packages; CHARACTER/HUMAN_CHARACTER/ANIMAL_CHARACTER -> faction, race;
  AI_PACKAGE -> inherits from, Leads to.
- Target stringIDs with no prefab are skipped (for example references to
  record types this milestone does not build). Pair-count numbers therefore
  describe the built model, not raw references.
- Each `kworld::PlacedInstance` becomes one entity with Position, Rotation,
  Scale, PlacementCell and PlacedBuilding, starting at `DetailLevel::Strategic`.
  It gets `IsA` its BUILDING prefab when one exists; otherwise
  `MissingBuildingPrefab{buildingID}`. 14,614 placements, 14,600 inherit,
  14 (3 distinct stringIDs) are missing. Transforms and zone cells are copied
  verbatim from `kworld::Zones::place`.
- Detail level is a component **value** (`Detail::level`), never a tag, so a
  promotion/demotion does not change an entity's archetype. Prefabs are
  templates and carry no Detail; live instances start Strategic.

## Fields modelled / skipped

Skipped fields are exactly the remaining GAMEDATA_SCHEMA.md fields for that
record (presentation/art/audio/editor metadata, plus references not needed by
this milestone). Relation fields listed above are modelled and not repeated.

- **CHARACTER** (60 fields, 30 modelled). Skipped: armour level; body;
  diplomatic status; is law enforcer; is military; is peaceful; max inventory
  level; mesh; min inventory level; AI Goals; announcement dialogue; backpack;
  blueprints; bounty factions; clothing; color; crossbows; death items;
  dialogue; dialogue package; dialogue package player; inventory; personality;
  shopping; starting health; stats; unique replacement spawn; vendors; weapon
  level; weapons.
- **ANIMAL_CHARACTER** (37 fields, 26 modelled). Skipped: sounds; AI Goals;
  animations; backpack; bird attractor; death items; stats; strafe anim;
  turning anim; weapon; weapon level. HUMAN_CHARACTER has no live records; it
  uses the same Character component as CHARACTER.
- **RACE** (89 fields, 30 modelled). Skipped: attachment points; barefoot;
  blood horizontal; blood vertical; body mask female; body mask male; body
  texture female; body texture male; buy value; carry bone; combat move speed
  mult; description; editor limits; female carry ragdoll; female mesh; female
  ragdoll; filenames prefix; flayed normal female; flayed normal male; flayed
  texture female; flayed texture male; hull size X; hull size Y; hull size Z;
  male carry ragdoll; male mesh; male ragdoll; morph num; nm female; nm female
  skinny; nm female strong; nm male; nm male skinny; nm male strong; part map
  female; part map male; portrait distance; portrait offset x; portrait offset
  y; portrait offset z; sounds; stats bad0; stats good0; swim offset; swim
  speed; weather immunity0; AI Goals; ambient sound; backpacks; combat anatomy;
  fists lv1; fists lv100; hair colors; hairs; heads female; heads male; limb
  replacement; severed limbs; special food.
- **SQUAD_TEMPLATE** (58 fields, 35 modelled). Skipped: building public; buys
  illegal; buys stolen; gear artifacts base value; initial door state; item
  artifacts base value; sell mult; vendors fill rate; building; building
  dislike; dialog animal; dialog leader; dialog squad; housemates; nest;
  personality; prisoners; race override; slaves; special items; special map
  items; vendors; world state.
- **FACTION** (66 fields, 32 modelled). Skipped: armors 0..5; building cost
  mult; faces weirdness; negative rank0; rank; rank0; AI fallback; AI Goals;
  assaults; biomes; buildings replacements; campaigns; coexistence; color;
  dialog default; forbidden items; hairstyles; item spawns armoury; item spawns
  bar; item spawns HQ; item spawns resident; item spawns resident small; legal
  system; no-go zones; personality; races; slave clothing; special squads;
  trade culture.
- **TOWN** (29 fields, 22 modelled). Skipped: debris; debris building; loot
  spawn; material; trade culture; trade prices; world state.
- **BUILDING** (110 fields, 26 modelled). Skipped: allow animals; auto
  pathmesh; build speed; build time; building category!; building height;
  cavernous; ceiling placement; creates player town; Description; description;
  destroyed boundary; destroyed navmesh; distant mesh; door axis; door comes
  out; door move dist; door move speed; door navmesh axis; door type; enforce
  ceiling; function; GUI; hulls offset axis; hulls radius; icon; interior
  ambience; interior integrity; interior terrain; inventory content name; is
  door; is exterior furniture; is foliage; is interior furniture; is node; is
  sign; itemfunc limit; link length; link type; links together; lock level
  bonus; lock level mult; match slope; max part repeats; max slope; no
  collision; num pathmesh hulls; num turret repeats; output per day; production
  mult; scaling; select sound; sfx loop; snap to me; snap to me turrets; snap
  to me X spacing; snap to me Y; snap to me Z; sound x; sound y; sound z;
  stackable bonus mult; store anything; store armour; store weapons; unwalkable;
  vertical position tolerance; bird attractor; construction; doors; farm data;
  functionality; gun turret; interior; interior mask; limit inventory;
  material; parts; shares interiors with; snaps to; sounds; spawns; upgrades
  to; wall subsections.
- **AI_PACKAGE** (12 fields, 7 modelled). Skipped: contract end talk passive;
  Leader AI Goals; Slave AI Goals; Squad AI Goals; Squad2 AI Goals.

All other record types in the schema (STATS, PERSONALITY, ITEM, WEAPON,
ARMOUR, DIALOGUE, faction campaigns, etc.) are not prefabricated in this step.

## Simulation skeleton

- Fixed tick of 50 ms (20 Hz). Full detail runs every tick, Squad every 20
  ticks (1 sim second), Strategic every 2,182 ticks (one game-hour: the exe's
  1x game day is 2618.1819 s, clock.hpp). The bodies draw one RNG value and update counters; the
  scheduler, boundaries and accounting are real.
- One xorshift64* stream per detail subsystem, seeded by splitmix64 from the
  world seed, so systems cannot perturb each other's sequences. `ksim_run`
  prints the final state hash; `Simulation::stateHash()` is FNV-1a over tick,
  seed, all three RNG states, system counters, detail values/counts and faction
  relation values.
- The only external write path is `Simulation::enqueue(Command)`. `world()`
  returns `const World&`, so code outside `Simulation` cannot mutate the ECS
  (checked with a compile-time assertion in `ksim_commands`). Commands apply at
  the start of the next tick; dead targets, non-Detail targets, invalid levels
  and out-of-range faction values are dropped. Commands update values in place;
  they never add/remove detail tags.

## Results (clean build, 2026-10-30, Release)

```
ksim_stats: prefabs 2965 (FACTION 103, TOWN 346, SQUAD_TEMPLATE 959,
CHARACTER 639, HUMAN_CHARACTER 0, ANIMAL_CHARACTER 43, BUILDING 603,
RACE 53, AI_PACKAGE 219); placements 14614 (14600 IsA, 14 missing);
relationship pairs 224/617/325/1197/481/60/103/91/10/51/47/15/3/853/127/
569/319/169/23/402/1001/121/2/234;
load 1.60 s; VmRSS 112,236 KB
```

Relationship pairs are counted per (source, target) entry by walking each
prefab's type; `ecs_count_id` with a wildcard counts source entities instead
(60 factions instead of 224 relation entries), which was caught by the tests.
Reference overlay counts from `tests/ksim_py/ksim_expected.py` match every
number above, as do the schema's `Referenced by` counts (FACTION.relations
224, FACTION.residents 91, FACTION.default resident 10, TOWN.residents 1197,
SQUAD_TEMPLATE.squad 853, AI packages 1001, ...).

## Tests

All four tests run the real shipped data. `ksim_world` reruns the kmod.py
overlay at test time (python3 -I), compares all nine type counts and all 24
relationship counts, every TOWN owner (325), every faction starting relation
value (224), every SQUAD_TEMPLATE member/leader/choosefrom/animals pair with
its `(v0,v1,v2)` (2,060 pairs), the stringID lookup round trip and `Source` on
all 2,965 prefabs, then repeats `kworld::Zones::place` and matches all 14,614
entities as a multiset on layer/instance/building/transform/scale/cell, checks
`IsA` on 14,600 and `MissingBuildingPrefab` on 14. `ksim_commands` checks
enqueue-before-tick immutability, in-place detail/relation writes, creation of
an unlisted faction pair, and rejection of invalid commands. `ksim_schedule`
checks ticks 1, 19, 20, 200, 2,181, 2,182 and 4,364. `ksim_determinism` runs two
same-seed simulations with a command sequence and compares hashes at ticks
0/10/50/100, then checks a different seed diverges and that entity ids and
command targets are identical.

## UNSURE / spec gaps (`// UNSURE:` markers are listed here)

1. **Tick length and game-hour.** ARCHITECTURE.md gives Full every tick,
   Squad ~1 s, Strategic ~1 game-hour but no tick length or game:real ratio.
   50 ms and 3,600 sim-seconds per game-hour are this step's choices.
2. **Initial detail assignment.** The docs define the levels and rates but not
   which entities start where. Placed instances start Strategic; prefabs are
   templates and have no detail value. Promotion today only happens through a
   command.
3. **Which fields are "schema-used".** The selection above is a judgement
   call: simulation-relevant scalars and required pair relationships only;
   every skipped field is listed so the omission is explicit, not silent.
4. **Unlisted faction relations.** They are not materialised as pairs; the
   schema says callers fall back to `defaultRelation`, but the exact lookup
   order (default vs. explicit, symmetric application) is not specified here.
5. **Missing BUILDING prefabs.** The 14 placements/3 ids have no merged
   BUILDING record (kworld reports the same 14 as missing scale). They are
   kept as entities with `MissingBuildingPrefab` rather than dropped.
6. **Record types without prefabs.** References from the nine modelled types
   to other record types (dialog packages, items, stats, etc.) are skipped;
   their raw counts are therefore not represented in `WorldCounts`.
7. **Detail-level systems are global.** Each level has one scheduled system;
   per-entity routing by `Detail::level` is left for the AI layer. The
   `levelCounts_` array is maintained and hashed but not yet used by a system.

## Runtime state, first slice (2026-10-30)

- **Template vs instance.** `ksim::tmpl` (prefabs, rebuilt from game data)
  and `ksim::state` (instances, saved). Every instance has a `state::Id`
  (singleton `Ids` hands them out); saves, the state hash and snapshots key
  on it, never on flecs ids. References inside components are `EntityRef`.
- **Instances:** `FactionState` (+ `Relation` pairs from `relations`),
  `TownState` (root, active variant, owner), `SquadInstance` (template,
  faction, home town, `SpawnSource`), `CharacterInstance` (own entity,
  `MemberOf` squad, Exclusive), `Position{x,z}` on squads only for now.
- **Clock** singleton = absolute tick since day 0 00:00; `Simulation::tick`
  advances it. New game starts Day 1 10:00 (tick 74,182).
- **Spawner** (`spawnDefaultWorld`): ports tools/world_model.py's default
  world. Seed 1: 103 factions, 224 relations, 169 towns, 840 squads (town
  resident 486, bar 112, default resident 50, town roaming 17, biome homeless
  161, nest 13, faction roaming 1; 665 positioned), 5,709 characters.
  `ksim_spawn` matches towns (active variant, owner, position), squads per
  source and per faction against docs/reference/world_model/data/*.tsv.
- **Save v0 + hash:** docs/formats/RUNTIME_SAVE.md. `ksim_save` round-trips
  the default world (308 KB) with an identical hash and re-saved bytes.
- **Snapshot:** `ksim_run --snapshot` writes `{clock, squads[...]}`
  (build/default_world_squads.json, 417 KB).

UNSURE (marked in code): start time Day 1 10:00 is inferred from save
`towntime`, not the exe; world state `player involvement` ignored; town-list
squads spawn regardless of their own `world state` (as world_model.py; 0 fail
today); missing override-town variant skipped; member counts follow fcs.def
text (no exe trace, no size multiplier); `choosefrom list` weighted by v0
with replacement, 3 SQUAD_TEMPLATE targets skipped; housemates/slaves/
prisoners not spawned; wilderness squads have no Position (needs the biome
map join).

Next slice (done, next section): merge `Simulation` and `RegionEngine` tick
drivers; save the RNG streams; one tick counter; `SetFactionRelation` writes
`state::Relation`, not the template.

## One tick driver (runtime state, second slice, 2026-10-30)

- **`Simulation` owns the `RegionEngine`** (unique_ptr). The engine holds no
  flecs pointer; the world is passed per call, so `Simulation` stays movable.
  Tick order: Clock++ -> due commands (enqueue order) -> detail systems whose
  interval divides the **absolute Clock tick** (phase survives a load) ->
  region engine phases 1/2 + boundary mirror -> scheduler mirror.
  `tickCount()` is gone; `clockTick()` is the only counter.
- **One command type** (`include/ksim/core/command.hpp`): plain data naming
  instances by `state::Id`, tick-stamped (`enqueue` stamps
  `max(tick, clock + 1)`), saved in the save trailer. Types:
  `SetFactionRelation` (FactionState Ids -> `state::Relation` pair; template
  `tmpl::FactionRelation` untouched), `SetPlayerAreas`, `SetSquadCentre`.
  `RegionCommand` and `SetDetailLevel` are gone: Detail is derived from player
  areas (placed buildings keep their static Strategic `Detail`).
- **Region entities = instances with `Position`** (squads today; characters
  still have none). Engine order, tie-breaks, targets and messages key on
  `state::Id`. Engine arrays are canonical; `Position` + new `state::Mover`
  (order, stand-in group/target/hits/steps, rng) are the one-way mirror,
  diff-written at the boundary. Singletons `Scheduler`, `StandInGroups`,
  `PlayerAreas` mirror the rest. Saves and the hash read the mirror;
  `Simulation(World)` adopts a loaded world (`RegionEngine::rebuild`, Id order).
- **RNG:** one stream per subsystem from the world seed (`rng.hpp`): streams
  0-2 detail systems, 4 = per region entity (`streamSeed(streamSeed(seed, 4),
  Id)`), 5 = per stand-in group (by index). All states are saved.
- **Stand-in workload is opt-in** (`RegionConfig::standIn`, default off): in a
  real game squads hold still until real AI exists. Tests turn it on.
- **Hash:** `Simulation::stateHash()` = `ksim::stateHash(ecs)`; the region
  index is not hashed, so the old `contentHash` split is gone.
- **Continuation tests:** `ksim_regions_determinism` (600 stand-in movers,
  save at 40, restore at 1/8/2 workers, == uninterrupted 100 ticks, re-save
  byte-identical) and `ksim_continue` (real new game, 665 movers, 30 ticks
  across strategic tick 74,188 + squad ticks, save with a command queued,
  load at 8 and at 1 worker, 40 more ticks == uninterrupted). A mutation
  (perturb the loaded Mover rng) is caught by both.
- Cost: the per-boundary mirror write shows in `ksim_regions_perf` (20k
  entities): boundary ~1.0 ms (N=1) / 0.8 ms (N=8) of 4.3 / 1.6 ms per tick.

## ksim review (Claude, 2026-10-30)

Checked against ARCHITECTURE.md; no kdata/kworld files were changed; paths already use KENSHI_DATA / KWORLD_REF_TOOLS.

Good: `World::ecs()` is const and `ecsMutable()` is Simulation-only (friend), so the command queue is the only external writer. `Detail::level` is a component value; prefabs are built once per record and instances use `IsA` (no per-instance prefabs); no queries are created, so nothing leaks. Per-subsystem RNG streams are split via splitmix64. Faction relations use `FACTION.relations` v0 and `default relation` fallback, matching GAMEDATA_SCHEMA.md. Tests rerun kmod.py for expected counts rather than hard-coding.

Noted, not fixed:
1. The three "systems" are a plain loop in `Simulation::tick`, not flecs systems/pipeline phases. Fine for the stub, but the AI layers should move to flecs systems (or a queryed per-entity dispatch) and route by `Detail::level`; today they touch no entities, and `levelCounts_` is unused by them.
2. Determinism test: the state hash mixes the seed, so "different seed diverges" is trivially true. It does hash real state (tick, RNG, Detail values, relation values), but positions/placements are not hashed (static for now); extend when they move. No test yet that a command changes the hash.
3. Commands are fixed-schema (two types). Expect to grow into a variant/queue per layer; applied only at tick start, which is correct.
4. Every placed instance starts Strategic and only a command promotes it; no promotion policy (camera/proximity) exists yet.
5. `extraRelations_` is a side list to make command-created relation pairs hashable; a flecs query over `FactionRelation` pairs would be cleaner.
6. flecs is built with `-O2` regardless of build type, and `enable_language(C)` is added at file scope.

# ksim regions (region-owned two-phase tick)

Implemented in `include/ksim/world/region.hpp` and `src/ksim/world/region.cpp`; tests are
`tests/ksim_regions_*.cpp`. `kdata` and `kworld` are unchanged.

## Design

- **Superseded in part by "One tick driver" above:** `Simulation` now owns
  the engine and drives its tick; the mirror is `Position`, `state::Mover`,
  `Region`, `Detail` and the `StandInGroups` singleton; `contentHash` and the
  engine's own `stateHash` are gone. The engine's `prev_` arrays are the
  source of truth during a tick.
- **Region is a component value.** `Region{index}` is a plain value component,
  set from the entity's position. Ownership changes only at the tick boundary,
  through a `Migrate` message applied after phase 2.
- **Two-phase tick.** `prev_`/`next_` are the frozen previous state and the
  output state. Phase 1 work units read only `prev_` (own region plus any
  region, for border perception and target lookup) and emit per-entity updates
  and messages; phase 2 applies updates and messages only to entities the unit's
  region owned in `prev_`. A write to an entity owned by another region is
  counted in `RegionStats::writeViolations` and skipped.
- **Determinism.** Work distribution uses a `std::atomic<size_t>` next-task
  counter but results never depend on it: each region writes only its own
  `UnitOutput` slots, each slot is written once, and messages are gathered and
  `std::sort`ed at the boundary by `(dest, type rank, target id, sender id,
  toRegion, x bits, z bits)`. Type ranks are Hit, Order, Migrate, so migration
  is applied last. Per-entity and per-squad RNG streams are splitmix64-seeded
  xorshift64*, advanced only when the entity actually wanders (or the squad
  steps), so thread scheduling cannot consume RNG.
- **Detail schedule.** Full-detail regions tick every tick. Squad-level regions
  tick every `squadIntervalTicks` (default 20 = 1 s at 20 Hz); the squad group
  step is a separate work unit keyed by squad id and runs on the same interval,
  emitting one Order per member to the member's frozen owner. Squads span
  regions; members are found through `squadMembers_`, rebuilt at the boundary.
- **Player areas.** Each centre maps to the square of regions within
  `areaRadiusRegions` (Chebyshev); overlapping region sets are merged
  repeatedly into one `Area` with a sorted region list. Only these regions are
  full detail.
- **Perception and border strips.** Targets are entity IDs, never pointers.
  Phase 1 scans the region grid cells that intersect the perception square
  (with a full scan beyond 64 cells); the frozen previous state is readable
  from any thread, ownership restricts writes only. A target is kept while
  within `keepRadius`, otherwise the nearest entity within `perceiveRadius` is
  locked. Hits are emitted to the region that owned the target at tick start
  and applied exactly once.
- **Reference comparisons** use `contentHash()`, which excludes the owning
  region index and the area/axis data, because a single-region reference and a
  multi-region run necessarily differ there. `stateHash()` includes everything.
- `tests/ksim_determinism.cpp` was changed for two reasons: (1) the earlier
  "ksim review" noted that no test proved a *command* changes the hash, so a
  fourth same-seed run with a different faction-relation value now asserts
  identity at checkpoints 0/1 and divergence at 2/3; (2) this tree copy needs
  `-DKDATA_KMOD=...` when configuring the parity
  test in the isolated job tree (the default resolved to a nonexistent path). The existing seed checks
  are unchanged.

## Tests (clean Release build, 25/25 pass)

All eight region tests pass at N=1, 2, 4, 8 workers (pool always ≤ 8):

- `ksim_regions_determinism` — 600 entities; state hashes at ticks 0/10/50/100
  equal for N=1/2/4/8; same-seed repeat equal; 9,216-unit regions N=1 vs N=8
  equal; a command at tick 5 diverges after checkpoint 0.
- `ksim_regions_ownership` — 300 entities, 8 workers, 50 ticks: 15,876 writes,
  0 violations, 87 entities migrated, hit messages all delivered, every owner
  matches `regionOf(x,z)`, flecs `Region`/`Position` match the engine, region
  membership sums to the entity count.
- `ksim_regions_migration` — 200 entities, 120 ticks, forced crossings: 1,307
  region crossings, up to 10 regions visited, membership sum constant, IDs
  unique.
- `ksim_regions_perception` — regioned (4,608, N=8) vs single-region reference:
  same target IDs, archer locks the cross-border neighbour, `contentHash` equal
  at ticks 1 and 50.
- `ksim_regions_areas` — overlapping player areas merge (12 regions, 2 areas);
  inside entity steps every tick, outside entity steps only at tick 20.
- `ksim_regions_target` — lock persists across many borders (4 regions), exactly
  one hit each way per tick, 100 hits, all delivered to the tick-start owner.
- `ksim_regions_squads` — 9 members spread over 9 regions: 0/1/2 group steps at
  ticks 19/20/40, 18 orders, every member sees the squad centre, identical
  IDs and `contentHash` to a single-region reference.
- `ksim_regions_perf` — 20,000 entities, 10 timed ticks after 2 warmup:
  N=1 ≈ 1.94–2.00 ms/tick; N=8 ≈ 0.86–0.88 ms/tick (Release, this machine).
  Hashes equal, 0 violations.

ThreadSanitizer (separate `build-tsan`, `-fsanitize=thread
-fno-omit-frame-pointer -g`, region targets only): all eight tests pass, zero
TSan warnings, exits 0. Under TSan the perf test reports ≈ 14.0 ms/tick at N=1
and ≈ 22.1 ms/tick at N=8 (instrumentation dominates).

## UNSURE items

1. **Message order and ties.** The key is stable, but entities that compare
   equal on every field keep the order of the atomic task counter; in the
   stand-in workload the tuple is unique per message, so this cannot occur
   today. A future message type with genuinely duplicate keys needs an explicit
   total-order tie-break (for example a sequence number assigned by the sender).
2. **One-tick hit delay.** Hits are applied in phase 2 of the same tick, so the
   target's hit count is first visible in the next tick. I read ARCHITECTURE's
   "one tick of delay" this way; if it means the message must survive until the
   next boundary, the application point (not the ordering) would move.
3. **Area merging granularity.** Merging is by any shared region, as specified;
   two player squares that only touch at a corner therefore merge. Whether
   "overlapping" means shared area or 4-neighbour adjacency is not stated.
4. **Squad centre as input.** The `SetSquadCentre` command teleports the centre
   for tests; real gameplay would replace it with goals/orders. No command is
   provided for perception radii or detail overrides.
5. **Non-full regions and border content.** Non-scheduled regions still apply
   incoming messages every tick, but their entities only move on their
   squad-interval tick. That matches the "~1 s" table but means a target can be
   hit in a region that did not recompute its own perception that tick.
6. **Flecs mirroring cost.** Syncing `Position`/`Region`/`Detail` for all
   20,000 entities at every boundary is a measurable part of the N=1 time;
   a dirty-set or a flecs-side archetype move would be cheaper, but would add
   another source of truth, so the mirror is deliberately simple.
7. **Global world bounds.** `clampWorld` uses the world extent to keep
   positions in range and to map them to regions; entities spawned outside are
   clamped at the first tick. The extent is a fixed 294,912 units in this
   stand-in, matching the reference single-region size.

## regions review

Reviewed by Claude after integration. Local: 25/25 pass (Release); TSan (separate
`cpp/build-tsan`, `-DCMAKE_CXX_FLAGS=-fsanitize=thread`, region targets) 8/8, no warnings.

Checked and fine: phase 1 reads only `prev_`, `regionMembers_`, `slotOf_` (all
const during the tick) and writes only its own `UnitOutput` / `nextSquads_[squad]`;
phase 2 touches only slots whose `prev_` owner is the unit's region; messages are
sorted by a total key (dest, type, id, sender, toRegion, x, z) before delivery;
`hash()` covers id, position, region, detail, squad, target, hits, steps, rng,
orders and squad state; targets are entity IDs; squad steps are keyed by squad ID.
Added: `RegionStats` per-segment timings and `maxActiveWorkers`; the ownership and
perf tests now fail if the 8-worker run never used more than one thread.

Noted, not fixed:
- **Ownership audit is a check, not an instrument.** `writeViolations` compares
  `prev_[slot].region` with the unit's region, the same condition that routed the
  message, so it cannot catch a stray write elsewhere (e.g. `next_[other]`). TSan
  catches cross-thread races only. A real test would route every write through a
  checked accessor (debug build) or poison unowned slots and compare.
- **Squad work units are claimed dynamically** via the atomic task counter, not
  mapped to a worker by squad ID. Results are thread-independent, so determinism
  holds; revisit if per-worker affinity (cache locality) matters.
- **Weak interaction in several tests.** `determinism` (600 entities) and `perf`
  (20k over 4096 regions, ~5 per region, radius 300) produce almost no hits or
  targets, so they mostly hash wandering. Interaction is covered by the
  target/perception/squad tests, but the perf number excludes dense-combat cost
  (message gather/sort would grow). Add a clustered perf scenario.
- **Serial mirroring.** `next_ = prev_` copy, message gather/sort, `rebuildMembership`
  and `syncEcsBoundary` (flecs lookup per entity) are single-threaded.
- `nearestTarget` duplicates `regionOf`'s axis clamp; message tie-breaks rely on
  uniqueness (see UNSURE 1).

### Why only 2.3x at 8 threads (20k entities, ms/tick)

| segment | N=1 | N=8 |
|---|---|---|
| prepare (copy, task lists) | 0.07 | 0.10 |
| phase 1 (parallel) | 1.49 | 0.32 (4.6x) |
| gather + sort | 0.07 | 0.07 |
| phase 2 (parallel) | 0.24 | 0.16 |
| boundary (membership + flecs mirror) | 0.25 | 0.27 |

Phase 1 scales well; the serial prepare/sort/boundary segments (~0.45 ms) plus
phase 2, whose per-region tasks are tiny against two barrier hand-offs, cap the
total (Amdahl). Message sorting is minor here only because the workload has few
messages; the flecs mirror and state copy dominate the serial part. Test size
is also a factor: 1 ms of work does not amortise barriers.
