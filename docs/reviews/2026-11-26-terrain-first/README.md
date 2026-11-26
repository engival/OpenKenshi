# Terrain-first whole-world pass and height / xz slop straightening (kenshi-k16.7.213)

Exploration with a prototype behind an option; the production mesh, save version and nav recipe do not change.
Code: `ground::Params::straighten` / `slopY` / `slopXZ` (default off; `cpp/include/knav/ground.hpp`, `cpp/src/knav/ground.cpp`: border rule in `Gen::evaluate`,
merge rule in `Gen::finish`), probe tool `knav_terrain_first` (`cpp/tools/knav_terrain_first.cpp`).
Runs: 4 threads under the worker lock, one ground window per 4608 u zone (36 u grid, stride 2, no cutters, no seeds), Release.

## 1. Terrain-only first pass, whole world (4096 zones)

| | faces | vertices | edges | wall (4 threads) | CPU |
|---|---|---|---|---|---|
| production Params | 3,860,756 | 6,156,455 | 15,244,622 | 218.7 s | 874 s (0.21 s / zone) |

The whole terrain pass is **under 4 minutes wall, 15 CPU-minutes**. The world bake (the 9 h figure in NAVMESH_CACHE.md "Shared tree") is
therefore not spent on terrain: it is the cuts, tops, buildings and links. 157,259 faces have more than 6 corners (largest 512): the planar merge
makes big convex polygons; the tile converter must split these for Detour (not measured here).

## 2. Straightening with slop

Sample sweep (every 8th zone in x and z = 64 zones, `--eval`), faces by setting (slopY, slopXZ in u):

| setting | faces | vs prod | vertices | corners >6 | mean height error (plane, vs 18 u samples) | CPU |
|---|---|---|---|---|---|---|
| prod | 64,487 | | 102,566 | 2,588 | 3.27 | 13.0 s |
| 1, 1 | 63,767 | -1.1% | 102,568 | 2,618 | 3.27 | 13.0 s |
| 2, 2 | 58,246 | -9.7% | 99,895 | 2,669 | 3.35 | 13.0 s |
| 3, 2 | 52,651 | -18% | 97,923 | 2,924 | 3.39 | 13.0 s |
| 5, 2 | 45,185 | -30% | 95,655 | 3,581 | 3.47 | 13.0 s |
| 10, 2 | 36,481 | -43% | 93,009 | 5,156 | 3.76 | 13.0 s |
| 2, 1 | identical to 2, 2 | | | | | |

Whole world (no eval): prod 3,860,756 faces; 2,2: 3,488,006 (-9.7%, vertices 6.00 M); 5,2: 2,702,021 (-30%, vertices 5.76 M). The sample predicts the world within 0.1%.

Findings:
- The gain is in **faces, not vertices** (vertices -6% at 5 u): the slop merges the fans into bigger polygons, it does not remove corners. Detour counts polygons, so faces is the number that matters for the 512-per-tile cap and the node count; but bigger polygons mean more 7+ corner polygons (+38% at 5 u), which the converter splits again, so part of the gain is returned there (not measured).
- The slop is cheap: no CPU change.
- Slop in xz changed nothing at 1 vs 2 u (2,1 = 2,2): the height rule (merge deviation, border chord) is what binds. Outline edges 61,127 -> 58,505 (2,2) -> 54,487 (5,2).
- **Production is already loose in height**: the simplifier's `heightError` is 15 u. Mean error against the 18 u samples is 3.3 u already; the slop adds 0.1-0.2 u at 2-5 u. The "hills kept" bound is that 15 u rule, not the slop.
- **Limits of this measurement (unsure; how to verify):**
  - The largest height error is 446 u in every setting including the unsimplified reference: it is a single 18 u sample over a cliff / sea edge against the 36 u grid, so the maximum does not discriminate. A p99 or a per-zone worst split by slope is needed before a bound is claimed (add to `heightError()` in the tool).
  - The xz drift out->ref is 17.65 u in prod and 25.46 u at (2,2) (242 / 382 of 2.8 M outline points over 2 u): these are above the 2 u bound, so some outline moves are not governed by the new rule (the trim and the inner removal of boundary vertices that neighbour a removed one). Not chased; the cause needs a look at the 140 extra points at 2,2.
  - "ref->out gone" (12% of the unsimplified outline, 378,230 of 3.2 M points) is the same in every setting: tiny walkable islands the production simplifier removes entirely.
  - Measured on terrain only, without cutters: the cutters' own edges are not straightened.

## 3. Is the height a suggestion? Code paths

- **Client** stands a character on its own ray: `client/src/bridge/simview.hpp:265` `kRayUp = 1.5, kRayDown = 4.5, kDrawnDown = 8.0`: the drawn ground is looked for within +1.5 / -4.5 u of the wire (navmesh) y (terrain competes inside that, floors 8 u down); `simview.cpp:203-221` (`groundAt`, `standRayAt`), `solveGround`. No hit in the window: the navmesh y is kept (it floats). The terrain used is the drawn triangles, `client/src/main.cpp:4224`, `client/src/host/session.cpp:849`. The comment at `simview.hpp:264` records the navmesh already 0.45-0.71 u over plank floors and up to ~4 u off on slopes. Tolerance: terrain may be 4.5 u below the mesh y or 1.5 u above it. **The mesh may drift up from the terrain by up to the remaining ~0.5 u of the 4.5 on slopes, but only 1.5 u down** (mesh under the terrain by more than 1.5 u: no anchor, the character sinks).
- **Sim**: the navmesh IS the sim's y; there is no terrain re-anchor on the walk. `knav/walk.cpp:13` `kStand` looks a standing body up within 1 u in xz and 12 u in y; `walk.cpp:20-25` a hop's rise over 20 u + 3 per u walked stops (`kHopRise`, `kMaxGradient`), a hop that lands over 8 u off the layer snaps back (`kLayerReach`). `Walker::snap` / `surfaceY` (`walk.cpp:258-275`) take caller reaches (40-800 u in the callers, `ksim/ai/ai.cpp:117`, `world_resolvers.hpp:1504`). A height error of 2-5 u is far inside these. The polygon height seen is the detail-mesh corner fan (`native_tiles.cpp:862-905`), so the fan error (equal to the plane error in the sweep) is the one that counts.
- `ksim/nav/los.cpp:331` compares with `terrainHeight` only for line of sight.

## 4. Recommendation

- **Terrain-first base does not make the full bake cheaper.** The terrain pass is 15 CPU-minutes of a bake measured in hours; cutting objects into a pre-built base would save at most that, and the generator already builds the ground grid once per window and shares rims (k16.7.215). The cost is the per-zone object work (cut, tops, linkTops, surfgen); it does not shrink with a base. The useful by-product is the measurement: terrain alone is 3.9 M faces, so the bake's output size and the 512-polygon-cap pressure come from objects.
- **Straightening is a size lever, not a speed lever:** -10% faces at slop 2 u, -30% at 5 u, with a mean error rise of 0.1-0.2 u. A safe bound by the anchor code: height slop of **2 u** (inside the client's -1.5/+4.5 window with room, far inside the sim's 12 u / 8 u). Do not go past 3 u until the p99 and the xz outliers above are checked, and note the +38% of 7+ corner polygons at 5 u. xz slop of 1-2 u gave no extra gain in this prototype.
- Not done: applying the pass to cut windows (only the terrain-only case was run), tile-converter polygon counts after splitting, any change to the production recipe.
