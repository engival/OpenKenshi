# Navmesh generator: geometric surfaces from collision shapes (design)

Status: **design; stages 1 (the interior generator, offline) and 2 (zone side, seam, door painter, offline) implemented, sections 0a, 0b** (kenshi-k16.7.135, for the implementation kenshi-k16.7.131; a Fable review of this note comes
before the implementation starts). User, 2026-11-21 (ask kenshi-1mvr, option 1): "Yeah, build our own generator. Make sure we
have the option to go back to recast if major issues arise, but the goal is to rip it out otherwise."

Why, in one line: the original generates building surfaces from the collision triangles (Havok, no cell size, zero
simplification error in interiors); our Recast voxel bake cannot be made clean by parameters (review k16.7.124 N1, git
`85fa929d:docs/reviews/2026-11-21-navcoll/REPORT.md`; 1 u cell: bake 7.6 x slower, +21% polygons, NAVMESH_CACHE.md "Stair seam").

Claim tags: `[src, conf]` with conf = high / med / low. `exe` = kinspect `findings/navbake_static.md` (Q#, C#), `cut` =
`navinterior_cut.md`, `havok` = `docs/reference/HAVOK_NAV.md`, `rev` = the k16.7.124 REPORT, `cache` = NAVMESH_CACHE.md.
**UNSURE** items carry the oracle question (UQ#, collected in the last section) that settles them, and the default taken.

## 0. Decisions after the review (kenshi-k16.7.137; they override the sections below where they differ)

Review kenshi-k16.7.136: kenshi-ksim `docs/reviews/2026-11-22-navgen/REPORT.md` (F#). Oracle answers: kinspect `findings/rwh2NN_*.md`.

- **Unit of work = the building** (F1). One shape set gives two outputs: the interior mesh (faces clipped inside the mask
  volume) and the building's zone-side faces (walkable groups outside it). The seam lies on the mask planes, equal on both
  sides by construction (an ordinary linked edge). The door painter runs on both. Supersedes section 4 "Doors" and section 6
  "At doors" / "Interior to terrain".
- **Tile assembly is a stage of its own** (F2). Recast's polygons for the tile become `native::Mesh` faces (a third producer),
  with the generated shapes left unfilled in that Recast pass. The generator's clip subtracts generated footprints and cutter
  solids from them; one `nativeTiles` call then builds the tile. There is no off-mesh stopgap. `native_tiles.cpp` has no
  off-mesh code (NAVMESH_CACHE.md corrected).
- **Mask volume = one convex hull of every group-13 corner point plus every convex-mesh mask vertex** (rwh.282 static,
  rwh.292; exe 1403c0510 / 140fb5260). This is neither the boxes' intersection (`cut` S2 is wrong) nor their union. The keep
  test is 3D: a face polygon in its own plane is clipped against the hull's half-spaces (F6). With no mask: keep everything.
  The review measured a union on the Longhouse; stage 1 grades the Longhouse under the hull rule. If the two disagree, the
  live capture wins and the oracle is asked again. The bake's bottom-plane tolerance (`bake.cpp:180`) stays, as a named
  constant.
  Recast path, scope (kenshi-k16.7.142, after tq: ksim_nav_islands +51, shipped_gate 360 / 704): the hull replaces the boxes'
  intersection only for masks of 3+ boxes (Longhouse) or with a convex-mesh shape (Hub interiors); 1-2 box masks keep the
  intersection. The hull on 2-box masks (4014-TwoStorey) made 33 upper-floor islands: native keeps those floors (knav_compare
  --at: surfaces at 872.7 over the hull, outside the intersection) but our Recast bake leaves them unlinked. 18 islands and
  shipped_gate 296 / 706 (base 273 / 669) remain from the 3+-box / hull masks (3795, 3809, 4028 Heft, 4078-D, 5212, 42175, 52180,
  57740): same cause, the generator's job.
  Recast path (kenshi-k16.7.142): hull keep-inside on the interior's upper shapes only; the generator applies it to the interior
  output only. The terrain carve stays the union of the mask boxes (UNSURE: the zone-side cut in the original is not proven to be
  the box union; the hull as carve gave 181 new height-sweep gaps at the Longhouse).
  Case ksim_route_walk route 195 leg 29 (Hub, -50432 -1106 -> -50432 -1280, ends on the deck at y 1022): not the hull. The leg
  fails identically with the old box-intersection clip, and knav_compare --at finds the original's surface there at 1021.9; the route's
  target (998.4, the ground under the deck) is wrong. The 213-vs-222 route count only changes which legs the sample draws.
- **Cut rule** (F4). A surface cuts a face where it lies above it by (0.1, 18] u (material 1 = WALKABLE_AND_CUTTING). Within
  0.1 u the two are coplanar and exactly one owns the overlap: the lower shape id wins. New invariant: overlap area = 0,
  reported as an area. A cut-only solid resting on a floor stays UNSURE (UQ4: the generator takes any solid above the floor
  within 18 u, asked statically).
- **Pruning is required** (F5, rwh.290). The interior has one seed: child +0xf0 if set, else a ray from the centre. Islands
  more than 4 u from it are removed. Ours keeps a component only if it connects through matched edges to the seed or a door
  link; a zone-side component must connect to terrain. Dropped area is reported. Supersedes the UQ9 default.
- **Groups** (rwh.283):
  - group = 9 + floor; `is unwalkable roof` -> 27; `is stairs` -> 15 + floor; `passable` or path mode 0 -> 23+;
    furniture 19+; door parts 5; mask 13; `[Steps]` parts are 9. The xml `<group>` is ignored.
  - A ruin keeps the mask {9, 10, 13, 19}: it loses its stairs, floors 2-3 and the group-27 cutters.
- **No vertex weld across shapes.** The exe's 2.236 u midpoint weld (rwh.284) is not copied, because it depends on order
  (F7). Within a shape the weld is 0.1 u. Edge matching is the main join path (F9), tested on its own: stair foot, stair top
  step, deck-to-deck, floor-to-floor. A riser is not a "solid between", and edges are split at the overlap ends.
- **Slope 60 deg** stays the default; 60 vs 90 stays UNSURE (UQ6, open). Each building's census reports faces steeper than
  48 deg (F8).
- **Narrow faces stay** (rwh.288). Faces narrower than 9 u are neither dropped nor flagged; width is a query-time matter.
- **Stitch bounds** (rwh.289): maxStepHeight 10 u, maxSeparation 2 u, maxOverhang 0.5 u (Havok 1.0 / 0.2 / 0.05). The same
  stitch applies at doors. The scalar 3.0 has an unknown role.
- **Arithmetic** (F10): each cut is kept as its exact line and snapped once at the end. Acceptance is measured on
  `native::Mesh`; the converter's 1/8 u re-quantization is allowed for in acceptance cases 5 and 7.
- **Fragmentation** (F11): an edge meeting two pieces is split, never left. If the face count fails, cut order (largest
  cutter first) and merge order are the levers.
- **Doors** (F13): every face piece inside the door's closed-frame volume carries the key, on both meshes. Removing them
  separates the two sides. The key travels on the face (`native.hpp`), not guessed from the centre.
- **One clearance convention, no wall band** (F14, review k16.7.124 N2): kenshi-k16.7.129 removes the band from the Recast
  path too. There is no per-tile clearance record; stage 4 waits on k16.7.129.
- **Converter linking across walls** (F12): `partnerOf` must respect `EdgeKind::Wall`. This is a small item of its own, and
  it covers sheet walls (UQ5 open).
- **Terrain** (F15, rwh.291): faces from shapes are planar within 0.25 u, terrain faces are not. Section 7 follows rwh.291
  (greedy smallest-area-first; 100 u2 / 15 u / concave 1.5 u2 / shrink 0.75; 10 u2 / 1 u inside the job box) and drops the
  10 u figure.
- **Detour stays** through this work (F16); decide after stage 3 from tiles over the polygon cap and route time outside our
  own search.
- **Output is keyed by layout, not placement** (user 2026-11-22: generic shops look copy-pasted between towns; the
  ground is what differs). The interior and zone-side outputs are a pure function of the **layout key**: building type,
  destroyed state, and its children (type + transform relative to the building, gather order); everything in building-local
  coordinates, snapped in that frame. Cutting always runs shell first (the building's own shapes, then mask and doors), then
  children (walkable children add faces, every child cuts), then pruning, so the shell result is a valid stage boundary
  ("shell key" = type + destroyed). Exceptions: `interior terrain` 1 buildings read the ground, so their key includes the
  placement. Only tile assembly (terrain clip, stitch) runs per placement. A disk cache of either stage is not built yet:
  kenshi-k16.7.148 counts how often layouts repeat, kenshi-k16.7.149 (rough) holds the cache until the stage 1 timings
  and that census show it pays.
  **Layout census** (kenshi-k16.7.148, 2026-11-23; base game load order, 11,714 `0-buildinglist` placements, 12 with no
  BUILDING record). Command: `python3 -I tools/layout_census.py <KENSHI_DATA> [--mask] [--listing FILE]` (stdlib, 6 s).
  Children = the `interiors.level` items of the placement's `interior layout name` whose record is a BUILDING and not
  `is node` (the NAVHASH.md child set), in item order; the geometry key also drops path-mode-0 children.
  - Placements with an `interior mask` or path mode 3 (walkable): 4,960 in 80 types (mask 1,422 in 42 types; walkable only
    3,538, almost all defensive walls, walkboards, bridges). 772 of them name a layout (64 of those names do not join
    interiors.level: no children are read for them, so those 64 count as childless).
  - `interior terrain` 1: 24 placements, 3 types (no shared key). The rest, 4,936 (mask 1,398), are keyable.
  - Distinct keys among the keyable: all 4,936: shell 103, layout 219 (exact floats 219, 0.1 u / 0.1 deg 219, geometry
    children 218); 4,891 placements (99.1%) share a layout key with another. Mask buildings only (1,398): shell 62, layout
    178, geometry 177; 1,360 (97.3%) share, largest group 70 (Old Storm House, 30 towns).
  - Most repeated (mask buildings, geometry key, count / towns): Old Storm House intact 70 / 30; Hive Commune Hut 11
    children 57 / 8; Small Shack destroyed 56 / 18; Small Shack 6 children 42 / 15; Defensive Gate III 36 / 27; Old Storm
    House destroyed 32 / 13; Outpost s-I 30 / 12; Storm House destroyed 27 / 11; Storm House 36 children 27 / 12. With walls
    included the top six are the six Defensive Wall types (545, 525, 448, 381, 285, 279; 20-29 towns each).
  - Children are stored **building-local** in `interiors.level` (the world pose is composed at load, `placeFurniture`), so
    no relative transform is recomputed. Copies differ by no float noise: exact-float, 0.01 and 0.1 rounding all give 219
    keys. Layouts with different names but equal content merge in the key; the child-type multiset without transforms
    gives 217, so two keys differ by transform only (one by a path-mode-0 child).
  - Not in the key and not checked here: the building's rotation (local frame, free), `interior terrain` reads, and
    exterior layouts (`EXT-`).
- **Acceptance E by stage** (F3):
  - interior stage: 7.82, 7.105, 7.114, 7.116;
  - zone-side / assembly stages: 7.83, 7.86, 7.107, A5.

## 0a. Stage 1 status (kenshi-k16.7.131, 2026-11-23)

Code: `knav/surfgen.hpp` / `src/knav/surfgen.cpp` (`knav::surfgen::generate`: shapes in, `native::Mesh` out, offline, no Detour). Graders: test `knav_surfgen`
(Hub bar, Longhouse from the game data), dev tool `ksim_surfgen_grade hubbar|longhouse [--capture DIR --instance N] [--out DIR]` (compares with the original's
live capture where it is at hand). Pipeline: walkable up-facing triangles (60 deg; a convex solid gives one polygon per coplanar group) -> exact cut by every solid
above the face in (0.1, 18] u (convex solids: the xz hull of the solid sheared into the face's plane and clipped to the slab; open meshes per triangle; sheets widened
to 0.1 u; PROJECTED at all heights) -> lower-shape-index ownership of coplanar overlap -> mask hull clip (`kworld::interiorMaskHull`) -> 1/256 u lattice, T-junction
repair -> **Delaunay retriangulation of each coplanar group and Hertel-Mehlhorn convex merge** (the peeled strips of the cut are an arbitrary partition; the merge of
those alone gave 205 faces, the retriangulation 122, a 0.05 u collinear tolerance 120) -> edge matching (antiparallel, gap <= 2 u, step <= 10 u, overlap > 0.2 u,
nothing solid between; both edges split at the overlap ends; nearest first, two passes) -> components, prune to the largest.

| measure | original (live) | generator |
|---|---|---|
| Hub bar faces | 81 (acceptance <= 121) | **120** (generation 0.5 s; Longhouse 0.9 s, unoptimised) |
| Hub bar area (3D) | 24,971 u2 | 25,463 (+2.0%); xz coverage: ours only 365 u2, capture only 12 u2 |
| bar boundary to furniture footprints, median | 0.24 u | **0.003 u** (3,113 samples) |
| bar axis-parallel boundary share | 73% | 70% (all wall edges, both sides; the axes are the dominant direction) |
| bar ground floor, worst vertex off one plane | 0.05 u | **0.000 u** (35 faces of the floor shape) |
| bar components after pruning / dropped | 1 | 1 / 5 specks, 24 u2 |
| Longhouse faces / area | 76 / 61,400 | **77** / 68,332 (+11%; xz coverage: ours only 2,134 u2, capture only 62 u2) |
| Longhouse upper floor (y 1080-1100) | 18,861 u2 | 19,668 u2, in the one connected mesh (stair joins it) |
| overlap area (invariant 0) | - | 0.018 / 0.006 u2 (merge snap noise) |

Not in this stage: the entrance steps (outside the mask hull: zone side, stage 2), the door face (stage 2), tables "round" in the plan (the bar's tables are one
exact-corner hole each: the cut is the footprint). Hub bar steps as ONE strip and shipped-interior comparison (hash-matched interiors) are not run: no stage 1
grader for them yet.

Deviations: D1 seed = largest component (no seed child `+0xf0` / ray: UNSURE UQ9, the Longhouse mask centre lies in the courtyard of the L); D2 pruning drops the
Longhouse's second component (roof walkway, y 1093-1113, 11,490 u2) that the capture lacks, while the main component still holds 3-4k u2 of roof the capture does
not (oracle question: which roof shapes are walkable in the interior job: group 10 `Exterior_ROOF` boxes are walkable here); D3 the exe weld (2.24 u) is not
copied (F7); D4 the stair-top / deck edge match tolerance (10 u step) is the section 0 value, not the 5 u of section 6.
Questions: TQ1 is the roof walkable in the interior mesh (above); TQ2 `ownerTolerance` 0.1 u: the original's tie-break between coplanar shapes is unknown.
What terrain would need: section 7 (nothing here changes it; the generator takes triangles of any WorldShape, so the height grid can be fed as walkable
`WorldShape`s with slope 40 once simplification exists).

## 0b. Stage 2 status (kenshi-k16.7.143, 2026-11-23)

Code: `surfgen::Input::side` (`Side::Zone`), `Input::paintDoors`, `surfgen::seamMismatch`, `native::Face::door`; `ksim_surfgen_grade <case> --side zone [--islands seam]`; test `knav_surfgen` (bar part).

- **Zone side** = the same pipeline with the keep test inverted: a piece is clipped to the mask hull, and the part outside (piece minus hull) is kept. No mask: nothing. Pruning: none by default (the rule is
  "joins terrain", which arrives with stage 3; `--islands seam` drops the components that do not reach the seam, which leaves only the 3 door faces on the bar).
- **Seam**: boundary edges lying on a hull side plane (|n.y| < 0.5, 0.05 u, longer than 0.05 u) get `Edge::linked` on both meshes. Hub bar: 3 edges / 16.23 u zone, 3 / 16.16 u interior; the plan gap between
  the two seams is 0.072 u (one half-unit sliver; the lattice snap of two clips of one polygon), the height gap 0.003 u.
- **Door painter**: the hull of each Door shape's points is the volume (0 u margin, kenshi-rwh.310); every piece is cut at it, the inside part gets `data` 4 (the baked state is open, as in the capture) and
  `Face::door` = the shape's key; paint only, no polygons added. `nativeTiles` takes `Face::door` before its footprint lookup. Hub bar interior: 1-2 faces / 13.6 u2 (capture instance 22: 14 u2); zone side 17.7 u2
  (the capture's 46 u2 of zone-side door faces lie on terrain faces).
- **Bar zone side: 19 faces / 2,033 u2 xz (2,852 u2 3D)**, 10 components. The bead's gate figure 65 faces / 8,855 u2 was not reproducible from the capture files (instance 10 holds 104 data-1/4 faces / 19,390 u2
  of the whole tile; no height, footprint or data filter tried gives 65 / 8,855). Ours has no roof (the roof boxes lie inside the hull: they are the interior's upper floor) and no terrain.
- **Entrance steps**: planar to 0.000 u (slope 42.3 deg), but 7 faces (biggest 1,221 of 1,552 u2), not one strip. Cause: the building's own wall solids (generator solid indices 7 8 9 12 14 16 19) cut the upper end
  and the edge at the 18 u band; the capture's visible steps are also 2 big faces (98.7 + 249.4 u2) plus pieces, the buried lower part (y < 1562) being gone there.
- **Longhouse zone side**: 38 faces, 24 seam edges / 245.6 u against 2 / 43 u on the interior: the interior prunes the roof component the zone side keeps, so most seam has no counterpart (D2 of 0a).
Questions: TQ3 which capture faces make the 65 / 8,855 of the gate (and does the zone side hold the roof); TQ4 does the original's steps keep the wall cuts at their upper end.

## 0c. Stage 3 status (kenshi-k16.7.144, 2026-11-23)

Code: `knav/assemble.hpp` / `src/knav/assemble.cpp`, test `knav_assemble`. Offline, called by nothing in the runtime bake. `recastFaces` turns a Recast `BakedTile`
into `native::Mesh` faces (corner heights from the detail mesh; door polygons faceData 5 with their keys); `assembleWorld` subtracts the generated footprints
(cut rule: generated face above the Recast face by (-0.1, 18] u) and cutter solids (mask-box carve, obstacles) from them, cuts the zone-side faces by terrain
triangles (under ground by > 0.25 u), welds, repairs T-junctions and pairs edges; `assembleTile` makes ONE `nativeTiles` call (world mesh + interiors).
Hub bar tile 376,586 (Recast bake without the bar's shapes): 79 Recast faces / 61,930 u2 -> world mesh 86 faces / 48,915 u2 (13,015 u2 removed under the bar,
the mask carve), 279 polygons, 0 overfull, 3 open shared edges.
Not done: the Recast pass is not wired (the caller must bake from the shapes minus the generated buildings; building identity is not on `WorldShape`), the
zone-side producer is k16.7.143's (not on this base), the seam edges between a cut Recast face and a generated face are plain walls until 143's seam linking,
door keys of generated faces (143), `ksim_door_sweep` and the section 10 E rows (they need the wiring: stage 4). Recast faces keep corner heights only.
Questions: TQ3 carve = union of mask boxes (k16.7.142 UNSURE) used here.

## 0d. Stage 4 status (kenshi-k16.7.145, current as of 2026-11-24)

Code: `knav/zone_gen.hpp` / `src/knav/zone_gen.cpp`, `knav/assemble.cpp`, wired in `MeshNavigator::bakeNativeZone`. Switch: `NativeCacheOptions::surfaceBuilder` (Shapes by default; env `KSIM_NAV_SURFACE=recast` forces Recast), a byte of the zone cache key; `kNativeCacheRecipe` 53 on this branch (the lead assigns the merge number; the assembly changes since 53 need a bump).

**What runs.** A walkable building with `interior terrain` 0 and an interior mask (`AreaBuilding::mask`) is generated (`surfgen::generate`, interior side and zone side) only when its box plus 16 u meets the zone's tile window (a neighbour zone's building touches none of the window's tiles; its own zone generates it). A building fails only when the generator throws or leaves no walkable face on either side (`ZoneGenReport::failedIds` with `failedWhy`; knav_bake prints the text). Before kenshi-k16.7.216 two more checks failed it, which with the Recast pass gone left the building with no mesh at all (397 buildings in 172 zones of the world bake: 297 "doors but no door face", 100 "no interior faces", each with a zone side of 4-47 faces): a building whose floor is all outside the mask hull (no interior mesh, zone side kept) and one whose door volume paints no face (the door stays unpainted) now generate. Recast bakes without the generated buildings; every tile of the window that a generated building's box (+16 u) touches is built by `assembly::assembleTile`; those tiles are never kept from the shipped navtiles (stale check, fresh zones too). A tile the assembly returns empty for (over the 512-polygon cap or the vertex limit, or an exception) is baked again from every shape (fallback by tile square). The log line `nav surface generator zone X,Z: ...` names each fallen-back tile with its reason (`over the polygon cap (n polygons from r Recast faces, z zone-side, world w, i interiors of f faces)`).

**The assembly** (`assembleWorld` + `nativeTiles`):
- `recastFaces` converts the Recast tile: polygon corner heights come from `tile->verts` (`detailVerts` holds only the extra vertices; reading it for the corners gave 30 of 146 corners another vertex's height at the Hub bar tile and was the cause of the earlier plane-off and island symptoms); a polygon whose detail vertices all lie within 0.25 u of its corner plane stays ONE face (outline keeps the detail vertices on its edges), else its detail triangles are the faces.
- Cutters (the mask hull, one per mask shape) cut the Recast faces over the mask box's own heights, as the Recast carve does; ground far under a raised floor (the Hub bar's, 1561 under 1576) stays. Generated faces cut the Recast faces within 18 u above them.
- Seam pairing (`meshOf`): boundary edges on one line (1 u, up to `Settings::seamStep` 5 u apart in height) become opposite pairs, so `nativeTiles` joins the polygons cut from them as a step. Only Recast + zone-side pairs and zone-side + zone-side of ONE building pair; Recast + Recast (its corners weld when the heights are read right), zone-side of two buildings and interiors never pair, so a wall stays a wall (`knav_assemble` pins each case; the Recast-only case fails without the guard). The 1 u line tolerance is the zone-side face ending where it rises 0.25 u over the terrain, 0.3-0.8 u off the footprint the ground is cut at (the Hub bar ramp foot).
- Every edge still open is `linked`: a streaming link joins only an edge of the same corners the other way round; the tile border becomes a Detour portal.
- `generateBuildings` runs on 4 threads, results in the area's order; `bakeNativeZone` keeps its last 6 results (dropped for a zone and its eight neighbours when `standBuildings` adds a placement; `addPlacement` is the gatherer's only mutator).
- surfgen guards: the T-junction repair takes at most 256 inserted corners per piece (the cold-bake runaway of 16-18 GiB: two corners 2 lattice steps apart at x 70141 inserted in turns for ever); edge matching uses a 4 u grid (same output). Hub zone generation 25 s -> 1-6 s. `KSIM_NAV_GEN_LOG=1` logs each building as it starts.

**Measured** (Hub zones 20,31..21,32 + 20,33): 23-25 buildings generated, 0 failed, 65 tiles assembled; 2 fell back (373,594 692 polygons, 373,595 703: generated interiors of 385 / 377 faces plus the world faces). Far zones: 12,35 13 of 45 tiles fell back, 46,16 and 47,16 about 8 each: all the polygon cap (a dense generated interior alone is 300-600 faces).

**Open.**
- Bar counter neck: the 1.4 u strip between two obstacle boxes is narrower than a body of radius 4; `ksim_town_bar_realHub` passes with the current assembly (oracle question: does the Recast furniture-ring lane rule match Havok here? unverified).
- Polygon cap: a generated interior of 300-600 faces fills a tile; coplanar-face merging in surfgen (or a higher cap) is what takes the fallback list down. `nativeTiles` also merges coincident polygons (same corners: a door piece held by interior and zone side), because Detour raycast loops forever between them (`stats.duplicates`).
- Not done: the door sweep, the E rows and the determinism run (navmesh tests are off, user 2026-11-23); `standBuildings` still bakes a whole zone with generation on the sim thread (k16.7.175).
Deviations: D1 pruning of the Recast mesh runs before assembly, so ground under a generated building is pruned/kept as the Recast seeds decide. Recast is NOT bit for bit inside assembled tiles (its faces come from the detail); tiles without a generated building are unchanged. Questions: TQ5 zone-side faces of a building whose zone tile is another zone's window are built by that zone's own bake only.

**Build order** (items under kenshi-k16.7):

| Stage | Work | Gate |
|---|---|---|
| 0 | this section; `partnerOf` respects walls (F12); mask hull planes (F6) | `knav_native_tiles`, `knav_longhouse_door` |
| 1 | kenshi-k16.7.131: interior generator offline in native form, graded on the captures (bar 81 faces / 24,971 u2, Longhouse 76 / 61,400 u2) and the shipped interiors whose hash matches | area, face count, boundary distance, one component, zero overlap |
| 2 | zone-side faces + door painter on both sides (done offline, section 0b) | bar zone side (65 faces, 8,855 u2: not reproduced), steps within 0.1 u, the link edge equal on both sides |
| 3 | tile assembly | `ksim_door_sweep`, `knav_longhouse_door`, `knav_interior_terrain`, E rows of this stage |
| 4 | runtime switch, cache builder byte, stale-interior path, fallback by tile square | section 10 F, `knav_deterministic` |
| 5 | retire the Recast building path, then terrain | `knav_compare` against shipped zones |

## 1. Scope and shape of the thing

- **In scope (this item's implementation):** surfaces that belong to collision shapes: building shells, interiors, furniture
  (the original's interior job; also the walkable shapes of a zone job). Terrain stays on Recast, joined by edge matching
  (section 7 says what moving it over takes).
- **Output is the existing intermediate form**, `knav::native::Mesh` (`knav/native.hpp`: `Face` = convex ring of edges,
  `Edge{a,b,oppositeEdge,oppositeFace,linked}`, `Face::data` 4 / 5 door), not Detour tiles. `knav/native_tiles` already
  converts that to Detour (tile clipping to 256 u, split to <= 6 corners, shared edges / off-mesh links, door keys). The
  generator is a second producer for the same converter; nothing downstream changes. [cache "Conversion", our code, high]
- **One face = one planar convex surface piece** (any corner count; the original's reach 13, shipped up to 19). The
  Detour 6-corner split happens only in the converter, so the overlay can still draw "same face" seams faintly (rev N6).
- **Unit of work:** one building interior instance (all its shapes, one result), or one zone's walkable-shape set. Pure
  function of its inputs; deterministic (sorted inputs, fixed arithmetic, no fast-math, no thread-order dependence;
  `knav_deterministic` compares 1 and 4 threads).
- **Where:** `cpp/include/knav/surfgen.hpp`, `cpp/src/knav/surfgen.cpp` (+ helpers if it grows); listed in `cpp/CMakeLists.txt`
  and committed as a stub first. Not under `ksim/`: knav is a library below the sim.

## 2. Inputs

What the original reads, and what we feed. Our shapes come from `kworld::CollisionLibrary` (the part's `xml collision`,
`COLLISION_FORMAT.md`); the review measured them equal to the live PhysX shapes (Hub bar 23 of 23, Longhouse 43 of 43, furniture
18 of 18, <= 0.14 u). [rev verdict 1, high]

| input | original | ours |
|---|---|---|
| Building's own shapes | PhysX shapes with owner == building, AABB query, mask `0x87fbe00` (groups 9-13, 15-22, 27) intact [exe Q2, cut §1, high]. Group 27 = material 0 (cut only); every other group walkable (material 1). | the part shapes of the placement, in world space. Walkable unless the part is cut-only / `is unwalkable roof`. **UNSURE (UQ2):** our shape records do not carry the PhysX group number; the 27 / 13 split needs a map from our fields. Default: group 13 = the interior-mask boxes (already parsed for `carveTerrain`), group 27 = parts we already treat as obstacle/cut-only. |
| Destroyed building | mask `0x82600` = groups 9, 10, 13, 19 only, plus the "destroyed boundary" mesh as triangles [cut §1, high from decompile, not run]. | read the building's destroyed state (squad template `building ruined`, `cache` Doors); keep only the destroyed-group shapes; add the boundary mesh. **UNSURE (UQ2):** group membership per part. Default: until UQ2 is answered, intact and ruined both use all parts except what we already drop for ruins (no regression from today). |
| Mask volumes (group 13) | each group-13 box's 6 planes into ONE `hkaiPlaneVolume`, inverted, used as a carver: everything outside is carved. Static reading = **intersection** of the boxes [cut §2, med: not traced past the plane add]. | `interior mask` boxes, one OBB each. **Live observation (review O1, Longhouse upper floor present): a union.** `cache` 1b also takes the union for the terrain carve. **UNSURE (UQ1).** Default **union** (it matches the live mesh and the Longhouse acceptance case). Zero mask boxes: keep everything (no mask). |
| Furniture / child buildings | included if the child's definition `path mode` != 0 and its parent is this building: 3 = walkable, else cut [exe Q2, med-high]. | children of the placement with `path mode` WALKABLE -> walkable shapes; OBSTACLE / PROJECTED -> cutters; IGNORE -> nothing. The review's 18 floor-0 pieces were matched this way. |
| Doors | shapes of group `0x20` whose object has the door flag; OBB -> `hkaiMaterialPainter` material 4 -> faces carry data 4 [exe Q2/Q4 C28, med]. Never geometry. | the door child's collision slab (placed with the parent's transform, `cache` Doors) -> door box. Never geometry (kept). Key = `kworld::doorKey` of the door id, known directly, so no "footprint + 8 u" lookup. |
| Terrain | zone height grid, all samples (the generator job); the **interior job takes none**, except `interior terrain` = 1 buildings [exe Q2, cache "Interior terrain", high]. | none in this item. The terrain Recast tile still exists under and around the building (section 6). |
| Trees, rocks, foliage | zone job only (tree boxes ±radius x scale x 0.1 Havok in xz, ±100 u in y; carvers) [exe C29/C30]. | not the generator's in this item (Recast keeps them). |
| Water | synthetic 2-triangle quad, material 3 [exe C46] | not in this item (section 7). |
| Characters | seed points for region pruning only [exe C31] | not an input. Seeds for building meshes: UNSURE (UQ9); default: keep every connected component of a building mesh that reaches a door or a floor over 1 u2 (no pruning) so no regression from today's seedless interior. |

Shape kinds: box (8 vertices, 12 triangles), capsule (16-sided prism, step 0.125 pi), convex hull, triangle mesh [exe Q2
`1403c1cf0`, high]. We feed the same: boxes and hulls keep their **convexity flag** (the cutter in section 4 is exact only for
convex solids); triangle meshes are cut per triangle.

## 3. Vertex weld and the walkable rule

**Weld.** The original welds any two interior input vertices closer than sqrt(0.05) Havok = **~2.24 u**, then Havok's own
`weldInputVertices` at 0.01 Havok = 0.1 u [exe Q2 C26 / Q3 C42-43, med: the constant's unit, squared or not, is read from the
decompile only]. Two sources disagree with "exact corners": the review measured the original's boundary to the furniture
footprint at median 0.24 u but **p90 2.24 u** [rev table]; 2.24 is suspiciously the weld distance. **UNSURE (UQ3).** Default:
weld at 0.1 u within one shape's triangle soup (to close hairline cracks), **no cross-shape weld**, so a table's corners stay
exact. A recipe constant `weldDistance`; if UQ3 says 2.2 u, parity needs only that constant and a cross-shape pass.

**Walkable triangle.** After the weld:
1. Drop degenerates: area <= 0, or a width below `degenerateWidthThreshold` 0.05 u [exe Q3, high].
2. Walkable iff the shape is walkable (section 2), the winding says up-facing (`triangleWinding` CCW, `up` +y), and
   `normal.y >= cos(slope)`.
3. Slope: **60 deg for building shapes** (override entries for materials 1, 2, 4 = 1.0472 rad), 40 deg terrain (default)
   [exe Q3 C42, med: override semantics from Havok field names, not traced]. The stair slabs are 41.7 / 42.3 / 48 deg,
   so they pass either way. **UNSURE (UQ6):** the table also has material 3 = 1.5708 rad (90 deg), and `materialMap` sends game
   material 1 (walkable building) to Havok material 3; if the override is keyed after the map, buildings are 90 deg, not
   60. Default 60 (the bead's rule). Oracle: a live interior face over 60 deg.
4. Planar grouping: triangles that share an edge and are coplanar within `planarTol` form one candidate face
   (`planarTol` 0.25 u, between the 0.1 u welded collision and the 0.5 u at which shipped faces count as non-planar). Slope
   class never mixes (a stair tread and its landing stay separate unless coplanar). Havok's `cosPlanarityThreshold` 0.99
   (8 deg) is **not** adopted: it would round a 41.7 deg stair onto the landing and reintroduce the height error of N3.

**Clearance / agent radius: none in the mesh.** The generated mesh is uneroded, boundary on the collision surface (shipped
and live meshes: median 0.00 u, p90 < 0.1 u; `erosionRadius` absent in all 5,244 meshes) [havok "No erosion", kenshi-k16.7.138,
high for the field, med-high for the inset; exe Q3 BLOCK_EDGES, med]. Radius is the router's per-query business (`TravelFilter::radius`,
portal clearance, funnel shrink: `cache` "Clearance"). Character height 18 u is the only clearance the generator uses (the
cutter, section 4). `minCharacterWidth` 9 u (BLOCK_EDGES) is **not** a generator input: it is what the router's per-portal
radius test already does. UNSURE whether BLOCK_EDGES makes the original also drop a face narrower than 9 u or only blocks the
edge (UQ7); default: drop nothing for width; the router's test refuses narrow portals.

## 4. Cut-outs, keep volume, the exact clip

For each walkable candidate face F (plane, boundary polygon in xz), the result is F minus cutters, intersected with the keep volume.

**Cutters.** A solid S obstructs F where, over a point (x,z) of F, S occupies a height in `(F.y + base, F.y + 18]`. `18` =
characterHeight [exe Q3 C42-43, high]. `base` is the rise below which a body under the character is stepped over
(it is what stops a floor's own thickness, or a shape whose top is F, from cutting F): **UNSURE (UQ4)**. Default
`base` = 0.5 u, our sizing in the spirit of `maxStepHeight` 5 u without the climb (a table 5 u high still cuts it).
- **Closed convex solid** (box, hull, capsule prism): lower surface `lo(x,z)` is convex and upper `hi(x,z)` concave, so
  `{lo < F + 18} ∩ {hi > F + base}` over the shape's xz hull is **convex**: the cutter is the hull clipped by two half-planes
  (each linear in x,z because F is planar). Exact, no sampling. The furniture "exact squares" and the volume fill of the Recast bake
  (`cache` step 3b, gateways, stairs hulls) are this one rule: the interior of a hull is never walkable because its
  top face is the walkable surface and its body cuts anything inside it.
- **Triangle meshes and open shells:** per triangle: its xz footprint clipped by the same two half-planes (convex per triangle);
  union over triangles. Down-facing triangles (ceilings, slab undersides) are cutters like any other, this is the "anything
  within 18 u above a face" rule. Thin vertical or steep sheets (walls modelled as zero-thickness triangles) have an empty
  footprint: they act as **barriers**, i.e. each is widened to `barrierHalfWidth` 0.05 u (the degenerate width) so it
  cuts a hairline gap and the polygons on either side are never merged or matched across it. **UNSURE (UQ5)** how
  Havok handles a sheet wall; default as stated, check on the Hub houses with open walls.
- **Cut-only shapes** (group 27, obstacle / projected furniture): cutters, never walkable. A PROJECTED part cuts at all
  heights (its footprint cut through all y).

**Subtraction.** F's polygon minus a convex cutter = <= n convex pieces by peeling half-planes (sorted cutter order: by shape
id, then triangle index, so the result is deterministic); pieces are merged again afterwards (section 5). Arithmetic: doubles
in building-local xz, vertices snapped to a 1/256 u lattice after each clip, y re-evaluated from F's plane (so a vertex is
exactly on its plane, never "highest of nearby columns": N3 cannot occur). A single vertex table with snapping closes
T-junctions created by clipping from two sides.

**Keep volume.** F ∩ (union of mask boxes). Done as F ∩ box_0, then (F − box_0) ∩ box_1, ... so the pieces are disjoint (no
double polygons). No masks: no keep step.

**Doors.** The door box's footprint, clipped out of the floor face it stands on, becomes its own face with `data` 4 (open
default) and the door key. A door **never changes geometry**, so a door's state cannot change a tile (`cache` Doors, kept).
Original: door faces 59.9 u2 = the box footprint 18.5 x 3.2 [rev N5, high]; ours today 491.5 u2 (footprint + 8 u). The door
face must span the doorway wall to wall so it cannot be walked around (`ksim_door_sweep`).

## 5. Polygon merge (convex), and the Detour vertex limit

1. Within each candidate face the cut pieces are re-merged: adjacent pieces on the same plane (within `planarTol`) with equal
   slope class and equal `data` are joined across their shared edge when the result is **convex in xz**
   (Hertel-Mehlhorn: repeatedly drop a diagonal if both end angles stay < 180 deg), deterministic order (longest shared edge first,
   tie by lowest vertex id). Collinear vertices (angle within 0.5 deg) are removed only when the neighbour face on that edge
   has the same corner, so a T-junction is never created.
2. No corner cap in the generator (255, the Havok default `maxNumEdgesPerFace`; the original's faces reach 13, shipped 19).
3. **Detour limit.** `DT_VERTS_PER_POLYGON` = 6 in the system Detour [`cache` "Why not one mesh"]: the converter splits a face
   with > 6 corners into convex Detour polygons that share the face id (the existing converter rule: shipped Hub 97% <= 6
   corners). The split adds only interior edges, no new boundary. Face count (the acceptance number) and Detour polygon
   count (the 512 per tile budget) are reported separately.
4. Faces are never merged across different `data`, different slope class, or a barrier.

## 6. Edge matching

Inside the generator, then the same rule across the seams. **Unit rule** (from Havok's settings: `maxStepHeight` 5 u,
`maxSeparation` 2 u, `edgeTraversibilityHorizontalEpsilon` 0.2 u, `edgeConnectionIterations` 2, ORDER_BY_DISTANCE [exe Q3 C42/C44, high for the
values, med for their exact meaning]; shipped interiors verified to need it: "edges paired by native identity ... bar stair top: a 4.4 u step", `cache` Conversion):

Two **boundary** edges (no neighbour yet) of faces A, B match when, in xz, they are within 2 u of each other, overlap for
more than 0.2 u, A and B lie on opposite sides, the vertical gap over the overlap is <= 5 u, and **no barrier or cutter
solid stands between them** within 18 u (the new check: it stops matching across a 2 u thick wall that is not a door).
Pairs are taken nearest first, two passes. The overlap is the matched part: both edges are split at the overlap's end points
(so the matched sub-edges coincide), set `oppositeEdge / oppositeFace`; the remainder stays a boundary. A weld-identical
shared edge (the usual case after the 0.1 u weld) is matched before this step.

| seam | rule |
|---|---|
| Within a floor, between coplanar cut pieces | not matching: the merge of section 5; leftovers share the vertex ids. |
| **Between storeys and stairs** | no special case: a stair tread meets its landing at a shared (welded) edge or a <= 5 u step edge, the unit rule joins them. The upper floor's slab edge over the stair is a boundary because the 18 u cutter removes the stair where the slab is within 18 u above (no steep sliver can exist: a face is one collision face, so it cannot span two storeys: N4). Faces over the climb gap with a wall between are refused by the barrier test. |
| **At doors** | the door face (data 4, key) is a polygon of its own; its two long sides are shared edges with the floor on each side, so the passage goes over polygons the state can bite (same as `cache` Doors: dedicated polygon area, not an off-mesh link). Its outer end meets the terrain tile (below). |
| **Interior to terrain (Recast tile)** | the interior's boundary edge in the doorway is `Edge::linked`; the converter makes the link as it does for shipped interiors (shared edge when the corners agree, off-mesh otherwise; interior doorway corners sat up to 0.3 u from the outside's, snapped within 1 u, `cache` Conversion). The Recast tile must hold walkable ground at the door: the existing door mat (footprint + 8 u, `kMatStairDrop`) stays in the Recast bake until the Recast path is retired; terrain meets the interior at the wall's outside face, and the door box spans the wall, so the gap is 0 to the wall thickness. `ksim_door_sweep` and `knav_longhouse_door` are the gate. UNSURE (UQ8): the original's stitch values (3.0 and 15.0 Havok, field mapping unknown, low). |
| **Zone and tile seams** | Detour portals on the 256 u tile border; the converter clips the faces to tiles, vertices on the border agree by construction (both sides clip the same line). |
| **With shipped tiles (k16.7.128's agreement rule)** | k16.7.128: a shipped face is kept when it lies on current walkable collision (within 0.5 u) and is not inside a cutter. The generator is the rebuild path for what fails that test. **Unit = the interior instance** (or the zone's tile): either every face of an interior is shipped, or every face is generated; mixing faces of one interior would put two vertex sets on one surface. Between a shipped interior and a generated one (or the generated one and a shipped world tile), the unit rule joins them; where both exist for the same spot, **never both** (the cache records which source supplied the tile; below). The agreement predicate and the generator's own cut share one implementation of the 0.5 u test (a generated face always passes it). |
| **With the Recast terrain under / around the shell** | the shell and furniture shapes still go into the Recast tile as obstacles (fill footprint, no walkable surface inside the mask: `carveTerrain`, `interior terrain` 0), so the terrain mesh has a hole where the shell is and the two meshes overlap only at different heights. Where a building has `interior terrain` 1 the terrain polygons stay and the generator's faces lie over them (as shipped: the terrain is walkable inside). |

Result invariants (tested in the generator, report per interior): no face steeper than the slope limit; every face within
0.1 u of one collision face's plane (a face is one collision surface); no two faces overlap in xz at the same height;
every boundary edge is a wall, a cut, a door end, or `linked`.

## 7. Terrain stage: what moving terrain onto the generator would need

The goal is Recast out entirely; this item does not do it, but the data model should not stand in its way. The same pipeline
with a different input source and slope limit already works on triangles; what is **new**:

1. **Input** the zone height grid as triangles (two per quad, the 36 u lattice, `cache` Bake; the exe reads every sample, n x n,
   step zone extent/(n-1), x0.1; [exe Q2 C22, high]), material -1, slope 40 deg.
2. **Simplification** (the main work). A cell-per-triangle terrain is ~100 triangles per 256 u tile; planar merge barely reduces a
   heightfield. The original reduces borders with `maxBorderSimplifyArea` 100 u2, `maxBorderHeightError` 15 u,
   `maxBorderDistanceError` 10 u (zone default), with a finer ring (0.1 / 0.1 / 0.3 -> 10 u, 1 u, 3 u inside the job AABB shrunk
   by 10 u: [exe Q3 C42/C49, med]) and 0 for interiors. UNSURE (UQ10): the exact algorithm (`hkaiSimplification`); it needs
   a grader, not a copy: `knav_compare` against the shipped zone (96% coverage today).
3. **Obstacle cutters** for rocks (per-triangle cut with the k16.7.59 750 u rule, which is the per-triangle cutter above), foliage
   cutters (12-gon holes), tree boxes (`navmesh cutter`, +/-100 y); all expressible as cutters in the same sense.
4. **Water classes:** Shallow / Swim from height against the sea plane: split polygons exactly at the water-line intersection with the plane
   instead of per-vertex height (the converter classifies by height today).
5. **Region pruning:** seed points + `minRegionArea` (we have it: navmesh.hpp "Region pruning"); the 3 x 3 probe seeds of the exe [exe Q2 C31].
6. **Scale and limits:** 1,152 x 1,152 zones, 512 polygons per tile cap, the 32-bit `dtPolyRef` (section 9). The generator is per-zone; fan out per zone,
   deterministic merge order.
7. **Retire** the Recast building path first (this item), then the door mats, volume fill, hull-roof carve, wall band, `refineHeights` (all exist only
   to repair what voxels do to collision shapes), then terrain.

Open before starting: whether terrain polygon-count parity (not exactness) is the success measure. The grader is the shipped zones.

## 8. The Recast fallback switch

- **One recipe setting**: `NavRecipe::surfaceBuilder` = `Shapes` (default after sign-off) | `Recast` (today's path), also a
  command-line / env override for a diagnostic run (`--nav-builder recast`, `KENSHI_NAV_BUILDER`). Granularity: global, plus a
  per-building deny list (a building that trips the generator's invariant checks falls back to Recast for that building and counts it;
  the count is in the open log line, like the patch-cache counters).
- **The cache records what built each tile.** The tile record (file version bump; the tile-record row of "File layout" in NAVMESH_CACHE.md) gets a `u8 builder`
  (0 Recast, 1 Shapes, 2 shipped) and a `u8 clearance` (0 none, 1 Edge-band area, 2 eroded by `u8 radius`). The world key
  `w<recipeVersion>_<dataHash>` gets the builder setting folded into the recipe version, so flipping the switch starts a new tree and
  never reads the other's tiles; no migration.
- **Today's Recast building path stays in `bake.cpp` untouched** until the retirement criteria: all acceptance cases below pass for
  the Hub, the Longhouse and the census of interiors (292 shipped interiors checked), `ksim_door_sweep` has no new gaps, no
  building fell back to Recast in a full-world interior bake. Then it is deleted (user goal). `NavRecipe` default stays `Recast` until the
  review of this implementation.
- **Mixed clearance convention (ksim 2026-11-22, k16.7.138).** Generated, shipped tiles: no erosion. Recast tiles today: the wall band
  (`Area::Edge`) and, in a fallback, a possible erosion at radius 4. The rule is **one convention per tile**, recorded in the tile; the router
  reads it per portal / wall edge: wall distance of an edge in a tile with `clearance` = eroded(r0) is `d_mesh + r0` (the boundary lies r0 inside the true
  wall); `Edge` band polygons count as walkable, not as wall; a seam between conventions is a tile border, not a wall. Nothing else in the
  router changes. The N2 decision (drop the band) is separate; this design works with either outcome, keyed on the tile record.

## 9. The Detour question (noted, not decided)

- **Limits** [cache "Why not one mesh"]: `dtPolyRef` 32 bit, no `DT_POLYREF64` in the system library: 9 bits of polygon (512 per tile), 13 bits of tile
  (8,192 resident), 10 salt bits. `DT_VERTS_PER_POLYGON` 6. A generator mesh is smaller than Recast's (81 faces against 194 in the bar; not measured at world scale), so the 512 cap is not a pressure on interiors; a tile with a big terrain-like generated set could be.
  Faces with > 6 corners split in the converter; the split count is what the cap sees.
- **Convexity**: Detour assumes convex polygons; our faces are convex by construction (section 5).
- **Non-planar**: shipped faces: 22% non-planar > 0.5 u, up to 37 u [cache]. Ours are planar within 0.25 u so the detail mesh is a fan (UNSURE:
  confirm `dtCreateNavMeshData` without detail meshes gives the right `dtGetPolyHeight` / closest point; the converter writes detail
  triangles today, keep doing so).
- **Queries we use from Detour today:** `findNearestPoly`, polygon height, raycast / `moveAlongSurface`, tile streaming and the BV tree. The search with
  clearance is **ours** (one node per polygon, portal clearance, our funnel: `cache` Clearance), because the system Detour has no virtual filter. So Detour
  is the polygon store, the spatial index and the tile pool; the graph search for r > 0 is not Detour's.
- **What a generator mesh changes for queries** (observation): faces are few and large (24 k u2 on 81 faces; median 98 u2) so A* nodes at portal midpoints are coarse
  (the existing `straighten` node placement helps); the 512-per-tile cap and 32-bit ref make the tile grid the only thing the 256 u tiles are for. A
  generator mesh could carry its own spatial index (a per-building R-tree over faces) and its own adjacency with no tile clipping at all
  (a building interior is one object of tens of faces), which would remove the converter's clip / re-merge (rev N6: 122 faces from 81). Whether Detour is
  needed after the terrain stage is the question the user raised and the coordinator filed; this design deliberately keeps the `native::Mesh` form so it
  is not blocked either way. Recommendation withheld.

## 10. Acceptance

Measured with `ksim_navcoll_export` + `tools/navcoll_render.py` (k16.7.122), the original's exports on dev (inputs of k16.7.124), numbers re-run in the review's table form.
Tests added with the implementation need `cpp/tests/validity/` rows (class, source, note).

**A. Hub bar (Storm House 3384, zone 20,32, instance 4933), generator rebake** [k16.7.131 acceptance, rev N1]
1. Floor-0 tables are **exact squares**: every table footprint has its 4 edges within 0.05 u of the collision footprint, <= 4 boundary edges per footprint
   (the original: 10 edges within 0.05-0.28 u; ours today 4-25 hexagons / octagons).
2. Boundary to furniture footprint median < 0.3 u (original 0.24; ours today 1.83); axis-parallel boundary share > 70% (original 73%; ours 49%).
3. Ground floor within 0.05 u of one plane (original local y 4.41-4.42).
4. Generator **face count <= 1.5 x 81 = 121** (Detour polygons reported separately).
5. The entrance steps (top face 42.3 deg) are **one strip**: a single face per step surface, vertices within 0.1 u of the face (original -0.05 to -0.09; ours +1.3..+6.2).
6. Door face area within 10% of the door box footprint (59.9 u2).
**B. Hub bar stair (hull col_46, k16.7.134, k16.7.139 / kenshi-qenj)**
7. Stair tread polygons within 0.5 u of the hull's top face (the bake: +2..3.5 u, median 2.9; original +0.02..+0.36).
8. The stair polygons **cover the tread width**, not a thin steep strip along the beam: >= 73% of the hull's top face (2158 u2), mean covered width >= 20 u
   of 28.3, mostly land (area 0), not edge band (`tools/nav_stair_coverage.py`, k16.7.139: original 73.2% / 20.5 u; the Recast bake 61-62% at radius 0-4, its
   land 10 u wide at radius 4, the rest edge band; erosion is not the cause, so no interim recipe change; k16.7.152: the script fitted each polygon's plane through its first 3 vertices, which
   are near-collinear on some stair faces, so it under-counted converted meshes. With a Newell-normal plane: raw shipped tile (`knav_hkt_dump faces 20 32`, interior 600815) **73.4%**,
   our native-shipped conversion **73.8%** (the old fit read 34.8%: a script artifact, the conversion loses no tread; raw face 62, 1170 u2 = converted 375 + 400), our rebake 61.3%
   (unchanged by the fit). The original's 73.2% was not re-run on the new fit (no export at hand).)
9. No steep slivers on the upper floor over the stair footprint (the bake: area-5 polys 500/501/508-511, 33-43 deg, 24-64 u above the slope); faces steeper
   than 15 deg: only the stair's (original 5).
**C. Longhouse (Storm_House_05, record 4028; interior = instance 10 / 76 faces, the README's labels are swapped)**
10. The whole upper floor and its stair are present (mask union, UQ1); `knav_longhouse_door`, `knav_interior_terrain` stay green.
**D. Doors** 11. `ksim_door_sweep`: no new gaps against today's result; every placed door has its key on exactly one face.
**E. Held Recast-bake defects, re-checked on the generator's mesh** (each as filed on its bead): k16.7.82 (4014-TwoStorey ground floor in ~30 islands);
   7.83 (wall-walk floors in zone 34,28); 7.86 (entry stair / ramp cracks at 3793 and 48729); 7.105 (3384-TwoStorey Travel Gear deep0 gap); 7.107 (hull-roof carve floors at
   height_sweep cells); 7.114 (prop ring after simplification); 7.116 (bar upper storeys at ~69990, -71700). Pass = the defect as filed does not reproduce.
**F. Infrastructure** 12. `knav_deterministic`: 1 and 4 threads identical, door lists included. 13. `surfaceBuilder = Recast` reproduces today's tiles bit for bit (cache
   entries equal). 14. Router across a Shapes / Recast seam and an eroded / uneroded seam: a route the old mesh found is found, at radius 0 and 4 (`knav_clearance` extended).
   15. Report-only: generation time per interior and per zone; invariant counts (section 6) zero.

## 11. UNSURE list (oracle questions for the lead to file)

- **UQ1** (group-13 boxes: union or intersection?) `cut` §2 reads an intersection; the live Longhouse shows a union. Ask: live, for a building with >= 2 group-13 boxes, the plane count of its `hkaiPlaneVolume` and whether a face exists above the lowest box top. Default: union.
- **UQ2** (per-shape PhysX collision group) What in our collision / building data (part flags, xml, group ids) gives each shape its group (9-13, 15-22, 27)? Needed for the destroyed mask `0x82600` and group 27. Ask: kinspect, per shape of the Hub bar and a ruin: owner, group, our part.
- **UQ3** (interior weld 2.24 u) Does the live interior merge two collision vertices 1.5 u apart? Ask: a controlled pair in a live interior (the p90 2.24 u match is the hint).
- **UQ4** (cut base) How tall must a body be to cut the floor under it? Ask: a live obstacle 3 u and 6 u high on a floor: hole or not.
- **UQ5** (zero-thickness sheet walls) How does the generator treat a wall that is one triangle sheet? Ask: a live open-wall house in the Hub (e.g. a house of those with no interior collision).
- **UQ6** (60 vs 90 deg for walkable building shapes) The override table has 60 deg for materials 1, 2, 4 and 90 deg for 3 while `materialMap` sends game material 1 to 3. Ask: a live interior face between 60 and 90 deg: walkable?
- **UQ7** (BLOCK_EDGES narrowness) Does the original drop faces / edges narrower than `minCharacterWidth` 9 u or only mark them? Ask: a 6 u passage in a live interior: edge flags in the mesh.
- **UQ8** (stitch tolerances 3.0 / 15.0 Havok) Which fields, and what vertical / lateral gap does a door stitch accept? Ask: stitch a doorway with 5 / 10 / 20 u gaps.
- **UQ9** (seed points for building meshes) Does the interior job prune by seed points (region seeds `+0x3bc` / probes)? Ask: an interior with an unreachable floor piece: present in the live mesh?
- **UQ10** (terrain simplification algorithm; only for the terrain stage) the `hkaiSimplification` behaviour behind the 100 / 15 / 10 numbers (section 7).

- **Shipped gate after the stage-0 hull, rebased on ksim 27b76536 (kenshi-k16.7.142, 2026-11-23).** Against ksim's baseline (under 298, over 681): under 315, over 723, covered 96.589%, surplus 4.416%. 44 of 221 zones move, all by a few pairs (e.g. 19,29 under 5->15; 55,34 under 3->11; 18,51 over 39->47; 12,35 over 17->22; 25,50 over 11->16); no per-building report exists. The earlier run with the hull forced off reproduced the base exactly, so the rise is the hull's kept upper floors (intended, k16.7.131 replaces the Recast link). Baseline regenerated with `--write-baseline`. The hull code is unchanged since b5a45900.

## 12. Objects on our ground (kenshi-k16.7.193, 2026-11-25; implementation kenshi-k16.7.194)

User 2026-11-25: "Placing every object on that new navmesh (needs a bit of design, but it should be a simple merge, not a recast); That's rocks,
iron mines, machines, bedrolls, features, outhouses, windmills, whatever..." Input: the object census (kenshi-k16.7.187,
`docs/reviews/2026-11-25-object-census/README.md`, figures below are from it). An **object** is any placed thing with collision that is not a
masked building: zone placements without an interior mask, collided foliage (rocks, ore rocks, fallen trees), cutter discs and map features.
Masked buildings stay sections 0-6 (interior + zone side), joined to the ground by kenshi-k16.7.188; an object uses the same machinery as a
building with no mask ("with no mask: keep everything", section 0), so there is one rule set, not a second generator.

**The rule, per shape, from the use the data already gives it** (`kworld::NavUse`, `collision.cpp:983-987`, `:1130-1180`):

| use | from | the ground | its own faces |
|---|---|---|---|
| none (IGNORE) | path mode 0, `is node`, seat furniture, ore zone placements (no parts) | untouched | none |
| Projected | path mode 1, cutter discs (`navmesh cutter` radius) | cut at every height under its xz cover | none |
| Obstacle | path mode 2, `is unwalkable roof` parts, collided foliage | cut by the span rule: (ground + 0.1, ground + 18] | none |
| Walkable | path mode 3, `walkable` foliage, **map features that are bridges** (O2: other features are Obstacle) | cut by the span rule | up-facing faces, as below |

Walkable foliage lying within the climb (5 u) of the terrain at every vertex is left to the ground: no cut, no tops (`cuttersOfArea`, full-detail
terrain, so every window decides alike; kenshi-ffmi.14). TreeFall01 at (-77686, -83856) is a slab 1-3 u over the terrain that the original walks
straight across (kenshi-rwh.331: path straight through, standing height = terrain; the shipped face there has no hole). Where a walkable log stands
higher the shipped mesh lies on its top (object census, TreeFall02 / 03 in zone 15.12: 34 of 58 and 40 of 58 top cells with a shipped face within
3 u), so the class keeps its tops. The collision is posed like the drawn instance (live PhysX, kenshi-rwh.333: TreeFall01's
global pose = the drawn quaternion with its slope arc, times the z-up fold; the slab's top 2.5 u over the terrain).

So: bedrolls, campfires, tents, turrets, torches, farms, ore markers: nothing (census 1: ~2,000 IGNORE placements, as today). Barrels,
generators: a hole. Mines, windmills, rocks, market stalls, wells: a hole, no top. Wall-walks, gates' ramps, outhouses, walkboards, platforms,
walkable rocks and fallen trees, bridges: a hole plus their walkable tops (a walkable foliage piece within the climb of the ground: nothing). Every other collided map feature (cliff blocks, wrecks, rock arches): a hole, no top (O2). No class gets its own branch; a census
class that needs one is a finding against this table, raised on kenshi-k16.7.194 before it is coded.

**Walkable tops** (Walkable shapes only):
1. Faces: section 3's walkable rule on the object's shapes (weld 0.1 u within a shape, slope <= 60 deg, UQ6), the cut rule F4 between its own
   shapes and against other objects' solids above them, planar merge (section 5). Narrow faces stay (rwh.288).
2. Terrain cut: each face is split where it crosses the terrain surface (`knav::ground`'s grid triangles, not the simplified mesh), and pieces
   more than 0.25 u under it are dropped: the underground tail of a ramp, a bridge's buried ends, a sunk footing (walls sit 40-80 u under ground,
   census 1). A face within 0.25 u of the terrain over its area is coplanar ground: the ground owns it, the face is dropped (no double floor).
3. Link: object faces to ground faces and to each other through the edge matcher of section 6 / kenshi-k16.7.188 (step 5 u, the code's `matchStep`; section 0's 10 u linked Heft's roof to its
   ramparts, kenshi-ffmi.3; separation 2 u, overlap 0.2 u, rwh.289), the edges split at the overlap ends (F11). The ground's cut line under a top lies on the cutter's xz outline, so
   a ramp foot meets the ground edge where the ground resumes.
4. Prune: a component that reaches the ground (directly or through other kept components, buildings' zone sides included) stays; the rest is
   dropped, the dropped area reported per record (F5). The census' floating cases (High Walkway +40, Lagoon Platform +332, Wall Spotlight
   +85) are kept only if a ramp or a building reaches them.
5. Output keyed by record + scale + destroyed state in the object's own frame (section 0 "keyed by layout"); the terrain cut, link and prune
   run per placement in the window.

**Map features** get collision first: kworld places each MAP_FEATURES placement whose record names a `collision` file (1,629 placements in
176 records) with its pose and non-uniform scale, use Walkable, unfilled (BRIDGE_FEATURE.md, kenshi-k16.7.183 folded into 194). Their
cliff blocks and rock arches are single meshes 1,300-2,400 u deep: the terrain cut (2) removes everything under the ground, the slope rule
removes the walls.

**Which feature is a bridge: the deck rule** (ksim 2026-11-25, after O2; no record names, no list, kenshi-k16.7.197). Map features carry
no use flag, so the table's "bridges" means geometry: an up-facing feature face is a **deck face** when the clear space straight under it,
down to the terrain or the next solid of any shape, exceeds characterHeight (18 u, the cut rule's own number) over at least half of its
area. Deck faces are Walkable tops (steps 1-5); every other face of a feature is Obstacle (it cuts, no top). Cliff blocks and wreck pieces,
solid down into the ground, only cut; bridges keep their decks; a rock arch keeps its top only as a deck, and step 4's prune drops a deck
that nothing reaching the ground links to. The rule does not depend on whether the original drops cliff tops as non-walkable or as
unreachable (196's unsure 1). The shipped mesh is not the target for bridges: it has a broken strip on the Metal Arch Bridge that the user
could not cross (BRIDGE_FEATURE.md), and 1 of 914 cells on the 345 u deck at (12399, -29427). Zone placements and foliage keep the table
as is: their use is in the data.

**Windows and run time.** The ground is built per window (GROUND_NAVGEN.md); an object belongs to every window its xz bound touches, the
window's clip keeps the part inside, the rim rule keeps the seam vertices equal. An object placed or removed at run time (construction,
destruction, a scenario) rebuilds the windows its bound touches with the same steps; nothing is patched in place.

**Data oddities, by rule not by name:** an object whose every shape is more than 0.25 u under the terrain cuts nothing and keeps no face (the 6
Pontoon Bridge placements at y -41,000 .. -92,000, census `outliers.tsv`); reported, not special-cased.

**UNSURE, measured in 194 before its tests are written** (against the shipped navtiles, `knav_compare --at`, no oracle round needed):
- O1: path-mode-2 tops (mine hulls, windmill towers, market stall roofs, rocks) are not walkable in the original. Default: no faces. **Confirmed.**
- O2: map features' tops other than bridges (cliff blocks, wrecks, rock arches) are walkable in the original. Default: yes (Walkable). **Refuted, row changed.**
- O3: an object top coplanar with the terrain (within 0.25 u) is ground, not a second face. Default: the ground owns it. **Confirmed.**
- O4: the cut base 0.1 u / 18 u holds for objects as for buildings (UQ4). Default: the same rule. **Confirmed.**
A measured "no" changes the table above in one row, recorded here with the figures.

**Measured (kenshi-k16.7.196, 2026-11-25).** Method: `cpp/tools/knav_object_probe` (`knav_object_probe docs/reviews/2026-11-25-object-census/records.tsv [--tol T]`)
places the census' sample sites (3 per record: first, middle, last placement; 1,746 sites, 585 zone placements of every class of census section 1, 439 map
features of section 2, 722 foliage samples with a collision file or a cutter radius of section 3) exactly as the census did, and lays a vertical-ray grid
(step 1-12 u) over each object's xz bound. Per cell it has the object's hits, the span-rule verdict of `ground.cpp` (cut or kept) and the terrain
(`knav::ground`'s 36 u grid). The shipped world meshes of the zone and its 8 neighbours (interiors left out; `knav::native::loadTile`, the tool behind
`knav_compare --at`, which was also run by hand on the first sites: Wind Generator (-83250, -37395) has no native face over its tower, the ground beside
it is at the terrain) are sampled at each cell centre. "Face within T of y" = a native face whose height at the cell is within T u of y. Per-site rows:
`docs/reviews/2026-11-25-object-census/probe_sites.tsv` (T = 3). T = 1 gives the same answers with lower rates, said below. The shipped faces are coarse in
height: only 28 % of ring points beside the objects have a native face within 1 u of our terrain grid (T = 3: about half; a site only counts as "ground
beside it" when >= 90 % of the cells touching the object have one, 267 of 1,746 sites; both are shown where they differ).

- **O1 (confirmed; the default stands).** An up-facing top of an Obstacle-use object (highest hit of the cell, > 0.25 u over the terrain) has a native face
  within 1 u of it in 1,062 of 488,153 cells (0.2 %; T = 3: 0.3 % for rocks, 0-1 % elsewhere). Tops over 6 u (below that a top is within the
  tolerance of the ground face itself), T = 1: mine / quarry machines 0 of 27,628 cells (Quarry Mine B 57914, -119668: 1 of 561; Hybrid Stone Mine II -61061, -110901: 0
  of 592; Stone Mine VI 87845, -88664: 0 of 568); windmills 0 of 1,226 (T = 3 per site: Wind Generator 56574, -119376: 0 of 139; -83250, -37395: 0 of 78; Wind Generator II
  -24853, -103428: 0 of 70); path-mode-2 props (market stalls, scaffolds, wrecks) 456 of 82,576 (0.6 %; Market Stall A -82815, -37762: 0 of 751; B -82483, -37578: 0 of 474;
  C -82433, -37740: 0 of 565); collided foliage (rocks, ore rocks) 39 of 290,406 (ResourceRock-IRON01 -12833, -144021: 0 of 197; Blister_Rock01 -27800, -90069:
  0 of 1,673; Canyonland_FatTop01 -78597, -75807: 0 of 758); generators, lamps, farm / well objects, pm1 barrels and crates, cages and training props, no-mask buildings' pm2 parts: 11 of 51,256. The few hits are tops within 1-2 u of the ground (a crate, a stone), which the ground face covers. So: no faces for Obstacle tops.
- **O2 (refuted for every feature but the bridges; the table's Walkable row changed).** Map-feature tops (Walkable use in the probe): native face
  within 1 u of the top in 5 % of 1.04 M top cells (T = 3: 8.7 %), 19-27 % of the cells at <= 60 u over the terrain, 5 % over 60 u; of 160 records with >= 100 top cells, 4
  have >= 50 % (T = 1): Metal_Arch_Bridge (2,328 cells, 51 %; per site 598 of 604 at -54433, -95121 and 785 of 810 at 9394, 114220, but 1 of 914 at 12399, -29427: that
  placement's deck, 345 u up, has no native face), Trestle_Bridge001 (1,154, 88 %: 499 of 556 at -45980, -55948; 577 of 598 at -49481, -55123), and two
  Outpost01_RUIN junk pieces (Concrete_Junk05, Metal_Junk01). The rest, by site: Canyonland_Zone_Cliffblock_01 1668 of 4345 (-85611, -130729; T = 3), Cliffblock_04 0 % (-69917,
  -100642); ROCK-Arch_Type_01 184 of 7,879 (-30922, -18912), 79 of 4,069 (-39785, -13909), 1 of 2,818 (-29849, -17110); Barge WreckPiece02 0 of 449 (93381, 50112), 18 of 439 (63681, 7206);
  Ashland / First Civ wrecks 0-24 % per record; Fossil LegBone01 726 of 2,003 (1605, 50650) and Generic Rock Cog 394 of 564 (43005, -65076) are the partial exceptions. The census'
  per-class Walkable use for features is wrong for all but bridges: **table row changed** to "Obstacle unless a bridge". How the generator tells a bridge without a name is
  a question for the lead (candidates: a deck whose underside clears the terrain by more than 18 u over most of its span, as the Metal Arch Bridge does, 84 % of
  its sample lines in the census; the BRIDGE_FEATURE.md record list). Open: whether the native bake simply drops a top it cannot reach (the cliff plateaus
  have no ramp) rather than refuses the mesh; this cannot be separated with the shipped tiles alone (**UQ**: oracle, one cliff block with a ramp).
- **O3 (confirmed).** Where the highest up face of a cell lies within 0.25 u of the terrain (4,368 cells at 1,746 sites, all classes), the native mesh has one
  face within 0.5 u of the terrain in 945 cells, none in 3,423 (no ground there: the object stands in a hole or on a slope the native rejects), and **two
  faces in none**. By class: map features 225 one / 865 none; collided foliage 481 / 1,884; walkable foliage 20 / 268; wall gates 41 / 8; wall ramps 11 / 2; pm3 walkboards 57 / 18;
  outhouses 9 / 10; walkable no-mask buildings 15 / 11. Sites: Defensive Gate III, Defensive Wall Ramp IV and Swamp Walkboard
  (-62079, 33594; 109289, -1918) are one face each. So the ground owns a coplanar top, one face, no second floor.
- **O4 (confirmed: 0.1 u at the foot, 18 u under the span).** Obstacle-use classes (foliage, mines, windmills, generators, lamps, farms, pm1 / pm2 props, cages),
  cells by the object's top over the terrain, share with no native face within 3 u of the terrain (the "hole"), sites with ground round them / all sites:
  top <= 0.1 u: 8-11 % at sites with ground round them (the baseline; 61-72 % over all sites, where the native has no ground beside the object either);
  0.1-0.25 u: 79-86 % / 80-82 %; 0.25-0.5: 83-90 %; 0.5-1: 86-91 %; 1-2: 94-96 %; 2-4: 96-98 %; 4-8: 98-99 %; 8-18: 100 %; > 18: 99-100 %. The lowest object
  that leaves a hole is therefore **0.1-0.25 u** high (1,188 cells; T = 1: 82 %), which is the rule's 0.1 u. Walkable-use classes (walls, gates,
  ramps, boards) read 0-20 % because their own top is the face within 3 u; they are not evidence either way. The 18 u upper end, by the clearance under the
  object (lowest hit over the terrain, cells whose lowest hit is an underside): 0.1-4 u 97 %, 4-8 97 %, 8-12 97 %, 12-16 96-98 %, 16-18 92 %, **18-20 53 % / 50 %**,
  20-24 9-12 %, 24-32 5-11 %, 32-64 5-8 %, > 64 2-18 % (sites with ground round them; the baseline is 5-12 %). The native mesh keeps its ground under an object
  with 20 u or more of clearance and loses it under less than 18: the rule's 18 u is where the measurement turns. The same rule, no change.

Limits: the shipped mesh's height is coarse against our grid (the T = 1 / T = 3 pair above brackets it); the tops are the highest up face of each cell, not every
layer (a stall's inner floor, a bridge's lower deck are not counted); site counts are 3 per record, rates are cell-weighted so the huge features dominate O2.

**Status (kenshi-k16.7.194 / kenshi-k16.7.197, 2026-11-25), steps 1-5 in.** Done in 194: kworld places every MAP_FEATURES placement whose record names a
readable `collision` file (`CollisionLibrary::addFeature`; Walkable, unfilled; `AreaGeometry::features`, outside `flatShapes()` / `areaDigest()`, so the Recast bake
and the nav cache key do not move); `gatherCutters` adds them as cutters. Done in 197, all in `knav::ground` (`Gen::tops`, `Gen::linkTops`, `ground.cpp`):
- A `Cutter` carries `top` (None / Zone / Deck) and a `label`. Zone: the Walkable-use shapes of a building with no interior mask and of walkable foliage;
  Deck: the Walkable-use shapes of map features. A masked building's tops stay surfgen's. A convex solid's triangles are wound outward at the gather.
- Step 1 faces: up-facing triangles at most 60 deg (`topSlopeDeg`), one polygon each; the feature deck test (below); the window's xz box clips them.
  Step 2 terrain cut: each face is clipped to the grid's terrain triangles (the diagonal rule of `build`), and over each the part more than 0.25 u above the
  terrain stays (the line where the two planes differ by 0.25 u); a face wholly above every terrain sample under its box skips the split. F4: the span rule of
  the ground cut (`cutByCutter`, now shared with the ground cut) against every cutter above the face, its own shape's other faces included (hits within 0.05 u of
  the face's own plane do not count); a part of a piece that a cutter takes nothing from comes back whole, not as the fragments of its cut lines (the split tree
  of `cutByCutter`, kenshi-k16.7.212: zone 52,52 was 4.4 M top pieces and 311,536 faces, 31 tiles over the polygon cap, now 88,959 and 17,920). Pieces under 0.04 u2 or thinner than 0.02 u are dropped. Step 3 link: `linkTops`, after the planar merge (the tops go through
  the same Hertel-Mehlhorn pass as the ground, no merge across the two since they share no vertices): boundary edges of different faces, at least one a top, run against
  each other (cos < -0.9), within 2 u in xz and 5 u in height over 0.2 u or more: the stretch of the overlap where both hold (two long edges that run together for part of their length link over that part, kenshi-k16.7.212; with both ends of the whole overlap tested, a link hung on how finely the faces were cut); every pair of faces whose edges meet this rule is one component for the prunes (step 4 and `pruneJoined`, `Stats::topTouching`), linked or not: a stretch links once, and a deck that met its neighbour only along an edge already linked to another face was dropped whole (zone 41,54); stretches are claimed longest first, edges are split
  at the overlap ends (each edge at its own end, ends within 0.01 u snap), and the sub-edges are paired as opposite edges (`Edge::oppositeEdge`; the two sides may
  differ in height). Step 4 prune: union-find over shared edges and links; a component stays when it holds a ground seed point or a rim edge (a top's seed test is
  skipped: a bridge over a seed point proves nothing); the dropped top area is reported per `Cutter::label` (`Stats::topDropped`; a placement id, "foliage" or
  "feature @ x, z"; shapes carry no record name). Step 5: this is per window; keying by record + scale is not done (the ground is built per window, nothing is cached).
  `Params::keepLooseTops` keeps the pruned tops (debug). Stats: `topShapes / topTris / deckTris / deckRejected / topPieces / topFaces / topLinks / topDropped* /
  msTops / msLink / underTerrain / topFaceIndex`. No Recast call anywhere on the path.
- The deck rule is in `Gen::clearUnder`: from the face down, the first surface decides: an underside (down-facing hit: the cooker's winding) means the face tops a solid,
  and the clear space is from that underside to the next top surface or the terrain (none when the underside is not above the terrain: a solid standing in the ground);
  a top surface or nothing means a sheet, clear down to it or the terrain. A face is a seed when more than 18 u is clear under half of its samples
  (1-16 per triangle). **Deviation D1:** a face of the same shape that touches a seed (shares a vertex) and has any clear space under it (> 0.1 u) is a deck face too,
  flooded from the seeds: the deck's end spans meet the rim with less than 18 u under them, and with the per-face rule alone the Metal Arch Bridge broke 100 u short of
  the ground at both ends (its 226 triangles are 30-60 u long). A face standing solid on the ground never qualifies, and a shape whose seed faces cover under `deckShapeShare` (0.2) of its up-facing area is no deck at all (F1), so cliff tops stay out.
- The feature gather uses the shape's reach against the window (`CollisionLibrary::featureReach`, the largest model vertex distance times the largest scale axis), not
  only `positionCell` +-1 (ksim's comment on the bead).

**Measured (kenshi-k16.7.197).** Tests: `knav_ground`, `knav_ground_features`, `knav_ground_objects` (new), `knav_surfgen`.
- Metal Arch Bridge, windows +-1200 u: 53 / -32184: one component of 688 faces over 1,291 u, 13 ground links within 150 u of each end; -54433 / -95121: 649 faces over
  1,161 u, 17 / 17; 12399 / -29427 (the 345 u deck): 867 faces over 1,224 u, 19 / 9. The canyon floor under the arch stays covered (76 of 122, 112 of 118, 86 of 134 deck
  vertices 60+ u up). The parts of the shape that reach nothing are dropped (-54433: 527 faces, 42,186 u2).
- Fixture sites (`knav_ground_objects`, the census README's first sample site per class, a sampling box of the object's reach; hole = ground area lost against the same
  window built without obstacles; tops = a top face in the box whose component reaches a ground face over a link): building with no mask, wall ramp, outhouse, pm3
  walkboard, walkable foliage over the climb (TreeFall03, -75884 / -88229), Trestle and Metal Arch bridges: hole + tops linked. Walkable foliage level with the
  ground (TreeFall01): no hole, no top. Mines, windmill, generator, pm1 barrel, pm2 stall, Obstacle foliage,
  cage, gate (masked): hole, no top. Ore node, turret, camp bed, torch, farm, pm0, grass: no hole. An isolated wall segment: hole, its wall-walk top is dropped (no ramp
  reaches it; the ramp site links its wall-walk to the ground over 313 faces in 200 u). Rock arch (-30922, -18912): hole, 199 top faces in reach, not linked (kept by the
  window rim: the arch is bigger than the sampling window). Wreck piece: hole, no top (1,132 faces dropped).
- The 6 Pontoon Bridge placements: every one reports 5-10 shapes wholly under the window's lowest terrain (`Stats::underTerrain`), which cut and keep nothing.
- Time per window (this VM, one thread, `generateBox`): a +-300 u window 0.35-1.2 s, of which gather ~0.3-0.5 s, tops 0.001-0.15 s and link up to 0.18 s (Heft patch
  800 u: tops 4 ms + link 1 ms of 467 ms; Trestle site: 153 + 178 ms of 1.2 s). Zone 46.16 (Heft, 4,608 u): total 3.3 s: tops 0.68 s + link 0.81 s, 20,522
  top faces of 22,329 faces (8,574 more dropped), 53,011 links.

**Findings (class that needs its own rule: reported, not coded).**
- F1, cliff blocks (FIXED, kenshi-k16.7.200): the face test alone passed the faces of a block whose underside overhangs the terrain by more than 18 u (Canyonland_Zone_Cliffblock_01
  at -85611, -130729: 97 of 260 up faces; 2,404 faces linked to the ground). **Second condition, picked by measurement: a shape-level seed share** (`Params::deckShapeShare` = 0.2): a
  feature shape's tops count only when its seed faces cover at least 20 % of its up-facing area (area-weighted); below it the shape is
  no deck and every face is Obstacle. Measured with `knav_ground_objects` `OBJ_SITE` (+-300 u reach, +-700 u window; "kept" = linked top faces; `DECK_SHAPE` / `DECK_SLOPE` env
  override the params), 77 of 439 feature sites of 196's `probe_sites.tsv` (all 38 with >= 50 % native share and a sample of the rest) plus the fixture sites; native share =
  196's per-site share of top cells with a shipped face within 3 u:

  | Candidate | Cliffblock_01 / _04 | Metal Arch (-54433, 12399, 53; the 9394, 114220 site has no top even without the rule: unsure, not checked) | Trestle (2 sites) | rock arch, wreck (5 sites) | native >= 50 % sites kept (of 38; 30 kept by the face test alone, 8 have no top at all) |
  |---|---|---|---|---|---|
  | none (face test + D1) | kept 2,404 / none | kept | kept | none (no seeds) | 30 |
  | (b) slope cap 40 deg | kept 2,405 | kept | **-45980 broken (2 of 1,089 faces)** | none | 25 |
  | (b) slope cap 30 deg | kept 2,340 | kept (a few faces less) | **both broken** (0 / 0) | none | not counted |
  | (a) shape share 0.5 | dropped | kept | **-45980 broken** (2) | none | 12 |
  | (a) shape share 0.25 | dropped | kept | kept (-49481: 22 of 25, -45980: 1,087 of 1,089) | none | 21 |
  | **(a) shape share 0.2 (picked)** | dropped (share < 0.1: S = 0.05 keeps it, 0.1 drops it) | kept (333 / 365 / 307 top faces, unchanged) | kept (1,089 of 1,089; unchanged to S = 0.3) | none | 21 (as 0.25) |

  Why not the slope cap: a cliff block's overhang slopes are shallow, so a cap of 30-40 deg keeps 97 % of its linked faces while it cuts the Trestle Bridge's own ramps. Why
  0.2: the cliff block's share lies between 0.05 and 0.1 and the Trestle Bridge's is above 0.3 (it breaks at 0.4) and the Metal Arch Bridge keeps all its faces at every S tried; 0.2 sits in the gap (S = 0.1 and 0.3 give the same three sites). The records the share drops although their shipped share is >= 50 % are rock formations, not bridges (RockStack01-04, Rock Cog,
  Cave-Spirng, Barge WreckPiece01, Fossil LegBone01, ROCK-Arch_Type_03: 9 of the 38); ksim's call is that the shipped mesh is not the target for decks, so they stay Obstacle. The
  records it keeps although their shipped share is under 50 %: Outpost01_RUIN junk (0.22-0.27), Gravel-Pile02 and AshChimneyFunnel (the same overhang geometry; a
  share rule cannot tell them from a deck). Raw sweep rows were not kept (the working files are under the dev worker's scratch dir for kenshi-k16.7.200). D1 stays: the flood still
  runs inside a shape that passed.
- F2, cutter discs (FIXED, kenshi-k16.7.199): the Projected disc of `navmesh cutter` foliage is a 12-sided prism with side walls and no caps, so the hit test never
  cut the ground. `Gen::cut` now also takes the xz outline of a Projected cutter's vertical walls (deduplicated segments): the ground is split along them and a piece
  is dropped when its centre is inside (even-odd), at every height, open or closed. FOLIAGE_Boulder01 at -71596, 616 now shows its hole (asserted).
- F3, mounted turrets stand on wall-walks: their site shows the wall's top (IGNORE adds nothing).
- F4, tops touching the window rim are kept even when no ground links them (the rim rule); the neighbour window decides. A bridge cut by the window rim keeps its half.
- F5, drop report per placement id or feature position, not record name (WorldShape has none).
- F6, Heft: tops are 92 % of the faces of zone 46.16's mesh (walls, floors and stairs of no-mask buildings: 1,893 shapes); the mesh grows 12-fold. A consumer that wants a
  smaller ground can drop the tops on a distance rule or merge at 0.99 planarity more aggressively; not tuned.

**Done when (kenshi-k16.7.194):** (O1-O4 measured, kenshi-k16.7.196, above); every class of census 1-3 has a
fixture site that shows its row of the table (hole, no hole, top linked to the ground); the Metal Arch Bridge is walkable deck to deck
end to end, joined at both rims, the canyon floor under the arch walkable; the Pontoon outliers report and are otherwise inert; no Recast
call on the path that builds a window with objects; generation time per window reported.
