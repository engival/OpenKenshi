# Building collision and render geometry

How to get world-space obstacle geometry for every placed building, for our Recast navmesh.
Tools: `tools/collision.py` (PhysX files), `tools/ogre_mesh.py` (render meshes),
`tools/building_geometry.py` (zone → records → files → world triangles; coverage and height checks).
Scratch verification scripts: `~ds/work/review/collision/` (`contain.py`, `place.py`, `binxml2.py`, `caps.py`, `one.py`).

Status tags: **V** = verified against game files (counts are from full sweeps), **D** = read from the
decompile, **H** = hypothesis.

## TL;DR

- Collision is PhysX 2.8 NxuStream. The **XML (`*.xml`, "NXUSTREAM2") is the source**; `*.bin` is a binary
  cache the game makes from it. Read the XML: every shipped `.bin` that has an XML twin holds the same
  geometry (V), and every collision path a record names is an `.xml`.
- Geometry lives in hex `<cookedData>` blobs (cooked convex hulls and triangle meshes, decoded by
  `collision.py`) plus analytic boxes, capsules, spheres and planes.
- Collision files are **z-up**; the game folds `(x, y, z) → (x, z, −y)` into the instance matrix (D, V).
  Units are the render mesh's units (V).
- Chain: zone instance → `BUILDING` (instance target) → refs `parts` → `BUILDING_PART` file fields
  `xml collision` (collision) and `phs or mesh` (render `.mesh`).
- World vertex = `(x, terrain(x,z) + y, z) + R(qw,qx,qy,qz) · (scale · physx_to_ogre(Actor · Shape · v))`.
  **Instance `y` is relative to the terrain** (V, new finding); `scale` = BUILDING float `scale` (D).
- Coverage of the 11,714 placements: 64% all parts with collision, 22% mixed (some parts are render-only),
  3% render mesh only, 11% no geometry at all (invisible ore-resource markers).

## 1. Collision files

### Census (V)

| Kind | Count | Notes |
|---|---:|---|
| NXUSTREAM2 XML | 1,136 | sdkVersion 283 / 284 / 285 (59 / 84 / 993), nxuVersion 103. All parse with `collision.py` |
| NxuStream binary `.bin` | 892 | magic `NXUSTREAM\0`, header sdk 284, nxu 103 in all |
| ... with same-stem `.xml` | 702 | 700 hold identical geometry and poses; 2 differ (`furniture/chest1`, `Things/Street_Lamp01_COL`) |
| ... named `<stem>_<scale>.bin` | 189 | scaled caches (see §1.4); 57/58 with meshes = XML vertices × scale |
| ... no XML at all | 1 | |
| `.phs` (old "Scythe" physics) | 86 | 17 BUILDING_PART `phs or mesh` fields point at one; not parsed |

Ignore the file name: both `foo.xml` and `foo_COL.xml` occur. Records name the file explicitly.

### 1.1 XML layout (V)

`<NXUSTREAM2><NxuPhysicsCollection sdkVersion=… nxuVersion=103>`, which holds:

- `NxConvexMeshDesc id=…` / `NxTriangleMeshDesc id=…`, each with a `<cookedData>` hex blob. Total blobs:
  1,335 convex and 376 triangle meshes.
- `NxSceneDesc`, which holds `NxActorDesc name=…` with `<globalPose>` and one or more shape descs. Each
  shape desc has a child `NxShapeDesc` with `<localPose>`, flags, group and material.

| Shape element | Count | Geometry |
|---|---:|---|
| `NxBoxShapeDesc dimensions="hx hy hz"` | 2,066 | half extents, centred on the shape pose |
| `NxConvexShapeDesc meshData=<id>` | 1,335 | cooked CVXM blob |
| `NxTriangleMeshShapeDesc meshData=<id>` | 376 | cooked MESH blob |
| `NxCapsuleShapeDesc radius height` | 245 | **H**: PhysX convention, along local Y, `height` = distance between the cap centres. Only 50/105 capsules fit inside the paired render AABB (local X: 42, Z: 44), so placement is unresolved |
| `NxPlaneShapeDesc normal="0 0 1" d="0"` | 91 | infinite ground planes in furniture files; skip for navmesh |
| `NxSphereShapeDesc radius` | 3 | |

Actors per file: 1 in 759 files, up to 444; 5 files have none. Static actors (`hasBody="false"`): 4,049;
dynamic: 54. Building collision is static; which records the dynamic actors belong to was not checked. Trigger flags are all false in the files; the game decides what is a trigger from
record fields (§3.3).

Pose text = 12 floats: rotation **row-major** (`m00 m01 m02 m10 …`), then translation; `p' = M·p + t`.
Evidence: the `.bin` stores the same pose as a 4×4 row-major `[R | t; 0 0 0 1]` with `t` in the last
column (V, 2,330/2,331 actor poses found byte-exact). The containment test (§4) agrees: 144 row vs 106
column.

11 XML files contain a bare `&` in the collection id and aren't well-formed; `collision.py` escapes it.
The `<gravity>` values (`0 0 -981`, `-9.81`, `-0.981`, `-98.1`) show the z-up authoring, but they don't
tell you the units. Geometry always matches the render mesh units.

### 1.2 Cooked streams (V: all 1,711 blobs decode, indices in range)

Triangle mesh, `NXS\x01MESH`:

| Offset | Type | Meaning |
|---:|---|---|
| 0 | 8 bytes | `NXS\x01` `MESH` |
| 8 | u32 | version (1) |
| 12 | u32 | flags: `0x0A` (302) = u8 indices, `0x12` (74) = u16 indices (bit 0x10) |
| 16 | f32, u32, u32 | 0.001, 255, 0 in all files (meaning unknown) |
| 28 | u32 | vertex count Nv |
| 32 | u32 | triangle count Nt |
| 36 | Nv × 3 f32 | vertices |
| … | Nt × 3 u8/u16 | triangle indices |
| … | | u32 Nt−1, remap table, OPCODE tree, adjacency (not needed) |

Convex hull, `NXS\x01CVXM`: find the chunk `ICE\x01CVHL`. Then: 7 × u32 header (`[0]=5, [1]=Nv, [2]=Nt`),
Nv × 3 f32 vertices, u32 (Nv−1), Nt × 3 **u8** indices (every game hull has ≤ 256 vertices).
The hull triangles are closed.

### 1.3 Binary `.bin` (V, partial)

Header: `NXUSTREAM\0`, u8, u32 sdk (284) at 0x0b, u32 nxu (103) at 0x0f. Records are tagged
`EF CD AB 00` (0xABCDEF), u32 type, u32 size, magic `67 45 23 01`. They hold the same cooked blobs
byte-identical (1,394 blobs), or, in 13 files, the mesh **uncooked** (raw f32 points + u32 triangles,
same vertices). Box/capsule/sphere/plane parameters and 4×4 poses are present as packed f32.
`collision.scan_bin()` extracts cooked blobs only. There is no full `.bin` parser, and we don't need one.

### 1.4 What the game loads (D)

`FUN_14055f6e0` (building part setup) reads `xml collision` into a load request together with the
building node's position and orientation, scale = record float `scale` (`FUN_1400dc3f0`), and the flags
`no collision` and `is unwalkable roof` (which selects a different collision group). `FUN_1404cd4f0`
then calls:

1. `FUN_1407e5ba0` = `[PhysicsActual::convertXMLToBin]`. If the extension is `bin` and the file exists,
   it is used. Otherwise the name becomes `stem` + (`_` + `toString(scale)` if scale ≠ 1) + `.bin`.
   If that exists, it is used. Otherwise `stem.xml` is loaded (NXU type 1 = XML), rescaled
   (`FUN_1407e4410`: vertices, pose translations, box/capsule/sphere sizes × scale), and **saved as that
   `.bin`**. The shipped `_0.85.bin` / `_1.1.bin` files are those caches (V).
2. `FUN_1407e5580` loads the `.bin` (NXU type 0 = binary). It builds the root matrix from the quaternion
   with columns reordered and negated, `M = R(q) · P` with `P(x,y,z) = (x, z, −y)`, translation = node
   position. It then instantiates the collection with that root (`FUN_1401d9900`; that this is
   `NXU::instantiateCollection` is **H**).

The `BUILDING` field `destroyed boundary` (20 records) goes through the same `.bin`-first logic
(`FUN_1403c79b0`).

**Consequence:** where a `.bin` and its `.xml` disagree, the game uses the `.bin`. That happens for 2 of
702 twins (`chest1`, `Street_Lamp01_COL`); the XML is good enough for us.

## 2. Ogre render meshes (V)

| Serializer | Files |
|---|---:|
| `[MeshSerializer_v1.100]` | 1,859 |
| `[MeshSerializer_v1.8]` | 1,460 |
| `[MeshSerializer_v1.41]` | 77 |

`ogre_mesh.py` parses all 3,396 `.mesh` files. In each one, the position count equals the vertex count
and all indices are in range. The vertex AABB equals the `M_MESH_BOUNDS` chunk within 0.1% in 3,085 files
and within 5% in 210. The other 41 are morphing hair/beards and one ramp, where the stored bounds are
padded. Use `vertex_bounds()` and don't trust the chunk. Chunk lengths of the outer `M_MESH`/`M_SUBMESH`
in v1.8/v1.41 don't cover their children, so the parser walks children by chunk id. Meshes are y-up,
in the same units as the world.

## 3. Linkage

### 3.1 Chain (V)

| Step | Record / field |
|---|---|
| Placement | zone `INSTANCE_COLLECTION 0-buildinglist`, instance `{id, target, x y z, qw qx qy qz}` (`WORLD_LAYOUT.md`). Overlay layers base → Newwworld → Dialogue → rebirth per cell by instance id; an empty target deletes |
| Building | target = `BUILDING` sid; float `scale` (present on all 622 BUILDING records; 1.0 / 1.1 / 0.85 cover 91% of placements) |
| Parts | `BUILDING` refs category `parts` → `BUILDING_PART` (ref values `(0|1, 100, 0)` mostly; meaning **H**) |
| Collision | `BUILDING_PART` file `xml collision` (639 parts set; 629 exist) |
| Render | `BUILDING_PART` file `phs or mesh` (1,175 `.mesh`, 17 `.phs`) |

Mod records overlay `gamedata.base` field by field. A part's mesh and its collision can come from
different files, so merge before resolving (`building_geometry.load_records`). Layer precedence is
assumed, as in `WORLD_LAYOUT.md`.

### 3.2 Coverage (V, merged records, `building_geometry.py --coverage`)

| Class | BUILDING records | Placements | Placed types |
|---|---:|---:|---:|
| all parts have collision | 403 | 7,498 | 154 |
| mixed (some parts render-only) | 139 | 2,537 | 76 |
| render mesh only | 60 | 331 | 16 |
| no parts | 20 | 1,336 | 3 (Iron Resource 1,241, Copper Resource 94, Ramp 1) |
| target missing | | 12 | 3 |
| **total** | 622 | 11,714 | 252 |

The resource nodes have no geometry at all (only a `functionality` ref), so they don't need navmesh input.

### 3.3 Parts the coverage table ignores, and flags that matter for the navmesh

| Item | Count | Effect |
|---|---:|---|
| BUILDING refs `interior` → BUILDING_PART | 128 refs, 44 with collision | loaded only when the player is inside (fcs.def). **Not included** by `building_geometry.py` |
| refs `interior mask` → BUILDING_PART | 59, all with collision | the XML is the "am I indoors" **trigger** hull (fcs.def): not an obstacle |
| refs `doors` → BUILDING | 47 | doors are child buildings with an `is door` part. Not included; how they are placed is unknown |
| refs `wall subsections` → BUILDING | 25 | not included |
| BUILDING_PART bool `passable` | 13 (9 with collision) | "collision only used for clicking": exclude |
| `is unwalkable roof` | 35 | collides; different PhysX group (D) |
| `offset X/Y/Z` | 47 parts | on collision parts, only position markers (no offset applied) plus `Streetlamp[Quad]01`; whether the offset moves collision is **H** |
| `destroyed collision` / `destroyed mesh` | 7 / 42 | destroyed state |
| `rotation function` | 43 parts | turret heads, windmills (moving parts) |

Rocks are not buildings. `MAP_FEATURES` (`mesh`, `collision`, 205 with collision) and `FOLIAGE_MESH`
(`collision`, 354) use the same XML format. Their placement is **not covered here**.

## 4. Transform to world

```
v_shape   cooked vertex / tessellated primitive         (PhysX, z-up)
v_part    = Actor.globalPose · Shape.localPose · v_shape
v_ogre    = (v_part.x, v_part.z, -v_part.y)               (P)
v_world   = (x, terrain(x, z) + y, z) + R(qw, qx, qy, qz) · (scale · v_ogre)
```

| Claim | Status | Evidence |
|---|---|---|
| `P = (x, z, −y)` | D + V | matrix built in `FUN_1407e5580`. Collision AABB inside the render-mesh AABB (5% tolerance), unposed files: 66/80 with P, 10 with `(x, −z, y)`, 6 identity |
| Actor pose applied, then shape pose | V | posed files: 144/276 with actor·shape, 66 shape only, 31 neither. Remaining misses are mostly parts whose collision deliberately covers a neighbouring piece (wall hull on a roof part, generic `LandscapeHeightBOX`) |
| Quaternion `(w, x, y, z)`, Ogre rotation, not conjugated | V | 7,028 wall-piece endpoints. Distance to the nearest other wall endpoint / piece length: median 0.27 with `q` vs 0.58 with the conjugate; 868 vs 94 endpoints within 0.05 |
| Uniform `scale` from the BUILDING record, on render and collision | D | `Node::setScale(scale)` in `FUN_14055e150`/`FUN_140561d10`; collision rescale in `FUN_1407e4410`; scaled caches = XML × scale (V). Not confirmed in world space: the wall test matches slightly more endpoints *without* scale (1,057 vs 868), but walls may be built to overlap |
| **Instance `y` is relative to the ground, floored at the sea plane (k16.7.10, 2026-11-09; was: terrain-relative, world y = terrain + y)**: world y = y + max(100, ground(x, z)), ground = the zone cell's water-body field (129 x 129, 36 u, bilinear) else the terrain; ours has only the terrain (`kworld::zoneBuildingY`, one rule for every placement; a door building keeps terrain + y). The original: `Building::createPhysical` (Steam 1405609e0), kinspect `findings/bridge_placement.md`: Greenlands bridge spans live 112.7395 (ours 112.74) and 115.7238 (ours 115.67: the field reads 107.54 there, our terrain 107.49); every building on ground under 100 (shores, river beds) was 100 - ground too low. Stale-tile counts did not move (knav_navhash --stale, all 3,995 tiles: zones 2,959 shipped / 1,036 stale, interiors 103 / 111 stale / 183 unverifiable, both before and after; ctest `kworld_zone_floor`). Old line: | V | vs GAMESTATE_BUILDING `world Y pos` (written from the node's world position): n = 14,637, p25/p50/p75 = −0.2 / 0.0 / +0.6, 89.5% within 20 units. The tail (p5 −6.7, p95 +30.6) is unexplained (**H**: terrain flattening under buildings, or edited saves) |

**One building, end to end (V):** `2-rebirth-INGAME` in cell (5, 44), *Swamp Lookout Tower*
(`50625-Newwworld.mod`, scale 0.78, quaternion tilted ≈ 48° yaw plus a slight lean), single part
`Rebel_Look-out_Tower_COL.xml`. Terrain at the origin is 1,948.9 and y = 12.7.

| | x | y | z |
|---|---|---|---|
| World collision AABB min | −122,661.5 | 1,938.3 | 55,558.4 |
| World render AABB min | −122,662.7 | 1,925.3 | 55,558.7 |
| World collision AABB max | −122,514.5 | 2,082.5 | 55,768.1 |
| World render AABB max | −122,514.3 | 2,088.3 | 55,768.7 |

The terrain under the footprint corners is 1,955–1,978: the tower's legs sit in the ground and it stands
about 130 units above it. Reproduce with `building_geometry.py DATA --zone 5 44 --obj out.obj`.

### 3.4 Storm house floors (V, 2026-11-01; kinspect item 23)

`Storm_House_01_Base_COL.xml` (base part 3395 of `3384-TwoStorey.mod`, `data/buildings/StormBuildings/`) decodes
fully: 10 actors, all one convex shape each: seven `Exterior_COL` foundation hulls (y -64..30 after the z-up
fold) and `Storm_House_01FloorCOL01/02/03` (x -89..94, y -59.4..4.48, z -93..94 in Ogre axes: 60-unit thick
slabs with the top at +4.48). Nothing is dropped or misparsed: CVHL hulls, the non-foundation actors and the
transform are right. The floors went missing in the **bake**, not in the loader (NAVMESH_CACHE.md, "Tilted
walkable hulls"). Hub placements are terrain-aligned: 88 of 130 Hub buildings carry an x/z tilt in the quaternion
(`qx`, `qz` up to 0.05, i.e. up to about 6 degrees), so a 180-unit slab spans up to 15 units of height.

## 5. Unknown / not done

- Capsule axis and origin convention (§1.1); only 245 shapes, mostly masks and small props.
- Placement of door, interior and wall-subsection child buildings; whether `offset X/Y/Z` moves collision.
- The 10% height-residual tail; the meaning of the `parts` ref values (`0|1, 100, 0`).
- The MESH header constants (0.001, 255); full `.bin` record schema (not needed).
- `MAP_FEATURES` placement transforms. `FOLIAGE_MESH` collision is placed like a building's (instance position, w-first quaternion, uniform
  scale: `CollisionLibrary::addFoliage`, line of sight uses it, LINE_OF_SIGHT.md section 7); **UNSURE** that the exe rotates and scales it
  as the render node (no render-AABB check was made for foliage).
- `.phs` files (17 parts) are unparsed; those parts count as "no geometry" today.
