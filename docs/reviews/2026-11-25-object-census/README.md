# Object census for our ground mesh (kenshi-k16.7.187, 2026-11-25)

Facts only; the input to the placement design (kenshi-k16.7.193). Tool: `cpp/tools/knav_object_census.cpp` (`knav_object_census OUT_DIR`,
base game load order, 17 s). Base: origin/ksim b021e8891.

## What was measured, and how

- **Terrain** = knav::ground's grid: fullmap.tif every 2nd sample (36 u), two triangles a cell, the diagonal with the smaller height step
  (`ground.cpp:153`). Every "over the ground" below is against that surface.
- **Collision** = what kworld places today: `CollisionLibrary::addBuilding` / `addFoliage` for zone placements and foliage; features.dat
  has no collision in ksim, so a feature's `collision` XML was placed here with `placeModel` as walkable, unfilled (the lab's way, BRIDGE_FEATURE.md).
- **Kinds**: box = the shape has `half` extents; convex = a closed hull; mesh = an open triangle mesh. **Up-facing** = normal within 60 deg of +y
  (boxes / hulls: outward +y faces only; meshes: either vertical, the winding is the cooker's). `up_area_le18` = that area with the triangle
  centroid <= 18 u over the ground. `up_h_*` = height of up-facing centroids over the ground.
- **Cut** = knav::ground's cutter rule replayed on a vertical-ray grid (step = extent/48, 0.5..6 u): a shape's hits straight above a point
  paired into spans (odd count = open shell, span from -inf), the ground point is dropped when a span reaches into (g + 0.1, g + 18); a Projected
  shape drops on any hit (`ground.cpp` Gen::cut). `cut_area` = u2 dropped. A "solid that should cut" = n_cut > 0.
- **Sits on the terrain**, per sample line through the object: `base_rel` = its lowest surface minus the ground (median over samples;
  < -0.5 sunk, > 0.5 floating, else flush), `top_rel` = highest surface minus ground, `pct_under/through/over` = share of samples where the whole object
  is below the ground / the ground lies inside it / the whole object is above it. `lowvert_*` = the vertices within 1 u of the lowest, minus the ground
  at their xz. `origin_delta` = the placement's y minus the ground at its x, z.
- Placements are from `Zones::placeMerged` (11,714 `0-buildinglist` survivors, 252 target records; 2 targets are missing from the merged data and
  have no row). Foliage from `generateFoliage` over all 4096 zones, 60 sample instances measured per FOLIAGE_MESH record (counts are exact).

Files: `classes.tsv` (rollup), `records.tsv` (one row per placed BUILDING record, with 3 sample sites), `features.tsv` (per MAP_FEATURES record),
`foliage.tsv` (per FOLIAGE_MESH), `outliers.tsv`, `ore_pairing.txt`. Sites are `zone cellX.cellZ (x, z)`.

## 1. Zone placements (INSTANCE_COLLECTION 0-buildinglist, record type BUILDING)

Class = my grouping by `building category` / flags / functionality (rules: `classOf` in the tool). Columns: n / records, geometry gathered, up-facing
walkable faces, solids that cut (placements with n_cut > 0), terrain relation (sunk/flush/floating by `base_rel`, placements).

| class | placements / records | path mode | collision gathered today | up-facing (n placements) | cut (n) | sunk / flush / floating, base_rel median | 3 sample sites (largest record) |
|---|---|---|---|---|---|---|---|
| building with interior mask | 1264 / 34 | 3 | out of scope, not measured | | | | |
| building without mask, walkable | 140 / 11 | 3 | 1-2 parts; 6 records have boxes, 6 hulls, 3 meshes | 140; decks of platforms / walkways | 115 | 88 / 1 / 51, -10.4 | Swamp Lookout Tower (36): 18.39 (-61697, 33427); 34.46 (13241, 65533); 43.45 (52490, 62107) |
| wall / fence (Defensive Wall I-IV, Short, Connector, Wedge, Makeshift, Swamp Fence) | 3507 / 20 | 2 and 3 | 1-2 parts, 1-24 shapes (boxes + hulls; the Makeshift and Swamp Fence are one hull) | 2766 (the wall-walk tops; Walkable use for the pm3 records, Obstacle for pm2) | 3485 | 3426 / 6 / 75, -50.4 | Defensive Wall III Short (545): 10.42 (-97261, 46611); 45.25 (61644, -29822); 52.41 (95978, 45126) |
| wall gate | 140 / 7 | 3 (`is gateway`, has mask) | 7-30 boxes / 1 hull / 2 meshes | 140 | 139 | 137 / 0 / 3, -52.7 | Defensive Gate III (42): 14.56 (-79465, 114501); 43.29 (50979, -10443); 52.42 (95398, 46236) |
| wall ramp | 261 / 3 | 3 | 1-2 shapes, hulls and boxes | 261 | 259 | 254 / 2 / 5, -52.7 | Defensive Wall Ramp IV (94): 13.23 (-83399, -38971); 30.49 (-8021, 81606); 18.35 (-61632, 15850) |
| ore node (Iron / Copper Resource) | 1335 / 2 | 2 | **none**: the records have no `parts` (0/0) | 0 | 0 | not measurable | Iron Resource (1241): 10.25 (-100815, -29562); 10.26 (-97817, -26489); 9.26 (-104233, -24328) |
| mine / quarry machine (Quarry Mine A/B, Stone / Ore Mine, drills, processors, refineries) | 139 / 19 | 2 | 1-2 shapes: 12 records a hull, 2 a box, 5 a mesh | 139 | 139 | 134 / 0 / 5, -18.0 (Quarry Mine A/B -25 / -42) | Quarry Mine B (34): 21.19 (-48381, -57820); 44.6 (57914, -119668); 41.48 (43175, 73914) |
| windmill (Wind Generator, II, Small, Broken is in misc) | 123 / 3 | 2 | 3 shapes (box + 2 meshes) / 1 box | 123 (tower / blades) | 116 | 92 / 10 / 21, -5.9 | Wind Generator (65): 13.23 (-83250, -37395); 44.6 (56574, -119376); 52.41 (95951, 45252) |
| generator (Generator II, Small) | 47 / 2 | 1 (Projected) | 1 box, used as Obstacle (pm1 records that are `is interior furniture` are converted from Projected, `collision.cpp:~900`) | 47 | 36 | 15 / 16 / 16, 0.0 | Generator II (28): 14.56 (-80511, 114759); 48.30 (77398, -6598); 62.43 (141554, 54098) |
| outhouse | 93 / 1 | 3 | 5 shapes (4 boxes, 1 hull) | 93 | 93 | 87 / 1 / 5, -8.1 | 13.23 (-83113, -37463); 26.30 (-27595, -5294); 9.62 (-102252, 138923) |
| turret (Mounted Crossbow, Harpoon, + `[light]`) | 695 / 8 | 0 (IGNORE) | none gathered (IGNORE appends nothing, `collision.cpp:1124`); 1 of 2 parts has a collision file | 0 | 0 | not measurable | Mounted Crossbow mkII (262): 18.51 (-61318, 91088); 30.21 (-8092, -48161); 21.42 (-47907, 47512) |
| camp (Camp Bed = bedroll, Campfire, Flat Shelter, Scrap / Mercenary Tent) | 622 / 5 | 0 (IGNORE) | none gathered | 0 | 0 | not measurable | Camp Bed (414): 18.23 (-60567, -39281); 43.11 (53235, -95517); 47.27 (71679, -21755) |
| light / lamp | 881 / 18 | 0 and 2 | pm0 (Torch Post, Spotlight, wall lights, 569 placements): none; pm2 posts: 1 shape (hull or box) | 312 (lamp heads) | 259 | 183 / 4 / 125, -5.0 | Torch Post (208): 16.8 (-71821, -107506); 21.9 (-50073, -105140); 44.52 (56071, 95041) |
| farm / well / water | 330 / 17 | 0 (farms), 1 (wells, tanks), 2 (tower) | farms (243 placements): none; wells, tanks: 1-3 shapes | 87 | 85 | 80 / 2 / 5, -5.0 | Cactus Farm XL (62): 26.16 (-24383, -69968); 45.25 (61429, -30985); 43.29 (51347, -10727) |
| cage / training / camp prop (Prisoner Cage, Pole, Spiked Cage, Training Dummy, Medium Tent) | 860 / 23 | 0-3 | 191 placements (pm0) none; others 1-4 shapes (boxes 14 records, meshes 3) | 669 | 566 | 329 / 14 / 326, -0.2 | Prisoner Cage (262): 20.55 (-53152, 109833); 49.42 (81808, 46963); 32.5 (618, -121903) |
| town prop, pm0 (Shade Cover, tents, ...) | 91 / 7 | 0 | none | 0 | 0 | not measurable | Shade Cover (34): 18.39 (-61960, 33764); 29.12 (-12548, -90272); 23.13 (-39115, -85201) |
| town prop, pm1 (barrels, crates, ...) | 116 / 16 | 1 (Projected; as Obstacle for furniture) | 1-4 shapes, 12 records boxes, 5 hulls | 116 | 103 | 35 / 60 / 21, -0.2 | Barrel A (28): 14.56 (-80108, 114776); 45.21 (64188, -50044); 51.8 (87620, -109864) |
| town prop, pm2 (market stalls, fences, scaffolds, barge / dock wrecks, tar-sand pipes, tower ruins) | 814 / 48 | 2 | 1-7 shapes, 21 records boxes, 24 hulls, 10 meshes | 813 | 743 | 401 / 71 / 341, -0.4 | Market Stall A (92): 14.23 (-82816, -37762); 25.50 (-30624, 83536); 59.36 (128596, 19421) |
| town prop, pm3 (Swamp Walkboard, Pontoon Bridge, ...) | 243 / 4 | 3 | 1-5 shapes | 243 | 235 | 169 / 4 / 70, -9.7 | Swamp Walkboard (115): 18.39 (-62079, 33594); 21.42 (-46641, 48769); 55.31 (109289, -1918) |

Details per record (shape kinds, `use_walk/obst/proj`, heights, cut area, sink) are in `records.tsv`. Facts worth reading off it:

- **Walls and gates sit sunk by design**: lowest surface 40-80 u under the ground (median -50), 98 % of 3,507 wall placements; the mesh carries a footing.
  Their top (`top_rel`) is 100-220 u over the ground (`height_med` 100-220). All are Obstacle / Walkable solids that cut.
- **Mines and quarries** are one big hull or mesh (footprint 20,000-35,000 u2, cut area 10,000-33,000 u2, `up_area_le18` up to 50,000 u2), sunk 5-40 u
  into the slope (the ground lies inside the shape for 65-97 % of its samples).
- **Floating**: 51 of 140 walkable no-mask buildings (platforms, High Walkway: base +40 over the ground, `pct_over` 100 %), Quarry Post (31, +95), Wall Spotlight (37, +85),
  Tarsand pipes (+45..+68), Small Generator (19, +25), Warehouse Roof (+91), Lagoon Platform (+332). Cutting nothing (n_cut 0, whole object over the ground): High Walkway (15), Swamp High Walkway (8), Wall Spotlight (37).
- **Windmills**: tower box + 2 blade meshes; the tower is a box that cuts; `up_h_max` 575 (blades). Wind Generator Broken is the same shape in "town prop pm2".
- **Outhouse**: a pm3 walkable with a seat (function 17, `chair-fixed`): 5 shapes, 4 boxes + 1 hull; it carries walkable up faces at +7 u median (`up_h_med`), cuts 645 u2, sunk 8 u.
- **Camp Beds / Campfires / tents / torches / turrets / farms** are IGNORE (pm0): nothing is gathered, so nothing cuts and nothing is walkable. The records do name
  collision files for part of them (`collision_parts` column: turrets 1/2, Torch Post 1/1, farms 0/4), which `addBuilding` drops at `collision.cpp:1124`.
- **Iron / Copper Resource (1,335 placements) have no geometry at all** (no parts). The rocks are the generated foliage with `building type` (section 3). Only 100 of the
  1,335 sit within 40 u of such a rock (nearest: p10 61 u, p50 394 u, p90 > 400 u; `ore_pairing.txt`): they are separate markers, not the rock.
- **`outliers.tsv`**: 6 Pontoon Bridge placements (Newwworld layer, instances 2, 6, 8, 10 in zones 10.53, 11.41, 27.40, 8.52) have y between -40,882 and -92,328
  (origin_delta -41,000 .. -92,700): their collision is 40,000-90,000 u under the world. Not on the terrain at all (96 Pontoon Bridge placements in total).

## 2. features.dat map features (MF01; record type MAP_FEATURES)

1,821 placements, 224 records; 176 records / 1,629 placements name a `collision` file (all readable), 48 records / 192 placements have none (hidden markers
or render-only). Layer: all from the base `features.dat` read by `Features::join` (`placements()[].layer`). Kinds in the 176 records: 91 hulls, 76 meshes, 26 boxes (a record can have several).
Measured placed walkable and unfilled (lab method), all 1,629 placements: every one has up-facing faces (1,629), **1,503 cut the ground**, sunk / flush / floating 1,365 / 11 / 252.
Top records: Canyonland_Zone_Cliffblock_01 (79), Barge WreckPiece02 (73), Generic Rock Cog Terrain Mapped (59), Barge WreckPiece01 (57), Cliffblock_02 (51), 03 (47).
Cliff blocks and rock formations are huge single meshes reaching 1,300-2,400 u under the ground (`lowvert_min`) and 1,200-1,400 over it; a big
`cut_area` (200,000+ u2).

| record | placements | collision | up-facing, cut | terrain relation (median per sample line) | 3 sample sites |
|---|---|---|---|---|---|
| Metal_Arch_Bridge (METAL_ARCH_BRIDGE, the k16.7.186 case) | 25 | 1 mesh (Metal_Arch_Bridge_COL.xml) | 25; 25 cut; `up_area_le18` 31,713 u2, `up_h` -114 .. 82.6 median .. 1450 | `base_rel` +82.9, `top_rel` +82.9; pct under/through/over 15.7 / 0 / 84.3; 24 floating, 1 sunk | 20.11 (-54433, -95121); 34.25 (12399, -29427); 34.56 (9394, 11421) |
| Trestle_Bridge001 | 2 | 1 mesh | 2; 2 | +177.6 over the ground; over 82.8 % | 22.19 (-45980, -55948); 21.20 (-49481, -55123) |
| Bridgehead | 2 | 6 boxes, 4 hulls | 2; 2 | +91.6 (floating 2); over 99 % | 31.24 (-3420, -33654); 50.32 (84745, 4061) |
| ROCK-Arch_Type_01 / 02 / 03 | 4 / 7 / 2 | 1 mesh each | all; all cut | arches: lowest surface -170 .. -644, top +28..+470 over the ground; sunk | 25.27 (-30922, -18912); 25.27 (-31501, -20230); 24.26 (-32538, -23421) |
| Barge WreckPiece01-04 | 57 / 73 / 43 / 47 | 2-28 hulls + 1-2 meshes | all; 219 of 220 cut | sunk -18..-28 (median) | see `features.tsv` |
| Canyonland_Zone_Cliffblock_01 / 02 / 03 | 79 / 51 / 47 | 1 mesh each | all up; cut 71 / 46 / 43 | sunk -1,330..-1,390 median | see `features.tsv` |

Bridges: the base Metal Arch Bridge deck is 83 u over the ground along most of its span (84 % of sample lines are over the ground), with both ends
entering the ground (`pct_under` 15.7 %: the whole bridge below ground at the ends).

## 3. Generated foliage (FOLIAGE_MESH via `generateFoliage`; trees, rocks, ore rocks)

11,582,023 instances in 638 FOLIAGE_MESH records.

| group | records | instances | collision in ksim today | up-facing, cut (of 60-per-record samples) | terrain relation |
|---|---|---|---|---|---|
| with a `collision` file | 327 | 674,584 | `addFoliage` places the file, Obstacle (Walkable if the record is `walkable`), `foliage` true; open shells get `solid` (`collision.cpp:1130-1180`) | 5,909 of 5,970 samples have up faces; 5,824 cut | sunk 5,136 / flush 180 / floating 634 |
| ... of those with a `building type` (ore rocks: Iron / Copper Resource) | 37 | 16,718 | same | 429 samples, 428 cut; kinds hull / 3-box junk | sunk 376 / 7 / 46; `base_rel` per record +33 .. -80 (mostly -2 .. -80), `pct_through` 2-92 % |
| ... `walkable` collision (TreeFall01-05, MonuRock01-04 and variants, AshCreteChunk001-006, FOLIAGE_WADI-Boulder02, CanyonLandCrater_Tree, Giant_MultiLimbTree) | 26 | ~58,000 (WADI 9,630; TreeFall 21,800; AshCrete 23,400; MonuRock ~3,000) | `NavUse::Walkable` | up faces and cut as above | in `foliage.tsv` |
| no collision, `navmesh cutter` radius only | 105 | 543,671 | a 12-sided Projected disc of the radius (`collision.cpp:1156-1180`), 1 u tall | none (a disc: no walkable face); cuts through all heights | on the ground (disc at the instance's y) |
| neither (grass, ground cover) | 206 | 10,363,768 | nothing | | |

Ore foliage (the "iron nodes"): `ResourceRock-IRON01..05` (+ Easy Reach / SWAMP variants, 150-1,200 instances each), `ResourceRock_Copper01/02`, `FOLIAGE_Ejecta_Boulder01` (782),
`Vast_Cluster_Piece01-07` (115-166), `TechRustyJunk_01` (4,118 + 442 SPARSER), `TechJunk01` (753), `Motor_*`, `FOLIAGE_Crystal_PINK01` (433), `FOLIAGE_Pyrite01`.
Typical: one hull, `base_rel` -30 .. -65 (stands in the ground), cut area 3,000-23,000 u2; the `Vast_Cluster` pieces vary (+12 .. +33 floating for 02-05, 07). Samples are in `foliage.tsv`
(`sample_sites`, e.g. `ResourceRock-IRON01` zone / x / z).

Road foliage (`5-roadfoliage`, `Features::roadFoliage()`): points with no record, no mesh, no collision.

## 4. What kworld / knav do with each today (file:line, base b021e8891)

- **Zone buildings, any path mode but IGNORE**: `CollisionLibrary::addBuilding` (`collision.cpp:893`) places the selected parts' collision; use = Projected for pm1, Obstacle for pm2 (and an
  `is unwalkable roof` part), Walkable for pm3 (`collision.cpp:983-987`); a node (`is node`) and a seat function-17 furniture become IGNORE (`:917`, `:925`); IGNORE appends nothing (`:1124`).
  `AreaGatherer::gather` (`collision_area.cpp:77`) returns them with their mask shapes.
- **knav::ground** takes every non-door building shape and every mask shape and every foliage shape as a **cutter** (`ground.cpp:1275-1316`, doors skipped `:1289`): Projected shapes cut through all heights,
  others by the ground + 0.1 .. + 18 span rule (`:366`, `:432-480`). Their walkable faces are **not used**: "the join with buildings' zone-side surfaces is not in yet" (`ground.hpp:19`). So for the ground
  today every class above with geometry is a cutter only, and none of their up-facing area (decks, wall-walks, mine tops) becomes walkable ground.
- **surfgen / zone_gen** generate surfaces only for buildings **with** an interior mask (`zone_gen.cpp:58`, `:171`); every class in section 1 (no mask: walls, ramps, outhouses, windmills, mines, props, platforms) never
  reaches them. Gates have a mask and so do go in.
- **Foliage**: collected by `AreaGatherer::gather` (`collision_area.cpp:121-126`), cut by ground (`ground.cpp:1314-1316`), `rockShell` open obstacle shells fill their footprint in the older bake (`bake.cpp:305-309`).
- **features.dat**: used only to feed foliage generation and rendering (`collision_area.cpp:48`, `:124`); no collision is placed, so none of section 2 (bridges, cliff blocks, wrecks, arches) reaches the ground
  cutters, surfgen or the bake (BRIDGE_FEATURE.md item 1).
- **IGNORE classes** (bedrolls, campfires, tents, turrets, torches, spotlights, farms, 1,700+ placements): no geometry anywhere.
- **Zone Iron / Copper Resource placements** (no parts): nothing; their rocks come through foliage.
