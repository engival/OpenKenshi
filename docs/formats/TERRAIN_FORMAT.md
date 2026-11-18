# Terrain and map files — `data/newland/land/`

World coordinates: see [Coordinate system](../ARCHITECTURE.md#coordinate-system)
(x, z ∈ [-147456, +147456), y up, 64×64 zones of 4,608 units).

Validator: `tools/terrain.py <data/newland/land>` (heightmap fit, `features.dat`,
`fogfeatures.dat`, `blendinfo.dat`, blend/biome identity).

## Image ↔ world mapping

All images below are oriented the same way: **column = x ascending, row = z
ascending, row 0 (image top as displayed) = z = -147456**. No flips, no axis swap.

| Map | Pixels | Units/px | px/zone | Orientation evidence |
|---|---|---|---|---|
| `fullmap.tif` (heightmap) | 16385² | 18 | 256 | strong: r = 0.979 vs seeds; flips/swap r ≤ 0.14 |
| `blendmap.png`, `biomemap.png`, `overlaymaps/biomemap.png`, `overlaymaps/ambientmap.png` | 1024² | 288 | 16 | weak: η² of height by biome 0.333 vs ≤ 0.295 for flips |
| `areasmap.tga` (BIOME_GROUP regions) | 256² | 1152 | 4 | moderate: η² 0.612 vs ≤ 0.448 (after honouring the TGA bottom-left origin) |
| `overlaymaps/colour.X.Y.png` (8×8 tiles of 2048²; X = tile column, Y = tile row) | 16384² | 18 | 256 | strong (client session, 2026-10-30): land-mask IoU vs `GUI_Map.dds` 0.920 vs 0.778 for the runner-up (each tile transposed), 64 swap/flip/per-tile variants tried. It is a pre-lit tint/erosion colour map, not satellite imagery; the client uses it as terrain albedo |
| `overlaymaps/new_overlay.X.Y.png` (8×8 of 1024²), `new_overlay.png` | 8192² | 36 | 128 | assumed |

`col = (x + 147456) / unitsPerPx`, `row = (z + 147456) / unitsPerPx`.
For the heightmap, 16,385 samples span exactly 16,384 × 18 = 294,912 units;
whether sample `i` sits at `-147456 + 18i` (vertex) or `+9` (centre) is
unresolved (fits are equal).

## `fullmap.tif` — heightmap

Classic little-endian TIFF, single IFD, Photoshop CS6 export, no GeoTIFF tags.

| Tag | Value |
|---|---|
| Width × Length | 16385 × 16385 |
| BitsPerSample / SamplesPerPixel | 16 / 1, unsigned (no SampleFormat tag) |
| Compression | 1 (none) |
| StripOffsets / RowsPerStrip / StripByteCounts | 43,686 / 16385 (one strip) / 536,936,450 = 16385²·2 |
| Orientation | 1 (top-left) |
| File size | 536,980,180 (an Exif IFD follows the pixels) |

Pixel `(col,row)` is the `u16` at `43686 + 2·(row·16385 + col)`.

**Height:** `y = raw × 9800 / 65535`. Evidence: `9800.0f` occurs once in
`kenshi_x64.exe`; on high terrain (raw > 20000, n = 59 seeds) median `y/raw`
= 0.149527 vs 9800/65535 = 0.149538. Over all 4,585 seeds the RMSE is 142 units
(seeds sit on navmesh, not exactly on the heightfield). Offset: none found.

### `gui/gfx/GUI_Map.dds`: in-game world map

8192² DXT1, with no border or legend. It covers the **full world square at 36 units/px**, in the same orientation as `fullmap.tif` (no flip or crop):
`x = -147456 + 36·col`, `z = -147456 + 36·row` (row 0 = top). **North is image-up, i.e. north = −z and east = +x** (from a map mod's compass).

Evidence (client session, 2026-10-30):
- The land mask was compared with `fullmap.tif` at 1024² by normalized cross-correlation. Identity scores 0.859; the best other orientation scores 0.570.
- After refinement the score is 0.947, at scale 1.00, with the offset within ~1 px at 1024² (~36 units).
- An overlay was checked visually.

The map art's coastline matches `fullmap` height > ~500 (not > 0). The sea is a flat (109,123,120).

The mod "Nice Map" (Workshop 1613829988) ships a drop-in replacement with the identical mapping.

### `navtiles/seeds.def`

55,020 bytes = 4,585 × `f32 x, y, z`, no header. Ground points on the navmesh;
used above as height ground truth. The `tileX.Y.hkt` files (3,995, X and Y in
0..63, Havok packfiles) are ignored by design.

## Blend / biome stack

| File | Format | Content |
|---|---|---|
| `blendmap.png` | 1024² RGBA | exactly 5 values = slot index: (255,0,0,0)→0, (0,255,0,0)→1, (0,0,255,0)→2, (0,0,0,255)→3, (0,0,0,0)→4 |
| `blendinfo.dat` | `KBI1` | 32×32 cells, each a 5-entry palette of 24-bit RGB keys + payload |
| `biomemap.png` | 1024² RGBA | 57 RGB biome colours |
| `overlaymaps/biomemap.png` | 1024² RGBA | 56-colour variant; differs on ~8.5 % of pixels; which one the game uses is unknown |
| `overlaymaps/ambientmap.png` | 1024² RGBA | 14 colours; meaning unknown |

**Identity (1,048,576 / 1,048,576 pixels):**

```
cell = (px // 32) + 32 * (py // 32)          # one cell = 32 px = 9216 units = 2×2 zones
biomemap[px,py].rgb == blendinfo.palette[cell][ slot(blendmap[px,py]) ]
```

### `blendinfo.dat` (1,364,352 bytes, fully consumed)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | magic `KBI1` |
| 4 | 4+4 | i32 width = 32, i32 height = 32 |
| 12 | 20,480 | 1024 × 5 × u32 palette key (RGB24, 0 = unused) |
| 20,492 | 4 | i32 payload size = 2048 |
| 20,496 | … | 1024 payloads, variable size |

Payload, by its first byte `c`:

| `c & 0xE0` | Size | Meaning |
|---|---|---|
| `0x20` | 1 | uniform cell, `c = 0x20 \| (1 << slot)`; 368 cells; blendmap is that single slot over the whole cell (368/368) |
| other | 2048 | 656 cells; every byte satisfies `b = popcount(b & 0x1F) << 5 \| (b & 0x1F)` (5-bit layer mask + its popcount) |

Uniform markers: 0x21 ×133, 0x22 ×83, 0x24 ×33, 0x28 ×117, 0x30 ×2.

**Layout of the 2048-byte payload:** a quadtree of slot masks; see `docs/reference/TERRAIN_SPLAT.md` §3. Earlier note: In 95 % of byte pairs both
bytes are equal; simple 32×32×2 and 64×32 layouts do not match the blendmap
slots (≈51 % agreement).

### Biome colour → BIOMES record (solved 2026-10-30)

**`biomemap.png` pixel RGB packed as `0xRRGGBB` == `BIOMES.index` (int field).** Checked on the merged stack base → Newwworld → Dialogue → rebirth:
- 57/57 colours match, covering 100 % of pixels;
- `overlaymaps/biomemap.png` also matches, 56/56;
- BGR order matches only 10.
The BIOMES record supplies the terrain texture layers (base/slope/cliff/grass/dirt/road), the `foliage` list (FOLIAGE_LAYER), `sun brightness`, `ambient light`, water and ground colour. Which of the two biomemaps the game reads is still unknown.

## `features.dat` — `MF01` (143,194 bytes, fully consumed)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | magic `MF01` |
| 4 | 4 | i32 zone count = 4096 (64×64) |
| 8 | 4096 × 8 | per zone: i32 absolute offset, i32 size (0/0 = empty) |
| 32,776 | … | blobs, contiguous in zone order to EOF |

Zone index = `zx + 64·zy`. Blob = `i32 count` + `count` × record:

| Field | Type |
|---|---|
| name | u8 len + bytes; a record stringID, e.g. `5677-Newwworld.mod` |
| position | 3×f32, world x y z |
| scale | 3×f32, usually (1,1,1); can be non-uniform or negative |
| rotation | 4×f32 unit quaternion; component order assumed w,x,y,z as in zone instances |

587 non-empty zones, 1,821 features, 224 distinct names; 1,816 lie inside their
zone (5 outliers, presumably multi-zone objects).

## `fogfeatures.dat` — `FF02` (4,501 bytes, fully consumed)

`FF02`, i32 count (28), then per record: u8 len + name, u8 type, 4×f32 RGBA,
f32 radius, then type-specific floats:

| Type | Count | Extra |
|---|---|---|
| 2 | 1 | 4 × f32 (vec3 + f32?) |
| 3 | 2 | 8 × f32 |
| 4 | 25 | 1 + 7×4 f32 (plausibly 7 planes: normal + distance) |

Meaning of the extra floats is unknown.

## `areasmap.tga`

256² uncompressed 24-bpp TGA (type 2), descriptor 0x00 = bottom-left origin,
4×4 px per zone. 71 colours.

**Pixel colour (TGA stores B,G,R; pack as `0xRRGGBB`) == `BIOME_GROUP.index`.** 71/71 colours match, covering 65,536/65,536 pixels (2026-10-30). BIOME_GROUP is the gameplay region: `music`, `ambience`, weather strength multipliers, nests, `homeless spawn amount`, acidic ground/water, `farm water usage`, and (per GAMEDATA_SCHEMA) seasons, arrival dialog and resources.

## Other files (format known, meaning unknown)

| File | Format |
|---|---|
| `overlaymaps/flowmap.png`, `flowmap_OrigFastWATER.png` | 2048² RGB |
| `overlaymaps/distant.png`, `watercolourmap.png` | 128² |
| `overlaymaps/debug.png` | 1024² RGBA |
| `overlaymaps/prosp.tga` | 22² RGB |
| `textures/` | 368 files: 363 DDS (`<Material>_DIF.dds`, `_NML.dds`), 5 water/crack PNGs, some under `Testers/` |
| `overlaymaps/DevIL.dll`, `joiner.exe` | tools, not data |

`data/land/` (not `newland`) is an older map: its `fullmap.dds` is a colour map,
not a heightmap. Ignore it.
