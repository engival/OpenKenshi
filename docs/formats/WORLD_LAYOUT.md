# World layout — `data/newland/leveldata/`

Zone and level files are ordinary GameData containers (see
[MOD_FORMAT.md](MOD_FORMAT.md)); this doc covers what they contain and how they
link together. Coordinates: see [Coordinate system](../ARCHITECTURE.md#coordinate-system).

Survey/validator: `tools/world_survey.py <Kenshi install dir>` (uses `tools/kmod.py`).

## Layers

The world is a merge of per-mod layers. `kenshi_x64.exe` contains
`./data/newland/leveldata/` next to the strings `Newwworld` and `rebirth`;
FCS merges `leveldata.level`, `*.zone` and `interiors.level` from a mod's
leveldata folder (`MergeDialog.cs`). Each layer pairs with a definition file
`data/<layer>.mod`.

| Folder | `.zone` | Other files |
|---|---:|---|
| `leveldata/` (base) | 1 (`zone.31.45`) | `interiors.level` |
| `Newwworld/` | 329 | `leveldata.level`, `interiors.level`; stale copies `interiors_old.level`, `leveldata - Copy.level`, `leveldata - Copy (2).level` |
| `Dialogue/` | 28 | `leveldata.level`, `interiors.level` |
| `rebirth/` | 314 | `leveldata.level`, `interiors.level` |

672 zone files cover 500 distinct cells (X 2..62, Y 0..62); 167 cells exist in
more than one layer (159 in both Newwworld and rebirth). Same-cell files
overlay by instanceID; an instance with an empty target is a delete. Layer
precedence (presumably base → Newwworld → Dialogue → rebirth, later wins) is
**not verified**.

Elsewhere: `data/leveldata.level` (20-entry townlist, no states),
`data/newland/leveldata.level` (empty townlist), `data/roads.level` (empty v13),
`data/leveldata/` (28 legacy zones). Treat these as legacy.

## Zone files — `zone.X.Y.zone`

`X` = x cell, `Y` = z cell: `X = floor((x + 147456) / 4608)`,
`Y = floor((z + 147456) / 4608)`. All 14,614 building placements lie in the
cell named by their file (0 exceptions).

| Version | Files | Notes |
|---|---:|---|
| 15 | 643 | no header; trailing block present |
| 16 | 29 (all `rebirth/`) | header (`version=1`, empty strings); no trailing block |

### Records

Only four types occur:

| Type | Count | Role |
|---|---:|---|
| `INSTANCE_COLLECTION` sid `0-buildinglist` | 650 (≤ 1 per file; 22 files are empty) | holds **every** instance in the zone |
| `GAMESTATE_BUILDING` | 14,637 | per-placement state (owner faction, squads, layouts, …); no instances |
| `INVENTORY_STATE` | 1,361 | empty records |
| `INVENTORY_ITEM_STATE` | 26 | item state fields |

### Instances (in `0-buildinglist`)

Instance layout (v15+): `str id, str target, f32 x y z, f32 qw qx qy qz, i32 n, str state[n]`.

| Kind | Count | Target | Transform | States |
|---|---:|---|---|---|
| Building placement | 14,614 | BUILDING record sid, e.g. `1234-Newwworld.mod` | world position + unit quaternion | `<id>-S23` (a local GAMESTATE_BUILDING); 1,361 also have a local INVENTORY_STATE |
| Inventory row | 1,361 | local INVENTORY_STATE / INVENTORY_ITEM_STATE | all zero, q = (1,0,0,0) | none |
| Delete (tombstone) | 19,603 | `""` | identity | usually none |

- 14,600 placement targets resolve to BUILDING records in `gamedata.base` +
  `Newwworld.mod` + `Dialogue.mod` + `rebirth.mod`; 14 point at missing
  `97576/97577/97578-Newwworld.mod`.
- Placement heights: p1/p50/p99 = -150 / 6 / 908 (one outlier at -92,726). They are **offsets above the
  terrain**, not absolute (see `COLLISION_FORMAT.md` §4).
- Tombstones need not match an id in the same cell: 8 rebirth cells
  (X 5–7, Y 4–6) each carry the same 980 deletes of `…-zone23.22-INGAME` ids
  that exist nowhere else installed. Several of these are byte-identical
  copies of each other (as are Newwworld `zone.7/8/9.18` and
  `zone.11.27`/`zone.13.28`). Unmatched deletes are no-ops.

### Trailing block

After the last record, v15 zone and `leveldata.level` files carry
`i32 n` + `n` ascending, unique `i32` (exactly `4 + 4n` bytes; `n = 0` for empty
zones). FCS does not read it; `kmod.py` reports it as `leftover` and does not
write it back. Zones: up to 39,637 values, `{1..n}` exactly in 472/621
non-empty blocks; contains every local GAMESTATE_BUILDING `handleI` in 571/573
files. Meaning **unknown** (an ID-allocator set is a guess).

## `leveldata.level` — towns and roads

| File | Records | GAMESTATE_TOWN | ROAD | Townlist entries (with state) |
|---|---:|---:|---:|---:|
| `Newwworld/` | 2,494 | 382 | 2,109 | 78 (78) |
| `Dialogue/` | 326 | 324 | 0 | 19 (19) |
| `rebirth/` | 593 | 590 | 0 | 286 (286) |

### Towns

- `GAMESTATE_TOWN_INSTANCE_LIST` sid `0-townlist`: one instance per town.
  `id = <n>-<layer>-INGAME`, target = TOWN record sid, `x y z` = world position,
  the four trailing floats are 0 (not a quaternion), state = `<id>-S5e`.
- The `-S5e` GAMESTATE_TOWN record holds ints `zones_count`,
  `zone_x_<k>`, `zone_y_<k>` (cells the town covers) and string `instance`
  (back-link to the townlist id). A plain (non-`-S5e`) twin with the same zone
  list also exists.
- Check: 274/274 towns with `zones_count > 0` sit inside one of their declared
  cells; 109 declare no cells.
- `rebirth/` repeats Newwworld's 78 townlist entries (cumulative snapshot).

### Roads (`Newwworld/leveldata.level` only)

| Record | Content |
|---|---|
| `ROAD` (2,109) | bool `hidden`; floats `weight`, `width`, `length`; ints `start`, `end`. Instances = polyline points (20,944 total): target `"-"`, transform `(x, y, z, 1.0, t, 0, 0)` with `t ∈ [0,1]` along the road. Not a quaternion. |
| `INSTANCE_COLLECTION` `4-roadlist` | 2,109 entries, `id == target ==` ROAD sid, zero transform |
| `INSTANCE_COLLECTION` `5-roadfoliage` | 2,666 points `(x, y, z, u, 0, 0, 0)`, target `"-"`; `u` unknown (rebirth has 59) |

`start`/`end` (15..4,217) match a ROAD id in ~44 % of cases; 13 roads have
`start == end`. Meaning **unknown** (junction ids?).

## `interiors.level` — building layouts

| File | Records | `INSTANCE_COLLECTION` (layouts) |
|---|---:|---:|
| `leveldata/` (base) | 17,280 | 301 |
| `Newwworld/` | 4,815 | 45 |
| `Dialogue/` | 7,903 | 53 |
| `rebirth/` | 14,014 | 91 |

Each collection is one layout (sid like `727-gamedata.base-Noble House`,
`EXT-…` for exteriors). Its string `associated building` resolves to a
BUILDING for 490/490. Instances are furniture/props in building-local
coordinates with a GAMESTATE_BUILDING state; the remaining records are those
states plus `ITEM_PLACEMENT_GROUP` / inventory states. A zone placement links
to its layout via its GAMESTATE_BUILDING strings `interior layout name` /
`exterior layout name` (join key `<target>-<name>` or `EXT-<target>-<name>`;
not re-verified here).

**Placement item nodes** (survey 2026-11-04 over the four layers):
- 8,692 layout instances in 219 layouts have target = an `ITEM_PLACEMENT_GROUP` def (type 83), not a BUILDING. Each carries
  one state record, also type 83, with string `base data sid` (= the def), int `floornum`, and sometimes `isInsideBuilding*`.
  1,085 of them override `min/max respawn time` (always 24 / 48).
- 21 defs (gamedata.base, Newwworld, rebirth). Each has refs `items`, `clothing`, `weapons` or `weapon manufacturers`, with v0 a
  weight (5..1000; v1 = v2 = 0), and ints `min/max respawn time` (24/48; Carpet 72/72; absent on some).
- Most used: Food 2,260 (cups, bowls, jugs, drinks), General Items 1,891, General Materials 936, Food Ingridients 833,
  Old Library 511, booze 482.
- Semantics (kinspect `findings/itemgroup_static.md`, ~/git/kenshi/kenshi-inspect 6be21b1): one weighted pick per spawn from the pooled
  lists, chance v0 / sum (0 counts as 100, negative drops); at most one item per node; when the item is gone the node waits
  randint(min, max) game hours (the def's; the node override is UNSURE, not read), then puts out one new pick. Owner: the building's
  owning squad, else nobody. Only armour takes a quality grade; a weapon is kept only when a group `weapon manufacturers` entry makes it (none: no weapons),
  then a weighted maker and model pick (ours: CombatData::rollQuality over the group makers, UNSURE).
- Ours: `state::ItemNode` (interact.cpp spawnMeshItemNodes / Simulation::stepItemNodes, hourly), spawned when the zone's navmesh is
  furnished (the original: when the interior is first shown). Test `ksim_item_nodes`.

## Unknown

- Trailing block meaning; `ROAD.start/end`; `roadfoliage` 4th float.
- Layer merge precedence.
- ~~GAMESTATE_BUILDING `world Y pos` vs placement `y`~~ Resolved: placement `y` is relative to the
  terrain; `world Y pos` ≈ bilinear terrain(x, z) + y (see `COLLISION_FORMAT.md` §4).
- Why 29 rebirth zones are v16.
- Spawn areas / nests: no `NEST` records in base or the three layer mods;
  `./data/newland/leveldata/platoon/` (referenced by the exe) is not shipped.
