# Navmesh straightening part 2: bounded drift, cut windows, Detour polygon counts (kenshi-k16.7.220)

Measurement only; production Params, save version and nav recipe unchanged (`Params::straighten` stays off). Follows
`docs/reviews/2026-11-26-terrain-first/README.md` (kenshi-k16.7.213). Probe: `cpp/tools/knav_terrain_first.cpp`, Release, 4 threads under the worker lock.
New in the probe: `--cut` (a window is `generateBox` with cutters and tops, not terrain only), `--zones X0,Z0,X1,Z1`, `--tiles` (the window's mesh through
`nativeTiles`, 256 u tiles: Detour polygons after the 7+ corner split), p50/p90/p99/p99.9 of the 18 u sample error per face-slope band, the drift split into
ground faces and top faces, `--outliers N` / `--dump X,Z,R` (print the outlier points / the outline around a point), `--cfg prod|SLOPY,SLOPXZ[,trim=W][,post=0|1]`.
Height errors in cut windows count ground faces only (tops are not terrain).

## Result: the four numbers

Sets: **terrain** = 64 zones (every 8th, no cutters, as 213); **Hub** = zone 52,52 cut (misnamed: it is the Ashland Dome I site in the Ashlands,
south-east of the map, not The Hub; `tools/name_site.py 94177 94434`; the label "Hub" below means this set) (2596 cutters, 14.5 k top faces); **forest** = zones 20,30 20,31 21,30
21,31 cut (foliage 268-1354 cutters each, a few buildings). Setting is slopY,slopXZ in u.

| set | setting | faces | Detour polys (after split) | max polys / tile | p99 plane error (18 u samples) | out->ref outline drift: max, points >2 u |
|---|---|---|---|---|---|---|
| terrain | prod | 64,487 | 246,036 | 104 | 15.9 | 17.65, 150 of 2.81 M |
| terrain | 2,2 | 58,246 (-9.7%) | 237,969 (-3.3%) | 104 | 16.5 | 25.46, 290 (see island bridge) |
| terrain | 3,3 | 52,651 (-18%) | 231,033 (-6.1%) | 104 | 16.5 | 17.65, 150 (slop 3: bound is 3) |
| Hub | prod | 18,082 | 52,760 | 1493 | 12.5 | 36.96, 432 of 347 k |
| Hub | 2,2 | 16,664 (-7.8%) | 51,116 (-3.1%) | 1481 | 13.1 | 36.96, 956 |
| Hub | 3,3 | 16,337 (-9.7%) | 50,754 (-3.8%) | 1479 | 13.4 | 19.56, 6908 |
| forest | prod | 17,859 | 31,922 | 208 | 13.6 | 11.00, 193 of 470 k |
| forest | 2,2 | 11,170 (-37%) | 22,875 (-28%) | 126 | 13.7 | 11.00, 42 |
| forest | 3,3 | 9,374 (-48%) | 20,358 (-36%) | 122 | 13.8 | 11.00, 9371 (slop 3) |

- **Polygons, not faces, are what Detour sees**: the faces gain shrinks to a third on terrain (-9.7% faces, -3.3% polys at 2 u) because the big merged polygons are
  split again (6 corners max, `kMaxCorners`); in forest windows (many cutter holes, small fans) the gain stays (-28% polys at 2 u). The 8192-per-tile cap
  (`kMaxPolysPerTile`; the "512" of the bead is the largest face's corner count in the probe) is nowhere near: no tile over the cap in any run; the Hub's densest tile
  has about 1480 polygons in prod and 2,2 alike (tops and cutters, not terrain).
- **Height (p99, per slope band, 18 u samples against the polygon's plane; prod's own `heightError` is 15 u)**, terrain: p99 by band 0-5 / 5-15 / 15-30 / 30-40 deg:
  prod 15.9 / 17.4 / 14.9 / 12.8; 2,2 17.1 / 17.7 / 14.9 / 12.7; 3,3 17.1 / 17.7 / 15.0 / 12.6. The flat band takes the most (+1.2 u at p99; p50 unchanged at 0.2),
  hills (15-40 deg) take nothing. Mean 3.27 -> 3.35 (2,2) -> 3.39 (3,3). Hub and forest are the same shape (+0.1 to +0.9 u at p99, per-band tables from the
  probe). The maximum is still the one sea-edge sample (446 u, 100 samples over 100 u in every setting incl. prod): it never discriminated, p99 does.
- **The "hills kept" bound** holds for all three settings: no band above 30 deg moves at p99, the changes are in the flat bands where the 36 u grid's own noise is.

## The xz outliers: which rule

1. **The 25.5 u of 2,2 (213's note) is not the trim, not the inner removal and not the merge.** `trim=0,post=0` changes nothing on terrain (no spikes there); in cut
   windows removing the trim does not remove the outliers either (Hub 2,2: 956 points >2 u, `trim=0` 1091, `post=0` 864). The border rule bounds the points it
   drops against the original outline (checked: forest 2,2 max drift 2.03 against prod 3.14; the same test is why 3,3 never exceeds 3.00 there).
2. **Terrain 2,2: islands bridged.** In zone 40,56 (outlier vertex 40320, 114336) the stats show 375 components, 367 dropped by the prune (a component that reaches no
   window rim and no seed goes): prod and the unsimplified reference drop 591 / 606 faces, 2,2 only 560. A chord removal that fills a pinch (two outline points
   within 2 u of each other) joins a small walkable island to the rim component, the prune keeps it, and its outline then sits far from the reference (which dropped
   it). 140 outline points of 2.8 M, none at 1,1 or 3,3 in the sample. It is walkable ground the slop made reachable, not an outline moved: it cannot be bounded by
   the same original-outline test (the test is local to a chord; the bridge is a connectivity change). Cheapest guard if it matters: in `Gen::evaluate`'s straighten
   branch refuse a removal whose chord A-B passes within slopXZ of an outline vertex not in the ring (the pinch case); not done here.
3. **Cut windows: the outline metric is polluted at seams.** Out outline edges that have no opposite are counted, including a ground polygon's long edge against a top
   or cutter-cut edge whose neighbour keeps more vertices (T-junctions the later weld/stitch closes). Hub: ground-face outline >2 u 238 (prod) -> 404 (2,2), top-face
   outline 274 -> 797; the maximum (37 u) is a top-face value in prod and 2,2 alike. The unsimplified reference had slits too when removal limits were 0 (a coplanar
   removal leaving a pinched star), which made the old 25 u partly an artifact: the reference is now built with no removal at all (negative limits).
   Unsure, to verify: that the Hub's +524 points >2 u are all seams and none are chords; compare after `weldMesh` / stitch, or restrict the outline to
   edges whose both end vertices belong to ground faces only.

## Recommendation

- **Switch on 2,2 (`straighten = true`, `slopY = 2`, `slopXZ = 2`)**; it is what the user asked ("1-2 u drifts"), the height rise is in the flat bands only (p99 +0.6 u over
  all samples, hills 0), and the outline of cut ground never leaves the 2 u band (forest max 2.03). Expected Detour polygon change: about -3% in open terrain and town cores,
  -28% in forest-like windows with many cutter holes (face counts -8% to -37%). 3,3 gains a further 3-8 points of polygons but breaks the 1-2 u promise (its >2 u points
  are all inside its own 3 u bound).
- Before the recipe bump: decide whether the 140-point island bridge (item 2) is acceptable (each is a walkable fleck of at least a cell that prod would prune); the
  guard above is the cheap alternative. And re-run the bake's links / door tests: the Hub's tile count of polygons barely moves, so the benefit there is small.
- Anything beyond 3 u is not recommended without the same per-band check.

## Follow-up (kenshi-k16.7.221): pinch guard, Hub seams, switch-on blocked by knav_ground

- **Pinch guard** (`Gen::indexOutline` / `Gen::pinched`, `cpp/src/knav/ground.cpp`): the straighten branch of `evaluate` refuses a boundary removal whose chord
  ends at a pinch vertex (two edge-connected pieces of ground touch there; in 40,56 the vertex 40356,114336) or passes within `slopXZ` of an outline vertex of
  another piece. Pieces are the edge-connected triangle components before any removal, so a cutter hole and the outer outline are one piece (no cost in cut
  windows). A removal past the bound still passes by the old border rule only when the guard is not what refused it. Zone 40,56: drift out->ref 25.46 -> 0.00, 0
  points over 2 u (prod 0.00), components 375 / dropped 367 as prod (dropped faces 561 vs prod 591: fewer, bigger faces, same components). 64 zones: drift
  17.65 / 150 points as prod (2,2 unguarded: 25.46 / 290).
- **Guard costs (2,2, Detour polygons after the split)**: terrain 64 zones 246,036 -> 238,257 (-3.2%; faces 64,487 -> 58,317); Hub 52,760 -> 51,126 (-3.1%; faces 16,664);
  forest 31,922 -> 22,919 (-28.2%; faces 11,165). Unguarded: 237,969 / 51,116 / 22,875. A first guard that also refused when the other outline was the same piece
  (cutter holes) made Hub and forest worse than prod; the piece test fixed that.
- **Hub outliers**: ground outline points more than 2 u from the reference and not on a top face's edge (probe line "ground outline off top-face edges"): prod 168 of 96,719,
  2,2 303 of 96,055 (max 18.6 / 23.2). So of the +233 ground-face points, 135 are not top seams; the rest of the outliers sampled are zero-length edges, tops and
  36 u ground-ground edges whose neighbour keeps a vertex (T-junction). Not bounded further: no `weldMesh` comparison done.
- **Switch-on NOT done**: with `straighten = true` at 2,2 `knav_ground` fails ("zone 52.52 patch: no face under a cutter between ground + 0.1 and + 18 u": a 17-corner
  and an 18-corner face, centres 94093.5,93923.4 and 93895,94376.6, corner heights 10-16 u over the grid ground) and at slopes 1,1 it fails "no corner repeats in a
  face". Both reproduce with the unguarded straighten of 220, so they are not the guard; they pass at 0.5 and 0.01, and zone 46.16 also grows needles with inradius
  1-2 u (the test exempts under 1). `knav_ground_objects knav_cutover knav_longhouse_door knav_outpost_floor2_bake ksim_raid_goals` pass at 2,2.
