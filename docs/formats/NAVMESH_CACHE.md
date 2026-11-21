# Navmesh cache (`*.knav`)

**The cache is the static bake only.** Runtime changes (buildings placed, finished, destroyed or
removed) are applied by rebuilding the affected tiles in memory (`knav::NavWorld`, below); the
changed mesh is never saved. Runtime tiles are derived data: reload the cache, replay the
building set, rebuild.

The runtime base is moving to the shipped tiles, patched by this bake where the world changes ("Runtime mesh", below).

Our own file, not a Kenshi format. Written by `knav_bake`, read by `knav::NavMesh::load`
(`cpp/include/knav/navmesh.hpp`). It holds the baked Detour tiles of a rectangle of zones.

## Bake

Input is gathered per zone rectangle (`knav::gatherArea`):

- Terrain: every second `kworld::Heightmap` sample, i.e. the game's 36-unit height lattice (the foliage height cache, FOLIAGE_EXE.md), two triangles per quad split along the (0,0)-(1,1) diagonal like the game's slope normals. The full 18-unit grid has a 40-50 degree scarp outside Hub gate B that the lattice smooths (see Parameters).
- Buildings enter by BUILDING `path mode` (fcs.def; enum order IGNORE 0, PROJECTED 1, OBSTACLE 2,
  WALKABLE 3, confirmed from which buildings carry each value; Hub census: 2 / 1 / 26 / 106):
  IGNORE adds nothing; PROJECTED cuts the xz hull of each shape through all heights (nothing
  rasterised); OBSTACLE rasterises every triangle unwalkable and fills closed shapes up to and
  above their top, so roofs are not standable; WALKABLE is the walkable-surface behaviour below.
  Part flags: `is unwalkable roof` makes that part OBSTACLE, `is stairs` is rasterised walkable and
  never fills its footprint. BUILDING `is gateway` shapes never fill their footprint either (it
  removed the stair tops); instead all `is stairs` and gateway convex hulls are `WorldShape::solid`
  and get the volume fill (Per tile, step 3b). The gate bodies are solid earthworks you walk over,
  not tunnels. UNSURE: `enforce ceiling` is read but not used.
  **Interior terrain** (kenshi-k16.7.18, oracle rwh.90): BUILDING `interior terrain` ("building interior includes terrain for generating
  navmesh", default 0; 1 on Metal Warehouse, Ancient Factory, Ash Dome Ruin only) decides whether terrain enters the interior's mesh. With 0 the
  terrain inside the building's `interior mask` volume is not rasterised (bake step 1b), so a sunk building's floor is walkable under the dune
  and joins its door; the volume is the union of the mask's boxes (each box's 6 planes; the Longhouse's mask is three boxes of different yaw and
  height), carried on every shape of the walkable building (`WorldShape::carveTerrain`). Terrain height and collision are untouched; terrain
  outside the volume stays. Open (default taken): a building with no mask box (a convex-hull mask) carves nothing. Interior floors and staircases are in every bake (`addBuilding(..., interiors = true)`
  default, 2026-10-31). Door children of a building enter as `NavUse::Door`
  shapes that mark passage and never geometry (Doors, below).
- Buildings: every layer-merged placement (`Zones::placeMerged`) whose cell is in the rectangle
  plus a one-cell margin. Parts are picked as `tools/building_geometry.py select_parts` does
  (a stable hash of the instance id replaces the random roll), collision comes from the part's
  `xml collision` file (`kworld/collision.hpp`, per `COLLISION_FORMAT.md`). Parts with only a render
  mesh are skipped and counted. Parts flagged `passable` or `no collision` are skipped.
  `is stairs` parts are rasterised but never fill their footprint.
- Foliage: `generateFoliage` for each zone. A FOLIAGE_MESH `collision` file is added as an OBSTACLE,
  or as WALKABLE when `walkable` is set (Hub: 207 of 6,876). With no collision file, `navmesh cutter`
  x instance scale is a PROJECTED 12-gon hole (Hub: 2,828).
  A rock's open collision shell (`WorldShape::solid` on an Obstacle that is not `closed`) cuts the xz cover of its own triangles,
  each projected through all heights, not the hull of the whole shape (kenshi-k16.19.4: the hull also cut the gaps between a pile's
  plates and the ground beside them; the Hub copper node: unwalkable 3,572 -> 3,040 u2 against a 3,208 u2 collision cover, worst
  overhang 9.8 -> 0 u). Still no walkable span on or inside a rock (c31bb853). Patch cache recipe version 3.
  A shell wider than a rock (xz span over 750 u on either axis; `kRockMaxSpan` in `bake.cpp`; height does not count) cuts each triangle only through that triangle's own heights
  (y range - 1 cell .. + 2 cells), as a voxelised mesh does, not through the shell's whole height (kenshi-k16.7.59, patch cache recipe 8). Cause of the 5312 shatter: the swamp
  trees' `collision` file `SwampTreeCanopy_Foliage002[smaller].xml` (FOLIAGE_Plant_Swamp-TwigLarger / -beard_Frame; 1,426 triangles) is a canopy 570 u tall at scale 1; the instance at (-56702, 79, 58201),
  scale 4.25, spans y -1176..1258 and 1100 x 1900 u, so its triangles' xz cover cut every height of the Ancient Lab (5312, zone 19,44) beside it: a diagonal band 100 u wide through the
  floors at y 160, 215 and 282, which split them into dozens of components (558 sets in the zone; 227 with the fix, 27 with no foliage at all). The shipped tile has the band's floors. Why width alone, and 750 (k16.7.59 review): 6,353 shells of the world's rock instances exceed 700 u or 400 u; probing the walkable spans a per-triangle cut leaves inside each one's footprint against the shipped tiles (polygon within 1.5 u xz and 8 u y) the agreement is 40% for shells under 700 u wide (tall boulders: the whole-height cut is right there), 59-85% above, flat around 700-760 (accuracy over all shells .731 for width alone; no shape feature beats it: triangle count .741, up/down-facing share, closedness are no better, and the swamp canopy is a closed manifold like a boulder, so closedness cannot separate them). The first version also cut per triangle when taller than 400 u: it left the ground inside tall boulders walkable and made a 1,030,976 u2 plateau island at (-34728, y 984, -20692) (a 705 x 718 u, 800 u tall rock shell on a mountain top; the original has no polygon on it, 108 of 108 samples) cross the open-world gate; the island is terrain behind cliffs (now 947,338 u2, report only).

Per tile (Recast, `cpp/src/knav/bake.cpp`):

1. Rasterise terrain and all shape triangles into a heightfield (walkable by slope, winding as written).
   1b. **Interior terrain carve** (`carveInteriorTerrain`, right after the terrain goes in, before the shapes): every terrain span whose
   surface lies inside one of a `WorldShape::carveTerrain` box volumes is unlinked from the heightfield. The slavers' Longhouse
   (4028-TwoStorey.mod, Heft camp, interior terrain 0): the dune that rises above its floor (terrain 1,178+ over a floor at 1,174.5) left the
   floor unwalkable at (68328, -71194); now the floor is on the mesh and reached from outside through the east door (`knav_interior_terrain`:
   80 polygons on the dune before, 0 after). Recipe change: patch cache recipe 3. The Hub's Storm Houses carry the same flag, so the world and
   Hub bakes change a little (their terrain under the walls); a world re-bake: 28 min wall (measured 2026-10-31), only tiles with such a building change.
   **Door mat under a stair** (kenshi-k16.6.229, user playtest 2026-11-14 cage_prisoner: every move_to on the Heft Longhouse's floor, 68500 1383 -70747, REJECTED no_route
   from the entrance stairs): the stair climbs into the doorway, but the house's closed walkable hull fills its footprint and nulled the stair's upper spans; the door
   mat gives back the spans within the window from `door.min - agentClimb`, so a diagonal band of the stair just under the window stayed nulled, 2 cells wide, and the stair's polygons stopped 2 u
   short of the door's (the interior an island). The window now starts a stair slope more below the door (`kMatStairDrop` = margin x tan 55 = 11.4 u). One region's areas change by under 0.01% (Land +290 u2,
   Edge -158, door -12 in region 11,4). Patch cache recipe 5 (navworld v14). v12 and v13 (navworld_v13, k16.7.36) both have the gap; v14 is the world bake with this change. Test `knav_longhouse_door` bakes the entrance's tiles itself and fails without the change.
   **The Longhouse's far end** (kenshi-k16.7.24, client lead's live check of k16.7.18: a move to the user's pick (68433.5, 1174.5, -71227.3) ended at
   (68404.4, 1173.0, -71246.6), 35 u short): not a bake fault, no tile changes (recipe stays 3). The pick lies inside the Longhouse's east block (collision
   shape 11 of the placement: a 41 x 22 x 78 box at yaw 43, y 1172-1255; dropping it from the bake puts a floor polygon on the pick, with it there is none),
   8 u outside the interior mask's box 2 (18.7 u outside box 0, 98 u outside box 1: the keepInside intersection and the carve union are not involved), over
   terrain at 1145. The original's own mesh has no surface there either: its interior 8042f reaches x 68416 and `knav_compare --at` finds nothing at the
   pick or 10 u west of it; its nearest floor is about (68413, -71237), 20 u away, ours (68410, -71244), 29 u away (our wall band / clearance). The floor over
   the rest of the footprint is on the mesh: ours covers 90% of the original's interior floor (y 1165-1185, beyond 4 u of a wall; v11: 73%), the pieces
   missing are the 4 u wall band. UNSURE (deferred: playtesting): what the user clicked (the block's foot?); `knav_interior_terrain` pins the nearest floor point and a
   complete path from outside the east door to it.
2. Filter low-hanging obstacles, ledges, low-height spans; build the compact heightfield.
3. Every closed shape (box, capsule, sphere, convex hull) nulls the spans inside its xz
   footprint hull from its bottom to 3 cells below its top. This fills solids: the rasterised
   hull alone leaves the ground inside a building walkable. The top face stays walkable (decks, floors).
   3b. **Volume fill** (before the filters, `bake.cpp`): each `solid` shape (convex hull of an
   `is stairs` part or gateway building, whose top may be sloped) is rasterised alone and added
   back as one span per column from its lowest to its highest sheet. Recast shapes are thin
   sheets, and buildings are sunk into the terrain (stair meshes extend deep, emerging wherever
   they meet the ground), so without this the terrain inside a slab or gate body stays walkable
   between the sheets: pockets under decks and ledges where a stair top meets the ground. The
   fill absorbs it; the top face keeps its own walkability, so the higher of terrain and
   collision is the surface, as in the game. These shapes' triangles are walkable up to 55
   degrees (`kStairSlopeDegrees`): the stair collision is a smooth 42-48 degree slab.
4. No erosion: the agent-radius wall band stays as `Area::Edge` ("The original's recipe" below), watershed regions (each split
   so it holds one span per column), contours, polygon mesh (6 vertices per polygon), detail mesh.
5. Detour tile data (`dtCreateNavMeshData`, BV tree on). Each polygon has an area (Land 0, Shallow 1,
   Swim 2, DoorOpen 3, DoorClosed 4; see Water and Doors) and the flag bit of that area (`1 << area`).

### Water (`knav/travel.hpp`)

What the game data says (`docs/reference/WATER.md`, from the exe plus the merged data):

- **One flat water plane at world y = 100.0** for the whole map: a constant in the exe, not read
  from any record, mask or zone. No per-zone, per-biome or lake-specific level exists
  (`fcs.def`/GLOBAL CONSTANTS have no sea-level field; the swamp, Shark and Deadlands acid
  pools are all basins under 100). The sea floor is raw 0 = y 0, so **the deepest water is 100
  units (10 m)**. Acid (`BIOME_GROUP.acidic water`) is damage only, not a path rule.
- Character wading/swimming (exe, WATER.md C390/C392): wet when `y + 2 <= 100`; wading when
  `6 * h + y <= 100`; swimming when `15 * h + y <= 100`, `h` an unresolved per-character float.
- RACE fields (merged data): `swims` (bool; false = "walks on the bottom of the lake"; Human
  true, Fishman, Skeleton, Crab false), `swim speed mult` (Human 1.0, Bull 1.5, Beak Thing 1.5,
  Shek 0.5, Fishman and Alpha/King Fishman 5.0, raptors 4.0), `swim offset`, `water avoidance`
  (-10..10, how far they go to avoid water), plus an undeclared `swim speed` = 10 on 14 races.
  CHARACTER stats have a `swimming` skill (1..70 in the data). **No absolute swim or wade speed
  is given**, only the multiplier; how the skill scales speed was not found.

The bake takes the walkable surface (terrain, decks, anything rasterised) and tags every span by
`depth = waterLevel - surface height`:

| Area | depth | meaning | flag |
|---|---|---|---|
| Land | < `wadeDepth` (6) | dry or ankle deep | 1 |
| Shallow | 6 .. < `swimDepth` (15) | wading | 2 |
| Swim | >= 15 | swimming (the surface under the water is still meshed: `swims = false` races walk it) | 4 |

Regions never merge across areas, so a shoreline is a polygon boundary and a bridge deck over
water (depth negative) is Land. UNSURE: `h = 1` (so 6 and 15 units, i.e. 0.6 m and 1.5 m against
a 2 m human; with `h` = body height 20 swimming would be impossible, the sea is 100 deep at most);
the `y + 2` wet margin is not modelled (Land covers it). The thresholds live in `NavConfig` and
the cache header, so a retune is a rebake.

Costs are per query, not baked: `TravelFilter` (`cost[Area]`, `allowed` mask) becomes a
`dtQueryFilter` (`configureFilter`) in `NavQuery::findPath` / `locate` and in the coarse graph.
Defaults, all UNSURE (time cost per unit distance relative to walking, since the data has only
`swim speed mult`): Land 1, Shallow 1.5, Swim 3. `TravelFilter::swimmer(mult)` divides the swim cost by the
race's `swim speed mult`; `TravelFilter::dryOnly()` excludes Swim. A race with `swims = false`
walks the bottom, so the sim should give it Swim cost 1 or 1.5 rather than excluding it; excluding is
for travellers that cannot cross deep water at all (carried loads, the sim's own rule).

Meshing depth cap (considered, not done): the sea is at most 100 deep, a swimmer
crosses any depth at the surface, so removing deep floors would cut islands off for swimmers
and for bottom walkers, and a cap saves nothing in the open sea where the floor is already one
polygon per tile.

### Doors and gates (`knav/door.hpp`)

**Data** (merged records; census by a throwaway reader over `kdata::GameData`). A door is a child
BUILDING reached through the parent's `doors` reference list (45 parents with doors, 47 refs; 13
`is gateway` buildings, 10 with a door, 3 without: Wall B Solo Gate, Wall B Gate, Lagoon Platform).
The child has its own `parts`; its part carries `xml collision` (the 36 `is door` BUILDING_PARTs
are the door meshes). Door fields (fcs.def ~430-450): `door axis`, `door comes out`, `door move
dist`, `door move speed`, `door navmesh axis` (0..2), `door type` (sound), `always locks` ("for
town gates, closed state is always locked"; set on the Defensive Gate level 4 door). **Placement:
the door's collision is modelled in the parent's local frame with no offset.** Gate4_Door_col.xml is
a 3 x 112 x 104 slab at local x 7.6..10.7, z 77..181, exactly in the gap between the gate's two
tower blocks (z 78..179), and both files have a `_0.85.bin` cooked at the parent's scale, so the door
is placed with the parent's position, rotation and scale (the `doors` ref values are `0,100,0`
or `0,0,0`, probably chance; not used). Its own `path mode` (OBSTACLE) is ignored on purpose:
doors never enter the geometry. Hub: 135 placements, 25 with a door (2 gateways: both "Defensive Gate
IV", instances 4400 and 4617; 23 houses), 20 of the 25 got polygons (18 before the stair-wedge and leaning-wall
fixes below; the wedge fix unblocked the entrances of 20,33/2 and 21,31/402). Interiors went in with the hull-fill fix: a house is a closed walkable hull without its doorway, and
its footprint fill (top minus 3) buried the deck or floor the door stands on (sill = deck top; 4 houses, 20,32/22 /32 /4937
/5013). The bake now fills walkable hulls first, gives each door mat (footprint + 8, sill - climb .. sill + agent height)
back what that fill took, then fills obstacles and projected shapes (walls beside the door keep their ground). The last bare
door, 20,32/26, sits 24 above the terrain on a deck whose house block top is 7 above it (under the 18-unit agent height, the
deck is low-headroom) with no steps in the collision: not a bug. Most Hub houses have no collision in their interior parts (53 of 79
added interior parts have none), so a linked door often leads onto only a threshold or a thin floor strip: ctest `knav_door`
walks the street -> door -> interior floor for each linked house door (4 of 22 reach a floor inside the building, 1 reaches a
higher floor); that census is a lower bound (crude ring sampling).
**Bake.** A doorway is always baked as walkable passage: door shapes (`NavUse::Door`, one per
collision shape, carrying the door key) are never rasterised and never cut anything, so a door's
state cannot change a tile. They only relabel ground that is already walkable: after the water
classes are set, every span under the door's xz footprint grown by 8 units (`kDoorMargin`; a
mat about 19 units deep for the 3-unit slab) and between the slab's base minus the agent climb
and plus the agent height gets that door's own Recast area. Regions never merge across areas, so
the mat becomes its own polygons. Each door of a tile takes its own area (key order, up to 57 per
tile), so touching doors stay separate polygons; afterwards all become `Area::DoorOpen` and
the polygon-to-door pairs are recorded. This is a **dedicated polygon area**, not an off-mesh
connection: an off-mesh link would be a second way across the wall and could not stop the walkable
passage, while the polygons the traveller must walk over are exactly where the state has to bite.
Door ground loses its water class (a gate in water is not a case that exists). Deterministic: the
door list of a tile is sorted by key, the pairs by polygon index, and ctest `knav_deterministic`
compares 1- and 4-thread files, door lists included.

**Door id** = `"cellX,cellZ/instanceID#n"` (`kworld::doorId`; n = index in the building's `doors`
list, removed refs skipped), **key** = FNV-1a 64 of that string (`kworld::doorKey`). The cache stores keys
only; `AreaBuilding::nav.doors` lists a placement's keys. `NavMesh::doorKeys()` / `doorPolys(key)`
give the door-to-polygons table (the tile records above, resolved to Detour refs at load and after
`replaceTiles`).

**State.** `DoorBook` holds, per door, `DoorState {Open, Closed, Locked}`; a door not in the book is
open to all. There is no class, faction or owner test on a door, as in the original (only a closed
door refuses; findings/rwh199_nav_validity_static.md section 2; the per-class denied mask of the first
slice is gone, kenshi-k16.7.111). `DoorSetting::verdict(throughLocks)`:

| state | verdict |
|---|---|
| Open | Free: DoorOpen cost |
| Closed | Wait: DoorClosed cost |
| Locked | Blocked (Free for a bash: `throughLocks`) |

Blocked is **excluded**, not a long wait, so a
search that has another way takes it and one that has none fails cleanly (`found` false) instead
of queueing at the wall.

`NavMesh::applyDoors(book)` repaints the polygons of the mesh's doors with Detour's `setPolyArea` /
`setPolyFlags`: area DoorOpen (Open) or DoorClosed (Closed, Locked), flag bit
`kDoorLockedFlag` (Locked). Nothing is rebuilt, reloaded
or re-added (the test compares the Detour tile refs, and the baked bytes in `tiles()` never change);
it costs the door polygons of the mesh. Like `replaceTiles` it is not safe against concurrent
queries (tick boundary). The book is kept and re-applied to tiles that `replaceTiles` / `NavWorld::apply`
bring in, so a rebuilt tile does not forget a locked door. `TravelFilter` gained `throughLocks`
(whether its queries exclude the locked bit), costs for the two new areas, and the door areas are always in
`allowed`. Costs (UNSURE, no speed in the data): DoorOpen 1; **DoorClosed 11** per unit of
distance over the mat, i.e. a wait of about 200 units of walking (13 s at 15 u/s); `door move speed`
has no stated unit, so it is not used. `PathResult` gained `cost` (the area-weighted length) and
`doors` (door polygons the path goes through).

**Coarse graph.** Clusters, nodes and portals are built with every door open and open to all (door
polygons are plain walkable regions), so they never depend on door state. A portal pair whose
stored path crosses doors has a `DoorEdge`: the doors crossed with the path length inside each, and a
`bypass`: the cheapest path inside the cluster that avoids every door polygon (cost -1: none). At
query time (`findPath(..., DoorQuery{book, class})`) the edge costs: through = stored cost + for each Closed
door `length x (DoorClosed cost - DoorOpen cost)`, or refused if any door is Blocked; the edge is the
cheaper of that and the bypass. Start and end legs are searched live on a scratch cluster mesh painted
from the book, so they follow the same rules. Per-edge work is a lookup into a sorted list: no cluster
is recomputed when a door changes (`update()` is still only for tile changes). Limits: with
several doors on one path only "all doors" or "no doors" are compared (no mixed route that avoids
one door and uses another inside one cluster); the closed wait is `length inside the door x 10`
of the stored straight path, the detailed path measures its own, so the two differ by tens of percent
on the wait itself (Hub gate: +378 detailed vs +273 coarse). `CoarsePath::doors` lists the doors the
route goes through. Graph file version 3 (below).

**Use by ksim (2026-10-31, `ksim/navigator.*`, `ksim/doors.*`, `docs/reference/DOORS.md`).** The sim owns the
book (town postures + overrides), calls `NavMesh::applyDoors` on every resident region mesh at a tick boundary and on each
window mesh it builds, and queries with `TravelFilter::throughLocks` / `DoorQuery{book, throughLocks}`. `PolyComponents` gained
`around(polys)`: the labels beside a door's polygons. The sim labels the connected sets with the door areas *excluded*
(`allowed & ~(DoorOpen | DoorClosed)`), lists per door the sets beside it, and unions per traveller class the sets of the
doors that class may pass (`Verdict != Blocked`), so a gate changing state relabels nothing. Door polygons themselves
have no label (a goal never lands on a door mat). Doors on a region seam are found through the seam strips.

Tiles are independent, so the bake runs on N threads; the result is sorted by (z, x) and does
not depend on N. The same input gives a byte-identical file (ctest `knav_deterministic`
compares a 4-thread and a 1-thread bake).

### Parameters (UNSURE: none are from game data; human mesh is 19.5 units tall, 10 units per metre)

| Name | Value | Why |
|---|---|---|
| cell size / height | 2 / 1 | agent radius is 2 cells |
| agent height | 18 | the original's Havok character height (HAVOK_NAV.md) |
| agent radius | 4 | 0.4 m; the width of the `Area::Edge` wall band, not an erosion ("The original's recipe") |
| agent climb | 5 | the original's Havok edge-matching step (HAVOK_NAV.md); UNSURE it is also the walk climb. Both Hub gates cross at 5 since the volume fill. `knav_coarse` once failed at 5 only because the test snapped the coarse route's linear y (128 units off the surface on a 768-unit hump) with a 3D-nearest query; it now looks for the surface layer by layer |
| max slope | 40 degrees | the original's `maxWalkableSlope`. Needs the 36-unit terrain lattice: on the 18-unit grid the scarp west of Hub gate B (x -51712..-51690, z 2810..3000, triangles 40-50 degrees) cuts the shelf in front of the gate off (`knav_dynamic`); the lattice crosses it below 40. Stair slabs bake at 55 |
| tile size | 256 units (128 cells) | `kWorldMin` and zone size are multiples of it; 18 tiles per zone |
| max edge length | 64 units | long flat polygons path badly |

**The zone 20,32 gap is a real obstacle (kenshi-k16.7.11, 2026-11-09).** The 4-6 u gap across open ground at x -52239..-52241, z 2586 (the
"Seam, west" bow of kenshi-k16.8.8: (-52201.3, 2595.2) -> (-52248.9, 2583.85)) is the tip of a hole in the mesh, one terrain lattice
triangle: the 36 u triangle (-52272, 2556), (-52236, 2556), (-52236, 2592), terrain heights 1245.36, 1273.17, 1261.21 (the lattice splits
along the (0,0)-(1,1) diagonal), has a
slope of **40.07 degrees**, just over the 40-degree `maxWalkableSlope`, so it is unwalkable, and its tip at z 2592 is what the straight line
clips. The shipped navtile has the same hole (`knav_draw --native --box -52300 2540 -52180 2640`: two holes, this triangle and the larger
slope to its east, outlined as wall edges, identical in shape to our rebake's, `--navworld`), so the original has the obstacle there too:
nothing to fix in the bake, and no building, hull or foliage is involved (so the k16.7.10 zone-floor y change did not touch it). The zone is
rebaked only because its hash is stale (the 3 stale Hub zones); the terrain triangle is the same in both. UNSURE: the original's slope
test is exactly 40 degrees on this triangle (40.0655 here): the shipped hole says it is at least not walkable at 40.07; a triangle at
39.9 or 40.0 would show the edge, not looked for.

## File layout (little-endian)

| Offset | Type | Field |
|---|---|---|
| 0 | `char[4]` | `KNAV` |
| 4 | u32 | cache version, 4 (3 has no door lists and is still read; 2 had no water fields and all polygons area 0) |
| 8 | u32 | `DT_NAVMESH_VERSION` of the Detour that wrote the tiles (7 for 1.6.0); load refuses a mismatch |
| 12 | f32 x 10 | cellSize, cellHeight, agentHeight, agentRadius, agentClimb, agentMaxSlope, tileSize, waterLevel, wadeDepth, swimDepth |
| 52 | f64 x 2 | originX, originZ: world min corner of tile (0, 0) |
| 68 | i32 x 2 | tilesX, tilesZ: the whole grid |
| 76 | i32 x 4 | window x0, z0, x1, z1 (inclusive): the tiles this file bakes; every record lies inside it |
| 92 | u32 | tile count N |
| 96 | N records | `i32 x, i32 z, u32 size, u8 data[size]`, then (version 4) `u32 doorCount` and `doorCount` x `u16 poly, u64 doorKey` (the tile's door polygons, ascending poly); ascending (z, x) |

`data` is the raw `dtCreateNavMeshData` blob (native layout of Detour 1.6.0 on little-endian 64-bit;
vertices are absolute world coordinates as f32). Tile (x, z) covers
`[origin + tile * tileSize, +tileSize)` on each axis, counted from the grid origin whatever the
window. Tiles with no walkable surface are absent.

## Why not one mesh, and what the limit is

A `dtPolyRef` is 32 bits: salt, tile id, polygon id, with at least 10 salt bits. The system
Detour is built without `DT_POLYREF64` (`/usr/include/recastnavigation/DetourNavMesh.h` has it
commented out; defining it for us would not match the library's ABI, and the library is not to be
rebuilt). The world grid is 1,152 x 1,152 = 1.33 million tiles, so one mesh cannot hold it.
A tile id is the *slot* in the mesh's tile pool, not the grid position: a `dtNavMesh` holds any
tiles of an arbitrarily large grid, up to `maxTiles`. So the rule is a **resident limit**, not a
bake limit: a mesh holds at most `kMaxResidentTiles` = 8,192 tiles (13 bits) of at most
`kMaxPolysPerTile` = 512 polygons (9 bits), leaving 10 salt bits. The loader and `addTile` check
both. A 4 x 4 zone region is 72 x 72 = 5,184 tiles, so one region plus a margin fits; the sim
loads the regions it has at full detail (plus the margin the coarse graph needs) and the
coarse graph covers everything else.

## World bake (`knav_bake --world DIR`)

One file per region of 4 x 4 zones (`kRegionZones`, 16 x 16 regions; the same blocks as the
16 x 16 blocks of `globalPathing.path`), `DIR/region_RX_RZ.knav`. Header: world-origin grid
(origin -147456, 1,152 x 1,152 tiles), window = the region's 72 x 72 tiles. Each region is gathered
and baked as `NavWorld` bakes it, with buildings and foliage from one zone around the region
too, so a tile near a region edge is the same bytes as it would be inside a bigger bake
(tiles read 20 units around themselves; ctest `knav_bake_seam`). Files are byte-deterministic
per region and independent of the thread count (with one measured exception, k16.6.274 below). Regions that are loaded together
(`NavMesh::load(paths, keep)`, `NavMesh::loadRegion`) are linked by Detour across the seams
because tile coordinates are shared.

### Whole-world bake (measured 2026-10-31)

`knav_bake --world DIR --threads 4` under `nice -n 10`: 256 region files, 1,321,459 tiles (of
1,327,104; the rest have no walkable surface), 11,600,942 polygons, 2.78 GB, 26 min of baking
plus 26 s of gathering (about 28 min wall, game data loaded once: 2.3 s). 4-6 s per region, up
to 8 s in the dense ones; no tile reached 512 polygons. That was the bake **before the water
areas**, when the sea floor was one flat walkable polygon per tile. With the Land/Shallow/Swim
areas (cache v3, same command, 2026-10-31): 1,321,459 tiles, **12,447,136 polygons (+7%), 2.91 GB
(+4.5%)**, 28 min (gather 28 s, bake 1,653 s). The sea floor stays in the mesh on purpose (swimmers
cross it, `swims = false` races walk it, the deepest water is 100 units), and every shoreline
adds polygon boundaries, so the files grew; the water rule is a correctness fix, not a size win.
The size win is on the graph side: a dry-only graph is smaller (below).

v15 (2026-11-15, k16.6.274, the first bake with the chance-0 interiors; `--threads 8`): 1,316,365 tiles,
**24,082,974 polygons, 4.46 GB**, gather 32.5 s, bake 1,162 s. 32 tiles are over 512 polygons and were baked
without the wall band (2 of them eroded and still over). The growth since 2026-10-31 is the recipe's (exact
heights, door mat, interiors), not this change: v14 is 4.2 GB the same as v15.

v16 (2026-11-17, k16.7.88, rev 02fd89cc, the first bake with k16.7.50's zeroing allocator; baked on dev into its navworld_v16 dir): `nice -n 10 knav_bake --world <navworld_v16 dir> --threads 4` (Release): 1,320,402 tiles, **24,192,935 polygons, 4.48 GB**, gather 23.7 s, bake 1,058 s (18 min wall); 34 tiles over 512 polygons (2 eroded, still over). Against v15: **82 of 256 region files byte-equal, 174 differ**; 71,828 tiles differ (of 1.32 M), +4,037 tiles, +109,961 polygons (+0.46%), the polygon deltas all in areas 0 (Land) and 5 (door mat / interior floor; per region about 1:2). Largest: regions 5,10 (4,221 tiles), 4,11, 5,9, 6,9, 4,10 (3,450-3,709); 14 regions differ by 1-2 tiles. The size of the diff is more than a handful of heap-noise tiles, so v15 was not just noisy: Which change besides k16.7.50 moved the other tiles is not known (`git log` shows no other bake-code commit); not chased, because v15 cannot be reproduced and nothing rests on it (ksim, k16.7.88). Determinism: regions 0,11 and 0,6 re-baked alone (`--regions RX RZ RX RZ`) at `--threads 1` and `--threads 8` are byte-equal to v16's files. In use since 2026-11-17 (`~/git/kenshi/navworld` on dev and the desktop points at v16; v15 kept beside it, `navworld_v15`): the 84 tests that read the world bake gave the same results as on v15 (83 pass, ksim_fight_trace a known failure of that day), so no pin moved.

**Thread-count independence (kenshi-k16.7.50, 2026-11-16):** a bake is byte-identical at any `--threads` and on every run. The 2026-11-15 note (k16.6.274: zone 29,16 at `--threads 8` gave two files, 8 / 2 in 10 runs) was not scheduling: the tiles are built independently and collected in coordinate order. Recast 1.6.0's `rcBuildContours` reads heap bytes it never wrote (valgrind, even at one thread), and tile (2,6) of that zone came out 26236 or 25988 bytes depending on what malloc handed it (11 of 30 runs at 8 threads; the 1-thread run happened to get the same heap each time). `bakeTileSet` (`bake.cpp`) now gives Recast a zeroing allocator (`rcAllocSetCustom`), so those bytes are 0 every time. The single-thread mesh is unchanged (the 26236-byte variant; no rebake of the world needed, but a region that held the other variant in v14/v15 differs from a re-bake, region_7_4 among them). Zone 29,16 at 8 and 16 threads: 20 runs each, one file, equal to `--threads 1`. Test `knav_bake_threads` (slow) bakes that zone at 1, 8 and 16 threads and compares bytes; it failed 3 of 3 before the fix.

### Exact heights in tiles with shapes (2026-11-14, kenshi-k16.7.36, patch cache recipe 4)

Recast's span tops are whole voxels (cell height 1) and its detail mesh adds one more, so a building floor baked up to 2 u above its collision. `buildTile` now puts every polygon and detail vertex of a tile that holds shapes on the highest walkable collision triangle under it (`HeightOracle`); heights only, polygons unchanged. Terrain-only tiles are untouched (flat shortcut still byte-identical). Details and numbers: `INTERIORS.md` "Navmesh height in a building tile". A world bake made before this has the old heights: re-bake.

### Bake speed (2026-10-31)

`KNAV_PROFILE=1 knav_bake ...` prints CPU seconds per tile stage (rasterize terrain / shapes, span
filters, compact, erode, distance field, regions, contours, polygon mesh, detail mesh, Detour data).
A tile costs about 3 ms even when it is one flat polygon, because Recast always runs the whole
142 x 142 column pipeline; the detail mesh is the biggest stage in dense tiles (35% in the Hub).

- **Flat tile shortcut** (`BakeTuning::flatTiles`, on by default, `--no-flat` turns it off). A tile whose
  box holds no shape, lies inside the terrain and whose terrain lattice is one flat sheet (height range
  <= 0.5 and the same voxels everywhere; the open sea floor, 25% of the world's tiles) is emitted
  directly: one quad, the area its depth gives, the detail mesh of a four-corner polygon. **Byte-identical**
  to the pipeline (ctest `knav_bake_flat`; also compared over 24 regions), same seams, same cache version.
  Needs Recast's voxel arithmetic reproduced (`spanCells`), so change both together.
- `--partition monotone` skips the distance field and the watershed: ~30% less CPU per land tile, polygons
  differ (-0.5% count in the Hub and open land), walkable area per class differs by <= 0.2% (small
  regions are pruned differently; doors included, +1.2% door area in the Hub). `layers` is rejected: it
  ignores area ids, so water and door polygons merge across their boundaries.
- `--detail-cells N` (default 6 = a height sample every 12 units): 12 keeps every polygon and cuts the
  detail stage by ~60%, surface height error against the default p50 0.3, p99 2.3, max 8.7 units
  (24: p99 4.2; 0 = no interior samples: p99 21, rejected). Only heights inside polygons change.

The defaults are the reference bake. The tuning is process-wide and not stored in the cache, so a runtime
rebuild (`NavWorld`) is byte-equal to the static bake only under the tuning that made it.

## Coarse graph (`knav/coarse.hpp`, `knav_graph`)

A region/portal graph derived from the baked tiles for squads and long queries (HPA*-style). It
is not part of the `.knav` file; `knav_graph --world DIR --out FILE` builds it region by region
against the region plus a one-cluster margin of its neighbours and writes one `KCRS` file.

- **Cluster** = 3 x 3 tiles (768 units; `kClusterTiles`). A cluster is 1/6 of a zone side, so
  the 72-tile region is 24 clusters wide and no cluster straddles a region file. Smaller
  clusters keep intra-cluster paths short (a Detour query is tens of microseconds) and keep
  portals close to where a route really crosses; larger ones make the graph much denser (the
  pair count grows with the square of the portals per cluster). A zone-sized cluster would have
  ~70 portals and 2,500 pairs of 4,600-unit paths.
- **Node** = a connected region of walkable polygons inside one cluster (union of polygons over
  Detour links, tiles of the cluster only). Numbered by their first polygon in (tile z, x,
  index) order. Each node keeps an anchor point on its first polygon.
- **Portal** = a tile edge of the cluster boundary, per (side, chunk, node, neighbour node),
  where a polygon edge meets a polygon edge of the neighbour. The point is the middle of the
  widest overlap of the two polygon edges, computed from the vertices (not Detour's quantised link
  span), so both sides compute the same x, z. A route crosses between the two matching portals
  of neighbouring clusters (a hop).
- **Edge** = every portal pair of one node: the length of the Detour path between the portal
  points inside the cluster (a scratch `dtNavMesh` of just the cluster's tiles, so it cannot
  leave), and the corners of its straight path. Different nodes are not connected (length -1).
- **Query** (`CoarseGraph::findPath`): both ends are located on their meshes (resident tiles
  needed only at the ends), A* over portals with the Euclidean heuristic, seeded from the start
  node's portals and ending through the end node's. Same cluster and node: a direct path. The
  result is legs (path inside one cluster, or a hop), each with its polyline. `positionAt(d)`
  walks the polylines: x and z always lie on a straight path through mesh polygons, y is linear
  between corners (snap with `NavQuery::nearestPoint` at promotion, as ARCHITECTURE says).
  `refine(mesh, leg)` recomputes one leg's detailed path against the current mesh.
  Failures are values: `StartOffMesh`, `EndOffMesh`, `NotBuilt`, `NoRoute` (an island).
- **Update** (`CoarseGraph::update(mesh, changedTiles)`): recomputes the clusters holding
  the tiles and their four neighbours (a cluster reads its neighbours' region numbering, which
  a change anywhere in a neighbour can renumber). Dependencies are exactly those, so the result
  equals a fresh build; ctest `knav_dynamic` checks that, and that removing the wall restores the
  graph byte for byte.
- **Determinism**: a cluster is a pure function of the tiles; threads only choose who computes
  which cluster. Serialisation is little-endian, clusters ascending by (z, x).

**Traveller filter.** A graph is built for one `TravelFilter` (`CoarseGraph::build(..., travel)`,
`knav_graph --dry` for the dry-only one) and stores it. Polygons of excluded areas belong to no
region, so water that a traveller cannot enter cuts clusters apart and an island behind it has no
portals: the query reports `NoRoute` (ctest `knav_water`). An edge's stored value is its **cost**:
the Detour path (cheapest by the filter's area costs) with each stretch between edge crossings charged
at its polygon's area cost. `CoarsePath::length` is the plain length, `cost` what A* minimised, and the
heuristic is the straight distance times the filter's cheapest cost. Hops across a boundary are
zero-length, so a water crossing is paid inside the cluster paths. A sim keeps one graph per kind of
traveller (swimmers, dry-only); `refine` uses the graph's own filter.

`KCRS` layout: magic, u32 version 3 (2 is still read: 3 area costs, no door edges), u32 cluster
tiles, f32 x 5 area costs (`kAreaCount`), u16 allowed mask, u32 cluster count; per cluster
`i32 x, i32 z, u16 nodes, f32[3] anchor x nodes, u32 portals`, per portal `u8 side, u8 chunk,
u16 node, u16 other, u8 slot, u16 poly, f32[3] point`, then for each pair (j = 1.., i < j)
`f32 cost, u32 corners, f32[3] corners`, then (version 3) `u32 doorEdges` and per door edge
`u32 pair, u32 doors, (u64 key, f32 length) x doors, f32 bypassCost, u32 corners, f32[3] corners`.

Measured: the Hub window (324 clusters) builds in 25 ms on 4 threads (1,333 nodes, 4,214
portals, 17,754 intra edges, 0.75 MB). The whole world: 17.5 s on 4 threads, 147,389 clusters,
365,729 nodes, 1,887,120 portals, 9,362,548 intra edges, 9.8 M stored corners, 256 MB, 1.2 GB
resident while building. A Hub to nearest-town route (3,977 units apart, two regions): 13 legs,
4,356 units (4,248 by plain Detour on a corridor mesh), 0.35 ms. Hub to the far NE corner of the
map (213,000 units): 791 legs, 174 ms (a lot of it open sea). Next: per-region graph
files so the sim loads only what it needs.

With water areas (2026-10-31, v3 bake): the swimmer graph is 147,389 clusters, 365,712 nodes,
1,887,178 portals, 9,363,770 intra edges, 10.75 M corners, 267 MB, 22 s on 4 threads. The
dry-only graph (`--dry`): 324,008 nodes, 1,251,674 portals, 5,758,523 edges, 8.99 M corners,
203 MB, 21 s (24% smaller). Edge costs are weighted, so corner counts rose a little.

Cross-check with `globalPathing.path`: its 16 x 16 blocks of 4 x 4 zones are our regions. The
original's file joins all 480 neighbouring block pairs; the swimmer graph also joins all 480
(`only ours 0, only globalPathing 0`), the dry-only graph joins 382 and misses 98 that the original
joins. So the original's pathing crosses water: it was baked for characters that swim.

## Dynamic rebuild (`knav/world.hpp`)

`NavWorld` holds the terrain, the fixed shapes (foliage) and the building set. `setBuilding` /
`setBuildingState` / `removeBuilding` only edit the set and mark dirty every tile the building's
shapes can reach (their box grown by the tile border, 20 units). `prepareRebuild()` snapshots the
dirty tiles and the active shapes (buildings by id, then fixed shapes) into a self-contained job;
`job->run(threads)` bakes them on any thread; `apply(job)` swaps the tiles in and is meant for the
tick boundary. Each tile is a pure function of terrain and active shapes, so the result does not
depend on thread count or edit order, and removing a building restores the static bake byte for byte
(ctest `knav_dynamic`). Jobs must be applied in the order prepared. DetourTileCache was not used:
the rebuild reruns the whole tile pipeline, which is ~30 ms per tile-set of 4 tiles on one thread.

A building is active when its path mode is not IGNORE, `builtFraction * 100 >= build threshold`
(walls: 80), and it is not destroyed with `destroyed navmesh` set. A destroyed building without
that flag keeps its shapes.

## Positions on several floors

A floor is a polygon, not a height: storeys, decks and stairs are stacked polygons at the same
x/z, linked through the stairs. The original does the same (kinspect: characters on the Hub gate
decks sit at exact deck values, e.g. 1555.0, not at terrain height).
- **Saved:** a character's x/y/z, with y included. Polygon refs are never saved: they change on
  every tile rebuild (placed wall, re-bake).
- **Runtime:** the current polygon ref. Movement is `moveAlongSurface` from it, so stairs and
  floors follow from links, and a ground-floor walker under a balcony stays on the ground floor.
- **Re-attach** (load, tile replaced, teleport): `findNearestPoly` at the known y with a small
  vertical half extent (a few units, below the storey gap). A tall box picks the closest polygon
  in 3D, which can be the wrong floor (the `knav_coarse` y-snap bug, 2026-10-31).
- Today squads carry x/z only, and route y is linear between corners; that is enough for overland
  travel, not for interiors. This plan lands with the Individual layer.
- **Stress test, built (ctest `knav_tower`, 2026-10-31):** BUILDING "Watchtower" (`4104-D-TwoStorey.mod`, a Storm house
  model: 285 units tall, four floors, a winding stair, player-built) on flat ground (the plateau at -85800, -28400, y
  163.9) through `NavWorld::setBuilding` and a dynamic rebuild (4 tiles; `removeBuilding` restores the empty site byte for
  byte). Results: the ground outside reaches the ground floor (y +5), floor 1 (+132), floor 2 (+188) and the roof (+289)
  (route 160 / 644 / 805 / 1104 units); the way down from the roof crosses every 30-unit band of height with at most
  14 units between path corners; 82 saved positions (path corners and midpoints, 73 of them with another floor or flight
  within 150 above or below) all re-attach inside a +-4 vertical box (was +-3; since the mask clip dfb67607 one chord midpoint on
  the winding stair sits 3.0-3.5 off the surface, so +-3 missed it; +-4 is still far below the storey gap; ksim 2026-11-09, kenshi-k16.7.4). What it took:
  - **Interior parts were not in the bake** (fixed 2026-10-31). The floors and staircases of a building are its `interior`
    references (each a part with `xml collision`; `is stairs` on the staircases), not `parts`. `CollisionLibrary::addBuilding(...,
    interiors)` adds them for WALKABLE buildings and is now on by default, so static, world and `NavWorld` bakes all have them. Hub
    cost: polys 44,379 -> 44,547 (+0.4%), cache 9,261,016 -> 9,290,440 B (+0.3%), bake time unchanged (3.0 s, gather 2.5 s); the
    world would grow by about the same ratio (about 12.45 M polys -> +0.4%, 2.91 GB -> +9 MB), only town tiles change.
  - **Leaning wall hulls.** The ring wall (24 OBSTACLE hulls, `Storm09-WallCOL`) leans inward 17 units over 280, and its
    footprint prism covered the staircase beside it (stair cut off at y 15..30). Closed obstacle hulls whose side faces lean
    more than 4 units (`leansOver`, bake.cpp) are now column-filled like stair solids, which is exact for a convex hull.
  - **Stair wedges.** `Exterior[Steps]_COL` is a closed wedge (44 degree top, buried below the ground); filled by footprint it
    removed the entrance strip. A closed walkable hull with an upward face between 40 and 55 degrees (`hasStairFace`,
    collision.cpp) is now `solid` (column fill, stair slope 55) instead of `closed`. This also gave two more Hub houses
    their door polygons.
  The world bake needs a re-bake for both fixes (house steps and leaning obstacle hulls change tiles).
- Re-attach tested the other way: a +-150 vertical box returned the saved polygon every time here because the saved y lies
  exactly on the surface; the trap needs a y that is off by more than the gap to a neighbouring floor (the `knav_coarse`
  y-snap case), so the small box stays the rule.

## Tilted walkable hulls (fixed 2026-11-01)

Bug: storm houses had no walkable interior. A closed walkable hull's footprint is filled with one
prism (`rcMarkConvexPolyArea`, bottom `min y`, top `max y - 3 cells`) so the ground inside is not
walkable while the top face stays. Hub buildings are tilted with the terrain (up to about 6
degrees), so the 180-unit floor slab of Storm House 3384 spans 15 units of height and `max y`
is its highest corner: the prism reached above the top face over most of the footprint and nulled the
floor there (3% to 22% of the floor tops walkable on the tilted houses; the untilted one had 36%, the
rest lies under the walls). Fix (`walkableTopLow`, bake.cpp): the prism ends 3 cells below the
*lowest point of the walkable top* (faces within 40 degrees of level), `max y` for a hull with
none. For an untilted box the two are equal, so those tiles are unchanged. Cache format unchanged
(still v4: only tile bytes differ; old caches load, they just lack the floors). Hub: polygons
44,536 -> 45,038 (+502, +1.1%) in the 3 x 3 zone bake, same 2,897 tiles; floor-top walkable
fractions of the 7 Hub storm houses now 30 to 44% (ctest `knav_storm_house`, which also walks
the kinspect route (-51085, 3145) -> east ramp (-51115, 3172, y 1563) -> floor y 1576.4 in
Storm House 3384). A prism cannot follow a tilted top: a hull whose terrain column rises above the
lowest top corner could keep a sliver of walkable ground inside it (not seen in the Hub).

## Interior furniture (2026-11-03)

Bug (user, live): the Hub bar's tables and counter were walkable. The bake read each building's `parts` and `interior` parts but
never its furniture, which is a separate layer: the placement's GAMESTATE_BUILDING `interior layout name` picks an
INSTANCE_COLLECTION `<building sid>-<layout>` of interiors.level (INTERIORS.md "Furniture placement"), each instance a furniture
BUILDING in the building's local frame. Fix:
- `kworld/layouts.hpp`: `InteriorLayouts` (the four layers merged as game data; `EXT-` collections left out) and
  `placeFurniture` (the client's buildings.cpp composition: position scaled by the building's scale, rotated by its quaternion,
  offset; rotations multiplied; scale = the record's `scale` x the building's).
- `CollisionLibrary::addFurniture`, called by `AreaGatherer::gather` for every placement with a layout: the pieces' shapes join
  the building's own, so they come and go with it (runtime removal, NavWorld rebuilds). Counted in `coverage.furniture*`, not in
  the placement counts; `knav_bake` prints them (Hub: 465 pieces, 114 with collision; 200 before seats were left out, below).
- PROJECTED furniture (tables, benches, path mode 1) goes in as OBSTACLE (`addBuilding(..., furniture)`, also for any record with
  `is interior furniture`): a projected cut runs through every height, so a table on floor 1 would cut floor 0. As OBSTACLE it
  stays within its own height band (closed hulls fill to their top; the headroom filter removes the floor under an open top).
- Seats (a functionality with `function` 17: chairs, stools, benches, sitting pillows) are not cut (PathMode Ignore): a sitter
  stands on the seat's spot. A stool under a table edge is still inside the table's agent-radius margin, so the sitter stands up to
  about 6 u off it and `routines::kSeatTolerance` is 8 (SIMLINK flag 15; ksim_town_bar). UNSURE: the original may use a use node.
- Feature row: FEATURES.md "Furniture blocks walking". Bar squads stop on the table rim (ai.cpp `kBarReach` 20).
- `is node` records (Node Generaal, Node Shopkeeper, Node Guardpost, n-guardpost, n-defensive position: AI standing markers)
  are IGNORE everywhere: their selection boxes cut the floor where NPCs are meant to stand (the bar lost a third of its floor).
- Lamps (`Ceiling Lamp`, path mode 0) were already IGNORE.

- The Hub bar's Shop Counter looks L-shaped in the client, but its collision (`shop_counter.xml`) is ONE box, 31.6 x 11.5 u; the short "west arm" is render only, so there is nothing to cut and the original's walkers stand there too (kenshi-k16.7.19, playtest kenshi-2vt: checked against the cache, the whole footprint is cut). Pinned in `knav_counter_arm`.

Tests: `knav_furniture` (every Hub piece with collision and >= 12 units across has no walkable polygon within 2 units of its
centre on its own floor: 24 of 24; the cache before the fix: 2 of 24); `knav_storm_house` leaves samples under the furniture out
of its floor fraction (the bar, house 4933: 28 % of its free floor). Player-placed furniture: not supported yet (`buildings.cpp`
refuses furniture sites); when it is, it goes through `addBuilding(..., furniture = true)` and the NavWorld dynamic rebuild.
Needs a world re-bake (navworld v8).

## Runtime mesh: the shipped tiles, patched (user 2026-11-04)

- **Default (k16.7.2, 2026-11-09)**: `ksim_server` with no nav argument starts on this mesh (`MeshNavigator::openNative`, the
  patch cache on, `--no-nav-cache` turns it off); `--native-nav` is accepted as a no-op alias. `--navmesh FILE` and `--navworld DIR` stay
  as explicit options (all three exclude each other); `--bake DIR` still only picks the AI's navigator. The server no longer
  bakes the Hub zone at startup. Time to the navmesh line, Hub 3 x 3 zones, dev VM, `--scenario duel`: cold cache 6.4 s, warm
  cache 2.8-3.5 s (wall 6.5 s / 2.9-3.8 s for the 6-tick dump; RSS 1.06-1.08 GB), of which the stale check is 1.5 s cold and
  0.15-0.3 s warm; the 3 stale-hash zones come from the cache warm (bake 0 ms). Still rebaked every start because the check
  cannot verify them: 1 interior (the gather order of an interior with children, k16.7.6; the three stale zones also drop
  their own interiors). Test `ksim_server_native_default`: no argument and the alias, a Hub route planned.
- **Base**: the shipped navtiles (`knav::native`), converted to Detour at load, from the player's install. No first-run world
  bake. Work: cut each zone mesh (4608 u) into our 256 u tiles (512 polygons per tile, 6 corners per polygon: split faces);
  interior meshes and their streaming links (`Edge::linked`) become shared edges or off-mesh links; faceData 4 / 5 ->
  DoorOpen / DoorClosed; Shallow / Swim from height against the water plane. Clearance at query time, as the shipped mesh
  has no erosion: kinspect `findings/navmesh_runtime_spec.md` (17738d1; static `navrun_static.md`) is the spec. Per agent
  radius = race `pathfind footprint radius` x size factor (humans 4 u, King Gorillo 12-30 u); start snap r + 0.1, goal snap
  off boundary edges by r + 0.05; A* tests each portal's clearance against r; funnel corners pushed out by r + 0.5; door
  face 5 blocks, 4 costs +5; water cost x race multiplier (human x7). Open there (its section 6): r vs 2r, narrow pockets.
- Shipped faces (Hub 5x5 zones, 74 k faces): 97% have <= 6 corners (up to 19), 2% concave in xz, 22% non-planar
  (> 0.5 u, up to 37 u).
- **Conversion** (`knav/native_tiles.hpp`, `knav_native OUT --region RX RZ` writes region files; ctest `knav_native_tiles`):
  each face ear-clipped in xz, the triangles clipped to the tile, merged back into convex polygons of <= 6 corners within
  the face; the triangles are the detail mesh, vertices kept to 1/8 u. Region 5,8: 1.1 s, fullest tile 302 polygons.
  Hub 3x3 zones: area 100.00% of the shipped faces, reach from a street 99.99% of theirs, the bar's back stair climbs.
  What the data needed: opposite edges hold their own corner copies (welded); edges < 0.25 u collapse (sliver faces);
  a corner on a straight edge is never an ear (T-junction); faces with no area left at 1/8 u stay as zero-width bridge
  polygons; some opposite edges run the same way (folded pairs, linked anyway); interior doorway corners sit up to 0.3 u
  from the outside's (snapped within 1 u; zone seams left to Detour's portals); and edge matching pairs edges that share
  no corners (bar stair top: a 4.4 u step), so edges are paired by native identity, the most overlapping piece.
  Door keys (`nativeDoorKeys`): a door face takes the door whose footprint + 8 u holds its centre, within 10 u below
  the door's bottom; Hub 5x5 zones: all 38 door polygons keyed. But the shipped faces mark few doors: 35 faces (all
  data 4, none 5) for 4 of the 65 placed doors; the other doorways are plain faces, so a locked door can't block them.
  The shipped Hub zone is stale developer data (kinspect live trace, 2026-11-04): mods.cfg empty, 5 saves on 2 instances
  give the same live hash (d4dc7de5 over 103 buildings) vs the shipped c9ebf91d; no single building change reproduces it.
  Of 74 zones seen only the Hub fails; elsewhere the zone matches but most stored interiors fail and rebake every load.
  Live Hub after its rebake: 3 doors (DoorStuff; the rest are doorless ruins), zone faces 12 x data 4 + 7 x data 5, the
  bar interior 1 x data 4. Our ruins had doors: fixed, a placement whose squad template has `building ruined` (Hub zone: 21 of 104, the live
  game's 21 destroyed) spawns none (kinspect: building setup skips `doors` while destroyed); 5x5 zones 65 -> 37 doors. The Hub's two
  Defensive Gate IV are such ruins: the user saw in game (2026-11-04) the gates standing but no doors in them; the live
  mesh has no door faces there. Original's cost: Hub Full 2.45 s.
  Live grader: kinspect `findings/navbuild/hub_live.knm` (3x3 zones round 20,32 after the rebake), `knav_compare --live`.
  v9 on zone 20,32: covers 96.1% of the live surface beyond 4 u (96.4% of the shipped), surplus 5.4% (5.0%): the stale
  tile barely moved our grade. At the ksim_world_follow trap (-48550,3265): 99.6% covered, the ledge is in the live mesh.
- **A no_route on a click into a crowded building (kenshi-h2v.25, user's bar_lure capture 2026-11-11, not reproduced):** nine clicks into the Hub Bar, all refused live. Offline every one routes, at radius 4 and 0, from the Wanderer's capture position and from its trail positions at each click's tick, on the world bake and on the native mesh, over the bare mesh and in a bar_lure simulation (`ksim_move_no_route`). So not a clearance budget, a doorway width or a start layer of the static mesh; characters are not in the nav query. What the live server had that the offline setups lack is unknown (door and furniture state after 14 min, a moved coverage window). `move_to` now logs `move_no_route id= seq= from= click= radius= why=` (the Route's failure) so the next occurrence names the cause. Unreachable-spot and narrow-gap behaviour: deferred: kenshi-rwh.58 (the refusal stays).
- **Clearance** (`TravelFilter::radius`, `NavQuery::findPath`; ctest `knav_native_tiles`, `knav_clearance`): radius 0 =
  Detour's search, for eroded bakes. Above 0 the search is ours (the system Detour has no virtual filter, so its A* can't
  see edges): one node per polygon, placed at the middle of the portal it was reached through as Detour does (a route that asks to be straightened, `TravelFilter::straighten`, places it where the straight line to the goal crosses the portal: "Node placement" below), and a portal is crossed only if some point on it (0.5 u steps, middle first,
  <= 32) is r from every wall (`findDistanceToWall`, walls = edges with no neighbour whatever the area); each portal
  caches the largest radius it passed and the smallest it failed. Start pushed off the walls to r + 1 u, goal to r + 0.5 u,
  straight-path corners: shrink first, pull second (`shrunkFunnel`, k16.6.248): each corridor portal end moves inward along its
  portal by max(0, r + 5 - wall distance at that end), at most half the portal (`kShrinkShare`), then the funnel runs over the
  shrunken portals, so a corner exists only where a wall holds it and nothing is pushed or reverted afterwards. `cost` follows the
  radius-0 corners.
  The search is bounded: a radius-0 probe of the same pair first (clearance only refuses portals, so no route without it
  means none with it), then at most 2400 nodes (`kClearanceNodes`); a search that runs out fails `OutOfNodes`, never a
  shorter walk. Hub: street -> bar floor 1 at r 4 in 0.7 ms (365 u vs 327 at r 0, corners >= 4.35 u from walls); the
  shipped Hub at r 30 runs into the budget (37 ms measured) and refuses. Start push confirmed (kinspect
  `navrun_static.md` C17/C41: the goal's snap at r + 0.1). Every route the sim asks for passes the race's `pathfind
  footprint radius` (`CombatData::pathRadius`, `Simulation::pathRadius`; the merged Greenlander is 4.0, the exe's
  CharMovement+0x364): move orders, AI travel, chases, interactions. A route that fails says why (NoRoute); none falls
  back to radius 0 behind the caller. `knav_clearance` walks a 7 u gap at radius 0 and is refused at radius 4 (the
  original's minCharacterWidth blocks gaps under 9 u), and takes a 12 u one at radius 4; `ksim_route_soak` asks 239
  routes over the world_sample window at radius 4 (worst 18.6 ms, the fail-fast no-route; worst found route 14.0 ms).
  **Node placement (2026-11-09, kenshi-k16.6.16; ctest `ksim_move_detour`):** Detour puts a polygon's search node at the middle of
  the portal it was entered through, so a polygon edge 140 u long (the Hub's tile seam at x = -52224, z 2574-2716; faces are
  large where the ground is open) makes the cheapest corridor by those midpoints go to the far side of the edge and back: eight
  short moves near the Hub ran 1.3-3.3x the straight distance (3 corners, the funnel pulled round the corner the corridor
  forces). The probe's own corridor was reused when it fit, so a body-sized route had the same shape. Now a route that asks for
  it (`routeFrom(..., straighten)`, on by default since k16.8.7: the player's move_to, AI, follow and chase routes; it was the click
  alone in k16.6.16, because turning it on for every route flipped `ksim_bar_fight` step 4 until the walker merge re-pinned it) whose ends are
  under 300 u apart (xz) runs the clearance search with each node at the point of the portal on the straight line from the
  parent node to the goal, also when the probe's corridor fits but its pulled-tight path is over 1.02x the distance (the shorter
  path stays); longer routes keep Detour's placement (the search with it expanded more nodes: `ksim_route_soak` mean 6.3 -> 12.8
  ms, worst found route 28 -> 107 ms, so it is not used on a long walk). The node sits at the point of the portal on that line
  (clamped to the portal; `crossingOn`), so the node's g is a real polyline length. Those eight are 1.00x now; the user's own
  query (character 41906, (-52165.3, 2607.2) to (-52155, 2653), from the desktop's navworld_v9 bake) plans straight on the
  runtime mesh and on navworld_v11 already, before the fix: not reproduced; the cause above is the one the scan found.
  **"Seam, west" start hitch (kenshi-k16.8.8):** the click (-52201.3, 2595.2) -> (-52248.9, 2583.85) with the click's ground height (about
  1256; without a click height the end snaps to x = -52232 and the route is straight) is a 3-point route that bows 7 u north round
  (-52243.5, 2602.3): 61.5 u for 48.9 straight, 1.26x (the walker adds to 1.28; the client saw 1.37x). It is the mesh, at every radius
  (r = 0 bends too, 1.28x): a 4-6 u gap in zone 20,32 (a 40.07-degree terrain triangle, in the shipped tile too: see "The zone 20,32 gap is a real obstacle") crosses the straight
  line at x = -52239..-52241, z = 2586 (the line is on the mesh before and after it), and the route goes round its north end at z 2594.
  The 82-degree heading flip the client saw is the walker, not the mesh: the route's first point (the start pushed 2.15 u off the walls
  by the route radius) is passed only by arriving within 1 u (LOCOMOTION section 9), so the slew overshoots it and steers back for one
  tick (client rerun, findings `kenshi-inspect/findings/ksim_verify/k16.8.8/`; fix: kenshi-k16.8.8 round 2). Walked from the snapped
  start at eight facings it turns at most 19 degrees in one tick from tick 2 to 11 (`ksim_move_detour`, a regression gate).
  **A start that does not fit (2026-11-07, branch ds/nav-tower-exit; user play):** a character standing where its body does not
  fit (`wallDistance < r` on the polygon the plain snap picked: a roof edge, a stair lip, a doorway threshold) had every route
  refused — the clearance search can never leave such a polygon, so the corridor ended there (`Closed`, a partial path). The
  Hub tower's roof is like that: its narrow walkway means most roof spots are too narrow for a 4 u body, and every order off the
  roof was `no_route` while the same ends route at radius 0. The start now snaps on first to the nearest polygon the body fits
  on (`locateWhere` with `wallDistance >= r`, the same test `nearestWalkable` uses) within 4 r (OURS: a body walks to its own
  start snap, it does not teleport) — the original's start snap (r + 0.1) — then the push above. The goal is left alone: a
  narrow goal polygon is reachable (the corridor arrives and the end is clamped onto it), only a narrow start closes, and a goal
  refit moves the end of every route whose click lands near a wall (`ksim_player_sleep`, `ksim_capture_live`,
  `ksim_slaver_caravan` went red under one). No fitting ground in reach: the plain snap stands and the search refuses as before.
  `knav_tower_exit`: the Hub tower's roof (41 starts: the logged spot and a ring to 8 u), its two refused ground clicks and the
  way back, radius 0 and 4 — red before (16 legs `Closed`), green after.
  UNSURE: clearance >= r (likely: Havok's cache keeps a radius; the compare is undecoded, C21-C27; a live probe can settle it).
  Open: the wall sweep at r while walking (sim), the straight-walk shortcut, water cost x race multiplier.
- **Patches**: our recipe bake rebuilds only the tiles a change touches: a player building placed, finished, destroyed or
  removed (`knav::NavWorld`), a placed object, a zone whose placements a mod changes (stale-zone test: placements vs vanilla).
  A patched tile is clipped to the same tile borders as its shipped neighbours; Detour joins overlapping border edges.
- **Requirement**: a patch must never leave a broken mesh (holes, slits, cut stairs, lost connections) in the area it
  rebakes. Our full-world bake against the shipped tiles (`knav_compare`, `knav_draw --native`) is the validation of the
  recipe and stays required. Small areas can afford a finer grid: 1 u cells fix the Hub bar stair seam (below).
- **How the original updates** (kinspect `findings/navbuild/README.md`, 86e42e6: a Storm House and a table built live):
  the rebake runs at Confirm (placement), not on completion; construction changes nothing after that. A building = one
  Partial job on its zone tile (the building carves, 16 ms) plus its interior baked as its own mesh. The zone keeps no
  floor inside the footprint (the interior owns it), with door faces (data 4) at the doorway. Then the zone restitches to
  its 4 neighbours, the interior to the zone, and the town gate flood fill reruns. Furniture (path mode 1) = an Interior
  job only (a clean cut); the zone is untouched and its hash ignores furniture. Saves hold no navmesh: on load a
  hash mismatch rebakes the zone (Full) and its interiors. Partial vs Full differ only in slivers along outer walls.
  So ours: patch at placement; a save stores no mesh; a load rebakes the changed zones from their buildings.
  NPC towns (same README, 43e9ca5): one bake engine. A tile file holds the zone mesh plus every interior in it (Hub
  20,32: zone + 21), each interior keyed by building uid + a hash of the building and its path-mode furniture (shipped
  interiors already have their tables cut). A matching interior is used as is, a mismatched one is rebaked, a zone hash
  mismatch rebakes the zone and all its interiors: the Hub does so on every load of kinspect's test save. So a shipped
  tile is a cache, valid only while its hashes match; our stale-zone test is that check. In practice: the Hub zone and
  most town interiors are always the original's own bake, so ours is the runtime mesh there, not a patch.
  The hashes (DECODED + VERIFIED-live; recompute recipe: kinspect `findings/navhash.md` s10, a6a27db): building = int
  pos x/z + 4 orientation ints (0 in all shipped content) + destroyed; interior = building + its children in gather order
  + destroyed; zone = the qualifying building fold + the zone's tree-position fold (Forest+4). The Hub's mismatched Bar
  and Residential Small interiors are a stale child list in the shipped tile, not a fold error.

## The stale-tile check in the loader (2026-11-05)

`MeshNavigator::openNative` runs the original's check (`kworld/navhash_source.hpp`; the recipe is NAVHASH.md) on the zones
a coverage wants, not on a whole region: a zone nobody loads costs no hash and no bake. The unit and its fallback:

- **Zone.** Our zone hash (building fold + forest word) against the tile's stored one. A mismatch, or a hash that cannot be
  verified, replaces the zone's whole 18 x 18 tile square with our bake and drops its shipped world mesh and every interior
  (the original rebakes "the zone and all its interiors").
- **Interior, in a fresh zone.** The shipped world mesh stays; an interior whose recomputed hash differs has its own tile
  square (the tiles its vertices reach) replaced by our bake, the tiles clipped to the same 256 u borders.
- **Unverifiable** (rebaked and counted apart from a mismatch): no stored hash, no shipped tile, an interior uid no
  placement pairs with, a named layout not in interiors.level, or an interior with children (our gather order is a guess,
  NAVHASH.md "Gaps"). A margin zone (one cell outside the region, its interiors may reach in) is not checked (UNSURE).
- **What the hash reads.** The zone files and the game data, not the running sim's world: objects a save re-creates
  (the Hub's three appended torches, NAVHASH.md "Object order") are not folded, so a zone that a save changes reads as
  stale and rebakes, never as fresh. Every other input is present at load time; none is approximated.
- A rebake is `knav::gatherArea` + `knav::NavWorld` for the zone (both already loaded once for the check), baked on
  `knav::configForWorldZones`, so the tiles land on the shipped conversion's borders and Detour stitches them.
- **Never silent.** `NativeStaleStats` (cumulative) and a `NativeTileReport` per decided zone (`nativeReport`), and one log
  line per coverage change with the shipped / stale-hash / unverifiable counts; `ksim_server` passes a stderr logger and
  prints the totals at startup. A caller that passes no logger gets stderr (`knav: ...`).

Measured on this install (2026-11-05; ctest `ksim_native_stale` pins the decisions):

- Zone 10,10 ships (stored = our e93d103c). Zone 10,11 ships (two biomes, the live word de107fea; stored = ours
  7c47f9a3). The Hub 20,32 rebakes (stored c9ebf91d, stale; ours f847e471). Zone 23,33 ships with both interiors; 24,18
  ships with its three bare interiors, its child-bearing one is unverifiable; 38,22 ships while its bare interior rebakes
  (stored c2d8cc6a vs ours 92dd6d72).
- `knav_navhash --stale`, all 3,995 shipped tiles: **2,944 zone hashes match, 1,051 differ, 0 unverifiable** (255 of the
  500 tiles with a zone file match). Interiors, decided only in fresh zones: **102 match, 111 differ, 181 unverifiable**
  (165 child gather order, 15 layout not in interiors.level, 1 uid unpaired).
- The forest term was a hash bug this found: a zone folds it when it has a tree layer, even with no surviving instance
  (NAVHASH.md "What reproduces"). With "instances exist" the scan read 2,014 zones stale. The biome-list order (10,11)
  took it from 1,059 to 1,051; the rest is still unexplained (NAVHASH.md "Stale shipped tiles").
- Load time, Hub 3 x 3 (`ksim_native_nav`, same machine): open 535 -> 2,904 ms, all 9 zones 1,800 -> 5,967 ms, RSS
  +135 -> +519 MB. The check's data (game data, heightmap, biomes, features, towns, layouts) loads once (the
  `AreaGatherer` the rebakes share); the Hub's check is 452 ms and its rebake 407 ms. A later region pays only for the
  zones its coverage wants (test `ksim_native_stale`: 10 ms check for a fresh zone, 300-450 ms for a zone with interiors).
- The game data is the sim's own when the caller passes it (`openNative`'s `gameData`; `ksim_server` passes
  `context.gameData`), so a server that already holds the merged data loads no second copy. Measured in one process
  (`ksim_native_nav native-gamedata own|shared`, the sim's merge loaded first): the Hub's 9 zones 5,519-5,818 ->
  4,417-4,451 ms and 590 -> 520 MB in total, the ~95 MB game data loaded once instead of twice. The rest the check
  loads (heightmap, biomes, features, towns, layouts) is still its own. `ksim_native_stale` opens zone 10,10 both ways
  and gets the same decision.

## Patch cache (v1 2026-11-06; the design below, as built)

Broken down: `ksim::NativeCacheOptions` on `MeshNavigator::openNative`, the entries, the hit/miss counters
(`NativeCacheStats`, one log line per open) and the corrupt-entry rebuild are in v1 (ctest `ksim_native_cache`). Not in
v1, still the design's defaults: the prune (below) and the decisions tier (below); the server has the cache on (it is the
default mesh) and turns it off with `--no-nav-cache`.

Why: `openNative` converts every region it wants and rebakes every stale or unverifiable zone on **every** start. The Hub's
3 x 3 zones cost 2.90 s and +519 MB each launch, 0.45 s of it the stale check and 0.4 s its one stale zone (section above).
Both are pure functions of the player's install, the mod load order and our recipe, so they can live on disk between runs.
Nothing is generated at build or CI time (user 2026-11-05): the cache is filled by the player's own client from their
install; the repo ships no cache and no generator for one; tests use a temp dir.

**What is cached (MINE: both, as separate entries).**
- The converted shipped tiles of a region: exactly `convertNativeRegion(rx, rz)`'s tiles, the region's 72 x 72 tile
  window. The one-zone ring it reads only feeds the conversion (an interior stored next door); its tiles are outside the
  window and are not cached.
- Our recipe bake of a zone: `bakeNativeZone(zone)`, the zone's whole 324-tile square. One entry serves both loader uses:
  a stale or unverifiable zone replaces its whole square with it, and a stale interior in a fresh zone takes the tiles its
  vertices reach from the same bake (the loader already bakes the whole zone for that too).
- Not cached: the Detour runtime mesh (rebuilt from the tiles in 12 ms), the coarse graph, the door book and its paint
  (`NavMesh` re-applies the book), and the tiles a session's own `knav::NavWorld` rebuilds from a save's placed objects
  (that is the session's state; the zone hash does not fold save objects, "What the hash reads").

**Where (MINE).** `$XDG_CACHE_HOME/kenshi-sim/navmesh` (default `~/.cache/kenshi-sim/navmesh`), overridden by
`KENSHI_NAV_CACHE` (tests, a portable install) and turned off by `ksim_server --no-nav-cache`. Below it one tree per
**world key**, `w<recipeVersion>_<dataHash>/`, where `dataHash` is `kworld::dataHash(loadOrder)`: the content hash of every
merged data file, which the server already computes for the SIMLINK HELLO. A different mod set, load order or game data
starts a different tree and never reads a neighbour's. The key is `NativeCacheOptions::worldKey`; 0 (no load order, as in
a test or a tool) means no tree and no cache at all.

**The key (MINE; the three parts you named, split between the tree and the entry).**
- Zone bake: `zone_<x>_<z>_<zoneKey>.knav`, `zoneKey = FNV(our recomputed zone hash, the zone's interior hashes)`. The
  cell in the name is for a human reading the directory; the key alone is the identity. Ours are the
  recomputed hashes (the check's `hashes.hash`), not the shipped tile's stored ones: the bake is a function of the current
  world, and a game update that changes only the shipped tile must not invalidate it. The recipe version and the mod load
  order hash (`dataHash`) are in the tree key, so the entry carries the zone hashes alone; a reverted mod set finds its old
  tree again instead of a cold one. The interior hashes are in the key because the zone hash folds only qualifying
  buildings: a furniture-only change moves an interior hash and must move the whole zone entry, which serves the interiors
  too.
- Converted region: `region_<rx>_<rz>_<regionKey>.knav`, `regionKey = FNV(sha of the shipped .hkt files the conversion
  reads, conversion version)`. The "zone/interior hash" is not an input here (the shipped tile file is); the files are read
  on a miss anyway, and on a hit hashing them costs less than the check. The conversion reads the region's 16 zones plus
  the one-zone ring (6 x 6 = 36 files, fewer at the map edge), so every one of them is in the key.

**Invalidation (MINE).**
- Mod change: `dataHash` moves -> new tree. Inside a tree the zone key still moves when a mod changes that zone's
  placements, so a mod that edits one zone does not rebake the others even if the tree is shared.
- Our recipe: bump `recipeVersion` on any change to the conversion, the bake recipe or its tuning, or the `.knav` layout.
  The tuning is process-wide and not stored in a file ("Bake speed"), so the cache is only valid under the default tuning;
  a tool that changes it must not use the cache (UNSURE: a tuning hash in the tree key instead of a refusal).
  Recipe history, latest: **24** (k16.7.71), two rules. (1) A closed walkable furniture hull (`WorldShape::furniture`; 2 cells or more in x and z) keeps its top faces and loses its side sheets. The centre-based fill (fillShapes) is then its one rule: the conservative sheets took a floor cell on each side of a gap between two hulls (Noble House wall 2172 / shelf 2594: 11.2 u of hull gap came out 6.75 u in the mesh, 10.75 u without the sheets; a cell size of 1 gives the same). Other closed hulls keep their sheets: a building's floor slab (Squin Bar, General Shop) is walked onto, and its sheets are the step from the ground to its top face (dropping them cut the doorstep off); a closed obstacle hull's sheets are the keep-out of a solid (dropping them put fighters inside props: ksim_fight_prop, ksim_route_corners, ksim_storm_door; it would also open the Slave Shop's listed door, which stays a door-sweep gap). (2) A wall-band (Edge) piece whose contour simplifies away (under 3 vertices) is relabelled Land and the regions and contours are built again. Edge cells are their own regions (a region has one area), and the watershed cuts a strip one or two cells wide into small pieces whose contour collapses; such a piece left no polygon and the mesh wall moved in by the band's width (188 cells of zone 46,16 at the old recipe; the wider channels of rule 1 exposed 1278 and closed the Farm Shop route). 22 and 23 are taken by k16.7.74 and k16.7.94 in flight. **21** (k16.7.89): furniture body ring (footprint + clearance + margin nulled, stairs and use-node paths spared) and the slide height fallback; 12-19: unused (k16.7.89 rounds). **11** (k16.7.67, before the renumber): the zone key folds the gathered geometry digest and the heightmap stamp (the bake is unchanged). **10** (k16.7.85): k16.7.50 made bakes deterministic; caches baked before it are rebuilt (no mesh rule change).
  `navworld_v15` (baked 2026-11-15, `--threads 8`, before k16.7.50) is shipped data, not the patch cache, so the bump does not touch it; it may hold a tile in Recast's heap-noise variant (region_7_4 is known); not rebaked.
- Game update: changes the merged data (`dataHash`), the shipped tiles (region keys) and, where the developers rebaked
  them, the stored hashes. Nothing stale is read.
- A save never touches the cache: the hash reads the zone files and game data, not the running sim's world.

**File format (MINE: reuse `.knav`).** An entry is a normal `.knav` cache file ("File layout") on the world bake's grid and
origin; its window is the region's 72 x 72 tiles (converted) or the zone's 18 x 18 (patch), and it carries the current
cache version and the writer's Detour version. `NavMesh::fromTiles(config, tiles).save(path)` writes one; `NavMesh::load`
and `NavMesh::readTiles` read them, so there is no new format and no new validator: the loader's existing refusals cover a
format bump, a Detour mismatch and a wrong grid. The tile borders are the world bake's, so converted and patched tiles
stitch across a zone and region seam as they do in memory today.

**Load flow (MINE).**
- `convertNativeRegion`: hash the shipped files, then read the region entry if it loads (and is on the same grid and keeps
  its tiles inside its window); otherwise convert and write (temp file, then rename). Only the wanted zones' tiles join
  the mesh, as today.
- `applyStaleCheck`: the check itself still runs; a stale or unverifiable unit reads its zone entry instead of
  `bakeNativeZone`, and a miss bakes and writes. A fresh zone with no stale interior touches no entry. A failed write only
  costs the next start a rebake (the tiles are already in memory).
- Hits and misses are counted in `NativeCacheStats` (`MeshNavigator::nativeCacheStats()`) and logged once per open by
  `logNativeCache()`: the tree, region and zone hits/misses, the files and bytes written, and the unusable entries
  rebuilt ("Never silent" stays).

**The modder's building (MINE).** A mod that adds or changes a building changes that zone's placements: `dataHash` moves
(new tree) and the recomputed zone hash moves (new `zoneKey`) -> the zone is stale, our bake patches its 324-tile square
once (0.3-0.45 s measured for a zone with interiors), and only that zone's entry is written; the neighbouring zones' entries
are reused. The build reads the mod's collision parts through the same `AreaGatherer` and game data every vanilla building
goes through, so a custom building gets a bake as workable as ours is for vanilla. A custom interior layout is caught by the
zone key (kenshi-k16.7.67): `dataHash` (kworld/loadorder.cpp) hashes only the load-order files' bytes, not the layout
tables (`interiors.level`), so it does not see one; the zone key's geometry digest (below) folds the furniture shapes. UNSURE: a mod that ships its own navtiles with
matching stored hashes is used as shipped (the original's behaviour); a custom mesh the gatherer cannot read falls back to
what the gatherer sees, the same gap as on vanilla data.

**Cold start (measured today; the warm rows are arithmetic from the numbers above).**
- Empty cache, first start: today's numbers (Hub 2.90 s, +519 MB; 9 zones 5.97 s) plus writing 4 region entries (about
  40-70 MB, at the world bake's ~11 MB per region and the conversion's ~1.6x polygons) and one zone entry (~1 MB).
- v1 measured (`ksim_native_cache` on dev, the Hub 3 x 3 zones, page cache warm): cold 5.44 s, +525 MB RSS, 10 entries
  (4 regions + 6 stale zones) 79.8 MB written; warm 1.87 s, +288 MB, nothing written; a truncated zone entry logged,
  deleted and rebuilt in 2.16 s. So the estimate's 40-70 MB is low: the converted regions are the bulk of the 79.8 MB.
- Warm, tiles only (the stale check still runs): Hub 3 x 3 about 0.55-0.65 s (the 0.45 s check plus 35-200 ms reading 4
  region files, the spread being the page cache) and about +355-450 MB: the check's own data is the bulk of the 519 MB,
  less the ~95 MB the server shares with the sim (above), the converted regions being the rest.
- Warm, tiles and decisions (the UNSURE option below): 0.06-0.1 s and +40-60 MB, the `navworld` open's numbers, because no
  game data loads at all; a stale zone on the way falls back to the full path for that zone.
- World-wide (estimate, not measured): the converted world is about 2.6-4.6 GB (the world bake is 2.91 GB for 12.4 M
  polygons; the conversion runs ~1.6x the Hub's polygons) plus ~1-1.4 GB of zone patches for the 1,051 stale zones, so
  4-6 GB after most of the map is visited; a prune is needed. A world-wide cold build is 256 conversions (~4.7 min at
  1.1 s) + 1,051 rebakes (~7 min at 0.4 s) + the checks: 13-16 min single-threaded, paid as play reaches each region.
  There is no world-wide *open*: the resident limit is 25 zones / 8,192 tiles ("Why not one mesh"), so the cache does not
  change RSS beyond a region's worth.

**Concurrency (MINE).** Entries are immutable and content-addressed: a writer writes `<name>.tmp.<pid>` and `rename(2)`s it.
A reader sees the old file or the whole new one, never a partial one, and two servers writing the same key write the same
bytes (the bakes are deterministic), so the race is harmless. No lock and no shared mutable index. The only deleter in v1 is
the corrupt-entry rebuild; the prune (never built yet) would be the other.

**Prune (UNSURE; not in v1).** On open, after the first `setCoverage`: if the cache is over a cap (default 8 GB), delete the oldest
trees by last use (touch the tree root on each start). Entries inside a tree are not evicted one by one; the keys are the
point. No age-only expiry: a tree the player returns to a year later is still valid. `--no-nav-cache-prune` turns it off.

**Decisions cache (UNSURE: recommended, but a second tier).** The warm row above still pays the stale check (0.45 s and
the check's data, the bulk of the 519 MB) because the entry key is the recomputed zone hash. A per-tree manifest (`decisions.bin`) could hold per
zone the stored and our hashes, the verdict and an input fingerprint (the zone file's size and mtime plus the game-data
files the fold reads), validated by `stat` at open; a changed or missing input falls back to the check. That is what makes
the warm start 0.1 s/+60 MB instead of 0.6 s/+450 MB, but it trusts metadata: a same-size, same-mtime edit of a zone or
data file would be missed. The fallback keeps "Never silent". Default if adopted: on for the server, off for the tools.

**Open questions for the coordinator** (each with the default taken):
1. Decisions cache: adopt tier two now, or leave the check on every start? *Default: leave it out of v1, a follow-up.*
2. Tree key by `dataHash` (the content of every merged data file) as proposed, or the mod names and order only (cheaper,
   but an edited mod file with the same name would be missed)? *Default: `dataHash`; the server already pays it.*
3. Converted-region key by content hash of the shipped tiles, or their size and mtime (faster open, weaker)? *Default:
   content hash.*
4. Zone key includes the interior hashes, so a furniture-only mod change rebakes that zone once — accept? *Default: yes.*
5. Names: `~/.cache/kenshi-sim/navmesh`, `KENSHI_NAV_CACHE`, `--no-nav-cache`, `--no-nav-cache-prune`. *Default: as
   written.*
6. Is the unverifiable case cached like the stale one (they share the bake)? *Default: yes, same entry.*
7. Do the client and the sim share this cache? The client runs on the same machine but its navworld is a different
   format today. *Default: no; the client session decides its own.*
8. Prune cap 8 GB, whole trees, on open? *Default: yes.*

## The original's recipe (2026-11-04, navworld v9)

The original bakes with no erosion and keeps clearance at query time (minCharacterWidth 9, BLOCK_EDGES), keeps only what a
seed reaches, and cuts the upper floors and stairs of a building that has an interior mask (kinspect
`findings/navseed_static.md`, `navgroups_static.md`, `navinterior_cut.md`). The goal is a recipe rebake that a modded building
also gets right; it patches the shipped tiles at runtime ("Runtime mesh"), and they grade it (`knav_compare`).

- **Wall band** (`bake.cpp`): what erosion by the agent radius would remove stays walkable as `Area::Edge` (5, cost
  `kEdgeCost` 2.0, UNSURE; client approved). A gap narrower than 9 u is not blocked (UNSURE: the original refuses it at query
  time). Coarse graph v4 carries six area costs.
- **Region split** (`splitOverlappingRegions`): the band's thin strips have no distance-field interior, so a watershed region
  wraps round a stair and onto the floor above; Recast logs "overlapping regions" and drops their contours. Each region is
  split, breadth first, into pieces holding one span per column. Region 5,8: bad-triangulation contours 50 -> 24 (0 with no
  band). `layers` partition re-measured and still rejected: door area 19,384 -> 42 u2, Edge 10 M -> 56 M u2.
- **Over-full tiles**: a 32-bit dtPolyRef caps a tile at 512 polygons (13 tile + 9 polygon bits, Detour's 10 salt bits).
  A tile the band takes over it is baked again with no band, then, if still over, eroded as before v9 (`WallBand` in
  `bake.cpp`); `knav_bake` prints both counts. Region 11,4: 2 tiles without the band; tile 1001,990 is 620 polygons with no band.
- **Seed pruning** (`knav/seeds.hpp`, `NavMesh::prune`): unreached polygons get flags 0, saved in the tile. `knav_bake --trace
  X Y Z` lists the seeds that keep a surface; `--no-prune` keeps everything. UNSURE: the seeds.def drop-to-ground mask 0x600.
- **Mask clip** (`WorldShape::keepInside`, `hullPlanes`): the `building floor` >= 1 and `is stairs` parts of a walkable
  building with an `interior mask` keep only their spans inside the mask hull (the intersection of its shapes, 2 u
  tolerance). UNSURE: destroyed buildings' group mask and `destroyed collision` are not read.
- **Interior part chance 0** (kenshi-k16.6.260): an `interior` entry whose chance (v1) is 0 is always placed, as the mask is.
  32 `interior` refs carry 0 in the merged data, all group 0 and on WALKABLE buildings (31 placed; Outpost Prison's is
  never placed). Of those, two parts have collision: Outpost s-IV's interior ramp (Newwworld.mod 42261, `is stairs`, the
  way up to floor 2; rolled as "never" it left floor 2 an island, `knav_outpost_floor2`) and the Metal Wallhouse's interior
  base (42180). The other 27 are render-mesh only and 3 are a blank interior, so they add no shape. `tools/
  building_geometry.py select_parts` takes the same rule. World bake v15 (2026-11-15, k16.6.274) is the first with both:
  a bake route from s-IV floor 0 to the floor-2 bunk room ends at y 1246.29 (13 points; v14: 2 points, y 1203.4), and
  `knav_compare --at` at the goal finds ours at 1203.4 and 1246.3 against the original's 1203.4 and 1246.2 (interior
  142042f covered beyond 4 u of a wall 45.7% -> 77.5%, ours where the original has none 0.6% -> 0.5%). The Wallhouse
  base is the same collision file as its exterior base 42183 (`Metal_Wall-House_BASE-COL.xml`), so it adds no surface:
  4 of the 8 regions holding a Wallhouse are byte-identical to v14, the rest change a tile or two (zone 49,42: one tile
  of 324, +256 bytes = one polygon), and `knav_compare` at the instances is unchanged (ours with no original surface 2.1%).
- **Measured** (Hub zone 20,32, `knav_compare --radius 4`): native surface beyond 4 u covered 95.6% -> 96.4%; ours where the
  native has none 6.0% -> 5.0% (TwoStorey roofs gone). Bar stair band (house 4933, `--y 1595 1642 --radius 0 --cell 1 --tol
  6`): 72% (82% with no band). Polygons per region about 2.4x (the band is its own polygons).
- **Stair seam** (Hub bar back stair, box -51185 2565 -51095 2655; kinspect live: one ramp, `findings/navcompare_bar/`): the
  stair solid (TwoStorey shape 21) runs edge to edge with floor slab 20 (y 1645) along its SW side. A 2 u cell on that seam
  holds both: the stair span has 9 u headroom (killed by the low-height filter), the slab's is a ledge (ledge filter): a slit
  and a tilted Edge face. `knav_bake --ledge off` joins them at the wrong height (stair box surplus 15 -> 31%), `--ledge
  drops` (drop test only) leaves a hole. cellSize 1 (Hub region): full-width stair and a thin wall slot at the seam, like
  the original; bake 2.6 -> 19.8 s, polygons +21%, no over-full tiles. Open: 1 u on patched tiles. Images: kenshi-inspect
  `findings/navmesh_compare/`.
- **Detail-vertex skirt** (kenshi-k16.8.41, recipe 20): a polygon's contour overhangs the drop it was simplified from, and Recast fills a
  detail vertex there from the nearest span of any layer under it. The slab's (y 1645) edge band over the bar stair well got one at the
  stair's y 1622: a 22 u skirt a walker stood on, ending in a slit over x -51136 z 2616. Recipe: on a wall-band (Area::Edge) polygon whose corners are level
  within the climb, an unsupported detail vertex (no collision under it) more than the climb off them takes the nearest corner height (`refineHeights`). Test
  `ksim_hub_stair_seam`. UNSURE: the original has no surface there; whether it avoids the overhang or never samples across layers is not decoded.
- `knav_bake --world` prints the walkable area per area id per region.
- FEATURES rows: "Navmesh to the original's recipe", "Town-gate stairs ... walkable"; ARCHITECTURE "Non-goals" (shipped base, our patches).

## Hub bake (zone cells 19..21 x 31..33)

2,912 of 2,916 tiles, 53,404 polygons, 11.6 MB. Gather 2.7 s, bake 3.7 s on 4 threads.
135 building placements, all with at least one collision part; 325 selected parts, 191 with collision,
134 render-mesh only (skipped; Storm house "extras", "black", spike covers). 26,429 foliage instances.

### Staleness: the recipe number, the zone key and the canary (kenshi-k16.7.67, 2026-11-17)

The review found staleness resting on a hand-bumped constant (`kNativeCacheRecipe`, navigator.cpp; k16.6.260 changed placed
parts with no bump) and a narrow key. Now:

- **Zone key** (`zoneCacheKey`) = recipe, the recomputed zone hash (integer position + destroyed), the interiors' hashes,
  plus `kworld::areaDigest(gatherer.gather(zone, zone))` and the heightmap file's size + mtime. The digest folds, for the
  3 x 3 zones the bake gathers, each building's id, full-precision pose (the rotation, scale), its nav fields and every
  shape's vertices, indices, use and frame (so a swapped collision mesh, a rotated building, other furniture or foliage moves
  it), and the foliage shapes. The heightmap is a whole-world file, so a change to it invalidates every zone entry. Cost: a
  zone entry lookup now gathers the zone (the gatherer's game data is loaded at open; model loads are cached). Region
  entries are unchanged (they hash the shipped tile bytes). Recipe 11.
- **Canary** (`ksim_nav_cache_canary`): bakes the Hub zone (20,32) fresh, cache off, and pins its digest beside the recipe
  number (`kPinnedRecipe`, `kPinnedDigest` in the test). When the bake changes the digest moves and the test fails with "bump
  kNativeCacheRecipe"; the fix is the bump (new line in the recipe comment) and both pins updated from the test's printed
  line. Shown once: `setKeepWindowEdge(false)` with no bump fails it (digest 0x9ad7b36c7ea3aca7), reverted.
- Not covered: the digest is of one zone; a change that only shows in another zone's geometry would pass it. Pin a second
  zone if a recipe change ever slips by.

## Streaming coverage (`MeshNavigator::openWorld`, `ksim_server --navworld DIR`, 2026-11-02)

The full-detail navigation of the server is one `MeshNavigator`. With `--navworld DIR` it is built over the world bake and
holds only the zones of the players' full-detail areas (`Simulation::syncNavCoverage`, `src/ksim/nav/navcoverage.cpp`; run
RUNBOOK "Navmesh that follows the players"). Without the option nothing changes (Hub bake or `--navmesh FILE`).

- **Unit: the zone.** A sim region is one zone (4,608 units = 18 x 18 tiles), the full-detail area of a player is the 3 x 3 regions
  around its region (`RegionConfig::areaRadiusRegions`), overlapping areas merge. The wanted zones are the zones of
  `RegionEngine::areas()`. `MeshNavigator::setCoverage(zones)` makes exactly those resident: at most `kMaxZones` = 25 (25 x 324 =
  8,100 tiles, under `kMaxResidentTiles`; more are left out, the later ones in area order, `CoverageChange::overflow`).
- **Mesh swap in place.** The mesh is one `dtNavMesh` for good (grid = the whole world, so `maxTiles` is the 8,192 limit). A change
  is `NavMesh::replaceTiles(removed, added)`: tiles of dropped zones are removed, new ones are read from the region files
  (`NavMesh::readTiles(paths, keep)`: only the tiles inside `keep` are read; the rest of a file is seeked over) and added;
  Detour links them to their neighbours across the zone and region seams (tile coordinates are shared). The `MeshNavigator`
  object, its `Walker` and query stay the same, so every holder of the shared pointer sees the new tiles.
- **When.** After the commands, and after `stepPlayerAreas` (before `stepResidents` builds anyone on the new areas), once per tick
  (a compare of the wanted set; the files are touched only on a change); in `enableNavigation` and at the end of `reload`, so a
  loaded game has its mesh before anyone routes. While there are no areas (before NEW_GAME, a scenario) the mesh keeps what it holds.
  A NEW_GAME adds the 3 x 3 zones of each place of its start-off first (`checkNewGame`), so the start can be tested; the
  start list says `ready` for a place whose zone has a region file (UNSURE: it is not loaded to see whether the place is on the mesh).
- **Derived, never saved.** The coverage is rebuilt from the saved `PlayerAreas` (test `ksim_nav_regions`: save, load into a fresh
  mesh, same zones, same hash, same continuation). A save never records which zones were resident.
- **Routes in flight.** A route is a list of points (`Fighter::route`, `DoorStep::route`), never Detour polygon refs, and the
  walker re-attaches to the surface every step; no cache of refs, connected sets or cluster graphs lives in `MeshNavigator`, so a swap
  breaks no walk, route, `InteractTask` or door step whose points stay on resident tiles. What does go stale: a route (or a click
  goal) that passes over a zone that was dropped (the character stops at the edge of the mesh: off mesh = no move); a goal beyond
  the resident zones is refused at order time (`no_route`) as an off-mesh click. Detour refs held by a caller across a tick
  (none are) would be invalid after a swap: tile slots are reused with a new salt.
  The AI's `KnavNavigator` (squad AI over `--bake`) is a separate object and is not touched.
- **Doors.** `NavMesh` keeps the door book it was last painted with and re-applies it to every tile `replaceTiles` brings in
  (`indexDoors`), so a returning tile has the same paint; test: a locked door's tile blob is byte-equal after drop and return.
  The sim's `state::Door` entities are made for the doors of newly covered zones (`furnishNavCoverage` ->
  `spawnMeshDoors`, `spawnMeshContainers`, both skip what already has an entity, by door key / building+position). The entities
  of a dropped zone are **kept** (state, health, lock level are saved game state; they are inert without fighters near);
  `syncDoorPaint` paints the whole book on the mesh, doors of absent tiles are simply not found. Gates start open
  (ff4017c), as at the Hub. A load clears the `furnishedZones_` memo and re-checks, never duplicating the saved entities.
- **Tile stream.** `Session` iterates `mesh().tiles()`: a new tile is sent (`NAV_TILE`), a dropped one leaves the iteration and goes
  as size 0; a tile that came back has a higher version (`replaceTiles` bumps it, no patch chain across it), so a client that
  kept the old one gets the new bytes. `meshId` is not changed by a swap (SIMLINK section 12). The STARTS list is re-sent when the
  tile count changes.
- **Cost (measured 2026-11-02, Release, region files in the page cache).** Hub 3 x 3 zones (2,897 tiles, 45,038 polygons) opens in
  35 ms (read 18 ms, `replaceTiles` 12 ms). Moving the window one zone (3 in, 3 out, +969 / -964 tiles): 17 ms (read 8, apply 8).
  A jump to 9 other zones (the teleport in the test): 24-34 ms. The whole test (50 zones in, 25 out) spends 60 ms reading and
  89 ms applying. Synchronous on the tick, once per zone change; a hitch of a few ticks at most (not measured in a live
  session). Cold files add the disk read of the tiles (10 MB per region file, seeks skip the rest): UNSURE how much on a
  spinning disk, not measured.
- **Native source (`MeshNavigator::openNative`, `ksim_server --native-nav`, 2026-11-05).** The same coverage, mesh swap and zone
  accounting over the game's shipped navtiles converted at load instead of a `knav_bake --world DIR`: a region (4 x 4 zones) is converted
  the first time one of its zones is wanted (the `knav_native` recipe, in memory; the region's tiles plus a one-zone margin, cut into
  the same absolute 256 u grid), only the wanted zones' tiles join, and a converted region is kept while one of its zones is resident
  plus the two most recently used regions (a step back across a border does not convert again); the rest are dropped, so the cache is
  bounded (ksim_native_nav walks A -> B -> C -> D over far 3 x 3 blocks: RSS 140 -> 185 -> 244 -> 244 -> 245 MB, the peak being the
  allocator holding the freed conversion temporaries). No disk cache yet; the patch cache is the next task. The Hub's 3 x 3 zones (4 regions) with the stale check: 2.90 s to open, 5.97 s for the 9 zones, 2,859 tiles, 91,012 polys,
  peak RSS +519 MB (the region caches and the check's game data; before the check 1.80 s, 2,848 tiles, 57,533 polys, +135 MB;
  navworld_v10 in 54-77 ms, 2,899 tiles, 113,156 polys, +39 MB). Street -> bar floor 1 at radius 4 in 0.6 ms, 390.5 u, ending on
  floor 1 (y 1646.1): the Hub's shipped tile is stale, so the tile is our bake's (the shipped one took 365.2 u).
  Tests: `ksim_native_nav`; since kenshi-k16.6.252 the streaming world tests run on the native source by default (tests/nav_source.hpp; the
  nav cache in the build tree) and `KSIM_NAVWORLD_BAKE=1` (non-empty) selects the bake; `ksim_nav_regions`, `knav_interior_walk`, `ksim_route_soak` stay on the bake. `ksim_nav_regions` passes on it now (doors keyed from the placed buildings).
  **World-test triage (2026-11-15, kenshi-k16.6.250; 76 of 84 pass native).** Made source-agnostic: `ksim_cage_wire` (the character ids
  come from the shared counter, which the coverage history moves; the pinned frames compare with both ids blanked) and the start of
  `ksim_slaver_deliver` (a real start height). Still failing on native, real gaps of the runtime mesh (not fixed):
  1. *Sealed pockets are not pruned* (fixed, kenshi-k16.6.270, patch cache recipe 9). The bake cuts every region no seed reaches (`NavMesh::prune`);
     the rebaked (stale) zones kept pockets under platforms and inside buildings, so a route target without a height snapped to a pocket
     and found no route (`ksim_bw_orders`, `ksim_slaver_deliver`: fail native without the prune, pass with it; kenshi-k16.6.272 was this cause, the camp building target in a
     sealed pocket so the hand-over route never existed). Fix: `bakeNativeZone` prunes
     the zone with its own seeds (`zoneSeeds`: seeds.def or the 3 x 3 probes; building origins; door outside points; the shipped tiles
     carry no reachability, the game prunes at its own bake) and keeps every set that touches the zone's outer border
     (`NavMesh::prune` `keepWindowEdge`: a portal edge to a tile outside the window; the set may join a seed through a neighbour). The cut is
     written into the tile flags, so the zone cache entry carries it: a pure function of the zone, no per-coverage-change cost. Shipped tiles
     are left as the game pruned them. An earlier attempt pruned the resident set after each `setCoverage`; it depended on the coverage window
     (a connector reachable only through a non-resident zone was cut: two false islands in `ksim_nav_islands`), so it was dropped.
     `ksim_world_hub` was not a mesh fault (k16.6.293, TOWN_LIFE_SLICE.md 5.14 "No terminal state").
  2. *Mesh surface vs the terrain heightmap* (cause found and fixed, kenshi-k16.6.271, 2026-11-15). The shipped polygons do deviate from
     `terrainHeight` by up to +-15 u on slopes and 29 u past a cliff at (-51300, -1004), but the walker is not what failed (a walker's y follows
     the mesh, as in the original: HAVOK_NAV.md "Walking height", terrain +-6 on open sand, +39 on stairs). The failing rule was
     `floorApart` (combat.cpp, `kFloorApart` 20 u): any two bodies more than 20 u apart in y were "on another floor", so `notDirect` sent the
     chaser into the routed Approach (states 10 / 11) and the lead steer (`!apart`) was off. Probes (debug prints, since removed):
     `ksim_chase_intercept` faster chaser starts 250 u from the runner on a slope: mesh y 522 vs 501 (21 u apart, bake 15), state 11 from tick 0
     (bake: Circle), it ran down the runner's line, passed it head-on at 1.5 s without a swing, then trailed it. `ksim_chase_scen` run L: the
     bake chaser lost ~1 s at the cliff (x -51317: y 968 against the player's 990 on the slope above, 22 u apart: states 11 -> 4 -> 3, speed
     reset 99 -> 10 and re-ramped), which is what put its swing at 9.15 s; the native chaser stays on the 975 u ledge beside the player (7 u
     apart), never stalls and swung at 7.85 s (the test's 8.15-10.15 window pinned the bake's stall). `ksim_spectate_fight`'s 5.2x step was the
     same switch flipping mid-fight. Fix: a height gap is a floor only if it also exceeds a hillside's slope allowance over the xz distance
     (`kFloorSlope` 0.4 x hypot(dx, dz), tuned: 0.84, the mesh's own 40 degrees, merged the tower's floors in `ksim_melee_floors`, 0.15 left native `chase_scen` / `spectate_fight` failing; a `lineClear` test broke `ksim_stair_chase`); stacked floors (80-127 u apart, a stair's xz) still count. With it the three tests pass
     on both sources. UNSURE (ours): the 20 u floor and the slope rule are ours, not the exe's (its test is a navmesh line of sight; kenshi-k16.6.295); verify by
     `KSIM_NATIVE_NAV=1 ctest -R 'ksim_chase_scen|ksim_chase_intercept|ksim_spectate_fight'` and `ksim_stair_chase`. Widening the walker's
     `kGroundSnap` to 40 was not the fix (the chase got worse).
  3. *Placement keeps the mesh height* (fixed, kenshi-k16.6.273). `ksim_feet_ground`: a world resident placed by `Walker::snap`
     (`residents.cpp` placement, character 5062 at (-50801.7, 3232.9): mesh y 1527.30, terrain 1526.16) stood at the mesh's y, 1.14 u
     above the terrain, and never moved (idle: nothing steps it, only a walking step took the terrain). Rule chosen: snap to the ground,
     as a step does. Evidence: the client draws the terrain heightmap, so a character on the mesh surface floats; `Walker::step` already
     settles to the terrain within `kGroundSnap` (kenshi-k16.7.14). The bake never showed it because its detail mesh was already
     terrain-height there. New `Walker::settle(p)` (same 5 u rule); used through one helper, `Simulation::placeOnGround` (snap, then settle only when `buildingAt` is 0: a
     floor keeps the mesh height), by the resident, scenario-file, `import_squad` and `save_import` placements (kenshi-k16.6.296;
     `ksim_place_ground` pins open ground and a building's ground floor). Not applied to the door/container/bed snaps
     (they stand on floors).
- **Determinism.** The mesh is a pure function of the resident zone set (the same tile bytes whichever order they came in; the
  Detour link order may differ, which could change the tie-break between two equal-length routes: UNSURE, not seen). Entity ids
  of doors and containers are taken from the shared counter when their zone is first covered, so two runs with different
  coverage histories number later entities differently; the same inputs give the same ids and the same hash
  (test: a walk from the Hub start gives the same track whatever else is resident at the start, and save/load continues identically).
- **Limits (UNSURE / open).** More than 25 zones wanted (a scattered multiplayer party) leaves the later areas without a mesh.
  `RegionConfig::fullDetailAll` is not streamed. A character that leaves the resident zones (chased out of the area) stands off
  mesh until it is demoted (a world squad that is not Full any more is demoted even while fighting off the mesh, and `position_corrected`
  distance -1 is logged once per episode: TOWN_LIFE_SLICE 5.14 "Second NEW_GAME and off-mesh fights"). No thread pool: the load is synchronous.
- Related: SIMLINK.md section 12 (tile stream), ARCHITECTURE.md "Pathfinding", RUNBOOK.md "Navmesh that follows the players".

## Island test (`ksim_nav_islands`, kenshi-k16.7.40; classified k16.7.43, 2026-11-15)

Slow ctest (`RUN_SERIAL`): walkable ground of the runtime mesh (`openNative`, nav cache on under `<build>/nav_islands/navcache`, key = the load order's `dataHash`) that the open world cannot reach.
Motivated by the Heft Longhouse interior (k16.6.229) and the Outpost s-IV ramp (k16.6.259).

- **Graph.** The world streams through in 256 blocks of 5 x 5 zones (a region plus one zone of overlap east and south, so every adjacent zone pair is in a block). Per block `knav::PolyComponents` over every area
  (doors open; off-mesh links are Detour links) gives sets; a world union-find over (tile, polygon) joins the blocks. 1.30 M tiles, 17.7 M polygons.
- **Main component** = the largest area. Every other component is an island.
- **Dropped, each by rule:** (1) *clipped*: polygons whose centre is more than 40 u under `Heightmap::terrainHeight` do not count toward area (a bake artefact under the ground; 3,686 components were nothing else);
  (2) *small*: area under 10,000 u2 (one room's floor; `--min-area A` probes lower); (3) *water only*: dry area (not Shallow/Swim) under the threshold (a swum sheet; the world-edge ocean strips and 1,089 others).
  "Sealed inside solid collision" is not a separate rule: the bake fills solids, no island needed it. 10,000 u2 is kept (k16.7.43): both known cases are above it (Heft Longhouse 25,194; s-IV 38,066) and a lower floor only adds room slivers.
- **Report** `<out>/nav_islands.tsv`, ranked by area: centroid, y range, box, owning building (the smallest interior-mask hull that holds the centroid, `Simulation::buildingAt`'s rule, INTERIORS.md: record, placement id,
  height above its base and the hull's top; the footprint for a maskless building with a door; a wall's collision box over terrain is not a building) or "open world", the island's *source*, nearest main polygon and the gap
  (centre to centre), baseline status.
- **Source** (k16.7.43): `shipped` (every polygon in a tile the loader kept: the original's own mesh), `rebaked` (a tile the loader replaced with our bake: a rebaked zone's whole 18 x 18 square or an interior's own square in
  a fresh zone, `NativeTileReport::rebaked`), `mixed` (spans both), `shipped?` (a zone with no report). An island in shipped tiles is the original's; one in a rebaked zone is ours until shown otherwise.
- **Gate.** The full list is mostly terrain pockets behind cliffs (8,694 of 8,778 candidates are not flat sheets), so only *gated* islands are compared with `tests/data/nav_islands_baseline.tsv`: those inside a building (mask hull or
  gated footprint, above), and open-world ones over 1,000,000 u2. A baseline line is `record floor_y area x z source accepted|bug|pending why`, keyed by owner record (or `open world@ZX,ZZ`), floor height within 8 u and area within a factor of 1.3, one line per island (k16.7.66; it was a centroid within 60 u, which drifted with a recipe change and let one line shadow a `bug` line beside it: 161 of 573 lines matched no island, and a `bug` line at 727 zone 47,44 was hidden by an earlier `accepted` one; the 573 lines became 527, one per gated island, 8 `bug`; the `bug` line of 727 zone 11,54 (17,790 u2) no longer has an island of its size). `accepted` = the original has the same island (or it is not a place the world
  should reach); `pending` = whose why names the oracle question that decides it. The test fails only on a gated island not in the file. `--write-baseline FILE` appends the new ones as pending.
- **Classes** (k16.7.53; 578 gated with the owner fix below, every one in the baseline, none pending). Roof/top, 53 world-edge sheet, 56 open-world terrain behind cliffs, 19 shipped navtile, and the 102 once-pending floors of rebaked zones, now
  decided by `--oracle` (below; k16.7.59 cleared 7 of the 20 `bug` lines): 20 `bug` (18 after k16.7.57's interior-only sampling; 19 of the 102, plus 3793's; the original's tile joins the floor to its main mesh; ours does not), 77 ours-only floor (19 with only another storey's surface within 60 u, 58 with no shipped polygon under them),
  7 shipped island. The 5312-TwoStorey ~300k u2 pockets 20-147 u under their base were terrain under a very large mask hull, not floors (40 gated islands in all were far under their owner's base): the owner now says "open world" for an island more than 15 u under the owner's base
  (`kHullTerrainDepth`), so they are ungated (open-world terrain pockets under 1,000,000 u2).
- **Oracle** (`ksim_nav_islands --oracle`, k16.7.53). After the runtime pass it frees the graph and walks the whole world again on `NativeCacheOptions::shippedOnly` (the shipped navtiles as converted, no stale replacement: 14.06 M polygons, the original's own
  main component). Each gated island's floor is sampled (up to 96 of its polygon centres, only polygons at least `kOracleBand` = 4 u inside the island: xz distance from the centre to the polygon's own wall edges, k16.7.57; an island with none that deep samples all its polygons and says `all polygons (none 4 u deep)` in the `sampled` column, none of the 578 did) with `findNearestPoly`, 1.5 u each way in xz: the nearest shipped polygon (3-D, within 60 u in y) is "the same floor" within `oracle-band` = 16 u in y (main component or an island of the shipped world; was 8 u before k16.7.56), else "another height". Verdict in `<out>/nav_islands_oracle.tsv`: under 30% of the points covered = `no shipped floor` (our bake adds the surface); else at least half the covered points on the shipped main = `connected` (a bug line; `connected (other height)` when most of
  it is the 60 u hit: treated as ours-only); else `shipped island`. Why interior only: a rim polygon within 1.5 u of the outside terrain reads as connected to the main mesh although the floor behind the wall is not (14535 zone 34,29: 16 of 36 'main' samples were rim polygons). k16.7.57 re-run over the 578 gated islands: 18 of the 20 `bug` lines stay `connected`; three baseline lines change. 14535 zone 34,29 (11509.8 -13820.5) `no shipped floor` (2 of 10 interior samples covered): bug -> accepted ours-only floor. 14535 zone 34,28 (11969.2 -14394.9) `connected (other height)` (1 of 6 at the floor's height): bug -> accepted ours-only; the k16.7.55 ramp finding on that line is kept in its note and not re-decided. 727 zone 18,51 (-60981.2 91379.0) `connected (other height)`: shipped island -> accepted ours-only. 10 other ours-only lines move from `no shipped floor` to `connected (other height)` (still ours-only, text not edited). Now 18 `bug`. Limit (closed in k16.7.56): a floor our bake puts more than 8 u off the original's was read as "another height", so a lost link there was not seen. See "Height band and re-decision" below.
  Adds about 4 min cached (total 245 s warm).
- **Height band and re-decision** (k16.7.56, 2026-11-17; the whole world on origin/ksim 34e5e569 before this change: 179 gated islands, not the 578 of k16.7.53: recipes 7-8 and the foliage fixes removed the rest). The oracle now asks one query per sample (nearest shipped polygon, `kOracleXz` = 1.5 u footprint, 60 u in y) and keeps its height offset, so one run reports every band: columns `main_within_dy_8_16_24_32_48_60` / `island_within_dy_...` (cumulative sample counts by |dy|) and `verdict_8u` (the old rule) beside `verdict` (band `--oracle-band Y`, default 16 u: half the 32-34 u spacing of the floors of 3793-TwoStorey at 4, 36 and 70 u above its base, so one band is one storey).
  Effect on the ours-only lines (28 `no shipped floor` + 12 `connected (other height)` at 8 u): band 16 moves 2 to `connected` (4014-TwoStorey ground floors at 4.6 and 3.7 u above base: the shipped floor is 9-16 u off); 24 moves 4, 32 moves 7 (the 1111-gamedata floors 24-32 u off are a different storey as far as the data says: not moved). `no shipped floor` stays 28; `shipped island` 84; `connected` 55 -> 57; `connected (other height)` 12 -> 10.
  Baseline matching is k16.7.66's (record / floor / area, closest unused line); the k16.7.56 "nearest line" change is dropped as superseded.
  **Re-decision of the accepted lines the oracle calls `connected`** (55 on this base, after merging k16.7.66's keyed baseline: counts unchanged; the 226 of k16.7.53 were a different population, the 578 gated islands before recipes 7-8): a class that says "no route by design" / "terrain, not a route" / "the original's own navtile" cannot hold when the original's mesh joins the floor to its main. All 55 became `bug` lines (57 with the two of k16.7.55; each says `k16.7.56, was <class>`): 36 roof/top, 7 shipped-navtile, 7 world-edge sheets, 3 open-world terrain pockets, 2 ours-only floors. By building: 4014-TwoStorey 31 (29 roof/top + 2 shipped navtile: floors 1.3-5.2 u above its base, 17-33 u from the nearest main polygon, nearly every sample on the original's main: the ground floor is cut into about 30 pieces in ours, not roofs), 52180-Newwworld 3, 56173-Newwworld 2, 5212-TwoStorey 2, and one each 57740, 48729, 5312, 3793, 3795; the 7 world-edge sheets (shipped tiles; the sheet is on the original's main, ours has it apart: a rebaked neighbour zone is the suspect, 226-256 u from the nearest main polygon) and 3 open-world pockets; 14535 zone 34,28 wall-walk floors at 93 u (19 of 19 and 10 of 10 samples at the same height: the two lines the k16.7.58 ramp fix removed are back at 12130 -13843 and 11533 -13829 on this base). Not examined, no bake change. The `connected (other height)` lines (10) stay accepted ours-only with a note.
- **3793-TwoStorey x3** (-71422 25534, -61762 -103495, -61086 -103548; floors 35-36 u above a base with its top at 85-88: a floor, not a roof): not new meshes. The mesh-affecting commits in f81860ba..a6910670 are none (k16.6.273 touches `knav/walk`, not the
  bake); they entered the gate with k16.7.43's owner rule (mask hull: the k16.7.40 file listed one 3793 line of the group, at -61175 -103809). Oracle: -71422 is `connected` (49 of 51 points on the shipped main: bug line), the other two have no shipped floor.
- **The 20 'bug' lines (k16.7.55, 2026-11-15; no bake change, recipe stays, no world rebake).** Probed with a map of the runtime and the shipped mesh per component (window runs: `ksim_nav_islands --zones 32 26 36 31 --oracle`, 18 s).
  - 14535 zone 34,29 (ground, 2 u above base): not a lost link. The floor is the ground inside the compound's wall (shape 9, an open walkable slab; its rim is a vertical skirt). Ours has a 2-4 u unwalkable slit at the rim
    (terrain wall band | rim | the plate's own band); the shipped tile has no ground inside the wall at all (nothing at y 380-500). The oracle's 16 of 36 'main' samples are rim polygons within `kOracleXz` (1.5 u) of the outside terrain:
    an edge artefact of the oracle (it should sample only polygons a wall band, 4 u, inside the island). Fix to file: oracle sampling, not the bake.
  - 14535 zone 34,28 wall-walk floors (y 541, 93-95 u above base): the shipped mesh joins them to the ground by a ramp near (12309, 506, -13722); ours keeps the ramp slab (shape 5, a 45 degree solid) as its own island. **Fixed in k16.7.58 (recipe 7).**
    Cause (k16.7.58): the ramp is a 47 degree face (ny 0.68) of an *open* walkable shell (52 triangles, `closed` 0, `solid` 0: the wall-walk building's collision mesh), so it got the 40 degree walk limit and its spans were unwalkable
    (rasterizer span dump after the volume fill: area 0 on the whole ramp, 63 on the floor above and the ground below). Only closed solid hulls (`WorldShape::solid`) had the 55 degree stair limit (`kStairSlopeDegrees`). The small 45 degree solid
    beside it (12355..12373) was walkable all along; it is the sliver the island map showed.
    Fix, narrowed from the data (`knav_slope_probe`, world-wide over the placed shapes against the shipped tiles: a face is walked when a shipped polygon covers its centroid within 1.5 u xz and 8 u y): giving *every* open walkable sheet the
    55 degree limit over-walks (of the 40-55 degree faces of open building sheets the shipped mesh walks 20%, of foliage rocks 6%), but a *ramp strip* is walked (87% of its area): `rampTriangles` in bake.cpp marks the faces between the walk limit
    and the stair limit that form a strip (edge-connected by shared vertices) whose lowest and highest vertices both meet a flat walkable face of the same sheet (a run from one walkable level to another), skipping foliage shapes
    (`WorldShape::foliage`) and closed or solid shapes; the rasterizer takes those faces as walkable (`RC_WALKABLE_AREA`) and the height oracle includes them at the stair limit. A roof pitch joins flat faces only through faces the roof already walks.
    Result: the 541 lines (11533 -13829 and 12130 -13843) are gone; the 11969 -14394 line (floor 540-546, 14342 u2, 22 u from the nearest main polygon) stays, a different cause (ramp not seen in the shipped or our mesh; k16.7.60).
    Whole world: no new island but one, a 10578 u2 top surface of 54019-Newwworld (208 u above base, next to the 13202 u2 one; roof, accepted). Patch cache recipe 7 (`kNativeCacheRecipe`; k16.7.59 took 8 on merge): rebaked tiles re-bake on first load;
    a world bake (`knav_bake --world`) changes the zones holding such strips, so a rebake is advisable but not needed for the runtime (rebaked zones re-bake themselves).
    Ruled out: the ledge filter (`BakeLedge::Off`: islands unchanged), door mats (no doors), T-junction cracks (none in the window), unlinked tile portals.
  - 5312 zone 19,44: y 100-300 shatters into dozens of components where the shipped tile is one mesh; not tile seams (20 of 927 portal edges unlinked, slivers). **Fixed (k16.7.59, recipe 8)**: a foliage canopy shell cut every
    height (Foliage, above). Not the voxel size, wall band, region merge, step height or the lab's interior parts: `--ledge`, `--partition`, `--radius 0`, no mask, no ramp gate, no barriers each left the band; skipping all foliage or cutting the shell per triangle closed it (found with `knav_draw --mesh <navcache zone entry> --components --probe X Z`).
    The same fix removed 47 baseline lines (all five 5312 bug lines, the two swamp 48727 bug lines at zones 19,41 and 18,45, and 40 accepted lines: terrain pockets, roofs and ours-only floors of the lab) and added five ours-only lines (pieces that joined past the 10,000 u2 gate or the 1,000,000 u2 open-world gate). 13 bug lines are left (15 islands: two lines match two each); cold full run with `--oracle` 797 s, `ksim_nav_islands` ok.
  - 40591/52180 zone 31,26 (x4), 48727 zone 19,2, 727 zone 47,44 (k16.7.60, 2026-11-17, dev box, recipe 9, no bake change): the six islands are **absent from a dev full run** (`ksim_nav_islands --oracle`, cold cache, rebased on ksim e5cca395: 1247 candidates, 179 gated, source 76 shipped / 91 rebaked / 12 mixed). Dev matches the re-keyed baseline in one direction only: every gated island has its line (0 not in it; all 76 shipped and 12 mixed lines match), but 348 of the 537 lines have no island on dev (342 accepted rebaked lines, almost all in buildings, and these six bug lines; 89 of 431 rebaked lines match). Windows around zone 31,26 show none either. The dev load rebakes 1,290 zones and 395 interiors (stale or unverifiable) like the desktop's documented counts, so the world data looks alike; why the desktop run sees ~340 more rebaked islands is not known: re-probe those six on the desktop cache. Cause: not found.
  - 3793 zone 16,37 (-71422 25534) and 48729 zone 9,51 (-104500 92056), the two bug lines that reproduce on dev (k16.7.60; `knav_draw --mesh <navcache zone entry> --components` against `--navworld`, and a scratch probe listing the shipped tile's linked edges): each floor is an interior mesh (3793: info a0951, 79 faces; 48729: 450cca, 223) joined to the zone mesh by exactly one streaming link at its door: the zone mesh's open-door face (faceData 4) has the linked edge, and the faces around it run down a ramp to the main ground (3793: 16 u run, y 1648 to 1632, 42 degrees in the middle; 48729: y 159 to 142, the tower's approach ramp). The link itself is not the difference (our interior floor sits where the shipped one does): ours breaks the way *to* the door. 3793: the lower stair piece ends at z 25454 (y 1640.7), the upper one starts at 25456 (y 1642.7), a 2 u crack in the entry stair whose middle is over the 40 degree walk limit; 48729: the ramp polygon and the tower's rim ring touch at the door but are separate components in ours, one in the shipped tile. Both read as cracks of a few u along the entry path of a rebaked building, not whole missing surfaces; the 3793 middle slope fits a ramp the k16.7.58 rule does not take (its bottom end may meet terrain, not a flat face of the same sheet), unverified. Next: dump the shape under the 3793 stair (`knav_slope_probe` over region 3,7) and the 48729 door join. Cause: not found; ruled out: a whole-floor bake cut, tile seam, door mat, the ramp slab of k16.7.58.
  - Each line carries a `k16.7.60:` note.
  - **Baseline repin (k16.7.85, 2026-11-17, recipe 10, cold full `--oracle` on dev, twice: same island set).** 1247 candidates, 179 gated (76 shipped / 91 rebaked / 12 mixed): the same 179 as before the bump, every one has its line, so the recipe change moved no island. The 348 baseline lines no island matched are dropped (527 -> 179): 341 accepted rebaked building islands (269 roof/top, 70 ours-only floor, 2 shipped-island), 1 accepted open-world terrain island, and 6 `bug` lines (40591/52180 zone 31,26 x4, 48727 zone 19,2, 727 zone 47,44; 'ours lost a link'). They were most likely bake noise from before k16.7.50 (rcBuildContours reading heap bytes), not world cracks; the dev run never reproduces them. The two `bug` lines left are 3793 zone 16,37 and 48729 zone 9,51 (k16.7.60 notes kept).
- **Self-checks** (a window of blocks; they fail unless an island does, or does not, lie there):
  - Longhouse, `--zones 44 14 48 18 --expect 68500 -70747 150`: on a build with recipe 5 reverted (`kMatStairDrop` 0, recipe constant 4) the world has an island of 25,194 u2 at (68443, 1415, -70716), building 3384-TwoStorey
    (46,16/7, the Longhouse at the entrance 68500 -70747), 140 polygons; with recipe 5 it is gone. Zone 46,16 is rebaked with our recipe (stale hash), which is why recipe 5 reaches the runtime mesh.
  - s-IV, `--zones 44 14 48 18 --expect-absent 67678 -71502 30 10000`: passes; with k16.6.260's selectParts chance-0 rule reverted (an `interior` part of weight 0 not placed) there is an island of 38,066 u2 at
    (67677.8, 1246.5, -71501.5) and a plain `--expect 67678 -71502 30` passes. The area filter is what pins it to that floor: the same building's other floors stay islands 5-25 u away (35,834 u2 at y 1167.7).
- **Cost.** Cold (no cache; conversion + rebakes): 18 m 19 s. Cached: 3 m 53 s to 4 m 25 s (233 s and 265 s, two runs). Peak RSS 7.7 GB (the per-polygon table is 17.7 M x 24 B; the owner lookups and nearest-main search add the rest), so `RUN_SERIAL TRUE`:
  beside a `-j12` suite on the 31 GB dev box that peak can exhaust memory.
- Not done: the s-IV shape (a stair/ramp collision shape with no walkable polygon on top) is not an island and this test cannot see it (k16.6.260's probe).

## Shipped-tile gate (`ksim_nav_shipped_gate`, kenshi-k16.7.66, 2026-11-16)

The runtime mesh is the original's tile where the stale check passes and our bake where it does not, and the island test and the door sweep see under-connection only. The gate holds our rebake of **every** zone that ships a tile and is rebaked (the zone verdict, `kworld::staleVerdict`) against that same shipped tile, both ways, on every run of the slow suite (4 min 51 s with the nav cache warm, 13 min 48 s cold). `cpp/tests/ksim_nav_shipped_gate.cpp`, baseline `cpp/tests/data/nav_shipped_gate_baseline.tsv` (a world line and one line per zone); the scoring is `knav/compare.hpp` (moved out of `knav_compare`, same method: 4 u cells here, tolerance 30 u, wall radius 4 u).

- **Covered %**: the original's surface at least 4 u from its own walls that our bake covers. **Surplus %**: our surface the original has no surface under (over-walk: roofs, wall tops, ground under a building, sea floor).
- **Connectivity**: up to 160 polygon centres sampled on each mesh per zone, paired (40-500 u apart), kept when both meshes have a polygon within 2 / 6 / 2 u of both points; reachable (same Detour component, doors open) in one mesh and not the other: **under** (shipped yes, ours no) or **over** (ours yes, shipped no). The zone and its four neighbours are resident, so a pair may cross a seam.
- **The gate fails** when the rebaked-zone count grows, the world cover falls or the world surplus rises by more than 0.005 points, the world under or over count rises, or a zone loses more than 2 points of cover or gains more than 2 of surplus. A recipe change that improves is checked in with `--write-baseline FILE` after the run is read. `--zones X0 Z0 X1 Z1` runs a window (no world gate); `--cell C` changes the grid (the baseline is at 4).
- **Today's recipe (patch cache recipe 8), 970 rebaked zones** (not 1,051: the grass mask, k16.7.13, took the stale scan from 1,036 to 970; the review's 1,051 is the 2026-11-05 figure): covered **87.31%** (931,029,163 of 1,066,396,732 samples), surplus **14.86%** (165,996,739 of 1,117,437,964), connectivity 145,090 pairs, **under 1,033, over 615** (288 zones have an under pair, 94 an over pair). 160 zones cover under 80% and 41 under 50% (the worst: zone 6,26 at 2.2%, 56,31 at 4.7%, 30,56 at 6.1%, all with no folded building; **cause found, kenshi-k16.7.75: open deep sea, not a gap in the rebake.** `knav_compare --zone` on each: ours has 1.32 M samples against the original's 1.32 M, and 94-98% of ours is "at another height in the cell" (|dy| p50 90-98 u): the original meshes the water plane (flat y = 100.0, `--at` gives native 100.0) where the sea is at least 15 u deep, ours keeps the sea floor (y 2.8 at -117500, -25000, depth 97) on purpose ("Water" above: swimmers cross it, `swims = false` races walk it). The 30 u height tolerance therefore scores the whole zone as a miss and as surplus. Nothing is stranded: ours covers the zone. The same mismatch is in the partly-sea zones (41 zones under 50% cover, about 30 of them with surplus 50-80%); it is not the forest-only case of k16.7.74, and no recipe step, seed, prune, grass mask or height band is at fault. Fixing it in the bake would mean a second walkable layer at the water plane over deep water (a swimmer's surface; a design choice for ksim: path heights, `swims` races and the polygon count all change), so the baseline is not repinned. The honest alternative is in the gate: count a sample of ours in a Swim area under the plane as matching a native sample at y = 100); 116 zones have a surplus over 30%. The 221 zones whose reason is the buildings score 89.4% / 14.1%, the 749 forest-only zones 86.7% / 15.1%: the rebake of a zone with nothing built in it is worse than the tile it replaced.
- **Repinned for recipe 9 (k16.6.270's per-zone prune, the only mesh change since the first baseline; kenshi-k16.7.66 rework, 2026-11-17).** Covered 87.306 -> **87.268%** (-404,173 samples, 543 zones, none gains; worst 57,47 -93.1 -> 91.4%, 34,46, 18,48), surplus 14.855 -> **13.245%** (the pruned pockets were ours-only), pairs 145,090 -> 154,182, **under 1,033 -> 343**, **over 615 -> 688** (52 zones up, 25 down; worst 18,51 +8). Not one merge of the batch but one: 270 (recipe 9); 7.62 / 7.63 (clearance), 253, 277 (floors) and 8.27 (walker) do not touch the bake. Net it is an improvement on three of the four figures, and the prune is the intended change, so the baseline is repinned rather than the change fixed; the 0.04 points of cover are surface the zone's own seeds do not reach (UNSURE whether the original's tile has them as a sealed pocket: verify with `knav_compare --zone 57 47`). The seam numbers are unchanged (943 / 1,990 / 71 seams, same lengths).
- **Why the zones are stale** (hash term that differs, as far as the hash lets one tell: the stored word cannot be split into the building fold and the forest word): 749 of 970 have **no folded building**, so only the forest word can differ (529 of them place `limit to grass areas` trees, whose paint is the part of the scatter still approximated, NAVHASH.md); in 1 more the buildings agree (stored = fold) and the shipped bake saw no trees; 220 have buildings and an unsplittable difference, 153 of them with grass-limited trees, 67 without, and 1 more has buildings and no trees of ours. No zone is unverifiable. So at least 750 of the 970 are stale on the forest alone, and the 220 with buildings may be too.
- **Tiles no building shape touches** (a zone's 324 tiles; "touched": a building's shapes, or a shipped interior's vertices, within the tile): of 314,280 tiles of the 970 zones, **309,085 (98.3%)** have none, 188,952 of them take foliage shapes (rocks, trees) and **120,133 are pure terrain**; **740 zones have no building tile at all** (725 of them forest-only): the shipped zone mesh would do there, and it is the better mesh by the figures above. The rebake swaps the zone's whole 18 x 18 tile square, so a zone with one building replaces 323 tiles of the original's terrain for it (kenshi-k16.7.69 for the interior across a zone border).
- **Seams** (unlinked portal length: boundary edges of walkable polygons on a zone border that face a walkable edge of the neighbour within 20 u in height and carry no Detour link, one side's edges counted, measured on the resident mesh; the 20 u is ours, the bake's climb is 5): rebaked | rebaked **943 seams, 3,666,780 u of portal, 2 u unlinked**; shipped | rebaked **1,990 seams, 5,994,296 u, 140,039 u unlinked (2.34%)**, 884 of the seams with some, median 0, p90 216 u, worst 48,57 | 48,58 (all 1,802 u), 47,25 | 48,25 (1,460 u of 3,372), 16,3 | 17,3 (1,210 of 4,541); shipped | shipped control (71 seams, every 55th zone pair) **0 u of 188,611**. A shipped / rebaked seam is the original's edge meeting ours: the vertices do not meet (the shipped faces are cut to 256 u tiles and our polygon edge sits where its voxels end), so Detour finds no portal. Whether a walker crosses at such a place is not tested here (no route crosses the 140 km in a test); a walker's path through the unlinked part fails or detours.

## Door sweep (`ksim_door_sweep`, kenshi-k16.6.251, 2026-11-16)

The island test sees walkable regions the world cannot reach; this one asks the question the player asks: can I walk in through the door? Slow test (label `slow`) on the runtime mesh (`MeshNavigator::openNative` over KENSHI_DATA with the nav cache on, `worldKey` = `kworld::dataHash`), one navigator per town (The Hub, Heft, Squin, Stack: 3 x 3 zones each, the 25-zone residency cap), every door open (`applyDoors` with an empty book).

- **Buildings.** Every placed building with a non-gate door entity (`spawnMeshDoors`) in those zones, one door each (the first with ground 30-60 u inside and a way out). A building whose doorstep has no open ground within 150 u (a solid body, a cliff face) is skipped and counted.
- **Outside points.** Mesh points 30-150 u from the door, in the open (`Simulation::buildingAt` 0), on both sides of its axis and all round (the sim's recorded way in, `Door::inX`, is a guess for a building whose pivot is not at its front: the sunk Longhouse's is the wrong way round). A leg passes when ANY of them routes.
- **Legs** (`MeshNavigator::routeFrom`, the human race's `pathRadius`, same call a walk makes): `floor0` to the floor-0 point 30-60 u inside the door; `deep0` to the floor-0 sample (48 u grid, on the mesh, in `buildingAt`) farthest from the door; `floorN` to the sample of each upper floor (`BuildingSite::floors`) nearest the door. A leg fails when there is no route, it ends more than 15 u short of the point, or the route leaves the building's footprint after entering it (`left`).
- **Known gaps.** `tests/data/door_sweep_known_gaps.tsv` (building key, leg, note) is the checked-in failing set: the test fails on any leg that fails and is not in it, and prints a listed one that now passes (remove the line). `--expect X Z R` replaces the gap file with "a failing building lies within R of (X, Z)".
- **Self-check.** On a build with recipe 5 reverted (`kMatStairDrop` removed from the mat's lower bound, `kNativeCacheRecipe` 4) the Longhouse (3384-TwoStorey Residential Small, door at (68511, -70753)) fails `floor0` and the test fails on it as a new gap; with recipe 5 it passes. The reverted run fails with 11 new legs on 7 buildings (the Longhouse, and six Heft / Stack doors: recipe 5 also joins those), rc 1.
- **Cost.** 47 buildings, 87 legs: 24 s cold (nav cache empty), 16 s cached, peak RSS 0.9 GB. Most of it is the data load and the sim, not the mesh.
- **Floor-0 gaps triaged (kenshi-k16.6.300, 2026-11-16).** Four of the six were the sweep's own outside points, not the mesh: the Stack Armory's door (-55416, -12236) stands at the top of a 40 u stair (a ramp slab, mesh y 1198 to 1242), and every candidate the sweep took (door height +-20 u) lay on a ledge beside the door that no stair joins, so the walk in from the street was never tried. Rays are now probed at the door's height and two lower (24, 48 u; window 60 u), every 10 u along 34 rays, thinned to 64 per building. Armory floor0, Armor Shop, Bar, Stack Residential and the Barracks/Slave Shop deep0 legs pass at radius 4 with no bake change (the mesh was never the gap; the knav_draw dump shows the ramp and a 13 u mat). Cost 22 s. Remaining of the six: Heft Noble House and Slave Shop floor0 (`out of search nodes`): not a bake fix yet. Noble House: radius 0 and 1 route, 2+ run out of nodes; the doorway mat (36 u, DoorOpen) joins a passage of 6-8 u (Edge band both sides) where the shipped interior face has one ~40 u wide (1518-1523 high, 10 corners). The interior is our rebake (stale hash), a walkable closed hull 41 x 72 u (y 1198..1608) covers that room; not verified that its fill is what removes the floor. Slave Shop: not examined past the candidate list. `knav_draw --probe` now prints the corners of each polygon.

- **Deep-room gaps triaged (kenshi-k16.6.301, 2026-11-16).** Of the six `deep0` lines, four pass on current ksim (Residential Small 4607..., Thieves, Stack Armory, Stack General Storage; lines removed). The other two are real gaps, not a back room behind an interior door: the sim has no other door entity in either building (`SWEEP_VERBOSE=1` now prints, per failing leg, the target, the building's other doors and the route at radius 4..0.5 from the entrance point and from outside). Heft Clothing Shop: routes at radius 1 and 0.5 but not 2-4, so a 1-2 u passage between the entrance and the far room (same shape as the Noble House line). Hub Travel Gear (-63703, 16617): no route from the entrance room at any radius down to 0.5, so the far-room sample is not joined to the entrance room at all (an island or a closed wall; for an item). No test artefact found, so the target pick is unchanged.
- **Deep-room gaps, second look (kenshi-k16.7.87, 2026-11-19).** Heft Clothing Shop is already gone from the gap file (k16.7.71's hull rule, recipe 24). Hub Travel Gear: the shipped ground floor (`knav_draw --native --box -63800 16560 -63620 16680 --y 436 452`) is one connected face set from the door mat through the middle to the far room (sample -63781 16585, y 446.4); the bake's (`--mesh navworld/region_4_8.knav --components`, 11 sets in the box, three on this floor) is cut into the far room + middle, the entrance strip, and a lower blob, with 10-20 u gaps where the original has floor. So not the original's narrowness. Not done (the worker guard forbids running `ksim_door_sweep` here, and the runtime rebake's navcache entry was not available): which bake step cuts them (prop hulls, wall band, closed-hull fill) and whether other `4104-D-TwoStorey` buildings share it. Next: `knav_draw --mesh <navcache zone entry>` on the runtime rebake (the bake in navworld may differ from it), then the same bake with props/hulls off.
- **Travel Gear bisect (kenshi-k16.7.87, 2026-11-19; ksim 91f26330, recipe 24).** One-zone bake (`knav_bake --zones 18 35 18 35`, 3 s; zone cell = floor((v + 147456) / 4608)) matches navworld region 4,8: same cuts. `knav_draw --mesh ... --path -63650 16635 -63781 16585 --y 436 452` ends `partial` at the tip of the entrance corridor (x -63720, z 16638), a ~3 u gap short of the far room's tip; the lower blob (x -63750..-63690, z 16640..16670) is pruned (no seed). Obstacle props near the gap: Obstacle boxes 11 x 12 at (-63724..-63713, 16630..16642) y 442..454, 30 x 22 at (-63713..-63683, 16635..16657) and 21 x 31 at (-63771..-63750, 16625..16656). Ruled out (temporary debug switches, path still `partial`): the prop body clearance ring (k16.7.89), dropping all furniture Obstacle shapes (the gap narrows but stays), `--ledge off`, `--detail-cells 0`, `--partition layers` / monotone. Dropping a closed walkable hull (y 372/376) removes the floor itself (no path), (373/377) changes nothing. Not found: the shape or step that leaves the 3 u. The original has a 12 u neck at the same place with a hole where the 11 x 12 box stands. Next: dump the cells' area ids (wall band Edge vs null) at x -63720 z 16638 and the walkable shape whose side sheet crosses it; check a second 4104-D-TwoStorey building. The probe tools: `knav_draw --path` works on `--mesh`, not on `--native`.
- **Travel Gear cause found (kenshi-k16.7.87, 2026-11-19).** Temporary dumps of the heightfield at the neck (-63708, 16627), tile by tile: the floor is rasterised whole (walkable span y 442.4..443.4, area 63); above it, from the terrain pass on, a non-walkable span y 454.4..458.4 (the dune the building is sunk into, steep so area 0), 11 u above the floor. `rcFilterWalkableLowHeightSpans` (agent height 18) nulls every floor span under it, which is the 3 u / 12 u cut: a diagonal band of the entrance vestibule. The building's interior mask has two boxes (-63739..-63627 x 16491..16608, y 638..822; -63814..-63632 x 16518..16714, y 419..814, AABBs of a yawed box) and `interior terrain` is off, so `carveInteriorTerrain` removes the terrain only inside them; the vestibule lies outside the yawed box, so its terrain roof stays. The original's interior mesh has the floor there (a 12 u neck round the 11 x 12 pillar box). Earlier suspects ruled out: obstacles, prop clearance, ledge filter, detail, partition, walkable hulls. Not a prop or wall-band problem, so not Heft-style. Fix options, none tried (each needs the shipped gate): (a) also carve terrain spans inside the building's closed walkable hulls grown up by the agent height (these hulls top out at 451, the roof span is at 454..458); (b) skip the low-height filter for a floor span whose ceiling is terrain and within a building's footprint hull; (c) a per-building exception. Other buildings: any sunk house whose entrance passage lies outside its mask box and under the dune shares this; not enumerated (a count of floor spans removed by the low-height filter with a terrain ceiling, per building, would list them).
- The gap file is a bug list, not a baseline to keep: each line is a door the player cannot walk through on the mesh, or (`out of search nodes` on a 100-u route) a route the search gave up on; `deep0` lines may also be a closed-off room of a building that the sample grid picked (a back room with its own door). Not done: doors other than the first of a building, gates, buildings outside the four towns, interior doors between rooms.

## Route-leg walk (`ksim_route_walk`, kenshi-k16.6.298; the last 3 legs, kenshi-k16.8.37, 2026-11-18)

Walks every leg of the soak's routes (221 routes, 6,885 legs, radius 4) with `Walker::step`; a stalled leg or one that ends on another floor is a planner or walker bug. Pinned at 0 now. The 3 legs it once pinned:

- **R15 leg 2 (off-floor) and leg 3 (stall at its start): one planner bug.** The route corner (-58761.0, 4608.0) is a point on a portal across a tile border (z 4608, tiles 346,593 / 346,594). Poly 594,22's edge there is one 256 u edge, (-58624, 2518.2) to (-58880, 2570.7); poly 593,3 faces it with vertices at -58752, -58680 and -58644 (T-junctions, 0.4 gradient terrain). The corner took its height by interpolating along the long edge: 2546.3, where the surface is 2562.7 (a 16 u error; no polygon within 8 u of the corner's own height). The walker followed the true floor, so it ended leg 2 16 u above the corner, and the test then restarted leg 3 from the route height (the carry-over the first report suspected: a walk of legs 2-3 on the real floor never stalls). Fix: `NavQuery::planPathFrom` sets each interior corner's height to the corridor polygon surface (`getPolyHeight`; the one nearest the interpolated height when two overlap), before the length is summed.
- **R103 leg 28 (stall): the test's arrival radius.** The leg's start corner (-53460, 2161) is the middle of a 4.5 u portal between polys 367,584,4 and 367,584,5, the only joint of a thin strip. The test counted a leg arrived within 1.5 u, so leg 27 ended at (-53460.1, 2159.6) on poly 4's side of the portal; the straight line from there to the next corner runs south of the portal into the wall, and the slide crept along it. The sim's own reach of a point is 1.0 u (`combat.cpp` kArriveReach) or crossing its bisector plane: with 1.0 the leg walks. Test fixed (kArrive 1.0). Not a walker bug. A walk that did end 1-1.5 u short on the wrong side of a narrow portal would be a stall the sim cannot hit, since it aims at the corner until 1 u.

Rework on ksim 09e7ccf0 (2026-11-20): three more legs showed after the base moved, two walker defects in `NavQuery::slide` (not the corner height):

- **The corner snap is capped at 24 u** (`kCornerSnapMax`): uncapped, on stacked floors (the tower) the nearest-height polygon could be another floor and `knav_tower` jumped 213 u between corners.
- **R221 leg 39, R234 leg 42 (stall 1.2-1.8 u short):** a hop that ends exactly on a tile-border corner (z 1024, x -48128 / z 2048) makes `moveAlongSurface` fail outright (no polygon visited), so `Walker::step` reported off the mesh. `slide` now retries the hop 0.03 u short of the target before giving up (the old shorter-retry only covered a hop that stopped short).
- **R73 leg 28 (stall at its start):** `moveAlongSurface` from a polygon corner returned a point 9 u off a 2 u hop (the Detour corner defect `Walker::step` already treats as a block), so the walker never moved. `slide` now counts a result farther than the aim + 1.5 u as no move and tries the neighbouring polygons; if none walks, the hop is blocked where it stands, on the mesh. Short hops (0.5, 1 u) and one 36 u hop from the same spot were fine.

## Findings

- **The gates were blocked by their own foundations, not by deck height; later, by the terrain inside them.**
  The Hub gate buildings have `Gate4_col.xml` parts: a convex foundation hull covering the whole passage
  (solid, top = deck, buried 90 deep) plus tilted stair and ramp slabs. Filling the hull's footprint
  prism removed the road (the slab tops are below the hull top). Not filling at all (the first
  fix) made the south gate (-50757, 2615) a direct 611-unit path, but left the terrain inside the
  bodies walkable as sealed pockets, which is why the west gate (-51562, 2780) needed climb 16 (a
  start under its deck was cut off at 5). The volume fill (step 3b) fixes both: climb 5 crosses both
  gates (south 626, west 824 from 120 units outside). The 134 render-only parts
  are house extras, not gate or stair parts; the Wall Ramp IV buildings (wall staircases) have collision.
- `dtNavMeshQuery::findNearestPoly` returns the closest point of any polygon whose *bounding box*
  meets the query box, so the hit can be far outside it. `NavQuery` rejects hits outside the box.
- **`findNearestPoly` crash with wide boxes (fixed 2026-10-31).** `dtNavMesh::closestPointOnPoly` (Detour 1.6.0, system
  library) picks the nearest *boundary* detail edge for a point outside the polygon and dereferences a null edge when
  the polygon's detail triangles flag none. Recast flags a triangle edge as boundary only when both ends are within
  0.001 of a polygon edge **in 3D**; a sliver triangle (region 6,2 tile 441,165 poly 7, three corners 20-40 units
  apart) whose detail vertices were sampled at another height has no flag at all. Only wide boxes hit it, because a
  narrow box rarely has a polygon whose nearest point is not interior. Not a bad bake, a buffer size or the salt bits.
  Fix in `navmesh.cpp` `repairDetailEdges`: when a polygon's detail triangles carry no boundary flag, they are
  reflagged in the xz plane (tolerance 0.05) on the Detour copy as the tile is added (load and `replaceTiles`), so the
  cache bytes are unchanged and old bakes work without a re-bake. Ctest `knav_snap` clears every flag of the Hub
  tiles and queries up to 1000 wide (segfaults without the repair). Detour itself is not patched.

Hostile world squads at the Hub (2026-11-02): the 3x3 coverage around the Hub holds wild squads (seed 1: 15 Dust Bandits at about (-48881, -823), zone 21,31, the nearest hostile, 4.3 km from the Hub start). They are built as Fighters and hostile once `worldTeam` reads the relation both ways (`docs/reference/TOWN_LIFE_SLICE.md` 5.14); the client only sees them within the session's interest radius (1000 u). Test: `ksim_nav_regions` `hostileAtHub`.

## Reviews 2026-11-16 (routes; runtime mesh)

Two read-only Fable reviews authorised by the user ("The routing bugs are the most visible annoyance in game right
now"), on ksim a67ad0f2 / 493f1de2; ksim checked the cited code before filing. Items own the work; open exe questions:
kenshi-rwh.158.

**Route pipeline.** Verdict: the stages contradict each other: an A* that avoids walls by cost, a funnel that ignores
it, a corner push that ignores geometry, a revert that undoes the push; each rework round patched one seam. Target:
one clearance-aware search, the corridor as the route, a radius-aware funnel (shrunken portals) run from the walker's
position each step and once at plan time for the client line, `pathLength` and door routes.

| # | finding | item |
|---|---|---|
| 1 | pull at r = 0, then `pushCorners` a fixed r + 5 along the bisector (no wall distance), then an all-or-nothing revert: a 3 deg bend becomes a 9 u kink; neighbouring vertices push apart (the "giant Z") | kenshi-k16.6.248 (redirected: shrink portals, then pull) |
| 2 | a goal the radius cannot enter exhausts the clearance search (end not refit; `!complete` rejected), then a radius-0 retry: likely the 37 ms spikes and 4-7 ms re-queries | kenshi-k16.7.62 |
| 3 | short routes searched twice (`roundabout` at 1.02 x), 48 KB per `pulledLength`, 4096-sized vectors per call, `routeToStand` up to 48 routes | kenshi-k16.7.64 |
| 4 | the Edge-band cost only changes which corridor is picked (the funnel pulls through the band) | kenshi-k16.7.44 |
| 5 | `ClearanceCache` (ref << 32 \| link index) never cleared on `replaceTiles`: stale fits after streaming / a patch | kenshi-k16.7.63 |
| 6 | steering re-implements `dtPathCorridor` by tolerances and keeps a second tracker (`routeNext`, window drift) | kenshi-k16.8.22 |
| 7 | the walker is a point; radius-0 fallbacks hand out wall-hugging routes silently | counted in kenshi-k16.7.62 |

**Runtime mesh.** Verdict: the patch bake is the primary mesh in towns (1,051 of 3,995 zones stale; most town interiors
ours) and grows a rule per side effect of another rule (5 door mat for the fill, 7 ramp strips, 8 canopy width), tested
one-sidedly (islands and door sweep see under-connection; over-walk is invisible).

| # | finding | item |
|---|---|---|
| 1A | the shipped tile as a standing two-sided gate (covered %, surplus %, connectivity) for every recipe change; why zones are stale; terrain-only tiles | kenshi-k16.7.66 (done: "Shipped-tile gate") |
| 1B | one solid voxelisation of the real triangles to retire the shape-kind rules (a guess until prototyped) | kenshi-k16.7.68 (rough) |
| 2 | prune after each coverage change floods from resident seeds only: the result depends on the coverage window (the two new islands); prune per zone at bake time, keep border-touching components | kenshi-k16.6.270 (redirected); kenshi-k16.6.294 moot |
| 3 | the goal snap picks the lowest floor on a tie and ignores components; no re-seat off the mesh (PATHFINDING_STATIC 1.10 / 1.11): a terminal state | kenshi-k16.7.65 |
| 4 | staleness rests on a hand-bumped recipe and a hash of the load-order files only (meshes, heightmap, interiors, rotation not in the key) | kenshi-k16.7.67 |
| 5 | stale interiors swap whole 256 u tiles; an interior across a zone border is rebaked on one side | seam numbers in kenshi-k16.7.66 (done: "Shipped-tile gate"); kenshi-k16.7.69 (rough) |
| 6 | the island baseline matches by centroid within 60 u (drifts with a recipe change) | kenshi-k16.7.66 (done: keyed by record, floor, area band) |
| - | no runtime patch found for a player-placed building on the native mesh (grep only) | kenshi-k16.7.70 (rough) |

Agreed by both: deterministic clearance A* and bake (fixed threads, pure tiles, no fast-math), failure never a silent
shorter walk, chase / walk failures end in cooldown or give-up, measuring 7 / 8 against the shipped tiles was the
right method.
