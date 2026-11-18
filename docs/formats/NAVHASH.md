# Navtile hashes (zone, interior) and the stale-tile check

The original's navmesh tiles (`data/newland/land/navtiles/tileI.J.hkt`, `knav::native`) carry a checksum per zone and per
interior. Its loader recomputes them and re-bakes a tile whose stored hash differs (`knav/native.hpp` "StoredHashes";
`kworld/navhash.hpp`; tool `knav_navhash`). Our runtime loader does the same (`kworld/navhash_source.hpp`,
`MeshNavigator::openNative`; NAVMESH_CACHE.md "The stale-tile check in the loader"). The exe-side decode is kinspect's
private findings/navhash.md (sections 1-4, 10 and 11); this file records our implementation and what it reproduces.

## Recipe

All values are 32-bit.

- `comb(s, v) = s ^ (v + 0x9e3779b9 + (s << 6) + (s >> 2))`.
- A float enters as `F(f) = (uint)(int64)f ^ 0xdeadbeef` (truncate toward zero, after a 2^32 subtract-wrap).
- **Building**: `comb F(x)`, `F(z)`, four orientation words, then `comb(s, 1)` when destroyed **and** a wall gate.
  The orientation words come from the game's rotation scratch buffer; every shipped one truncates to 0
  (navhash.md section 7: about 16,000 values over six towns), so `F(0)` is folded four times.
- **Interior**: the building hash, then each child's building hash in the gather order, then `comb(s, 0x100)` when
  destroyed (not the gate term).
- **Zone**: the included objects' building hashes in the zone's object order, then `comb(s, forest word)` when the zone
  has a tree layer (any non-grass FOLIAGE_LAYER of its biomes); the term is folded even when no instance survives and the
  word is 0. The forest word is below.

**Vegetation** (section 11 of the private reference, kinspect d07e64d, DECODED + VERIFIED-live): `comb` from 0 over the
zone's tree-loader instances in spawn order, each that passes the mesh gate, as `F(x)` then `F(height)` — the world
**height (Vector3 y), not z**. Instances are the raw scatter positions (the spawner's Vector3, before the loader's u16 page
encoding). The fold covers every tree-loader instance — trees, boulders, canyon rocks, bushes — and never grass (the
GrassLoader path does not call the spawner). The gate keeps an instance when its FOLIAGE_MESH has a `navmesh cutter` > 0
or a collision file (see the mapping below). Our `kworld::vegetationWord` reads `FoliageInstance::navFold`, set by
`generateFoliage` from those two record fields.

Positions are the placement's x and z **as f32** (the engine's stored type): the hash sees the truncated int64 of the
float, not of the double.

## Mapping the exe filters onto our records

Matched against the capture's fold, not guessed; the mappings are UNSURE where one observation cannot separate the
candidates.

| exe test | our record | evidence |
|---|---|---|
| destroyed && gate (building term) | GSB `destroyed` and BUILDING `is wall gate` | the Hub's only two destroyed gates (4400, 4617) get it; the other 19 destroyed placements do not |
| interior's destroyed term | GSB `destroyed` | the Hub's 19 zero-child interiors equal `comb(building, 0x100)` |
| object filter (five tests) | BUILDING `path mode` == 0 excludes | the one Hub placement not in the fold is the Mounted Crossbow mkII, the only Hub building with `path mode` 0 |
| child set | layout items whose record is a BUILDING and not `is node` | the Bar's live 49 children = the 48 interior BUILDING items + its exterior "Bar Sign" item |
| vegetation-fold gate (`EntData+0x40 > 0 \|\| EntData+0x80 != 0`) | FOLIAGE_MESH `navmesh cutter` > 0 **or** a non-empty `collision` file | the Hub's live fold covers every record with one of the two and no record without either; the `collision` part carries the rock stacks, iron rocks, pyrite and tree falls whose cutter is 0 |

Interiors pair with placements by the saved uid `(handleI + 1) << 16 | handleC` (the Info string's hex id). Three
shipped uids carry extra high bits; a low-24-bit retry pairs them (the Hub's `408e0815` is the 19th match).

## What reproduces

- Zone 52,37 (Asp Gulch, single biome, no grass gate): 1567 folded instances, vegetation word `0xcb3d3cdc`, with the
  zone's building fold `0x5a5e4b92` the stored zone hash `0x4dc1666b`. Exact after the height query was made f32: the
  bilinear now runs on the exe's f32 index `trunc((p - zoneMin) / 36)` and f32 fraction, not a double ratio rounded
  once. A double ratio differs in the last bits; at one Canyonland_FatTop01 instance it put y at 1472.999512 instead
  of 1473.0, and the fold's trunc saw it. The instance stream and the gate were already exact (our 1567 folded x
  equal the live capture's fold-call x in order).
- The Hub (`20,32`): 19 of 21 stored interior hashes (all zero-child ones; the Bar and Residential Small stored values
  are the stale pair navhash.md section 6 describes). The two child-bearing live hashes equal the parent hash plus the
  captured child hashes in the captured order: `8f673f92` (Bar, 49 children) and `49dc10f3` (Residential Small, 13).
  Our child *set* matches the capture exactly (Bar 49; Residential Small 13 of our 15 candidates - two carpet items are
  in the layout but were not gathered), and so does every child's own hash.
- The Hub's zone hash: the live building fold `0xfeeabfec` is reproduced from our 104 placements in the captured object
  order, with `comb(0xfeeabfec, 0x91949d55 veg word) = 0xd4dc7de5` (the live zone hash).
- The stored zone hash `c9ebf91d` is from a different world state (stale), not from our data.
- Zone 10,10's vegetation word `0x4b059683` is reproduced from our generated scatter alone: 48 instances, one
  FOLIAGE_MESH (`47951-rebirth.mod`, roborock.mesh), the gate passes all 48, and the zone has no buildings, so the full
  zone hash is `comb(0, 0x4b059683) = 0xe93d103c` — exactly the stored (and live) value. The ctest asserts this.
- The forest term is per tree layer, not per surviving instance: all 949 zones that our scatter gives no instance and
  that have no zone file match the stored zone hash as `comb(0, 0)` (with "instances exist" they all read as stale;
  the world scan went from 2014 stale zones to 1059). Of the 7 instance-less zones with a zone file, 6 match as
  `comb(fold, 0)` and one is stale.
- Zone 10,11 (two biomes) reproduces the live word `0xde107fea` (full zone hash `0x7c47f9a3`, the stored one) once the
  biome list is in the game's order: the zone tile keeps its biomes **widest ground first**, and the scatter runs each
  biome's layers at that position. The map's first-seen order is the reverse here ([Cinder Ridge, Red Coast] against the
  live [Red Coast, Cinder Ridge]); the Hub's [Swampfeeder Canyon, Canyonland FlatTop] agrees with the same rule. The mesh
  gate folds 179 of the zone's 655 instances, and our 179 folded (trunc x, trunc y) pairs are exactly the live ones
  (kinspect navhash section 14). UNVERIFIED: the game builds the list per 9216-u biome-map cell (up to 5 slots, a
  per-tile mask); with two live lists, per-tile pixel count is the rule that fits both and the cell's own counts fit
  neither.

## Object order

The zone object list follows the merged zone-file instance order (layers in order, a same-id override replaced in
place) minus the excluded objects. Objects re-created by a save's state are appended after the data pass: at the Hub
the live save re-created three torches (24501, 24503, 5339) and the captured fold has them last. Our data-derived
order therefore gives the Hub's building fold `0x982a5c29`; with the save's appended objects it gives `0xfeeabfec`.

## Gaps (UNSURE)

- **Child gather order.** The order is not the layout order (the live capture's is a near-permutation of it for
  Residential Small and heavily permuted for the Bar). It looks like the iteration order of the original's child
  container over object handles, which depends on handle allocation, so we cannot reproduce it from data yet. Interior
  hashes with children therefore do not match stored tiles; the tool reports their building hash and child set.
- **The Hub's vegetation word.** Exact: the scatter folds `0x91949d55`, the live word, and all 1090 live instances
  match ours in order (same mesh, position within 0.5 u; the gate keeps 1090 of our 2710 attempts). The last scatter
  fix was the slope test's height-cache indexing: the game reads the collector zone's own 129 x 129 cache with the
  cell `trunc((p - zoneMin)/36)` clamped to `[0, 128]` and a missing edge neighbour contributing 0
  (FOLIAGE_EXE.md section 3, kinspect findings/navhash.md section 13). Reading the neighbouring zone's lattice rows
  for out-of-zone cluster members changed pass/fail at the zone edge and shifted later draws; with the per-zone rule
  the whole fold lines up. The Hub zone hash still needs the save-order building fold `0xfeeabfec` on top (see
  "Object order"); our data-derived order folds `0x982a5c29`, giving `0xf847e471`. The zone-10,10 word is exact
  (`0x4b059683`, full zone hash `0xe93d103c`).
- **Stale shipped tiles.** Many stored tiles were baked from a world state that differs from ours. The loader's scan
  (`knav_navhash --stale`, 3995 shipped tiles): **2959 zone hashes match, 1036 differ**, none is unverifiable; 261 of
  the 500 tiles with a zone file match. Interiors are decided only in fresh zones: of 397, 103 match, 111 differ, 183
  cannot be verified (167 child gather order, 15 a layout not in interiors.level, 1 a uid with no placement). The
  interior match floor alone (`knav_navhash --world`, 500 zone tiles): 264 of 493 bare interiors match; 78 more equal
  ours with the 0x100 term (the tile's bake had the building destroyed while the zone file says intact); with children
  1 of 671 matches (the order gap above). This is what the loader's stale check repairs (NAVMESH_CACHE.md "The
  stale-tile check in the loader").
  **Zone 52,37 (Asp Gulch) is exact** since the height query became f32 (see "What reproduces"): its 1567 folded
  instances fold to the stored `0x4dc1666b` (building fold `0x5a5e4b92`, vegetation `0xcb3d3cdc`). The f32 fix alone
  moved 23 zones from stale to fresh (1059 -> 1036 against the pre-fix scan).
  UNSURE: why the remaining 1036 zones differ. 797 have no zone file, so the vegetation word alone differs. The
  biome-list order fixed 8 of them (10,11 and neighbours) and the mesh gate was already right (it reproduces 10,11's
  179 folds). The rest is not one of the scatter's filters: on zone 16,15 (Floodwastes, stale) removing the road test,
  the erasers, clustering, the child scatter, the slope test or the altitude test one at a time never produces the
  stored word. Floodwastes is 22 of 22 zones stale and its Thin Tree layer and Canyonland_foliagerocks_small's
  Bouldersmall18-20 are `limit to grass areas` meshes, whose paint (`1406cd970`: noise + `grass spot` discs,
  FOLIAGE_EXE.md section 4) is not implemented; the live capture of 16,15 has 37 Thin Tree placements against our 34
  and 17 fewer Bouldersmall instances, i.e. the gate itself. Asp Gulch has no such mesh and is now exact.
- **The grass mask (`limit to grass areas`) — implemented 2026-11-10 (kenshi-k16.7.13).** rwh.14 showed the exe reads the overlay PNG's R as the grass density (the painter below is the authoring pass, not needed): the gate is PNG R + the G the scatter writes (`grass spot` discs) > 149, up to 20 re-rolls (FOLIAGE_EXE.md section 4, UNSURE items there). Effect: `knav_navhash --stale` zones matching the stored hash 2959 -> 3025 of 3995 (stale 1036 -> 970; with a zone file 261 -> 275 shipped, no zone file 2698 -> 2750), interiors 103 -> 105 shipped; zone 16,15's Thin Tree layer is 37 trunks as in the live capture. The paragraph below is the history before the rwh.14 finding ("one function missing": the fBm is not needed). kinspect navhash section 15
  decodes the painter (`1406cd970`), the grass-spot discs and the gate: per-region RGBA image (RoadOverlay's
  geometry: 1024², origin region min - 18, pitch 36900/1024), a 129×129 patch per zone; the painter writes channel 2
  per record of the biome's grass list as `trunc(n*255*weight)` with a 300-unit altitude falloff and a blackout
  noise; the scatter writes channel 1 discs (radius `grass spot` × scale, `clamp(2R/px_per_unit, 0.3, max)` with a
  squared falloff, written `(uchar)(v*96)`); the gate accepts a candidate when channel 1 + channel 2 > 149, up to 21
  attempts, then drops the instance. Only the 5-octave fBm (`FUN_1406cc0e0`, GOG `exe+0x6cba80`) is undecoded: the
  answer gives its frequency `(span/dim)*2*noise scale` but not its basis. The game exe is not on this host (the
  install has `data/` only), so it cannot be disassembled here, and the kinspect budget is spent. Channel 1 alone
  maxes at 96, below the gate, so the painter cannot be skipped.
  Measured reach (`knav_navhash --stale --zones`): 1931 of the 3995 shipped tiles place at least one instance of a
  `limit to grass areas` mesh — 1180 of the 2959 fresh zones and 751 of the 1036 stale ones. A wrong gate would
  therefore risk the zones that match today, which is why an approximation is not shipped.
  Zone 16,15's gap is the mask alone: every layer that the gate does not touch reproduces the live capture's folded
  instance set exactly (BIG 23, Standard 136, DeadPine 167, Metal_Tower 48, Ejecta 5, RUIN 4, robo 18, all sets at
  x within 0.05). The Thin Tree layer has 37 live placements against our 34 and Canyonland_foliagerocks_small 1816
  layer instances against our 1833 (the Bouldersmall18-20 gate).

## Tool

`knav_navhash X Y` - one zone: placements, folded objects, building fold, stored zone hash, the vegetation word and
the full zone hash (generated scatter, raw positions, the mesh gate), and every interior (stored, ours, children).
`knav_navhash --trees X Y` - every generated tree-loader instance in spawn order (`x y z fold recordID`), for
diffing against a live capture (kinspect `findings/navhash/navb_zone*.json.gz`).
`knav_navhash --world` - every zone tile with a zone file: interior match rates, the 0x100 diagnostic, unjoined
layouts. `--zones` adds a line per zone. `--stale` - every shipped tile: the loader's zone and interior decisions
(shipped / stale hash / unverifiable, with the unverifiable reasons), totalled for all tiles, for tiles with a zone
file and for tiles without one. `--stale --zones` adds a line per shipped tile (stored / ours / vegetation word /
tree counts / instances of `limit to grass areas` meshes).
