# Ogre .skeleton (animation clips)

Reader: `cpp/include/kworld/ogreskeleton.hpp` (shared by sim and client, stdlib only).
Dump: `kskeleton_dump FILE [--motion BONE]`. Test: ctest `kworld_skeleton` (every file in the
install; clip census vs `docs/reference/combat/data/anim_clips.tsv`; sampling identities).

## Files (install census, 2026-10-31, VERIFIED)

365 `.skeleton` files, one empty (rejected). `Serializer_v1.80` 264, `v1.10` 100.
- `character/meshes/{male,female}_skeleton/*.skeleton`: the human clip libraries, 30 bones,
  174 male / 166 female clips. Named by GAMEDATA `male/female skeleton file` and ANIMATION_FILE.
- `items/armour/**` (221), `character/hair/meshes/**` (104): per-mesh copies of a biped
  skeleton (mostly 30 bones), the armour/hair mesh's skeleton link. How the game drives them from the
  character (shared skeleton instance by bone name?) is not traced.
- `animal/meshes/**` (33): one skeleton per animal, its own clips.

## Layout

Little-endian. u16 `0x1000` then the header string `[Serializer_v1.80]` / `[Serializer_v1.10]`;
strings end with `\n`. Then chunks: u16 id, u32 length (length includes the 6-byte header).

| id | content |
|---|---|
| 0x1010 | v1.80 only: u16 blend mode (0 average; all files 0) |
| 0x2000 | bone: name, u16 handle, pos f32x3, rot f32x4 (x, y, z, w), optional scale f32x3 |
| 0x3000 | parent: u16 child handle, u16 parent handle |
| 0x4000 | animation: name, f32 length (s); children 0x4010 (base name, f32 base time; unused here) and 0x4100 tracks |
| 0x4100 | track: u16 bone handle; children 0x4110 keyframes |
| 0x4110 | keyframe: f32 time, rot f32x4 (x, y, z, w), translate f32x3, optional scale f32x3 (length 38 / 50) |
| 0x5000 | animation link: file name, f32 scale (none in the install) |

**Quirk (VERIFIED):** the bone chunk length leaves out the name: 36, or 48 with a scale
(`animal/meshes/polySurface1.skeleton`, scale 2.54). Ogre's serializer includes the name; the
reader accepts both and sizes the chunk from its content. Animation and track lengths do cover
their children (checked on every file). Keyframe spacing is not uniform; keys are sorted on load
as Ogre inserts them.

## Semantics (as Ogre 1.x applies them)

- A keyframe is an offset from the bone's bind pose: `pos = bind.pos + key.pos`,
  `rot = bind.rot * key.rot` (normalised), `scale = bind.scale * key.scale`.
- Interpolation (Ogre default, linear): position/scale lerp, rotation nlerp with the shortest
  path. Before the first key: the first key. Past the last key: towards the first key at
  `length + its time` (the loop wrap). Callers wrap or clamp t themselves.
- Model space: `rot = parent.rot * rot`, `scale = parent.scale * scale`,
  `pos = parent.rot * (parent.scale * pos) + parent.pos` (Ogre Node, inherit all). Skinning matrix
  = model(t) x inverse(model at bind).
- Model space is Y up; human root `Bip01` sits at y ~ 10 (mesh height 19.46, 10 units/m).

## Clips (human, from the dump)

- Locomotion is split into body halves: `walk lower` / `walk upper` / `walk upper sword`,
  `run lower` / `run upper` / `run upper sword`, `strafe lower` / `strafe upper`. Loops are in
  place (Bip01 motion over a full loop ~0). The game layers halves by bone mask; which bones belong
  to which half is in the ANIMATION records or the exe (not traced).
- Idle stances `idle_stand_*`, `idle_weapon_*` are one-frame poses (0.0333 s, 2 keys per track).
- Combat clips carry root motion (e.g. `aimcrossH`); `rootMotion(bone, t0, t1)` gives model-space
  delta and yaw (twist about +Y).

OPEN (sim side): whether COMBAT_TECHNIQUE `anim blocked frame` / `anim stop frame` /
`anim hesitate point` are 30 fps frames or 0..1 fractions of the length. The reader exposes
length and time only.
