# Kenshi `data/` asset inventory

Census of every file type under the game's `data/` directory, with format,
reader and relevance to a headless simulation. Drafted by a DeepSeek worker
(`~ds/work/out/assets/`, scripts `census.py`, `heads.py`, `imgcensus.py`,
`gamedata_*.py`, `build_families.py`), reviewed and corrected 2026-10-30
against `~ds/git/kenshi/kenshi-game/data` (reviewer scratch: `~ds/work/review/schema/`).

Status legend in the Notes column: **verified** = re-measured by the reviewer;
unmarked = from the draft, spot-checked or plausible but not re-measured.
Exe function addresses are from the Ghidra export (`~ds/git/kenshi/kenshi-decomp-v2/funcs`);
the ones marked † were opened and contain the quoted literal.

Record paths in the game data are written `.\data\...` (relative to the
install root, one level above `data/`).

**Totals (verified): 16,217 files, 12,130,256,175 bytes (~12.1 GB).**

| Top-level dir | Files | Bytes |
|---|---:|---:|
| newland | 7,693 | 6,868,050,644 |
| buildings | 2,627 | 1,871,179,415 |
| items | 1,369 | 1,124,485,799 |
| character | 612 | 748,792,818 |
| audio | 24 | 405,814,339 |
| foliage | 199 | 322,699,314 |
| animal | 128 | 268,391,561 |
| gui | 120 | 115,406,448 |
| particles | 644 | 112,353,249 |
| land | 290 | 85,406,566 |
| materials | 238 | 68,666,065 |
| icons | 1,934 | 64,068,693 |
| meshes | 250 | 49,456,213 |
| (root of `data/`) | 20 | 24,132,302 |
| leveldata | 30 | 1,074,172 |
| ragdoll | 27 | 223,094 |
| editor | 12 | 55,483 |

Extension counts below are case-insensitive (`.XML` and `.xml` together).
Sizes in decimal MB.

## Inventory

Relevance: **needed** = the simulation must read it; *later* = needed for a
later milestone or as reference; render / audio / UI / editor = not needed
headless; unknown = purpose not established.

| Type | Count | MB | Example | Format | Read by | Relevance | Notes |
|---|---:|---:|---|---|---|---|---|
| `.base` | 1 | 4.65 | `gamedata.base` | GameData v16, 9,399 records | FCS `GameData.cs`; `tools/kmod.py` | **needed** | verified. `MOD_FORMAT.md` |
| `.mod` | 3 | 17.94 | `Dialogue.mod` | GameData: `Dialogue.mod` v17, `rebirth.mod` v17, `Newwworld.mod` v16 | FCS `GameData.cs`; `kmod.py` | **needed** | verified. Content layers on top of `gamedata.base` |
| `.zone` | 701 | 27.33 | `newland/leveldata/zone.31.45.zone` | GameData v15 667 · v16 29 · v13 3 · v10 2 | exe FUN_1407784d0† (`"zone."`, `".zone"`); `kmod.py` | **needed** | verified versions. Per-zone placements; `WORLD_LAYOUT.md` |
| `.level` | 14 | 30.65 | `newland/leveldata/interiors.level` | GameData v15 13 · v13 1 | exe FUN_140777ce0† (level-editor save path, `leveldata.level`) | **needed** | verified. 10 under `newland/leveldata/{,Dialogue,Newwworld,rebirth}/` (incl. 2 `leveldata - Copy*.level` and `interiors_old.level`), 1 `newland/leveldata.level`, 1 `leveldata/shader_ball.mod/leveldata.level`, 2 at the root (`leveldata.level` 1,511 B, `roads.level` 12 B). `WORLD_LAYOUT.md` |
| `.tif` | 2 | 537.2 | `newland/land/fullmap.tif` | TIFF: `fullmap.tif` 16,385² u16 grey; `land/overlay.tif` 256² RGB8 | exe FUN_140815b40† (`"fullmap"` → Ogre Terrain) | **needed** (height) | Height = raw·9800/65535 (`TERRAIN_FORMAT.md`) |
| `.dat` | 3 | 1.51 | `newland/land/features.dat` | magics `MF01` features, `KBI1` blendinfo, `FF02` fogfeatures | FUN_14039b610†, FUN_140a13750†, FUN_14086db80† | **needed** (`features.dat`); others render | verified magics. All three specified in `TERRAIN_FORMAT.md` |
| `.def` | 2 | 0.07 | `newland/land/navtiles/seeds.def` | f32×3 seed points, no header | FUN_1403c5220† (`./data/newland/land/navtiles/seeds.def`) | *later* (navmesh reference) | Second copy at `land/navtiles/seeds.def`. `TERRAIN_FORMAT.md` |
| `.hkt` | 3,995 | 527.0 | `newland/land/navtiles/tile50.48.hkt` | Havok packfile (`1e0db0ca cefa11d0`), `hk_2014.2.0-r1`, `hkaiNavMesh` | Havok AI; version string FUN_140bbe120†; dir FUN_1403a6370† | *later* (reference only; we build our own navmesh) | verified: all 3,995 in `newland/land/navtiles/`, same magic |
| `.path` | 1 | 1.23 | `globalPathing.path` | portal graph | no reader found in exe or FCS | unknown | `GLOBAL_PATHING_FORMAT.md` |
| `.phs` | 86 | 0.76 | `ragdoll/weapons6A.phs` | proprietary ragdoll / attachment / physics definition, no magic | via record fields (`phs or mesh`, `male ragdoll`, …); FUN_140551660† reads `"phs or mesh"` | *later* (ragdoll, attachment points) | See [.phs](#phs). First f32 is 1.4 in 85 files, 1.1 in 1 |
| `.xml` physics (`<NXUSTREAM2>`) | 1,136 | 48.6 | `buildings/furniture/barrel.xml` | PhysX 2.8 NxuStream XML collection | NxuStream code in exe (FUN_1402437e0†) | *later* (static collision) | verified split: newland 563, buildings 526, items 42, foliage 4, character 1; 74 named `.XML`. Collision is covered in a separate doc |
| `.bin` | 892 | 9.58 | `buildings/furniture/engcrate08.bin` | NxuStream **binary**: `NXUSTREAM\0` + 1 byte + u32 284 + u32 103 | exe has a matching binary reader (FUN_1402437e0†); whether the game opens these files is not established | *later* / unknown | verified magic on all 892. See [.bin](#bin). Collision is covered in a separate doc |
| `.raw` | 256 | 33.82 | `land/grasssplits/fullmap.20.30.raw` | headerless 257×257 u16 LE (132,098 B) | no reader found | unknown | verified size on all 256. See [.raw](#raw) |
| `.bod2` | 87 | 0.15 | `character/bodies/Griffin.bod2` | proprietary body definition; u32 version 15 (74), 13 (9), 16 (4) | extension literal FUN_141539b10†; loader not found | *later* (appearance) | verified versions. 74 in `character/bodies/`, 13 in `character/bodies/export/` |
| `.body` | 16 | 0.01 | `character/bodies/Zed.body` | older body definition; u32 version 10 (8), 9 (3), 13 (2), 8 (2), 5 (1) | not found | *later* (appearance) | verified versions |
| `.cfg` | 9 | 0.01 | `mods.cfg` | text | engine / FCS / Ogre | root `mods.cfg`: user config; `character/skins/*/{male,female}_editor.cfg` (6): `Key=Value` body-editor limits; 2 Ogre/Caelum cfgs | `mods.cfg` here lists the user's Steam-workshop mods, none of which are in this copy |
| `.list` | 1 | 0.001 | `__mods.list` | text mod list | launcher / FCS | user config | same caveat as `mods.cfg` |
| `.info` | 1 | 0.0003 | `_rebirth.info` | XML `<ModData>` | FCS / Steam workshop | editor | |
| `.txt` | 8 | 0.01 | `tips.txt` | text | game UI | UI | `namesM/F/MF.txt`, `factionNames*.txt` look like name lists (reader not traced) |
| `.dds` | 2,001 | 8,062.6 | `buildings/stones.dds` | DXT5 1,206 · DXT1 713 · DXT3 47 · DX10 header 34 (all DXGI 72 = BC1_UNORM_SRGB) · uncompressed 16-bpp 1 | Ogre | render | verified counts. Draft labelled DXGI 72 as B8G8R8X8; it is BC1 sRGB |
| `.mesh` | 3,396 | 1,404.6 | `buildings/typeB.mesh` | Ogre `MeshSerializer_v1.100` 1,859 · v1.8 1,460 · v1.41 77 | Ogre | render (bounds possibly *later*) | verified counts |
| `.skeleton` | 365 | 38.9 | `character/hair/meshes/hair06.skeleton` | Ogre `Serializer_v1.80` 264 · v1.10 100 · 1 zero-byte | Ogre | render / animation | verified |
| `.png` (`icons/`) | 1,934 | 64.1 | `icons/1012-gamedata.base.686-gamedata.base.png` | PNG | game UI | UI cache | Named `<stringID>.<stringID>.png` (two record ids). Includes icons for mods absent from this copy (`limbs.mod`, `Universal Wasteland Expansion.mod`, …), so at least part is generated on the user's machine; draft's "editor-only" was wrong |
| `.png` (other) | 585 | 712.6 | `land/overlaymaps/biomemap.png` | PNG | Ogre / UI | render (biome map: see `TERRAIN_FORMAT.md`) | verified split: items 231, particles 156, newland 143, materials 27, land 11, foliage 5, buildings 3, character 4, gui 4, meshes 1 |
| `.tga` | 11 | 23.6 | `land/areasmap.tga` | TGA, 24-bpp | Ogre; `ROAD.imagefile` | render | includes `land/areasmap.tga`, `land/nogoAreas.tga` (under `land/`, not `newland/`) |
| `.bnk` | 24 | 405.8 | `audio/SFX_Bank.bnk` | Wwise soundbank (`BKHD`); header version 112, `SFX_Bank.bnk` 88 | Wwise; `Init.bnk` literal FUN_140258b70† | audio | |
| `.fbx` | 7 | 166.0 | `buildings/furniture/brokendrone.fbx` | FBX binary | none | editor (source art) | |
| `.max` | 1 | 3.85 | `newland/Assets/Plants/WaterPlant_A.max` | 3ds Max OLE file | none | editor | |
| `.pxproj` | 12 | 0.01 | `buildings/furniture/brokendrone.PxProj` | 3ds Max PhysX exporter project XML | none | editor | |
| `.dll`, `.exe` | 1 + 1 | 2.31 | `newland/land/overlaymaps/DevIL.dll` | PE32+ | — | editor (DevIL + `joiner.exe` tools) | |
| `.ico` | 3 | 0.30 | `kenshi.ico` | Windows icon | — | UI | missing from the draft |
| `.ttf` | 5 | 1.40 | `gui/fonts/Sentencia_1.ttf` | TrueType | MyGUI | UI | |
| `.layout` | 53 | 0.24 | `gui/layout/item_box_v.layout` | MyGUI layout XML | MyGUI | UI | |
| `.xml` GUI / editor | 11 + 11 | 0.2 | `gui/kenshi_theme.xml`, `editor/editor_data_human.xml` | `<MyGUI>` / `<Editor>` | MyGUI / FCS character editor | UI / editor | `RACE.editor limits` points at the `editor_data_*.xml` files |
| `.xsd` | 1 | 0.003 | `editor/editor_data.xsd` | XML schema | FCS | editor | |
| `.material`, `.os`, `.program`, `.compositor` | 260 + 2 + 17 + 10 | 0.3 | `buildings/walls.material` | Ogre scripts | Ogre | render | 1 zero-byte `materials/deferred/water.material` |
| shader sources (`.hlsl` 56, `.frag` 13, `.vert` 12, `.glsl` 5, `.fragment` 5, `.vertex` 5, `.h` 1) | 97 | 0.4 | `materials/common/constants.hlsl` | shader source | Ogre | render | |
| `.cache` | 1 | 1.01 | `materials/shaders.cache` | blob with `DXBC` containers | Ogre / D3D (inferred) | render | |
| `.pu` | 186 | 1.14 | `particles/scripts/Street_Torch_Flames.pu` | ParticleUniverse script | ParticleUniverse | render | |
| `.zip` | 2 | 0.19 | `meshes/OgreCore.zip` | ZIP | Ogre | render | |
| `.jpg` | 1 | 0.10 | `gui/gfx/physSplash.jpg` | JPEG | UI | UI | |
| `.uvw` | 1 | 0.005 | `buildings/materials/1.uvw` | starts `ffffffff 04000000 2c000000`, then floats | not found | unknown | |
| (no extension) | 4 | 0.005 | `particles/scripts/Ash-Flakes_CameraCover` | 2 ParticleUniverse + 2 Ogre material scripts | as `.pu` / `.material` | render | |

Zero-byte files (verified): `materials/deferred/water.material`,
`items/armour/meshes/trousers_m.mesh.skeleton`.

## Special cases

### .bin

- All 892 files start `NXUSTREAM\0`, one byte, then u32 284 and u32 103.
  702 have a same-stem `.xml` twin (case-insensitive); 190 do not.
- **Draft error (corrected):** the draft said the exe rejects these files
  ("This old version of NxuStream is no longer supported", FUN_140245210).
  That check is in a lookup that maps XML element names to the NxuStream
  class enum (it continues into a table of `SC_*` names, FUN_140213750), not
  in the file loader. The binary loader FUN_1402437e0 (mode `"rb"`) reads 10
  bytes and compares them with `NXUSTREAM\0`, reads one endian byte, then
  requires SDK version 0x11c (284) and format 0x67 (103). The shipped `.bin`
  files match exactly. The same function writes this header on save.
- So the exe *can* read these files. Whether the game ever opens a `.bin`
  (rather than the `.xml` twin) is not established: no record field points at
  a `.bin`, and the callers of FUN_1402437e0 (FUN_1401dd080/1dd1f0/1dd4f0)
  were not traced.
- 22 of 86 `.phs` files contain paths to `.bin` files under `…\cooked\`
  directories that do not exist in this copy.

### .raw

256 files `land/grasssplits/fullmap.X.Y.raw`, X 10..25, Y 16..31 (complete
16×16 grid), each 257×257 u16 with no header. They are under `land/`, not
`newland/`. No `.raw` or `grasssplits` literal was found in
the exe. Meaning unknown; the draft's weak correlation with `fullmap.tif`
suggests it is not a heightmap. Probably irrelevant to the simulation.

### .bod2 / .body

Both only in `character/bodies/` (`.bod2` also in `bodies/export/`). First u32
is a version (see table). `Griffin.bod2` (v15) contains GameData-style strings
(`default`, `1--INGAME`); `Zed.body` (v10) contains slider names
(`sex female`, …). Layout beyond the version is unknown. Referenced by
`CHARACTER.body` (`gamedata.base`: 11 `.body`, 4 `.bod2`, 119 empty, verified).
FCS's asset filter only lists `.bod2`.

### .phs

86 files: `ragdoll/` 27, `buildings/` 26, `animal/` 15, `items/` 12,
`character/` 3, `foliage/` 2, `meshes/` 1 (verified). No magic; first f32 is
1.4 in 85 files (draft said 1.1 for props; `buildings/stone.phs` is 1.4).
Contents include bone names and mesh / `cooked\*.bin` paths. Referenced by
`RACE` (`male/female ragdoll`, `male/female carry ragdoll`,
`attachment points`), `BUILDING_PART.phs or mesh`, and the `physics file`
fields of ARMOUR / ITEM / WEAPON. The full layout and the exe's parser are not located.

Attachment points (read off `ragdoll/attachments-weapons.phs` by kenshi-0f, 2026-10-31; the RACE field is set in rebirth.mod / Newwworld.mod,
empty in gamedata.base). Each point is a u32-length-prefixed name (`hip`, `back`, `back2`), a few ints, then the length-prefixed bone name
(`Bip01 Pelvis`, `Bip01 Spine2`). After it come the bone's bind transform in skeleton (model) space (vec3 position, then a 3x3 rotation stored
by columns; it matches `male_skeleton.skeleton`), one byte, and the item's bind model transform (3x3 by columns, then vec3). Next is a block of
`ffffff69` sentinels and zeros, then the length-prefixed editor preview mesh (`katana04F.mesh` at the hip, `katana04R.mesh` on the back) and a vec3
scale of 1. The meaning of the ints and bytes is unknown. Client reader: `Characters::readAttachPoints` (`client/src/characters.cpp`). The exe
adds a built-in `hands` point = `Bip01 Prop2` with no offset (FUN_1405373e0).

## How records reference asset files

Asset paths are in the record **file** section (`MOD_FORMAT.md`), Windows
style, `.\data\…`.

| File | non-empty file values | resolve (case-sensitive) | resolve (case-insensitive) |
|---|---:|---:|---:|
| `gamedata.base` | 4,009 | 3,791 | 3,798 |
| `Newwworld.mod` | 5,353 | 5,156 | 5,168 |
| `Dialogue.mod` | 415 | 414 | 414 |
| `rebirth.mod` | 1,605 | 1,589 | 1,589 |

`gamedata.base` by extension (verified): `.dds` 1,749 · `.mesh` 1,554 ·
`.xml` 540 · `.phs` 83 · `.png` 63 · `.body` 11 · `.bod2` 4 · `.tga` 3 ·
`.skeleton` 2. No record references a `.bin`, `.raw`, `.hkt` or `.dat`.

**Draft error (corrected):** the draft said the ~218 unresolved paths belong to
uninstalled mods. They are base-game paths missing from this copy, e.g.
`buildings\misc\MarketStall_HangingCloth05.mesh`,
`character\skins\hu\heads\human_male_head_normal_HI.dds`,
`items\armour\samurai01\Samurai boots.mesh`; a few `rebirth.mod` values are
not paths at all (`coloring`). A loader must treat a missing asset as
non-fatal. (Asset paths are irrelevant headless, except `.phs`/collision
later.)

Fields that hold paths, by record type (from the draft's
`gamedata_resolve.py`, spot-checked):

| Record type | Fields | Ext |
|---|---|---|
| `BUILDING_PART` | `phs or mesh`, `xml collision` | `.mesh`/`.phs`, `.xml` |
| `BUILDING` | `distant mesh`, `icon`, `destroyed boundary` | `.mesh`, `.png` |
| `FOLIAGE_MESH` | `mesh`, `texture map`, `normal map`, `collision`, `leaves *`, `* map 2` | `.mesh`, `.dds`, `.xml` |
| `MATERIAL_SPEC`, `MATERIAL_SPECS_CLOTHING` | `texture map`, `normal map`, `metalness map`, `color map` | `.dds` |
| `CHARACTER` | `body`, `mesh` | `.bod2`/`.body`, `.mesh` |
| `RACE` | `male/female mesh`, `male/female ragdoll`, `male/female carry ragdoll`, `attachment points`, `body texture *`, `body mask *`, `nm *`, `part map *`, `editor limits`, `flayed *` | `.mesh`, `.phs`, `.dds`, `.xml` |
| `ARMOUR`, `WEAPON`, `ITEM` | `mesh`, `mesh female`, `ground mesh`, `icon`, `physics file`, … | `.mesh`, `.phs`, `.png`, `.dds`, `.xml` |
| `ATTACHMENT` | `mesh`, `mesh female`, `texture map`, `head texture` | `.mesh`, `.dds` |
| `BIOMES`, `GRASS` | texture layers, `water normal`, `color map`, `grass sprite` | `.dds`, `.png` |
| `ROAD` | `imagefile` | `.tga` |
| `ANIMATION_FILE` | `male animation`, `female animation` | `.skeleton` |

Verified examples: `BUILDING_PART` "hpg-mid-windows-2" (`1104-gamedata.base`):
`phs or mesh` = `.\data\buildings\techtowers\hpg-mid-windows.mesh`, no
`xml collision`. `ARMOUR` "Halfpants [sneaky chain]" (`1395-gamedata.base`):
`mesh` / `mesh female` = `halfpants_m.mesh` / `halfpants_f.mesh`. `RACE`
"Garru" (`3998-gamedata.base`): male and female ragdoll =
`.\data\animal\ragdoll\pack_beast.phs`. `CHARACTER` "Wanderer"
(`136-gamedata.quack`, defined in `gamedata.base`): `body` =
`.\data\character\bodies\default.bod2`.

## Unknown / unverified

- `.raw`: meaning; no reader found.
- `.bod2` / `.body`: layout after the version word; loader.
- `.phs`: layout; parser; how `phs or mesh` dispatches `.mesh` vs `.phs`.
- `.bin`: whether the game loads them (the exe has a compatible reader).
- `.uvw`, `.path`: no reader found.
- `.cache`: assumed to be Ogre's shader cache.
- Wwise bank header version → Wwise release.
