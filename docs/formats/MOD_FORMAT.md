# Kenshi game-data record format (`.base`, `.mod`, `.save`)

Ground truth: the FCS editor's reader/writer, decompiled in
`reference/fcs/forgotten_construction_set/GameData.cs`:

| What | Method |
|------|--------|
| file-level read | `GameData.load(string filename, ModMode, …)` |
| file-level write | `GameData.save(string filename)` (always writes version 17) |
| header | `readHeader` / `writeHeader` / `loadHeader` |
| one record | `Item.load(BinaryReader, …)` / `Item.save(BinaryWriter, uint)` |
| strings | `readString` / `writeString` |

Everything below was checked against four shipped files (see [Verification](#verification)).
`.save` uses the same reader in FCS but no `.save` was checked here.

## Primitives

All little-endian. No alignment, no padding, no checksums, no trailing data.

| Name | Encoding |
|------|----------|
| `i32` / `u32` | 4 bytes |
| `u8` | 1 byte |
| `f32` | IEEE-754 single |
| `bool` | 1 byte; written as 0/1 (`ReadBoolean` treats any non-zero as true) |
| `str` | `i32 byteLen` + `byteLen` bytes of **UTF-8** (no terminator, no BOM). `byteLen <= 0` reads as `""`. Length counts bytes, not chars; non-ASCII text does occur (90 strings in `Dialogue.mod`). |

FCS quirk: `readString` uses a shared 4096-byte buffer that it doubles **once**
when too small, so a string longer than 8192 bytes crashes FCS. Longest
observed string: 1534 bytes.

## File layout

```
i32  fileVersion                 1..17; anything else = "Invalid mod file version"
     header                      only if fileVersion is 16 or 17
i32  lastID                      editor's next-numeric-ID counter
i32  recordCount
     record[recordCount]
```

### Version branches

| fileVersion | Effect |
|---|---|
| 1–6 | no instance `states` list |
| 1–7 | reference entries are 8 opaque bytes (`ReadInt64`, discarded); instance has no `ref` string |
| 1–6 | record has no `stringID`; synthesised as `"{id}-{filename}"` |
| 7+ | record `stringID` string present |
| 8+ | reference entry = `str targetID` + `i32 v0` (…); instance `ref` string present |
| 1–8 | no `vec3` / `vec4` sections (present when `> 8`) |
| 8–9 | reference entry carries only `v0` (no `v1`, `v2`) |
| 10+ | reference entry carries `v0 v1 v2` |
| 11–14 | record has a "tag" table (see below) instead of the flags word |
| <14 | FCS discards all field data of `CONSTANTS` (type 27) records |
| 1–14 | instance ID and state IDs are `i32`, synthesised as `"{n}-{filename}"` and `"{n}-{filename}-INGAME"` |
| 15+ | record flags word `u32`; instance/state IDs are strings |
| 16 | header present, **no size prefix**, ends after `referenced` |
| 17 | header has a `u32` size prefix and optional tail sections |

### Header (v16, v17)

```
[v17 only] u32 headerSize        bytes following this field, up to end of header
i32  version                     mod's own version number (1 in all observed files)
str  author
str  description
str  dependencies                comma-separated filenames ("" = none)
str  referenced                  comma-separated filenames ("" = none)
--- v17 only, present iff bytes remain before headerSize end ---
u32  saveCounter                 incremented on every FCS save
u32  lastMergeResolve
u8   mergedCount
     { str modName; u32 item1 /* unused by loader */; u32 merge } [mergedCount]
--- v17 only, present iff bytes remain ---
u8   deleteCount
     { str modName; u32 version; str items /* ':'-separated stringIDs */ } [deleteCount]
```

The reader seeks to `headerEnd` afterwards, so unknown extra header bytes are
skipped. Each optional block is guarded by `position < headerEnd`, so older v17
writers that stop early are valid (`Dialogue.mod`: header ends after
`mergedCount`, no delete block, `headerSize = 67`). While reading merged
entries the reader peeks the next `i32` and stops if it is `> 256` (a guard,
not a field). `writeHeader` drops from `referenced` anything also listed in
`dependencies`.

### Record

```
i32  size        byte length of the whole record INCLUDING this field; reader ignores it.
                 FCS's save() back-patches it; older writers leave 0.
i32  type        itemType (table below)
i32  id          numeric ID; 0 in every observed record (identity is the stringID)
str  name        display name
str  stringID    [v7+] primary key, conventionally "{n}-{originFile}", e.g. "997-gamedata.base"
--- v15+:
u32  flags       see "Record flags"
--- v11..v14:
i32  tagCount
     { str key; bool keep } [tagCount]   ignored for gamedata.base
---
i32 n;  { str key; bool  v              } [n]    booleans
i32 n;  { str key; f32   v              } [n]    floats
i32 n;  { str key; i32   v              } [n]    ints (also enums, colours, EnumValue)
[v9+]  i32 n; { str key; f32 x,y,z      } [n]    vec3
[v9+]  i32 n; { str key; f32 x,y,z,w    } [n]    vec4 / quaternion  (x,y,z,w order)
i32 n;  { str key; str   v              } [n]    strings
i32 n;  { str key; str   path           } [n]    filenames (FCS `File` type)
i32 nCat;                                        references
     { str category; i32 nRef;
       { str targetStringID; i32 v0; i32 v1; i32 v2 } [nRef] } [nCat]
i32 nInst;                                       instances
     { str  instanceID           (i32 before v15)
       str  targetStringID       (absent before v8; "" = instance deleted)
       f32  x, y, z              position
       f32  qw, qx, qy, qz       rotation -- note w FIRST, unlike vec4 fields
       i32  nStates;  str stateID[nStates]   (absent before v7; i32 IDs before v15)
     } [nInst]
```

Notes:

- **Key ordering.** `modData` is a `SortedList<string,object>` with the default
  (culture-sensitive, case-insensitive-first) .NET comparer, so each section's
  keys come out in that order; reference categories likewise. Verified: every
  list in all four files is in case-insensitive order, and 0.1–1.5% of lists
  are *not* in ordinal (byte) order. A writer should use the
  same collation if it wants byte-identical output; a reader must not assume
  ordinal order.
- **Ints** section is built from `modData` values that are `int`, any C# enum,
  `EnumValue` (`.Value`), or `Color` (`ToArgb() & 0xFFFFFF` — alpha dropped).
- **Strings vs filenames** are encoded identically; only the section tells them
  apart. On load a string value does not overwrite a key already holding a
  non-string.
- **Reference values** `v0,v1,v2`: meaning is per category and not fixed by
  the format (`(v1,v2)` is most often `(0,0)` or `(100,0)`; all three slots carry
  real values in some categories, including negatives). `v0=v1=v2=0x7FFFFFFF` (`Reference.Removed`) means *this mod
  removes the reference*. `save()` writes live (modified) refs of a category
  first, then removed ones.
- **Instances** are placed sub-objects (observed: building nodes such as
  `usenode`, `Dummy01`, light nodes); `targetStringID` is the item placed.
  Empty `targetStringID` = instance deleted.
- **Deleted record**: a `bool REMOVED = true` in the bools section. FCS injects
  it on save when the record is in state `REMOVED` and strips it again on load.

## Record flags (v15+)

```
bits 0-3   ItemLoadFlags:  1 = MODIFIED, 2 = RENAMED
bits 4-31  saveCounter     header.saveCounter at the time this record last changed
```

- `MODIFIED` = the record changes an item defined by an earlier file (base or
  dependency). **Clear** = the record *defines* (creates) the item.
- `RENAMED` = this file changes the item's name. Without it, if the item
  already has a name, the record's `name` is ignored.
- On load: if bit 0 is clear, bit 1 is cleared (`RENAMED` only counts on a
  modifying record).
- Observed low nibbles: `0` (new), `1` (modified), `3` (modified + renamed).
- Older (v16-era) writers set bit 31: `0x80000002` new, `0x80000001`
  modified, `0x80000003` modified+renamed. In FCS terms that is
  `saveCounter = 0x08000000`; `applySaveVersion` clamps it down on the next save.
  The `0x80000002` "new" value is just bit 1 set without bit 0, i.e. no flags.

Errors FCS raises from these flags: `MODIFIED`/`RENAMED` on an item that doesn't
exist yet → state `MISSING` ("ModifiedItemNotFound"); no `MODIFIED` on an item
that already exists → "ItemAlreadyDefined".

### Tag table (v11–v14 only)

Per-record `{key → keep}` filter. When present (and the file isn't
`gamedata.base`), a field is applied only if its key is in the table with
`true`; a reference to `X` is applied only if `"-ref-X"` is present, and is
treated as a removal if that entry is `false`.

## How the flags combine across files (load semantics)

FCS loads `gamedata.base` (+ other `.base`) with `ModMode.BASE`, dependency
mods `LOCKED`, the edited mod `ACTIVE` (`SINGLE` = one file, standalone). Each
file is an **overlay**: a record for an existing `stringID` merges its fields
key-by-key over what is there; references merge by `(category, targetID)`;
instances by instance ID. Records are only written for items with local
changes, and on load in `ACTIVE` mode any value equal to the base value is
dropped — a mod stores only diffs.

Editor state flags (`GameData.State`, not stored on disk, derived from the
above): `ORIGINAL 0, OWNED 1, MODIFIED 2, LOCKED 4, REMOVED 8, MISSING 0x10,
INVALID 0x20`. `ModMode`: `BASE 0, ACTIVE 1, LOCKED 2, SINGLE 3`.

## itemType

Implicit sequential C# enum (`itemType.cs`):

| # | name | # | name | # | name | # | name |
|--:|---|--:|---|--:|---|--:|---|
| 0 | `BUILDING` | 29 | `BUILDING_PART` | 58 | `MEDICAL_PART_STATE` | 87 | `CHARACTER_PHYSICS_ATTACHMENT` |
| 1 | `CHARACTER` | 30 | `INSTANCE_COLLECTION` | 59 | `FOLIAGE_LAYER` | 88 | `LIGHT` |
| 2 | `WEAPON` | 31 | `DIALOG_ACTION` | 60 | `FOLIAGE_MESH` | 89 | `HEAD` |
| 3 | `ARMOUR` | 32 | `TEMPORARY_INFO` | 61 | `GRASS` | 90 | `BLUEPRINT` |
| 4 | `ITEM` | 33 | `MOD_FILENAME` | 62 | `BUILDING_FUNCTIONALITY` | 91 | `SHOP_TRADER_CLASS` |
| 5 | `ANIMAL_ANIMATION` | 34 | `PLATOON` | 63 | `DAY_SCHEDULE` | 92 | `FOLIAGE_BUILDING` |
| 6 | `ATTACHMENT` | 35 | `GAMESTATE_BUILDING` | 64 | `NEW_GAME_STARTOFF` | 93 | `FACTION_CAMPAIGN` |
| 7 | `RACE` | 36 | `GAMESTATE_CHARACTER` | 65 | `GAMESTATE_CRAFTING` | 94 | `GAMESTATE_TOWN` |
| 8 | `LOCATION` | 37 | `GAMESTATE_FACTION` | 66 | `CHARACTER_APPEARANCE` | 95 | `BIOME_GROUP` |
| 9 | `WAR_SAVESTATE` | 38 | `GAMESTATE_TOWN_INSTANCE_LIST` | 67 | `GAMESTATE_AI` | 96 | `EFFECT_FOG_VOLUME` |
| 10 | `FACTION` | 39 | `STATE` | 68 | `WILDLIFE_BIRDS` | 97 | `FARM_DATA` |
| 11 | `NULL_ITEM` | 40 | `SAVED_STATE` | 69 | `MAP_FEATURES` | 98 | `FARM_PART` |
| 12 | `ZONE_MAP` | 41 | `INVENTORY_STATE` | 70 | `DIPLOMATIC_ASSAULTS` | 99 | `ENVIRONMENT_RESOURCES` |
| 13 | `TOWN` | 42 | `INVENTORY_ITEM_STATE` | 71 | `SINGLE_DIPLOMATIC_ASSAULT` | 100 | `RACE_GROUP` |
| 14 | `WORLDMAP_CHARACTER` | 43 | `REPEATABLE_BUILDING_PART_SLOT` | 72 | `AI_PACKAGE` | 101 | `ARTIFACTS` |
| 15 | `CHARACTER_APPEARANCE_OLD` | 44 | `MATERIAL_SPEC` | 73 | `DIALOGUE_PACKAGE` | 102 | `MAP_ITEM` |
| 16 | `LOCATIONAL_DAMAGE` | 45 | `MATERIAL_SPECS_COLLECTION` | 74 | `GUN_DATA` | 103 | `BUILDINGS_SWAP` |
| 17 | `COMBAT_TECHNIQUE` | 46 | `CONTAINER` | 75 | `HUMAN_CHARACTER` | 104 | `ITEMS_CULTURE` |
| 18 | `DIALOGUE` | 47 | `MATERIAL_SPECS_CLOTHING` | 76 | `ANIMAL_CHARACTER` | 105 | `ANIMATION_EVENT` |
| 19 | `DIALOGUE_LINE` | 48 | `GAMESTATE_BUILDING_INTERIOR` | 77 | `UNIQUE_SQUAD_TEMPLATE` | 106 | `TUTORIAL` |
| 20 | `TECHTREE` | 49 | `VENDOR_LIST` | 78 | `FACTION_TEMPLATE` | 107 | `CROSSBOW` |
| 21 | `RESEARCH` | 50 | `MATERIAL_SPECS_WEAPON` | 79 | `AI_SCHEDULE` | 108 | `TERRAIN_DECALS` |
| 22 | `AI_TASK` | 51 | `WEAPON_MANUFACTURER` | 80 | `WEATHER` | 109 | `AMBIENT_SOUND` |
| 23 | `AI_STATE` | 52 | `SQUAD_TEMPLATE` | 81 | `SEASON` | 110 | `WORLD_EVENT_STATE` |
| 24 | `ANIMATION` | 53 | `ROAD` | 82 | `EFFECT` | 111 | `LIMB_REPLACEMENT` |
| 25 | `STATS` | 54 | `LOCATION_NODE` | 83 | `ITEM_PLACEMENT_GROUP` | 112 | `ANIMATION_FILE` |
| 26 | `PERSONALITY` | 55 | `COLOR_DATA` | 84 | `WORD_SWAPS` | 113 | `____XXX___` |
| 27 | `CONSTANTS` | 56 | `CAMERA` | 85 | `NEST` | 114 | `OBJECT_TYPE_MAX` |
| 28 | `BIOMES` | 57 | `MEDICAL_STATE` | 86 | `NEST_ITEM` | | |

## Verification

Throwaway parser (scratch, not kept in repo) implementing exactly the layout above,
run on the shipped files, 2026-10-29:

| File | ver | header | lastID | records | leftover bytes | record `size` field |
|---|---|---|---|---|---|---|
| `gamedata.base` | 16 | refs `Newwworld.mod` | 5007294 | 9 399 | 0 | all 0 |
| `Newwworld.mod` | 16 | deps `gamedata.base` | 5007294 | 2 511 | 0 | all 0 |
| `Dialogue.mod` | 17 | size 67, saveCounter 1, no delete block | 5007294 | 39 077 | 0 | all 0 |
| `rebirth.mod` | 17 | size 70, saveCounter 9, full tail | 5007294 | 8 571 | 0 | all == actual length |

Top record types: `gamedata.base` DIALOGUE_LINE 3080, DIALOG_ACTION 2609,
BUILDING_PART 831, BUILDING 367, DIALOGUE 351 (66 types). `Dialogue.mod`
DIALOGUE_LINE 18677, DIALOG_ACTION 17455, DIALOGUE 1563 (26 types).
`rebirth.mod` DIALOGUE_LINE 2128, DIALOG_ACTION 1160, SQUAD_TEMPLATE 831,
CHARACTER 551 (63 types). `Newwworld.mod` FOLIAGE_MESH 645, BUILDING_PART 520,
MAP_FEATURES 304 (41 types).

Other observations: no file uses `vec3`/`vec4` sections (counts always 0) or
instance `states`; no removed (`0x7FFFFFFF`) references; instances only in
`gamedata.base` (656), `Newwworld.mod` (827), `rebirth.mod` (416);
`REMOVED` bools in the three mods (26 / 88 / 104 records).

**Round-trip.** Re-serialising the parsed model in file order reproduces all
four files **byte-for-byte** — the format has no slack the parser misses.
FCS's own `save()` would *not* reproduce them, because it:

- always writes version 17 with the size-prefixed header and full tail, and
  increments `saveCounter`;
- back-patches the record `size` field (0 in three of the files);
- recomputes the flags word as `ItemLoadFlags | saveCounter<<4` (rewrites the
  `0x8000000x` values);
- emits records in `Dictionary<string,Item>` iteration order of the *merged*
  item set (roughly load order, with removed slots reused), only for items with
  local changes, not in the source file's order;
- emits only diffs vs. the base (values equal to the base are dropped on
  `ACTIVE` load), normalises `Color` ints (`& 0xFFFFFF`), re-sorts keys by the
  culture comparer, and writes removed references after live ones;
- drops `referenced` entries that also appear in `dependencies`.

A file loaded in `BASE` mode lands in `data`, which `save()` never writes, so
`gamedata.base` cannot be re-saved by FCS at all (only via `SINGLE` mode).

## Errors in the community doc

`<Kenshi>/KENSHI_MOD_FORMAT.md` is not reliable. Main errors:

- "Type 15 = `.save`/`gamedata.base`, Type 16 = `.mod`": the leading int is a
  format version (1–17), not a file type. `gamedata.base` is v16; current
  FCS writes v17. The v17 header size prefix and saveCounter/merge/delete tail
  are missing entirely.
- "`UnknownFlag`" before the record count is `lastID`; it is present in every
  version, not just "type 16".
- The first record int is called `InstanceCount`; it is the record's byte size
  (or 0) and is ignored by the reader.
- `ModDataType` values (`-2147483646` etc.) are presented as an enum; really a
  bitfield (`MODIFIED=1`, `RENAMED=2`) plus a 28-bit save counter. The quoted
  values only happen to match old v16 writers that set bit 31.
- Fields are described as bare values. Each is actually a counted list of
  `(key, value)` pairs in a fixed section order. The doc omits references
  (category lists with three ints each) completely.
- Quaternion order is given as W,X,Y,Z for 4D vector fields; those are X,Y,Z,W.
  Only instance rotations are W-first.
- Instances: IDs are strings (v15+) and include a target-item string and a
  states list. They are not "`[L: InstanceID]` + transform".
- Strings are described as 1-byte chars. They are UTF-8, and the length is a
  byte count.
- "Trailing memory dump data" in type-15 files: there is nothing in the
  reader for this, and all four files end exactly after the last record.
