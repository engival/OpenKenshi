# `data/globalPathing.path` — format

A coarse world-scale portal graph. Reverse-engineered from the bytes alone:
no reader was found (the string `globalPathing`/`.path` occurs nowhere in
`kenshi_x64.exe`, in ASCII or UTF-16, nor in the FCS sources). The file may be
unused by the shipped game. Field names are descriptive.

Validator: `tools/global_pathing.py <globalPathing.path>` (consumes every byte,
checks every invariant below, prints `PASS`).

## Summary

| Property | Value |
|---|---|
| Size | 1,225,412 bytes = `12 + 2587·40 + 93,492·12 + 16` exactly |
| Nodes | 2,587 portals, IDs 1..2587 in file order |
| Edges | 93,492 directed = 46,746 undirected; every edge stored in both directions with identical cost and class |
| Grid | 16×16 blocks of 18,432 units (each block = 4×4 zones), see [Coordinate system](../ARCHITECTURE.md#coordinate-system) |
| Height | none; portals are 2-D (X/Z ground plane) |

All little-endian, no padding.

## Layout

```
u32 unk0 = 3              unknown (version?)
u32 count = 2587          node count
u32 count2 = 2587         equals count; meaning unknown
node[count]               variable length, no size prefix
u32[4] = {0, 16, 0, 16}   trailer; unknown (grid bounds 0..16 per axis is a guess)
```

### Node (40 + 12·E bytes)

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0 | u32 | `id` | 1..2587, sequential |
| 4 | u32 | `a` | block column (X), 0..15 |
| 8 | u32 | `b` | block row (Z), 0..15 |
| 12 | u32 | `na` | neighbour block column |
| 16 | u32 | `nb` | neighbour block row |
| 20 | f32×4 | `minX minZ maxX maxZ` | portal rectangle, world units |
| 36 | u32 | `E` | edge count (2..79, mean 36.1) |
| 40 | E × edge | | |

### Edge (12 bytes)

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0 | u32 | `target` | node id, never self, no duplicates per node |
| 4 | f32 | `cost` | traversal cost |
| 8 | u32 | `class` | only 20 (33,044 edges) or 27 (60,448); meaning unknown |

## Portal geometry (0 violations over all 2,587)

Block `k` spans `[-147456 + 18432·k, -147456 + 18432·(k+1))` on its axis.
Every portal is a 1-unit-thick rectangle on an internal block boundary:

| Kind | Count | Test | Boundary | Neighbour |
|---|---|---|---|---|
| H | 1,263 | `maxZ - minZ == 1` | `minZ = -147456 + 18432·(b+1)`; `[minX,maxX]` inside column `a` | `(a, b+1)` |
| V | 1,324 | `maxX - minX == 1` | `minX = -147456 + 18432·(a+1)`; `[minZ,maxZ]` inside row `b` | `(a+1, b)` |

- Interval endpoints are multiples of 72 units (4608/64).
- Only the 15+15 internal boundaries carry portals; none on the world rim.
  Boundaries are ~87 % covered; gaps presumably mean impassable.

## Graph semantics

- Every edge joins two portals whose block sets `{(a,b),(na,nb)}` share
  **exactly one** block: an edge is a path *across* that block between two of
  its boundary portals.
- `cost / distance(portal midpoints)`, median: class 20 ≈ 0.99, class 27 ≈ 3.96.
  So cost tracks Euclidean distance, with class 27 ≈ 4× more expensive.
  Whether class is a terrain type, and the exact cost formula, are unknown.

## Unknown

Header words 0 and 2, the trailer, edge `class` semantics, the cost formula,
how portals were generated, and whether the game reads this file at all.

## Related: `data/roads.level`

12 bytes: an empty GameData v13 container (`13, lastID 0, 0 records`).
The actual road network is in `newland/leveldata/Newwworld/leveldata.level`
(see [WORLD_LAYOUT.md](WORLD_LAYOUT.md#roads)).
