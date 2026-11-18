# SIMLINK: the live sim <-> client protocol

Status: **protocol version 5 (2026-11-01)**; version 5 = CHAR_APPEARANCE gains a trailing `portraitSerial u32`; version 4 = CHAR_APPEARANCE gains a trailing `name strid`, CHAR_DYNAMIC `flags` bit 12
(run), the INVENTORY record (type 25), order 61 `drop`, toggle `which` 2 (run), event 6 sent (all in section 6, 7, 13); version 3 = the `move_to` body of section 7 (click height `y` and a `flags` byte
replacing `queue`); version 2 = CHAR_DYNAMIC `driveSeq`, `y` with `yValid`, CHAR_ROUTE
(type 23), event 12, the order 20 body. The client wishlist is kenshi-0f's. The sim
side is kenshi-b0's. Server = ksim (later the region-threaded server), client = kclient
(`client/src/simlink.*`, owned by the client session).

**First playable cut:** hello, string table, character appearance (race, sex, head, equipment),
character dynamic (pos, yaw, speeds, flags, action), clock, and the orders `move_to`, `attack`
and `stop`. Every other record exists in this spec but may arrive empty or not at all.

## 1. Transport and framing

- TCP. Loopback by default: `ksim_server --listen 127.0.0.1:PORT`. The client either spawns the
  server as a child or attaches to one that is already running. The same code will work against
  a remote server later.
- Every frame: `u32 len` (bytes after this field), `u16 type`, then the payload. Little-endian,
  no padding, floats are IEEE f32 unless a field says f64. A frame is at most 16 MiB (server to
  client) or 64 KiB (client to server, section 11); a bigger one is a protocol error.
- One connection carries both directions, ordered and reliable (TCP). The snapshot stream and
  the event stream are separate frame types on the same socket. Each is in tick order.
- Strings on the wire are `u16 len` + UTF-8 bytes, and only appear in the string table, hello,
  and error text, and in the client's order bodies (save names, an item record) and NEW_GAME (name).
- Any protocol error: the side that sees it sends `ERROR` (type 3: `str text`) if it can, and closes. No resync.
- **Future transports (PARKED, user 2026-11-07: "make the game WORKING first").** The sockets are confined to
  `simlink_server.cpp`; `Session` reads and writes framed bytes only.
  - Pipe / AF_UNIX mode is small. Any byte stream carries the protocol unchanged (`--stdio` for a spawned child, or a
    socket path). Admin = local, as for 127/8 today.
  - UDP is a contained rework, not a free one. The protocol leans on TCP in three places: send-on-change (a record is
    resent only when its version moves, and the server assumes the last send arrived), events / orders / saves (need
    order and delivery), and the load write order (above).
  - The plan for UDP: two lanes. A reliable ordered lane (ack + resend) carries events, orders, the string table and
    saves. A latest-wins lane carries the versioned state records (CHAR_DYNAMIC, appearance, INVENTORY, ...). The
    server tracks each client's last ACKED version per record and resends until a version >= it is acked, so a stale
    packet is simply superseded.
  - The framing, versions and ids already fit that plan.
  - User proposal 2026-11-07, refined:
    - Keep: a packet sequence number, a keepalive, a ~5 s history, and a full resync past it.
    - Change: no catch-up request and no new-start flag. Every packet carries the newest received sequence plus a
      32-bit bitfield of the ones before it (ack + mask, piggybacked both ways), so losses show without extra messages.
    - Change: the history keeps, per sequence, WHICH records (id, version) and reliable messages were in it, not the
      bytes. On a loss, reliable messages are resent. A state record is resent at its CURRENT version, and only if
      nothing newer was acked since, so stale data is never retransmitted.
    - A gap older than the history triggers a full resync (today's connect snapshot).
- **Trailing fields (rule, 2026-11-02, agreed with the client coordinator):** a server -> client record only ever grows by appending fields at its end. A client reads a tail only when its bytes are present, and **ignores unknown trailing bytes** (logged once per frame type, not an error), so an older client works with a newer server and the reverse. Every new tail is still proposed with its byte layout and approved before code (ROLES.md). Client -> server orders stay strict: an order with bytes the server does not know is malformed (kick), so a new optional order field is accepted only once the server knows it.

## 2. Handshake

The first frame each way is `HELLO` (type 1):

| Field | Type | Notes |
|---|---|---|
| magic | u32 | `'KSIM'` = 0x4D49534B |
| protocol | u16 | 5 (4 had no CHAR_APPEARANCE `portraitSerial`; 3 had no CHAR_APPEARANCE `name`, no flag bit 12, no type 25; 2 had the old move_to body `x, z, queue u8` (no click height); 1 was the first cut: no `driveSeq` in CHAR_DYNAMIC, no CHAR_ROUTE, no event 12, y always 0, order 20 with a `tick` field) |
| role | u8 | 0 client, 1 server |
| dataHash | u64 | hash over the merged load order, defined exactly: FNV-1a 64 (offset 14695981039346656037, prime 1099511628211) over, for each file in load order, its file name bytes, one 0 byte, the file size as u64 LE, and the FNV-1a 64 of the file content as u64 LE (`simlink::dataHash`, ksim) |
| name | str | build / version text, for logs |

A protocol or dataHash mismatch: send `ERROR` and close. After both hellos, the server sends the
string table so far, then a full snapshot.

## 3. Ids and the string table

- **Entity ids** are the sim's `state::Id`: u64, unique per world, never reused. They are
  stable across save and load, so the client may key caches on them.
- **String ids:** u32, dense from 1. 0 means none. `STRTAB_APPEND` (type 2):
  `u32 firstId, u32 count`, then per entry `u8 kind, str text`. Kinds: 1 record stringID (sid),
  2 display name, 3 ANIMATION name, 4 other. The server only appends; ids are never reused in a
  connection. It sends an append before the first frame that uses a new id.
- Records refer to game records by the string id of their sid (`"458-gamedata.base"`), never by
  per-tick strings.

## 4. Clock and rates

- `tick` is the sim's absolute tick (u64, 0.05 s of sim time, since game day 0 00:00).
- Server -> client rates (wall clock at game speed 1):
  - 20 Hz (every tick): dynamic records for full-detail characters in the interest area.
  - 1-2 Hz: squad positions world-wide.
  - 1 Hz and on change: clock, weather.
  - appearance: on first sight and whenever its version changes.
- The client renders about 100 ms behind and interpolates between stamped snapshots, so stamps
  matter more than an exact rate.
- **Game speed and pause change only how fast ticks run in wall time.** They never change what
  a tick computes, so the sim stays deterministic.

## 5. Interest vs simulation detail (rule)

Revised by the user (2026-10-31): **whatever the client can view must be at full detail**, so
the camera does drive simulation detail, as in the original.

- **The original:** zone streaming keeps the camera's zone and its 8 neighbours loaded
  (re-centred after a 4,147-unit drift), plus a ±340-unit box around each requesting character
  (ORIGINAL_AI.md "Zone streaming"). The world pauses until the zones around the camera are
  loaded. **There is no camera roam leash** (measured, kinspect 567b510; SQUAD_FOLLOW.md 9): the
  free camera was parked 300-16000 u from the player and stayed put; streaming followed it. Only
  clamps: ground height, zoom 10..2000, and a +-147456 world box checked on save load. Per-frame
  character updates: player faction always, others within camera distance 3250 plus the frustum.
- **Ours:** full detail = the union of the view area and the bubbles around the player's
  characters. The view area is the camera's zone plus its 8 neighbours, with the same re-centre
  hysteresis. Like the original, the camera is not leashed by default. A leash is a possible
  server option for multiplayer (DECISION PENDING, the user's call). The server sends the current full-detail
  bounds, and the client never draws a character outside them as full detail.
- **Determinism:** the view focus is an input, so it is a sim command (order kind 51
  `set_view`). Because it is quantized to zones with hysteresis, it changes rarely. It is logged
  with its tick like any order and saved with the game, so replay and save/continue still
  match. Small camera moves inside the focus zone send nothing.
  **Implemented 2026-11-04** (`Simulation::stepPlayerAreas`): the focus is one more player area (a region centre), so a camera alone builds a town's residents; before, only the player characters did (the client saw no one in Heng).
- `SET_INTEREST {x, z, radius}` stays a pure send filter (what to transmit; default 1 km) and is
  not a sim command. Characters inside it that are not at full detail are sent at the squad
  rate, positions only, flag `lowDetail` (drawn as distant figures or squad markers, never with
  full-detail actions).
  **Player-controlled characters are always sent** (the original's "player faction always"; fixed 2026-11-05, a client
  loop blocker): interest and the detail bounds filter only the others, so panning the camera away never drops the player's
  own characters (event 11) or empties the selection. `Session::sendTick`, the member loop. Multiplayer: every player
  character today (one player faction); later only the session's own faction.

## 6. Server -> client records

All of them start with `u64 tick`, once: where a record below lists `u64 tick` first (CLOCK, DETAIL_BOUNDS, EVENT), that is the same field, not a second one.

### SNAPSHOT_BEGIN / SNAPSHOT_END (types 10, 11)

The server brackets one tick's records: `u64 tick, u8 full`. `full` = 1 means every full-detail
character in interest is in the set; 0 is reserved for future deltas. Version 1 always sends full
sets. Low-detail characters (squad rate) are not in every set, so absence never means "gone": a
character leaves interest only by event 11 (kenshi-0f, 2026-10-31).

### CHAR_APPEARANCE (type 20)

Sent on first sight and when `appearanceVersion` changes.

| Field | Type |
|---|---|
| id, squadId | u64, u64 |
| faction, race | strid, strid |
| sex | u8 (0 male, 1 female) |
| headIndex | u16 (index into the RACE's `heads male` / `heads female` list, as the client's `head=N`) |
| hair, beard, hairColour | strid x3 (ATTACHMENT, ATTACHMENT, COLOR_DATA sids; 0 = none) |
| skinTone | f32 (0..1, the client's `skin=`) |
| bodyCount | u8, then bodyCount x {strid key, f32 value} (slider values; muscle/strong/skinny stats) |
| itemCount | u8, then itemCount x {strid item, strid material/spec, strid colour, u8 section} |
| crossbowAmmo | u16 |
| hiddenLimbs | u8 bitmask (amputations / LIMB_REPLACEMENT) |
| appearanceVersion | u32 |
| name | strid (protocol 4, last field: the character's display name; the CHARACTER record's name for uniques ("Engival", "Izumi", "Beep"); a random spawn carries its record's name too ("Hungry bandit"): the sim has no random-name generator yet, OPEN; 0 = none) |
| portraitSerial | u32 (**protocol 5, sent since 2026-11-01 (proposed by kenshi-0f)**, last field: the original's per-character portrait serial, rolled once at creation as rand x 4e9 (1406223f0) and saved. The client turns the portrait camera by it: yaw = ((s % 500) / 1000 - 0.25) pi, pitch = ((s % 252) / 1000 - 0.083) pi (CLIENT.md "portraits"). Ours: rolled at spawn (`Simulation::spawnFighter`, one draw of the pure stream `streamSeed(streamSeed(seed, kPortraitStream), Id)`, uniform in [0, 4e9)), saved in `Fighter::portraitSerial` (save v32), part of the appearance, never changes; `portrait_index` (the atlas cell) is client-only and not sent) |

Sections: 0 back, 1 back2, 2 hip, 3 head, 4 shirt, 5 armour, 6 legs, 7 boots, 8 backpack, 9 hand
(the drawn weapon; drawing or sheathing moves the item and bumps `appearanceVersion`).

### INTERACTABLE (type 24), doors, ground items and containers (kenshi-b0, 2026-10-31; containers added 2026-10-31, protocol unchanged)

Sent when an interactable first enters the detail bounds (and interest) and whenever its `stateVersion` changes; nothing
while it is unchanged. Leaving the bounds, or being destroyed (an item picked up), is event 11 with the interactable's id
(the client drops it; it comes back with a new record). Ids share the character id space.

| Field | Type |
|---|---|
| id | u64 |
| kind | u8 (1 door, 2 ground item, 3 container, 4 producer, 5 bed) |
| record | strid (door and container: the BUILDING record; item: the ITEM / WEAPON / ARMOUR record) |
| pos | f32 x3 (a door: the centre of its navmesh polygons) |
| usePoint | f32 x3 (where to stand; a door's equals pos, UNSURE: the original approaches from a side; an item's is the nearest walkable ground) |
| state | u8 (door: 0 open, 1 closed, 2 locked, 3 broken; container: 2 locked, 0 not; item: 0). Broken (a bashed door, hp 0) is open for ever, cannot be closed, its lock is gone |
| health, maxHealth | f32, f32 (door; 0, 0 for an item). every door has 16 / 16 (the hp measured on the Hub bar door; UNSURE for the others, the data has no hp field). Each bash hit changes `health` and bumps stateVersion |
| count | u16 (item stack size; 1 for a door) |
| stateVersion | u32 (1 at spawn, +1 per change) |

A **container** (kind 3) is a storage BUILDING: a placed one with `has inventory` (crates, barrels, shelves, safes), or a storage piece of a placed building's **interior layout** (almost every town chest is layout furniture: a shop counter, a barrel, a weapon cabinet; task AL, 2026-11-06). It has a use point on the navmesh (the nearest walkable point, within 20 units of its place). health, maxHealth and count are 0, 0, 1; its stateVersion changes with its owner or its lock. Its contents are an INVENTORY record under the container's id (below). The owning faction is not on the wire: the client learns that taking is theft from event 14 (the original shows the red loot cursor on foreign storage, CURSORS_LIVE.md). A layout box exists once its zone's navmesh is furnished (as the doors and item nodes do); it is keyed by the building and the layout piece, so no second one is made.
- **Tail (kind 3 only, after the ownership tail; approved by the client lead 2026-11-13, kenshi-k16.7.28; additive, no protocol bump; `u64 building, u16 piece`): which piece this container is, so the client draws no mesh of its own for it** (yaw stays 0: the client draws the piece with the piece's own rotation). A layout piece: `building` = the owning building's doorKey (the value CHAR_DYNAMIC `building` and the kind 5 tail carry), `piece` = the piece's index in that building's interior layout's instance list **exactly as read from the data, every entry counted** (none skipped as unbuildable, hidden or not drawn; the sim's own key may differ, the wire never does). A placed world container (a crate as its own placement): `building` = its placement key (`doorKey(doorId(instance, 0xC0DE))`), `piece` = 0xFFFF. Player-built storage: `building` = the placed BUILDING instance's doorKey, `piece` 0xFFFF (UNSURE: the sim makes no container for a player building yet, so no such record is sent today). A scenario stand-in crate: 0 and 0xFFFF; the only case where the client draws the record's mesh itself (at `pos`, yaw 0). Test `ksim_containers` (data index) and `ksim_interact` (the wire).
- **Lock (Implemented 2026-11-06, client-approved, tail values only, no new field or protocol bump):** a BUILDING's `has lock` (Weapon Cabinet, Safe, Metal Chest; 2,285 layout pieces in the base world) starts the box locked. `state` = 2 then, and the existing `lockLevel` tail carries the lock's level (the door formula, DOORS.md sections 6-7); unlocked = state 0, lockLevel 0. A locked box refuses loot and transfers (`locked`, event 10; event 13 result 1 when its state changes on the way), needs order 60 action 3 (the pick, any hands and skill; the same chance and timer on the lock) and answers with TASK_PROGRESS 39 kind 1 while it is picking; the unlock bumps the container's stateVersion. UNSURE: the level is rolled from the owner faction's `doors lock level` / `lock level random` plus the record's `lock level bonus` (the container's own `containers lock level` is not read by the exe, GATES_CARRY_STATIC.md b); the position of the lock action in the exe's cursor (the client picks it from state 2, as for a door).
- **`itemtype limit` (Implemented 2026-11-06):** 3 = armour only, 2 = weapon only, 4 (or none) anything (Armour Stand, Weapon Display; UNSURE: only these two values are read). A transfer that puts the wrong kind into such a box is refused (`item_type`); taking is not limited.
- The scenarios (duel, squad) place two stand-in crates beside the player (`spawnContainerAt`: key 0, unlocked even when the record has `has lock`; a scenario helper, not a placement).

**Producer (kind 4) - Implemented (MINE_SELL_SLICE M1-M3, save v49).** A placed BUILDING, or a wild foliage deposit (a FOLIAGE_MESH with a `building type`), whose functionality `produces` and `consumes` nothing: Iron / Copper Resource and the small variants, Ore Mine, quarries, wells, drills, all through one path (`cpp/include/ksim/producers.hpp`; no record is named in code). It exists as an entity only while a player character is within 450 units (the kind 4 record then arrives; leaving the bounds is event 11); an emptied one is dropped again and made again with the same Id (Id = 2^62 + the site, so it is stable across saves).
- `record` = the BUILDING; `pos` = the placement; `usePoint` = the navmesh-snapped point within 60 units (kinspect: mines have no use nodes, every operator walks to the origin and counts as arrived within the functionality's `use range`, 100 on a mine; that range is also the reach of orders 63 / 68 on it). `state` = 1 full, 2 working (an operator, or powered), 3 idle. `health, maxHealth` 0, 0. `count` = units in the output. `stateVersion` = the producer's version + its stock version (both only grow): it moves on a state, operator-count or stock change.
- **Tail, kind 4 only, after `stateVersion`:** `strid output` (the produced ITEM), `u16 cap` (`produces` v0), `u8 operators`, `u8 maxOperators` (the BUILDING's `max operators` when >= 0, else the functionality's; Ore Mine / Iron Resource 3, Copper Resource 2 in the merged data), then (**Implemented 2026-11-02, additive, no protocol bump; a decoder reads them only when present**) `f32 progress` (0..1 toward the next output unit, shared by the operators), `f32 rate` (progress per sim-second summed over all current operators; 0 when stalled: full, no operator, no power model for a fully automatic one), `u64 progressTick` (the sim tick `progress` is valid at: the tick the record is written). The client shows `clamp(progress + rate x (now - progressTick) x 0.05, 0, 1)` (now and progressTick in ticks, 20 per sim-second), as TASK_PROGRESS. The record is sent again whenever the rate changes because every such change already bumps `stateVersion`: an operator joins or leaves, the producer fills (state 1) or frees, a unit is made (the stock version; progress wraps to 0). Production is continuous per tick (`progress += sum of the operators' per-tick progress`, one unit at >= 0.999), independent of any clip cycle, so the value is well defined at every tick. A skill level-up changes the rate slightly without a re-send (UNSURE; the next unit re-syncs it).
- **Contents:** INVENTORY + ITEM_QUALITY + ITEM_GRID under the producer's Id, always on while it is in bounds (as containers). ITEM_GRID has one section 0 of exactly the output item's footprint holding one stack up to `cap` (the item's own `stackable` is ignored for it, `state::Inventory::stackLimit`). Orders 63 / 68 with take = 1 accept it as the container (reach: its `use range`); take = 0 puts the produced item back (implemented 2026-11-12, kenshi-k16.19.13) while it fits: another item is `not_allowed`, over the cap is `full` (63) / `no_room` (68); orders 64 / 65 on it answer `not_allowed`. Theft (event 14) only when the producer has an owner faction that is not the taker's (a wild deposit has none).
- **Order 60 action 4 `use` on a kind 4 target** walks to the use point and then keeps working until the producer is full or a replacing order cancels it. Refusals (event 10): `bad_target` (not a producer), `full` (state 1), `no_slot` (`operators` = `maxOperators`; slots are a counter). Rate and skill: `producers.hpp` "THE RATE AND SKILL" (items per game day per operator = `production mult` x `output rate` / max operators x (0.5 + 0.9 L / 100) x richness, L = Labouring through the functionality's `stat used`; the operators share one progress; one unit and progress back to 0 at >= 0.999; richness = the biome's `iron mult` / `copper mult`, floor 0.1; XP every tick the order runs). A fully automatic producer (max operators 0) makes no progress until a power model feeds `producers::powerFraction` (hook).
- **The stop** is event 13 (action 4) per operator: result **7 full** (the output reached `cap`; new value), 5 cancelled, 4 the actor KO'd or dead, 3 the target gone, 2 unreachable, 1 refused (the last slot or the room went while walking).
- **TASK_PROGRESS kind 9 (operate)** exists only when the functionality's `has progress bar when used` is set (kinspect item 59 MQ9: 0 on both mines, so a mine shows none): progress = the producer's progress to the next unit (shared by the operators), rate = progress per sim-second from all operators, value 0.

**Bed (kind 5) - Implemented 2026-11-06 (client-approved; additive, no protocol bump).** A real layout bed of a placed building
(`Simulation::bedViews`; `routines::SiteEntry`, a synthetic yard bed has no record and is not sent). Emitted like kind 4: the
record is filtered by the same detail bounds and interest, and sent on first sight and whenever its `stateVersion` changes.
- `record` = the bed's furniture BUILDING (interned as a record strid like every other; the layout piece's record travels with the
  site, it was dropped on the way into the site index before 2026-11-06); `pos` = the placement's floor point, where the walk ends;
  `use` = the use node itself: the same x, z and the node's own rotated y a few units above the floor (the pose tail's `poseX, poseY,
  poseZ` for a sleeper there; a level bed's node sits on the floor, a tilted piece carries it up or down); `state`, `health`,
  `maxHealth` 0; `count` 1.
- **The set is a function of the data, not of play:** every placed building with an interior layout is indexed (`primeSiteIndex`,
  once per simulation), so a save and a load send the same ids whatever the scorer had indexed before the save. Indexed on demand,
  the rebuilt index kept only what its triggers re-indexed: a probe saved 10 beds in 3 buildings, loaded, and listed none 4000
  ticks later; live, 744 beds became 712. Ruins (`BuildingSite::ruined`, no interior) contribute none.
- **Id:** `2^62 + FNV(the layout site key: building key, slot)`, never a runtime index, so it is the same across a save/load and a
  server restart (test `ksim_player_sleep` pins it).
- **`stateVersion`:** 1 free, 2 occupied. It toggles with the occupant (the client re-reads the record); the wire's `yaw` (the
  ownership tail) is the lie-down facing.
- **Tail, kind 5 only, after the ownership tail:** `u64 building` (the owning building: the same doorKey the CHAR_DYNAMIC
  `building` field carries), `strid furniture` (the bed's BUILDING record, as `record`), `u64 occupant` (the character whose
  routine targets the bed -- asleep in it or walking to it; 0 free; the same rule as the order's `occupied` refusal).
- **Order 60 action 9** names a bed by this id (section 13).
- **Tail, kind 2 severed limb only (item function 0x11), after the ownership tail (client-approved 2026-11-11, kenshi-h2v.13.9; additive,
  no protocol bump, a decoder reads it only when the 36 bytes are there):** `f32 skinR, skinG, skinB, f32 bloodR, bloodG, bloodB,
  f32 scaleX, scaleY, scaleZ`. **The colours are 0..1, as the exe stores them** (SeveredLimbItem +0x1f4 skin = 1 - the victim's Skin Tone,
  +0x204 blood = the race's `blood colour`, 0xRRGGBB / 255; the scale is +0x1e8, the victim's Forearm / Calf bone size). They are stamped
  at the sever and kept for the item's life (save v104), so they outlive the victim. Every other item sends nothing after the ownership
  tail. UNSURE (ours): our skin tone is one 0..1 number (`skinTone`), so skin is that grey on all three channels, and we have no bone
  sizes, so the scale is (1, 1, 1). Test `ksim_limb_tint`.

**Implemented 2026-11-04 (client-approved; additive, no protocol bump): the ownership tail, every kind.**
It comes after `stateVersion`, and for kind 4 after the producer tail. A decoder reads it only when the bytes are there; a server that
has it always sends it.
`strid owner, u16 lockLevel, strid model, f32 yaw`
- `owner`: the FACTION record that owns it, 0 = nobody's. Door: the building's owner (isObjectMine). Container and producer: their
  `ownerSid`. Ground item: the owner of the placement it was spawned from (below), 0 for anything dropped. Taking an item whose owner
  is set and is not the taker's faction is theft (event 14), so the client can label steal / loot cursors and the inspect panel before
  the act. The client compares it against its own faction sid: the SQUADS `faction` strid of a squad listed in its PLAYER_SQUADS
  (the player faction's FACTION record, the one theft compares). It is not CHAR_APPEARANCE `faction`, which is the look. There is no
  "mine" bit, because in multiplayer it differs per session.
- `lockLevel`: a door's lock level (DOORS_LIVE.md: the owner's `doors lock level` +- random + the door's bonus); 0 for the other
  kinds. The building inspect panel's condition is the existing `health / maxHealth`.
- `model`: a ground weapon's MATERIAL_SPECS_WEAPON (its quality model: the mesh / "Mk II" grade); 0 otherwise.
- `yaw`: a ground item's rest yaw (radians, [0, 2 pi)), from its Id (streamSeed(Id, kRestYawStream)), so fixed for the item's life
  (OURS: no physics; it lies flat, UNSURE how the original's ragdoll drop rests). 0 for the other kinds.
- Owned placement items (Implemented 2026-11-04): the layouts' ITEM_PLACEMENT_GROUP nodes put out kind 2 ground items owned by the
  building's faction; they refill after taken (WORLD_LAYOUT.md "Placement item nodes").

A damaged door is `health < maxHealth`; the client shows the repair cursor, but the RMB stays OPEN (CURSORS_LIVE.md).
Doors start Closed (Locked when the door record says `always locks`; UNSURE, the data has no other initial state).

### INVENTORY (type 25), on first sight and on change, player characters and containers (protocol 4; containers and equip added 2026-10-31, no format change)

What a character or a container owns. Sent for player-controlled characters in the detail bounds, and for every container (INTERACTABLE kind 3) in the detail bounds (`id` is then the container's id, every entry is slot 0 carried, `version` bumps on every transfer or loot): a record on first sight (and again when the
character comes back into view) and whenever `version` changes; nothing while it is unchanged. Never sent for AI characters.

| Field | Type |
|---|---|
| tick | u64 |
| id | u64 (the character) |
| version | u32 (1 at spawn, +1 per change: pick-up, drop, loot) |
| n | u16, then n x entry |
| entry | `item strid` (ITEM / WEAPON / ARMOUR record), `count u32`, `slot u8` |

`slot`: 0 = carried (in the pack; **one entry per stack**, see "Grid inventory" below), 1 = the wielded weapon, 2 + n = worn in the ARMOUR record's
`slot` n (AttachSlot: 1 back, 3 hat, 5 body, 6 legs, 8 shirt, 9 boots, 12 backpack, 14 belt; so the Rag Loincloth, slot 6, is 8).
Equipped entries have count 1. A character starts with what makeFighter gave it (its weapon unless fists, its armour). Looting
moves everything, equipped pieces included, into the looter's pack (slot 0) and strips the body (its next CHAR_APPEARANCE has no
items). Order of entries: equipment in spawn order, then pack entries by first acquisition, then (when a backpack is worn) the entries inside it (slot 0, ITEM_GRID section 1). The index of an entry is its position in this list; it is only valid at the INVENTORY `version` it came with. **Equip and unequip (order 62, section 7)** change the `slot` of an entry and bump `version`: equipping moves one carried unit into the weapon slot or the ARMOUR record's slot (a weapon or an armour piece already there goes back to the pack as one carried unit), unequipping moves it back. The fighter's combat numbers follow on the same tick, rebuilt from the entry's **item instance** (below): the same manufacturer, model and level the piece was generated with, so unequip + equip gives exactly the same numbers. Equipping takes no time (LOCKS_EQUIP_STOCK_LIVE.md d). When the pack holds several stacks or instances of one record, order 62, 61 and 63 name only the record, so they take the first matching entry in pack order (UNSURE: which one the original hands over); orders 66-68 address one entry.

**Item instances (2026-10-31).** Every entry is an instance with a quality: a weapon has its WEAPON_MANUFACTURER and its MATERIAL_SPECS_WEAPON model (the "Mk II", "Meitou" grade name) and the model's item level; an armour piece has its item level (0.2 x grade, +1 grade by `armour upgrade chance`, for a piece with part coverage; 1 for clothing without). Plain items (food, kits) have none and stay stacks. Carried stacks merge only when record, manufacturer and material are equal (the grid inventory below; the item level is not part of it, but weapons and armour never stack). Quality is kept across equip, unequip, drop (61), pick up (7), loot of a body or container (5) and transfer (63); a dropped stack becomes a ground item with that quality. Container stock draws a quality at spawn (`CombatData::rollQuality`: grade by the generic vendor weights 50 / 100 / 50 / 2 / 0 / 0, quality 5 / 20 / 40 / 60 / 80 / 95, a manufacturer that lists the weapon in `weapon types`; UNSURE, the original's stock roll is only documented for armour). The first aid kit is not an instance: a kit's charges live on the medic once opened (`Fighter::kitSid` / `kitCharges`), a carried kit is a full stack (OPEN: per-instance charges).

**Grade (measured, `docs/reference/GRADES_GATESQUADS_KO_LIVE.md` a).** The item's grade is the int g = round(level x 100). `grade` is the exe's name band of g: < 11 0 prototype, <= 20 1 shoddy, < 41 2 standard, < 61 3 high, < 81 4 specialist, else 5 masterwork (the shop qualities 5 / 20 / 40 / 60 / 80 / 95 give 0..5). The game shows the word only for 0 < g < 100, and only on armour, crossbows and robot limbs (weapons show the model name). 255 = no grade (clothing without part coverage). The colour uses other bands of g: < 16 ItemGrade0, < 36 1, < 51 2, < 66 3, <= 80 4, > 80 Grade5 (absent from kenshi_colours.xml: the lookup's default, not identified); ItemArtifact is a separate item flag, not a grade (not sent).

### ITEM_QUALITY (type 27), after an INVENTORY record (additive, no protocol bump)

`u64 tick, u64 id, u32 version, u16 n, n x entry`; `id` and `version` are those of the INVENTORY record it follows (same snapshot / tick), `entry` = `u16 index` (into that INVENTORY's entries), `strid maker` (WEAPON_MANUFACTURER record, 0 = none), `strid model` (MATERIAL_SPECS_WEAPON record, 0 = none), `f32 level` (0..1, the item level), `u8 grade` (above, 255 = none). Only entries that have a quality are listed; nothing is sent when none has one. It was made a separate frame, not new INVENTORY entry fields, so the protocol-4 client decoder (which reads the INVENTORY entries by position) keeps working and skips the new type (SIMLINK.md section 11: unknown frame types are skipped by length). A client that wants the grade name shows the model record's name for a weapon, and the grade word for armour. Ground items (INTERACTABLE kind 2) carry no quality on the wire: it is the server's, and shows once the item is picked up or the container's INVENTORY is read.

### ITEM_GRID (type 28), after ITEM_QUALITY (grid inventory, 2026-11-01; additive, no protocol bump)

`u64 tick, u64 id, u32 version` (those of the INVENTORY it follows), `u8 ns, ns x {u8 section, u8 w, u8 h}`, then `u16 n, n x {u16 index, u8 section, u8 x, u8 y, strid material, strid colour}`: one item per INVENTORY entry (the grids are always sent, even with no entries). Sent after every INVENTORY (player characters and containers), so a client that does not know it skips it by its length. The rules are `docs/reference/INVENTORY_GRID_STATIC.md` (kinspect 33); the code `cpp/src/ksim/inventory_grid.cpp`, test `ksim_inventory_grid`.

- **Grids** (`ns`): section 0 is `main`: 8 x 10 for a character (a constant, INVENTORY_GRID_STATIC (a)), for a container its BUILDING `storage size width / height` (`2939-gamedata.base` Storage: Ore 6 x 4). Section 1 is the worn backpack's `backpack_content`, w x h from its CONTAINER `storage size`, present only while one is worn (equipped entry in AttachSlot 12). Not in this cut: a carried (unworn) backpack's contents in a window, animals' grids, robotic limbs, trade windows.
- **Entries**: `section` 0 / 1 is the grid the entry sits in, `x`, `y` its top-left cell; 255 = equipped (its INVENTORY `slot` says where, x = y = 0). The footprint is the record's `inventory footprint width / height` (no rotation). Entries inside the worn backpack follow the character's own in the INVENTORY list (slot 0, section 1); they move with the backpack item (take it off and it lands in `main` with them).
- **`material`** is the MATERIAL_SPECS_WEAPON (weapons: the quality's model) or MATERIAL_SPECS_CLOTHING (armour, containers, items) record of the instance, 0 = none; **`colour`** the COLOR_DATA record, 0 = none. The icon is `data/icons/<item sid>.<material sid>[.<colour sid>].png`. A clothing material is picked once, when the instance enters an inventory, from the record's `material` list (seeded; every vanilla list has one entry); **colour is always 0 today (OPEN, UNSURE: the data gives no simple pick; the game derives it from the `color map` and the faction)**.
- **Stacks**: a stack takes one footprint and holds up to ITEM `stackable` (1 when absent) x, inside a worn backpack, `max(stackable, stackable bonus minimum) x stackable bonus mult` of the CONTAINER (Traders Backpack Medium: min 2, mult 3, so a Bread stack holds 6); weapons, armour, containers, maps, crossbows and limbs are always 1. Merge key: record + manufacturer + material (colour is not part of it). Existing stacks fill first, then the first free cell scanning row by row from the top left; `main` only: **nothing overflows into the worn backpack** (INVENTORY_GRID_LIVE (b)); an item goes into the backpack only by an explicit move to section 1 (order 64), a transfer to section 1 with a rect (order 68), or `grid::autoplace(..., section 1)` (a non-empty container never goes into a backpack). A whitelisted section would use base 1 for the stack cap (`Rules::stackCap(..., whitelisted)`; no section modelled has a whitelist).
- **Deviation (UNSURE)**: the original tries its equipment sections first, so a picked-up weapon lands in `hip` and armour in its slot. Here a picked-up, looted or transferred item is never auto-equipped (the fighter has one wielded weapon and one armour list, and existing orders and tests treat pack items as carried): it goes to `main` / the backpack only, and equip orders (62, 67) fill the slots.
- **Pick-up and loot that do not fit**: pick-up of a ground stack puts what fits in the pack and leaves the rest on the ground (INTERACTABLE count falls); nothing fitting is event 13 result 1 (refused: "I can't pick that up", the item stays). Loot (body and container) takes what fits, in entry order, and leaves the rest with the source; event 13 **result 6 = partial** (some taken, some left), result 1 when nothing fitted. Theft (event 14) is reported only when something was taken.
- Drops put one ground pile per kind (stacks the grid split by a cap of 1 lie together); a dropped backpack keeps its contents (`state::GroundItem::contents`).


### ITEM_LOCK (type 57), after ITEM_GRID (kenshi-h2v.15.1, client-approved 2026-11-11; additive, no protocol bump)

`u64 tick, u64 id, u32 version` (those of the INVENTORY it follows), `u16 n, n x {u16 index, u8 flags, u16 level, f32 chance, f32 hardness, f32 maxHardness}` (17 bytes each). Sent after the ITEM_GRID of every INVENTORY of a **player character**, `n` = 0 when no shackles are worn; a client that does not know 57 skips it by its length. `n` is 1 at most today: `index` is the worn shackles' entry in that INVENTORY (the armour slot 9 entry).
- `flags`: bit 0 locked (a picked or broken pair stays worn, open: bit 0 clear), bit 1 can cut (the wearer carries a TOOL item, ITEM `item function` 9; UNSURE: the exe's hasTools is not decoded), bit 2 can break (the wearer's strength beats 3 x `hardness`).
- `level` is the lock level (20, OURS, UNSURE: the record carries none); `chance` the wearer's own pick chance per roll, `pickChance(level, lockpicking)`, 0..0.9, for the menu's "Pick Lock {1}%" (the client shows `int(chance x 1000) x 0.1`); `maxHardness` 30 (OURS, UNSURE) and `hardness` what is left of it: the cut drains it.
- When: with each INVENTORY, so on connect and whenever the inventory `version` changes. The sim bumps the version when a pick or a break opens the shackles and when they come off; **a cut in progress does not bump it** (`hardness` is as of the last resend: the bar is TASK_PROGRESS kind 7). A skill, strength or tool change shows at the next resend too.

### CAGES (type 64), per session, on connect and on change (kenshi-k16.5.7, client-approved 2026-11-13 on kenshi-k16.5.6; additive, no protocol bump; test `ksim_cage_wire`)

For the caged-character hover and the two-menu right-click (client k16.2.105): which cage a character is in, and that cage's lock. `u64 tick, u16 n, n x {u64 id, u64 cage, strid record, u8 flags, u16 level, f32 hardness, f32 maxHardness}` (31 bytes each). The list is always **complete**: every caged character in the session's view (a character the client knows, enemies included), sorted by `id`; a client that does not know 64 skips it by its length.
- `cage` is the cage's building key (`kworld::doorKey` of its placement: the key space of CHAR_DYNAMIC `building` and event 49 `turret`) and the **target of the cage orders** below; `record` is that building's BUILDING record (its name reads "Prisoner Cage").
- `flags` bit 0 locked (`cageDoorLocked`, the test CHAR_STATS state 5 / 6 uses); the other bits are 0. `level` is the lock level (`cageLockLevel`: the door roll, the value CHAR_STATS `lockLevel` shows for a focus). `maxHardness` is the BUILDING `hardness` (30 where unset) and `hardness` what is left of it: a cut drains it.
- When: on connect (`n` may be 0), and when a character is put in or let out, its lock is opened (picked, cut, broken) or locked again, or it enters or leaves the session's view; `n` = 0 is sent once when the last one goes. **A cut in progress does not resend** (`hardness` is as of the last send, the bar is TASK_PROGRESS kind 7), the ITEM_LOCK rule. The sim tests the set and the lock's open flag every tick and the doors' state once per second, so a lock that opens because its doors broke shows within a second.
- The pick chance is the **picker's**, not the cage's: ask MENU_ENTRIES (order 99 on the cage key).
- Why a frame and not a CHAR_DYNAMIC tail: caged characters are few and change rarely, CHAR_DYNAMIC's tail is long and costs every tick.
- **Decoder fixture** (the client's ask 2): `cpp/tests/fixtures/cage_prisoner_cages.hex`, two whole frames (u32 length, u16 type, body) of scenarios/cage_prisoner after load + 2 ticks, seed 3: `cages` = CAGES with the prisoner in the Heft slave shop's cage (locked, level 46, hardness 30) and `menu` = the MENU_ENTRIES reply for the Wanderer on that cage (bit 16 + pickChance). `ksim_cage_wire` compares the live frames with it byte for byte (`KSIM_WRITE_FIXTURE=1` rewrites it). The strid is that session's string table index, so it is only meaningful with the strings that session sent.

### CHAR_DYNAMIC (type 21), every tick at full detail

| Field | Type | Notes |
|---|---|---|
| id | u64 | |
| x, y, z | f32 x3 | y is real (floors, ladders): the navmesh surface height; the client ground-snaps only when `yValid` is clear. The server sets `yValid` whenever it has a navmesh (always, for the duel) |
| yaw | f32 | radians, root motion included |
| fwdSpeed, latSpeed | f32 x2 | units/s, in the character's frame, and the body's **actual** motion over the tick (the wire carries the displacement; implemented 2026-11-06, branch ds/velocity-report, test ksim_velocity_report): a separation nudge, a reposition push or a snap is in it, not only the walker's own step. The walker's commanded speed is not sent (`Fighter::forwardSpeed/lateralSpeed` still drive the sim's own tests) |
| flags | u32 | bits: 0 combat (clear while the fighter's combat state is Approach, 11 / 10, so the run-in to a target draws jog / run lower clips; set from the hub, circle, wait, attack and defend states or a technique on, COMBAT_ANIM.md 3, k16.6.129; UNSURE: the exe's writer of ctx+0x244 is not decoded), 1 stealth, 2 drawn, 3 KO, 4 dead, 5 prone/ragdoll, 6 player-controlled, 7 carrying-left, 8 carrying-right, 9 yValid, 10 lowDetail, 11 blockMode, 12 run (protocol 4: the run gait is on; clear = walking at the race walk speed; set by default, toggled by order 10 `which` 2; with speed modes: set when the mode is not Walk, `kFlagRun`), 17-18 speed mode ((flags >> 17) & 3, order 10 `which` 5; **implemented 2026-11-05**: the character's `Fighter::moveMode` 0 Walk, 1 Jog, 2 Run (the default), 3 Group, `kFlagSpeedModeShift`), 13 hold (additive 2026-11-01, no protocol bump: standing in hold after order 4 hold = 1, or the AI-off hold; any replacing order clears it, order 4 hold = 0 included; the HUD HOLD tick), 14 hostile (**approved by the client 2026-11-02, implemented b0** (`kFlagHostile`, `Session::writeDynamic`; test ksim_interact): additive, no protocol bump; set when the sim's own fight test says this character would fight the receiving session's player characters: today `enemy()` in combat.cpp, team differs from the session's player team and the character is not dead; with the faction / crime work it becomes the relation test, the bit and its meaning unchanged. Per session (each session's player faction, MULTIPLAYER), never set on the session's own characters. The client's RMB picks attack when set, talk otherwise), 15 seated (**approved by the client 2026-11-02, built** `kFlagSeated`, `Simulation::seated`; additive, no protocol bump: a resident on RELAX standing within 8 u of a layout seat of its squad's bar table (TOWN_LIFE_SLICE 5.17; 1 u until 2026-11-03: furniture is now cut from the navmesh, so the sitter stands at the walkable point nearest the stool, up to about 6 u off; `routines::kSeatTolerance`); never for the ring fallback; **extended 2026-11-10 (kenshi-k16.6.30): also a goal-43 (STAY_IN_HOME) resident at its home seat (a chair of its home layout that serves no table, reserved like the bar seats) or a floor-sit (a synthetic seat: the member's own spot, height and yaw, clip `sitting idle`, the synthetic bed's convention), cleared by a fight target, being downed, KO'd, carried or walking**; the character keeps the position the sim gave it (the client may draw the sitter on the nearest seat piece); yaw is the walk heading; the seat's snap point, facing and clip are the pose tail below), 16 asleep (`kFlagAsleep`, **approved by the client 2026-11-04, implemented**: a sleeper lying in its bed, `Fighter::asleep`, set by the routine on arrival at a bed (TOWN_LIFE_SLICE 5.16) and cleared on waking, a fight, being downed or carried; the bed's pose is the pose tail below), 19 indoors (`kFlagIndoors`, **approved by the client 2026-11-07, implemented**: `Fighter::indoors`, in a building's interior AND under a ceiling, so a roof / top deck inside the footprint is clear (INTERIORS.md "Indoors state per character"); additive, no protocol bump), 20 aim tail (`kFlagAimTail`, **approved by the client 2026-11-07, implemented 2026-11-07**: a trailing `aimYaw` f32 after the pose / rest tails, set while a technique with a clip plays — `Fighter::aimYaw`, the look hand the swing's clip root is turned by; additive, no protocol bump; test `ksim_simlink`), 21 mounted (`kFlagMounted`, **approved by the client 2026-11-10**: the fighter mans a turret (`Fighter::mount`); a trailing `turret` u64 follows the aim tail, the very end of the record; additive, no protocol bump; test `ksim_simlink`), 22 strafe left, 23 strafe right (`kFlagStrafeLeft` / `kFlagStrafeRight`, **approved by the client 2026-11-14, kenshi-k16.8.15, implemented k16.8.17**: the applied strafe of a direct drive, `Fighter::driveStrafe`, order 20's trailing strafe; no trailing data; additive, no protocol bump; tests `knav_walk`, `ksim_move`, `ksim_simlink`) |
| carriedBy | u64 | 0 if none. Set from the moment the lift attaches the body (40 % of the 2.4667 s lift clip: 0.987 s) until the put-down releases it or the carrier drops it; x, y, z follow the carrier exactly while it is set. The carrier shows flag bit 7 (carrying-left; the lift names the left hand). Additive in meaning only: the field and the flags existed, protocol unchanged |
| legR, legL | f32 x2 | percent effective leg health (LOCOMOTION.md) |
| action | strid | ANIMATION / technique record, or 0 |
| actionStart | u64 | tick the action started |
| actionSpeed | f32 | clip speed (CharStats x technique, or 1) |
| drive, heading, driveSeq | u8, f32, u32 | applied direct-drive state, heading, and the clientSeq of the last applied drive order (section 12) |
| building, floor | u64, i16 | interior; 0 outside. The server sends the fighter's `building` (perception's same-building gate): the kworld::doorKey of the building's placement, set while the fighter stands inside the building's volume and cleared when it is outside, however it got there (walk, teleport, load, spawn). **Implemented 2026-11-02** (`Simulation::stepBuildings` each tick, for a fighter that moved, via `Simulation::buildingAt`, wire layout unchanged): the volume is the placement's `interior mask` hull (the exe's indoors trigger, INTERIORS.md "Building membership"), else the whole footprint for a building with a door that has a way in. This replaced the door-plane crossing, which left the Hub Storm House at 0 (the ramp does not cross the slab and the door's inside normal pointed outward). A carried or dead character keeps its value (a body put down takes its carrier's). **`floor` (implemented 2026-11-05, branch ds/floor-index; tests ksim_building_membership):** the storey of the building the fighter stands in, 0 = the ground floor; computed with `building` each time the pair is set (`Simulation::buildingFloor`, `BuildingSite::floorAt`). The building's upper walking surfaces are `BuildingSite::floors`: the tops of its interior-mask-clipped walkable shapes (`kworld::WorldShape::keepInside`, the upper floors and their stairs), ascending, clustered with a 30 u gap, one per storey (the ground floor has no shape). A fighter is on floor k from halfway up: its `y` at or above the midpoint between the k-th surface and the one below (the placement y for k = 1), never later than the surface less 6 u (2026-11-05, client item 6: roof hiding turns over mid-stairs; UNSURE: the exe's threshold). An outside fighter (`building` 0) has floor 0. UNSURE: the exe's own on-the-wire `floor` (STORM_HOUSE_INTERIOR_LIVE.md) has not been compared against ours; the per-building surface heights are not on the wire yet (the client splits the height evenly: propose before building) |
| appearanceVersion | u32 | |
| actionFlags | u8 | **Trailing, additive (approved by the client coordinator, Implemented 2026-11-02, no protocol bump; a decoder reads it only when present, as the earlier trailing fields).** Bit 0 = loop: the client holds and repeats the clip until `action` changes. Only the operate action sets it; combat clips and every other action send 0 |
| poseX, poseY, poseZ, poseYaw, poseClip | f32 x4, strid | **Pose tail (approved by the client 2026-11-04, Implemented 2026-11-04; `Simulation::pose`, `Session::writeDynamic`; tests ksim_town_bar, ksim_town_beds). Trailing, after `actionFlags`, present only while flag bit 15 (seated) or 16 (asleep) is set, so `actionFlags` is always present then; additive, no protocol bump.** The body's snap point and facing (kinspect bed_seat_pose.md); x, y, z above stay the walkable stand point and the mover stays put; the client snaps the body and the name tag to the pose. Seat: the furniture origin, facing the chair's local +Z, or its table for a `chair` / `sit on floor` seat (functionality `flags` 1; a bench's flipped `flags` 2 variant is UNSURE, sent as +Z). Real bed: the use node the sim gave the sleeper, facing the node's -X (node.q x RotY(-90 deg)). Synthetic bed (yards, no furniture): the sleeper's own spot and yaw. `poseYaw` is in the record's `yaw` convention. `poseClip` is the clip name (ANIMATION record name, interned as `action`), always resolved, never 0: the piece's BUILDING_FUNCTIONALITY `animation`, else sleepinbed (bed) / sitting chair (seat); a synthetic bed sends sleeponfloor, a floor-sit sitting idle |
| restYaw | f32 | **Rest tail (approved by the client 2026-11-06, implemented 2026-11-06; task BG, `reference/PHYSICS.md` 2-3). Trailing, after `actionFlags`, present only while flag bit 5 (prone: down, KO'd or dead) is set and neither bit 15 nor 16 is (a pose tail and this one never coexist); additive, no protocol bump.** While bit 5 is set the x, y, z above **are the body's rest point** (no separate field: a prone body does not move, and the logical position re-syncs only at state ends), and `restYaw` is its lying direction, head to feet, radians, the character yaw convention (toward `(sin yaw, cos yaw)` in world XZ, 0 = +Z). OURS: rolled at every ragdoll start (EVENT 48, cause 0-3) from the fighter's own deterministic stream, so a replay and a save/load lie the same way; saved with the fighter (`Fighter::restYaw`, v76). A body that goes down with no event of its own (a limb collapse) rolls one on that step |
| aimYaw | f32 | **Aim tail (approved by the client 2026-11-07, implemented 2026-11-07; `Session::writeDynamic`; test `ksim_simlink`). Trailing, after the pose and rest tails (so the very end of the record), present only while flag bit 20 is set; additive, no protocol bump.** The aim a swinging technique's clip root is turned by (`Fighter::aimYaw`, set at the technique's start), radians in the record's `yaw` convention ((-pi, pi], 0 = +Z). Sent while a technique with a clip plays: `integrate()` turns the clip's root motion by this aim. An animal technique has no clip and sends none, and a seated or prone record never carries it (its pose / rest tail stays the last bytes). |
| turret | u64 | **Mounted tail (approved by the client 2026-11-10, implemented 2026-11-10; `Session::writeDynamic`; test `ksim_simlink`). Trailing, after every other tail including the aim tail (so the very end of the record), present only while flag bit 21 is set; additive, no protocol bump.** The turret the fighter mans: the `kworld::doorKey` of the turret's placement, in the same key space as `building` above and event 49's `turret` (a decoder of the record before this tail stops before these 8 bytes). Set while `Fighter::mount.building != 0`: the gunner stands at the post and shoots with the turret's gun (RANGED.md D5-D7); it clears when the gunner dismounts. UNSURE (default: mirror the other tails): the bit follows the sim's mount, not the client's "manned" building state; BUILDING carries no manned flag. |

**Capture actions (k16.6.137, rwh.114; `Simulation::captureAction`, test `ksim_capture_actions`; values in the existing fields, no layout change).** While a capturer works a slave chain: `action` = "medic" (string kind 3, the ANIMATION record name; `actionFlags` bit 0 = loop, `actionStart` = the chain's strip tick) from the strip 181 to the end of the cuff 166 (12 s); during a lift (action 8, CarryStep phase 0): `action` = "shoulder lift left" (2.4667 s clip, not looping, `actionStart` = the lift's start, rate 1; the body attaches at 40 % = 0.987 s, the task ends at 95 % = 2.343 s). A put-down plays the same clip at rate -1 and sends no action yet. Fields already decoded; a client that does not know the clip names plays nothing.

**Operate action (Implemented 2026-11-02; `Simulation::operateAction`, `Session::writeDynamic`; test `ksim_producers`).** While a character is an operator of a producer (it holds the order 60 `use` task and is in the producer's operator list, standing and conscious), `action` = the clip name (string kind 3, like the combat clips): the name of the ANIMATION record that the producer's BUILDING_FUNCTIONALITY `animation` ref points at (verified against the merged data: a ref category on the functionality, not a string; Ore mine, Stone mine and farming point at ANIMATION `stonemine`; the iron and copper deposits have none). No ref (or an empty one) = `stonemine`, the exe's own fallback (kinspect `findings/operate_anim.md`). The building reaches it through its `functionality` list (the first live entry, as `producers::producerFunctionality`). A wounded operator keeps the same clip (the exe's operate task never reads `animation dazed` / `animation KO`). `actionStart` = the tick the operate task started: set when the character is added to the producer's operators, stable for the task's lifetime (it is not re-sent per production cycle; the clip and the output are not linked, production is continuous per tick). `actionSpeed` stays 1. When the task ends (full, a replacing order, KO, death) `action` is 0 again, or the combat clip if one runs, and `actionFlags` is 0. A character whose race has no such clip in its animation set plays nothing (client side, M). After a load the start tick is the first tick the sim sees the operator (the start is not saved).
- **Playback speed (client-side formula, NOT a wire field):** `min(1.5, 0.55 + 1.45 x skill / 100) x 1.1`, `skill` = the operator's skill in the functionality's `stat used` (Labouring on every mine; the same stat as the production rate). **The skill is not on the wire today** (no skills record exists; ITEM_STATS is item data), so the client cannot apply it yet; until a skills record exists it plays the clip at its own speed. Ask for one if the animation speed matters.

### CHAR_ROUTE (type 23), on a new route, a reroute and a clear

The route a character is walking, so the client can draw the path line and interpolate. Server
pathfinding only (section 12): the client never makes a route.

| Field | Type | Notes |
|---|---|---|
| tick | u64 | the usual leading tick |
| id | u64 | |
| clientSeq | u32 | the order that made the route; 0 = the AI's own. The clear of an order's route carries that order's seq (the sim zeroes `routeSeq` when the order arrives; the session keeps what it last sent) |
| routeVersion | u32 | per character, bumped on every new route, reroute and clear |
| flags | u8 | bit 0 truncated (more waypoints than were sent), bit 1 the destination is an approach point (e.g. attack range), not the clicked spot |
| n, then n x {x, y, z} | u16, f32 x3 each | the waypoints still ahead, y = the surface height |

- The first waypoint is the next one after the character's position at `tick`. There is no progress
  field; the client trims passed points itself.
- Sent when the route is set or replaced, and with `n = 0` when it is cleared, finished or abandoned
  (a clear is not sent to a client that never heard of a route for that character). A new client gets
  the current route of each character it starts seeing. Sent after the CHAR_DYNAMIC records, inside the
  snapshot bracket.
- Only for player-controlled characters inside the detail bounds. The server flag `--ai-routes` (off
  by default) also sends AI routes, for a debug overlay.
  The server flag `--log-world-squads N` (off by default) is server-side only: it logs the built world squads to stderr every N game minutes (RUNBOOK.md); nothing on the wire.
- `n` is capped at 256 (`kMaxRoutePoints`); a longer route sets the truncated bit and the rest is not
  sent until the next reroute.
- Server implementation: `state::Fighter::route` and `routeVersion`; the version bumps in
  `Fighter::clearRoute` and when a `FighterMoveTo` command applies.

### CHAR_MEDICAL (type 22), 2.5 Hz (kenshi-b0, 2026-10-31; server implemented, no protocol bump)

`u64 tick, u64 id, f32 blood, u8 n, n x {strid part, f32 health, f32 stun}, u16 severed`. `tick` is the usual leading field (every record has one).
Sent for every character in the detail bounds (AI characters too) on first sight and then every 8th tick (2.5 Hz), staggered by id
so a crowd does not burst on one tick. Parts are the character's body parts in the race's anatomy order (`part` = the LOCATIONAL_DAMAGE
name as a kind-4 string); `health` is the part's health (not clamped at 0; it sinks to -3 x the part's max, which the client has from
the race: -300 for a 100-max limb, **-15 for a severed one**, whose max is forced to 5), `stun` the stun on it. `blood` is the blood level
(max by race and strength). Health rises only through first aid (interact action 10) and the bandage pool; there is no natural
regeneration, and an untreated part over 20 % damage slowly loses health (MEDICAL_HUNGER_LIVE.md d). Only a severed limb leaks blood
(0.002 x |health| per unit, RESOLUTION 5.3); an intact limb never does. No bandage pool is on the wire.

**`severed` (Implemented 2026-11-02, additive, no protocol bump; a decoder reads it only when the frame has the bytes):** bit i = part row
i, in the order the rows are sent, is a severed limb (dismemberment, RESOLUTION 5.4: a non-vital humanoid arm or leg whose max health is
forced to 5; the client draws the stump). **Always present on a server that has it, even when 0: an absent field means an old server,
not "nothing severed".** Only non-vital humanoid arms and legs can be set. A character has at most 16 parts (asserted in `ksim_simlink`),
so a u16 holds them all. The mask means **state 1 (severed) only**; when prosthetics arrive (limb state 2) they get their own mask, not these bits.
A sever is permanent for now. Event 47 announces the moment of the sever. On a sever the server also drops the victim race's
`severed limbs` item for that limb (RACE list, v0 slot 0 L arm, 1 R arm, 2 L leg, 3 R leg; only rebirth.mod / Newwworld.mod races have it,
others drop nothing) as a normal ground item (INTERACTABLE kind 2) at the character's position, and when both legs are gone the worn
boots (AttachSlot 9) drop too; one leg keeps them, weapons stay on an arm sever (kinspect `findings/cq_sever_visual.md` sections 1, 2).
UNSURE / later: the original spawns the limb at the Forearm / Calf bone with the hit impulse. It is tinted: see the limb tint tail (INTERACTABLE, below the kind 5 tail).

**Panel tail (Implemented 2026-11-04, client-approved; additive after `severed`, no protocol bump):** the medical panel's
remaining inputs (kinspect `findings/medical_bars.md`).
`f32 bloodMax, f32 bleed, f32 hunger, u8 flags, u8 n, n x {f32 max, f32 bandage, f32 splint, u8 limb}`. `n` repeats the part count
(always equal to it, a self-check).
- `bloodMax`: the scale of the Blood row (`getMaxBlood`, race x strength; CombatRules::maxBlood).
- `bleed`: blood lost per second (positive). The Blood row's arrows are `min(ceil(bleed / 0.05), 100)` x `<`.
- `hunger`: 0..3 (the UI shows x100, three bands).
- `flags`: bit 0 = digesting (a food item being eaten; the exe's `fed` buffer > 0, the `>` arrow); bit 1 = in bed (asleep: no default
  arrow). Bits 2 and 3 are the portrait's (kenshi-k16.6.160, client-approved 2026-11-13 on k16.6.158; additive, no bump; a decoder
  that masks only bits 0-1 is unaffected):
  - bit 2 **hurt**: the portrait's red pulsing background (the exe's `1404123a0`, rwh.117, minus its two camera gates, which are the client's):
    `(!incapacitated && engaged && !caged) || beingEaten`. incapacitated = KO, dead, down (ragdoll) or carried; caged = in a cage-type building
    (`cageBuilding != 0`); beingEaten = an eat application on this victim in the last 10 s (the exe's `char+0x279 & 7`). **engaged** stands in for the
    exe's `AI+0xb0` float, whose writer is not decoded (UNSURE, kenshi-k16.6.159): OURS = a living combat target (any combat state, Approach
    included) or a body in the arms, held 15 s after both end. The hold is not saved: a load forgets a hold in progress (deferred: playtesting).
  - bit 3 **putIn** (`char+0x2f8 == 1`): lying in a real bed, asleep by its own bed routine or put in one by a carrier (task 99). A floor sleeper
    (bit 1 only) and a caged character are never putIn. The dim rule is then CHAR_DYNAMIC KO | dead | carriedBy != 0 | putIn.
  Other bits 0.
- Per part, in the same row order as above: `max` is the part's current max, max health minus wear (M, the slat's scale; the race max
  is not enough once wear exists); `bandage` is the bandage pool not yet converted (the yellow past the green); `splint` is the jury-rig
  bonus (+0x4c, the white: `green + 0.01 x splint`); `limb` = bits 0-1 the limb state (0 original, 1 stump, 2 replaced, 3 crushed;
  ours sends 0 or 1, since there are no prosthetics or crushing yet), bit 2 robot (a robotic race's part or a replaced limb: the blue
  layer).
- Same presence rule as `severed`: always sent by a server that has it; absent bytes mean an old server.

A new record type needs no protocol bump; the client's decoder must learn type 22 (the ksim_simlink test
checks the frame and does not feed it to the client decoder yet).

### CHAR_DETECT (type 26), on change (kenshi-b0 perception slice, 2026-10-31; additive, no protocol bump)

`u64 tick, u64 id, f32 meter, u8 state, u64 by`: the player's detection meter for one **player-controlled** character `id` in view.
`state`: 0 none, 2 being noticed, 1 noticed. `meter` is 0..1: the highest sneak meter among the hostile characters that are filling one
on `id` (state 2; the meter notices at 1.0), or exactly 1.0 with state 1 (some hostile is aware of `id`). `by` is the observer it is
from (the highest meter; the first noticer for state 1), so the client can point at it. Sent when the state or the meter (in steps of
1/64) changes, never more than once per tick per character, and once with state 0 when it clears (then silence until it rises again);
nothing is sent while it stays 0. A character that leaves the client's view forgets it (the client drops the character with event 11).
The meter is the original's per-pair "spotted by" record (PERCEPTION_LIVE.md item 18): a non-sneaking character in a hostile's cone,
reach and line of sight goes straight to state 1 with no filling; a sneaking one fills `dt / ((0.5 + 5 s (1 - v)) x (4 - 3 attn))` per
update (about 9-11 s for a good sneaker in front of an average guard), drains by `dt / 260` outside the cone and is wiped when the line
of sight breaks. Only hostile (other-team) observers count. A client that does not know type 26 skips it (the frame length).

### SEEN_BY (type 59), on change (approved 2026-11-11 by the client lead, kenshi-h2v.1.13; implemented, kenshi-h2v.1.3)

`u64 tick, u64 id, u16 n, n x u64 observer`: for one **sneaking player-controlled** character `id` in view, the hostile characters in the session's view that know it, observers sorted by id. "Know" is the stealth KO's abort test (`stealthko::victimKnows` = `perception::aware`; the approach of order 60 action 14 aborts on it, STEALTH_KO.md), one predicate for both, so a hostile absent from the set is one the KO does not abort on. "Hostile" is CHAR_DYNAMIC's `kHostile` flag (not player-controlled, not dead, a team outside the session's player teams), so an observer that turns hostile while aware enters the set on that change. Additive, no protocol bump; a client that does not know type 59 skips it.
- Sent when the set changes (including when the sneak starts with a non-empty set), at most once per tick per id, in the tick's frame after CHAR_DETECT. Once with `n = 0` when the sneak ends, the character dies or the set empties; nothing is sent while it stays empty (absent = empty).
- A character that leaves the client's view forgets it (event 11); the server resends when it comes back into view with a non-empty set. A connect snapshot resends too.
- Client rule (kenshi-h2v.1.13): while the selected character sneaks and carries nobody, a hostile character absent from its set gets the red fist and right-click sends order 60 action 14; a hostile in the set gets the normal right-click. No chance is carried (a KO_QUERY / KO_CHANCE pull is a later proposal).
- Test: `ksim_simlink` (SEEN_BY case).

### DETECTED_BY (type 60), on change (implemented 2026-11-12, client-approved; kenshi-h2v.27.1)

`u64 tick, u64 sneaker, u16 n, n x {u64 observer, u8 state, u8 friendly}`: the original's detection map of one **sneaking player-controlled** character `sneaker` in view, restricted to the observers in the session's view, sorted by observer id. The client draws one arrow per entry (`arrow.mesh`, red for state 1, yellow for 2, green for friendly; geometry in the client's `rwh63_arrow_live.md` note). Additive, no protocol bump; a client that does not know type 60 skips it. SEEN_BY is unchanged (a separate, KO-oriented set).
- **Sneaker**: player-controlled, sneak on, not in combat (`perception::inCombat`), not unconscious, not dead.
- **Entry** (`sensor::detected`, from the observer's `SightRecord` of the sneaker): `state` 2 = the observer's scan sees it and the meter is <= 1.0 (yellow), 1 = noticed (meter > 1.0, or force-noticed; red), each refreshed within the last 4.0 s of game time (`kDetectFresh`; the exe's stale limit DAT_14168bf24; a state-2 record is rewritten by every scan, a state-1 one by `sinceSeen`). `friendly` = 1 for an observer friendly toward the sneaker (`sensor::ally`: same faction, or relation >= 50): sight never gives it an entry, only a force-notice does, within 4.0 s (`SightRecord::sinceNoticed`, unsaved), always state 1.
- **Observer gate** (a failure drops the entry at once): awake, not caged (`cageBuilding`) or in a bed, not dead or KO'd or down, not carried, not a slave. This is a filter on the wire only: perception's own decisions and the stealth KO's abort are untouched.
- Sent when the set or any entry's state/friendly changes, at most once per tick per sneaker, in the tick's frame after CHAR_DETECT beside SEEN_BY. Once with `n = 0` when it empties (sneak off, combat, the sneaker down or dead, the last record gone); nothing while it stays empty. A sneaker that leaves the client's view forgets it (event 11) and the set is resent on return. A connect snapshot carries the current non-empty sets (the sent map is session state, never saved).
- Test: `ksim_simlink` (DETECTED_BY case: a neutral passer-by gives yellow then red; a caged observer none; sneak off n = 0 and silence; the snapshot; a friendly only through a force-notice).
- **Force-notice writers** (the original's `140858f40` callers, red_arrow_rules.md section 5) against ours. `perception::notice` is our force-notice:

| original writer | ours |
|---|---|
| `onTargetedBy` 140999f00 / 140999d50 (targeted / attacked) | present: combat `learn` (the attacker and the victim, both ways) |
| stealth-KO victim notice of the assassin (task bodies, KO family) | present: `combat.cpp` stealth KO start `notice(victim, assassin)` |
| alarm responders `140354260` -> `1405cb0a0` (~100 u) | present (210 only): `crime_goals.cpp` `noticeAlliesNear`, a responder at the alarm force-notices each ally within 100 u (UNSURE: the exe's "sensed ally" is read as proximity; deferred: playtesting) |
| bounty spotting `140854d10` | present: `crime.cpp` witness step, after the timer refresh the witness `notice`s the criminal |
| dialogue action block `140680560` | present for DA_ATTACK_CHASE_FOREVER 6 / DA_ATTACK_STAY_NEAR_HOME 55 / DA_ATTACK_IF_NO_COEXIST 57 and DA_CRIME_ALARM 33 (`dialogue_sim.cpp`: the speaker notices the target); missing for DA_MASSIVE_ALARM 56 (no executor yet) |
| `14034cd80` Task_StealthKO::tick (a failed KO roll) | present: the row above (the KO start notice is exe-exact, only on the failed roll) |
| `14034ffa0` Task_Kidnapping vf1 (start) / `1403552d0` vf2 (tick), 246 KIDNAP_ORDER (rwh.76) | present for the target already down or dead (`arrest.cpp` `stepArrestOrders`, an order with lift 246: the target `notice`s the arrester at the start and once more on the next tick, then the order ends); a successful lift writes none, Task_CuffTarget never notices. Missing: the start roll `140885600` for a conscious target is not decoded, so a conscious target is not failed (UNSURE); nothing issues a 246 order yet |
| hand-based notify `1405ce1e0` (the generic put-down; rwh76 Q2/Q3) | present (2026-11-12, h2v.27.9): `carry.cpp` `captiveNoticesCarrier`, from `dropCarried` and the carry task's put-down: a kidnap captive (`Fighter::captive`, the exe's CharStatus +0xe9) released while conscious and alive, not allied to its carrier (`alliedTo(captive, carrier)`, the exe's isFriendly), force-notices the carrier. An unconscious, dead or friendly captive, and a plain body, write nothing |

  Wired 2026-11-12 (kenshi-h2v.27.5 - .9): the rows above marked present. The others stay missing for the reason in the row.

### SQUADS (type 30), 1-2 Hz

`u32 n`, then n x `{u64 id, strid faction, f32 x, z, u16 members}`.

### PLAYER_SQUADS (type 31), on connect and on change

Agreed with kenshi-0f 2026-11-01; protocol stays 5 (a new record type, skipped by older clients). Sources: kinspect `findings/ui_captures/squads.md` and `findings/squads13_static.md`; rules in `reference/FACTION_SQUADS_LIVE.md`.

`u64 tick, u64 faction (state::Id of the PlayerFaction), strid factionName, u16 factionCount, u16 maxFaction, u16 maxSquad, u16 maxSquads, u16 n`, then n x `{u64 squadId, str name (u16 length + UTF-8, up to 32 characters, "" allowed), u16 m, u64 member x m}`

- Sent only to the session that owns the faction (MULTIPLAYER: a faction's squads belong to its player).
- Squads are in faction-list order, which is also the HUD tab order. Members are leader first.
- Empty squads are included; the HUD shows a tab only for a squad with members.
- `factionCount` counts the living members (the "4/30").
- The limits come from GLOBAL CONSTANTS `max faction size` / `max squad size` / `max squads` (vanilla merged: 30 / 20 / 10).
- `squadId` is the same id as in SQUADS (30) and `CHAR_APPEARANCE.squadId`.
- An applied order 70-75 is followed by a fresh record.
- The active HUD tab, the selection and the camera stay client-side.
- **Trailing `i64 money` (Implemented, MINE_SELL_SLICE 3.1):** `state::PlayerFaction::money` after the squad list; a money change counts as a change, so the record is sent again.

### SAVES (type 32), on order 83 and after every save, load and delete (kenshi-b0 + kenshi-0f, agreed 2026-11-01)

`u16 n`, then n x `{str name, i64 mtime (unix seconds), u32 saveVersion, u8 status, u32 day, f32 hour, str factionName, str leaderName, u16 members, i64 money}`, newest first.
- **status:** 0 ok; 1 older version (migrates on load); 2 unreadable or newer (the client greys it).
- **Sent** in answer to order 83, and unasked to every admin session after each save (80), load (81) and delete (82).
- **Cheap to list:** the header fields come from a small metadata block at the front of each save file (RUNTIME_SAVE), so listing never restores a world.
- **Admin session:** for now the loopback / first session (author 0). Every other session gets `not_allowed` for orders 80-83. The rule moves to MULTIPLAYER 6.2's `pause_policy` model once multiplayer lands. This revises MULTIPLAYER section 4: save and load are SIMLINK session controls gated by server-side authority, not a separate admin channel.
- **Save dir:** `ksim_server --save-dir DIR`. The default is `$XDG_DATA_HOME/ksim/saves`, else `~/.local/share/ksim/saves`. The server refuses a dir inside the game install.
- **Client:** the panel lists NAME | DATE and fills its details box from day / hour / faction / leader / members / money. A thumbnail, if the original shows one, is the client's (a PNG beside the save, keyed by name).
- **Load write order (a guarantee the client relies on):** the loading session gets event 9 (applied, carrying the save's tick), then SAVES, then the full resend from SNAPSHOT_BEGIN, all in **one write**. The client clears its world maps on that event 9, because a snapshot never signals removal by omission. Keep this order (`Session::runSaveControl`, `simlink_saves.cpp`); a change needs 0f's agreement.
- New game: order 84 new_world, STARTS (33), NEW_GAME (111) and its reply (62) are below; MULTIPLAYER section 4.

**Implemented (2026-11-01, kenshi-b0 sim worker).** Code: `cpp/src/ksim/simlink_saves.cpp` (`Session::runSaveControl`, `writeSaves`, name rules, directory listing, atomic write), `cpp/include/ksim/simlink_saves.hpp`; the metadata block `ksim::describeState` / `readSaveInfo` (RUNTIME_SAVE.md v36); `Simulation::reload` (load in place); `ksim_server --save-dir`. Test: `ksim_saves` (frame 32 decoded by hand). Choices and UNSURE points:
- **Wire:** like every record, SAVES starts with `u64 tick` (the server's clock tick when written; after a load, the save's tick), then `u16 n` and the entries. `mtime` and `money` are two's-complement `i64`.
- **Answers:** applied = event 9 (appliedTick = the clock tick at the time; after a load, the save's tick), then SAVES, then (load only) the full snapshot, all in one write. Refusals are event 10: `unsupported` (the server has no save dir, e.g. `--dump-snapshot`), `not_allowed` (not the admin session; checked before the name, so a non-admin learns nothing), `bad_value`, `bad_target`, and **`io_error` (UNSURE, new: a save or delete the filesystem refused; the log has the errno text)**. A malformed body (trailing or missing bytes) is a protocol error like any order.
- **Not logged:** 80-83 never enter `ServerContext::orderLog`, so replay never sees them; they run when the frame is handled, which is between two ticks.
- **Admin:** `Session::setAdmin`; every session starts non-admin and `SimLinkServer` marks a client that connects from 127.0.0.0/8. Unasked SAVES to the other admin sessions goes out at their next `sendTick` (the sessions register in `ServerContext::sessions`); after a load every other session also gets the full snapshot at its next `sendTick`.
- **Name rules:** exactly the list in order 80, plus the **UNSURE** extras: control = U+0000-U+001F, U+007F and U+0080-U+009F; strict UTF-8 (overlong forms, surrogates, past U+10FFFF are bad); "characters" are code points; also at most 255 bytes (64 four-byte characters would pass the 64 rule but exceed the file system's name limit). The file is `<saveDir>/<name>`, no extension. Listing skips dot files (the atomic write's temp file is one), directories, symlinks and names that fail the rules.
- **Status:** 0 = this build's version with an intact head (the recorded file size equals the real one). 2 = anything else: not a runtime save, truncated or extended, an older version (the loader has no migrations, so "older, migrates" would be a lie: **status 1 is never produced yet**), or a newer one. A save with an intact head and a damaged body lists as 0 and fails the load with `bad_value`; the world is untouched (the load reads the file on a scratch world first).
- **Load:** `Simulation::reload` replaces every instance in place (the arena, navigation, AI configuration, weather and wilderness spawner, the session observers stay and are rebuilt from the loaded state), then each session's `Known` caches (`Session::resetKnown`: known characters, looks, bounds, squads, weather, nav tiles, inventories, interactables, detect, blood, pending events) are cleared and `sendTick(initial)` runs. Tested: the state hash equals the saved one and the run on from the load equals the uninterrupted run. `ServerContext::looks` (the scenario's per-character sex / head / faction, not saved state) is left as it is: it only holds for the Ids the scenario spawned, so loading a save from another scenario shows default looks (UNSURE; fine for saves of the same server).
- **Money / members / leader:** money is `state::PlayerFaction::money` (a new game's start money since save v37; 0 for a scenario's faction). members = living characters in the player faction's squads; leader = the first member of its first non-empty squad.
- `resetPositions` is read and ignored. Load order mismatch (a save made with other mods) is `bad_value`.

### STARTS (type 33), on connect and after new_world (kenshi-b0 + kenshi-0f, agreed 2026-11-01)

`u16 n`, then n x `{strid startoff, u8 ready}`. It lists every NEW_GAME_STARTOFF record of the merged data, in record order.
- **ready 1:** a NEW_GAME with that start-off would be placed now; its start zone has a navmesh.
- **ready 0:** NEW_GAME answers `not_ready` (MULTIPLAYER section 6, R3).
- The client greys the not-ready starts, so it never sends the destructive new_world (84) for a start that cannot begin.
- Sent once after the handshake, and again whenever readiness changes (for example after new_world, or after an on-demand bake).

**Extension: squad members for the character creator (approved by kenshi-0f, **implemented 2026-11-01** by the sim worker; asked by kenshi-0f for NEW_GAME n > 0).** Each entry grows to:

`{strid startoff, u8 ready, u8 k, k × strid race, u8 m, m × Member}`, with Member = `{strid race, u8 female, str name, u8 flags}`.

- **`k` races** are the start-off's `force race` list (RACE stringIDs, kind 1). k = 0 means any creator race.
- **`m` members** are the start-off's squad as the sim expands it (`CombatData::makeFighter` context, the same expansion NEW_GAME uses), in member order, leader first. **m is exactly the n a NEW_GAME Spec list must have** (`squad_size` otherwise), so the client never re-derives templates.
- **Random picks** (`choosefrom list`, ranges, a random race or sex) are drawn once per start-off from the server seed and the start-off's sid. That draw is deterministic: the same seed gives the same squad. STARTS shows exactly what NEW_GAME will build, and NEW_GAME uses the same draw. It only changes with a new server seed.
- **Member fields:** `race` and `female` are what the member will be. `name` is its default name.
- **`flags`:**
  - bit 0 = editable: the race is a creator race (a playable RACE, the creator's list);
  - 0 = an animal or other non-creator member;
  - other bits are 0, reserved.
- **A Spec for a non-editable member** sends `race` and `female` back unchanged, `head` / `hair` / `beard` / `hairColour` = 0, `skin` = 0, `sliderCount` = 0, `stance` = 0, and its name (renaming allowed, UNSURE). Otherwise the answer is a rejection `spec_mismatch`, and the sim ignores the look fields for it.
- **An editable member's Spec** is checked as NEW_GAME says. Its race must be in the `force race` list when k > 0.
- **Wire compatibility:** this changes the STARTS entry layout. Protocol stays 5, because the only decoder is kenshi-0f's, updated in lockstep. Old clients misread STARTS.

**Extension implemented (2026-11-01):** `Session::writeStarts`, `Simulation::startSquad` (`newgame.cpp`, the ONE expansion), `Simulation::startStatuses`; test `ksim_newgame`. As proposed, with these readings:
- **The draw.** `startSquad(start)` seeds `RngStream(streamSeed(streamSeed(seed, kStartSquadStream = 14), FNV-1a(startoff sid)))` and expands the `squad` refs (`startMembers`: template ranges, `choosefrom list` draws), then per member draws its own `makeFighter` rng state (`StartMember::rng`), its sex (`female chance` percent, made a sex the race has in the creator), and builds the fighter once to read its race and name. NEW_GAME builds each member from that same rng state, so the race, name and sex STARTS showed are what is built (n = 0 and an unchanged race exactly; a Spec that changes the race rolls its weapon and armour afresh). The squad depends on the server seed and sid only, not on the command's tick: **n = 0 starts changed** (a ranged template now draws at STARTS time, no longer at the command's tick; `squad_size` now compares with the drawn m, not the ranges' minimum).
- **`k` races** = the start-off's `force race` narrowed to the creator's races (`CreatorCatalog::allowedRaces`); 0 when the list is empty or leaves no creator race (the exe's fallback: every creator race). So The Slaves 6, Holy Nation Citizen 2, The Hive Exile 3, the rest 0.
- **`female`** for n = 0 members is also put on the character (`FighterLook::female`, CHAR_APPEARANCE `sex`); `name` is the CHARACTER record's name; `race` is 0 when the record cannot be built (such a start answers `unsupported`).
- **`flags` bit 0** = a creator race and not an animal. The `squad` expansion spawns only CHARACTER records, so in the vanilla + rebirth data every member is editable (no start has a non-editable one); the non-editable rule (`spec_mismatch`: race or female changed, or any look field, skin, slider or stance non-zero; renaming allowed and name-checked) is tested on `CreatorCatalog::check` with a synthetic slot. UNSURE: animals of a template (the Wandering Trader's pack animal, the dog) are not members yet, so they are not in m either.
- **Wire:** exactly the layout above, per entry `startoff, ready, k, k x race, m, m x {race, female, name, flags}`; k and m are u8. STARTS is resent when any of it changes (the list compare covers it). The member list is built for every start-off, ready or not.

**Extension: the outfit (kenshi-k16.5.4, client-approved, implemented 2026-11-12).** Member grows to `{strid race, u8 female, str name, u8 flags, strid character, u8 a, a × strid armour, strid weapon}`.
- **character:** the CHARACTER stringID the member was expanded from (kind 1).
- **armour:** the ARMOUR sids the member wears at NEW_GAME, in the sim's equip order; a = 0 = none.
- **weapon:** the WEAPON sid it carries; strid 0 = none (fists).
- **Same roll:** `Simulation::startSquad` builds each member once with `makeFighter` and its `StartMember.rng`, and records the Fighter's armour and weapon sids; NEW_GAME builds from that same rng state, so with n = 0 (or an unchanged race) STARTS and the spawn agree by construction. A Spec that changes an editable member's race rolls weapon and armour afresh, so the listed outfit is the default one (UNSURE whether the original refits per race; deferred: playtesting).
- Nothing else is carried (backpack, money). No protocol bump; lands with the client reader. Test: `ksim_newgame` compares every Nobodies member's STARTS character, armour and weapon with the spawned Fighter.

**Implemented (2026-11-01, kenshi-b0 sim worker).** Code: `Session::writeStarts` / `sendTick` (`cpp/src/ksim/simlink.cpp`), `Simulation::startStatuses` and `StartCatalog` (`cpp/src/ksim/newgame.cpp`, `cpp/include/ksim/newgame.hpp`). Test: `ksim_newgame` (frame 33 decoded by hand). Choices:
- **Wire (UNSURE):** exactly as agreed above, `u16 n` then the entries, with **no leading `u64 tick`** (the section 6 "all start with a tick" rule is not applied to this record; SAVES has one, STARTS does not; a client decoder reads `u16` first). `startoff` is a kind-1 strid of the record's stringID (STARTS interns it, so the client sends the same strid back in NEW_GAME).
- **Sent:** with the connect snapshot (every `sendTick(initial)`, so also after a load and after new_world, as the last frame of the write), and at a later tick whenever the navigation mesh's tile count changed and a start's flag with it. Not sent by a server without game data.
- **Catalog:** every NEW_GAME_STARTOFF of the merged game data in record order, minus a record a later file `REMOVED` (vanilla load order: 13; "The Hungover Ninja" is removed by rebirth.mod). Fields read, checked against the merged data: `money`, `force start pos` + `start pos X/Z`, `town` refs (resolved to the TOWN's first 0-townlist placement, `kworld::Towns::join`), `squad` refs, `faction relations` (v0 only), `research`, `force race`. No `town` ref that resolves: the exe's fallback (-43500, -6625).
- **ready:** at least one of the start-off's places has walkable ground on the navigation mesh within `MeshNavigator::kClickReach` (340) of it (the same snap a placement uses). With several places, NEW_GAME draws among the ready ones. No navigation mesh: nothing is ready. The server bakes only the Hub zone today, so:
  - **ready (4):** Wanderer, Nobodies (the Hub is one of its two towns), The Holy Sword, Son of a Captain.
  - **not ready (9):** Rock Bottom (forced position 66484, -90000), The Cannibal Hunters (Cannibal Village), The Wandering Trader (Trader's Edge), The Freedom Seekers (Waystation), Guy with a dog (Bast), The Slaves (Rebirth), Holy Nation Citizen (Bad Teeth, Stack), The Hive Exile (Hive Village), Empire Citizen (Sho-Battai). They become ready when their zone is baked (no on-demand bake yet).

### JOBS (type 34), to the owning session, on connect and on change

The original's per-character jobs list. Design: ARCHITECTURE "V1: the original's per-character jobs"; source: kinspect `findings/jobs17_static.md`; UI: `docs/UI.md` `jobs.*`.

`u64 tick, u16 n`, then n × `{u64 id, u8 enabled, u16 rows, rows × {u32 jobId, u16 type, u64 target, u8 active}}`.
- **Who gets it:** one entry per character of the session's player faction that has a list or whose list changed.
- **The connect snapshot** sends every character's list, including empty ones.
- **A later tick** sends only the characters whose list or `enabled` changed, each with its whole list. A character's entry replaces what the client had for it.
- `type`: the exe's TaskData number (jobs17 Appendix A). The client owns the row names: "Medic", "Operating machine", and so on.
- `target`: 0 when the type has none. Otherwise a character or interactable id, and the client draws `: <name>`, or nothing when it doesn't know the id (the exe's dead-target case).
- `jobId`: per character, never reused (saved).
- `active`: 1 when `enabled`, or when the type is one of the six that still run with JOBS off (16, 31, 45, 57, 58, 250; also 289, which counts as Staying close). The client draws inactive rows at half alpha (`Job::dim`).
- Row order is priority, top first.

**Implemented (2026-11-01, kenshi-b0 sim worker).** Code: `cpp/include/ksim/jobs.hpp`, `cpp/src/ksim/jobs.cpp` (the rules, the order checks, the executors), `Session::writeJobs` / `sendTick` (`cpp/src/ksim/simlink.cpp`). Test: `ksim_jobs` (frame 34 decoded by hand; the client has no type 34 decoder yet, so `ksim_simlink`'s loopback has nothing to compare with). Wire exactly as specified above (`u64 tick, u16 n`, entries as listed). Choices:
- **Who gets it:** every living `playerControlled` fighter (not the player faction's squad lists: the same set in the scenarios, and it needs no faction). All sessions of the server get it, as PLAYER_SQUADS does.
- **Snapshot:** every `sendTick(initial)` (so also after a load and a resend) sends all of them, empty lists included. A server with no player character sends no frame 34 at all (UNSURE: not an `n` = 0 frame).
- **A later tick:** a character is sent when its `(enabled, rows)` differs from what this session last sent. A character not yet sent whose list is the default (enabled, no rows) is not sent. A dead character is dropped from the session's memory and not sent again.
- **`active`:** `enabled` or the type is one of 16, 31, 45, 57, 58, 250, 289. All four executors are in that set, so the sim never ignores a row yet; the flag only greys rows that a later type will ignore.
- **Orders 76-78 and toggle 4** (section 7), answers as listed there, plus these choices:
  - **76 type check order:** `bad_value` for type 0 or above 300 (no TaskData); `unsupported` for a type with `kind` 0, one whose replacement has `kind` 0 (Sleeping, Sleeping on ground, Hiding), and one without an executor (everything but 4, 5, 61, 16, 25, 58, 31, 289, 45); then `bad_target`. For Bodyguard the companion's executor is checked too. A type that is not in the exe's table at all and below 301 is also `unsupported` (UNSURE: it cannot be told from a `kind` 0 type).
  - **`bad_target`:** a targeted type (31, 289, 45) needs a living character that is not the ordered one. A non-targeted type ignores `target` and stores 0.
  - **Dedup:** the stored (replacement) type decides: by type when it has no target, by type and target otherwise. A dropped duplicate answers `applied` and sets no `enabled`; only an added row turns JOBS on (the exe returns before `setChaseMode(1)` on a duplicate). A Bodyguard row that is a duplicate still adds a missing companion row (at the end).
  - **Companion of Bodyguard:** always 31 (UNSURE: the exe's `owner vf+0x248` test for 289 is not read).
  - **Fan-out:** every id is checked first; one bad id refuses the whole order, nothing is applied.
  - **77, 78:** the character must be a living player character (the section 11 id reasons first), then `bad_target` for a jobId that character does not hold.
  - **Toggle 4:** `value` 1 on, 0 off, any id, creates the list when it has none; a character with a list keeps its rows.
- **Executors** (`Simulation::stepJobs`, every 10 ticks, after `stepOrderQueues`): per character the first row it can plan, below its own fight (`target` on a living hostile) and above idle:
  - **Direct orders first:** a character with an interact task, a queued order, a door or carry step, a walk, drive or follow it did not start from a row, an attack order or a hold is left alone. The goal a row started is told from a player's by `state::Jobs::running` / `runType` / `runTarget`.
  - **Medic 58:** the nearest same-team living character in 600 units (UNSURE: ours) other than itself that `checkInteract` FirstAid allows, with a route (the three nearest are tried). Needs a carried kit with charges (or one opened); no kit, no plan. Starts `beginInteract` FirstAid, clientSeq 0, so event 13 for it names clientSeq 0.
  - **Attacking enemies 16:** the nearest living different-team character it is aware of (`perception::aware`), as an attack order (`FighterAttack`).
  - **Staying close 31 / 289:** `FighterFollow` of the row's target; skipped when the target is dead, itself, a loop, or already followed.
  - **Bodyguard 45:** an attack order on the nearest aware hostile whose `target` is the protectee, else the same follow (so with the companion row it is a follow plus defence). The companion row 31 follows too (met, skipped).
  - **Priority:** rows are tried top down; an unplannable row falls through. A running goal is replaced only by a row above its own, or by its own Bodyguard row changing from follow to attack. A removed row, or JOBS off under a row not in the six, drops its goal at the next replan.
  - **Not built:** the idle behaviours (ditch items, sleep when injured, sit when idle) do not exist in the sim, so JOBS off only gates the rows. Turret kind 2's separate pass waits for turrets.
- **Obedient Slave (k16.6.111, every slave since k16.6.116):** a character that is enslaved (the cuff 166, the processing 182) has its rows wiped and gets one row 187 (`enabled`; its target is the captor hand, k16.6.116, saved with the row); 187 has no executor, 76 for it answers `unsupported`, 77 / 78 work (removal only drops the row, UNSURE: kenshi-rwh.99). No new field.
- **State not on the wire:** `state::Jobs` also holds `running` / `runType` / `runTarget` (the goal the sim started from a row), saved so a load continues the same goal.

### PROSPECT (type 35), to every session when an order 79 task finishes (specified 2026-11-01, kenshi-b0; 0f's option (a): the sim owns the maths; implemented 2026-11-01)

Sources: kinspect `findings/prospecting.md` (summary and the live check) and `findings/prosp19_static.md` (static). UI: `docs/UI.md` `prospecting.*`. Checklist: `docs/FEATURES.md`.

`u64 tick, u64 id` (the prospector), then three blocks.

**Overview:**
- `f32 x, f32 z, f32 altitude`: the position, with the terrain height.
- `f32 science`: snapshotted at the end. The client prints `int`.
- `f32 r`: the half-width, `2000 + 6000 × science / 100`. Area Size = `int(0.2 r)`.
- `strid zone`: the biome group's name. `""` when the position is outside every group.
- `f32 wind`: the zone weather's raw wind. The client prints × 0.22369 mph.
- `u8 nEnv`, then `nEnv × {u8 class (1 Green, 2 Arid, 3 Swamp), f32 fraction}`, only the classes above 0. `nEnv` 0 means "Dead".

**Resources:**
- `u8 nRes = 5`, then `nRes × {u8 resId, f32 value}`.
- The order is Water 5, Fertility 6, Stone 2, Iron 1, Copper 3.
- `value` is the raw point sample at the position, not clamped. The client shows `int(v × 100)` and colours it by the raw float (0.5 / 0.75).

**Map:**
- `u16 N` = `int(6 + 2.46 × science)`. Pixel (c, row) samples `(x − r + (c + 0.5)·2r/N, z − r + (row + 0.5)·2r/N)`; row 0 is the lowest z.
- Then `N × N u8 grey`: the altitude background, 0 at ≤ 500 and 255 at 10000.
- Then 3 × `N × N u8`, the samples for Water, Fertility and Stone, each `round(clamp(v, 0, 1) × 255)`. A sample at or below 0.01 is sent as 0 and the client leaves the pixel at the background.
- The client paints the exe's colour rule (R = lerp(grey, 255, (1−v)v), G = lerp(grey, 255, v²), B = grey).
- Then `u16 nDep`, then `nDep × {u8 resId (1 Iron, 3 Copper), f32 x, f32 z, f32 richness}`. These are the deposits within 1.2 r, with their raw richness and no falloff. The client draws a 1 px dot, or 2×2 when N > 40.
- The size is at most 4 × 252² ≈ 254 KB, sent once per prospect.

The whole map uses the prospector's zone record, as the exe does. Nothing is saved, and there is no XP. The client keeps the latest PROSPECT; it is a singleton, so the latest one wins. The camera marker is the client's.

**Implemented (2026-11-01, kenshi-b0 sim worker).** Code: `cpp/include/ksim/prospect.hpp`, `cpp/src/ksim/prospect.cpp` (the maths, `Simulation::checkProspect` / `runProspect` / `stepProspect` / `prospectAt`), `Session::writeProspect` (`cpp/src/ksim/simlink.cpp`), `CommandType::Prospect`, `state::ProspectTask`, `World::depositSites`, `WeatherSystem::windOf`. Test: `ksim_prospect` (frame decoded by hand against values computed from the raw records and the heightmap; the client has no type 35 decoder yet). Wire exactly as specified above (counts: `nEnv` u8, `nRes` u8 = 5, `N` u16, `nDep` u16; the three planes are `Water, Fertility, Stone` in that order; the zone is a kind-2 strid, 0 for none). Choices:
- **Order 79:** the section 11 id reasons, then `unsupported` (no game data, heightmap or areas map), then `not_allowed` for: **indoors** (`Fighter::building != 0`, the building the character stands in, set by the door crossing), **carried** (`carriedBy`), **down** (`down` or `unconscious`), **in combat** (`target` is a living, conscious, not carried character of another team; the Jobs rule), **not on solid ground** (UNSURE: the sim has no ground type; the terrain height under the character is below 100, the navmesh's water level, so a swimmer or wader is refused. The exe tests the ground type == 0xb). Checked at the session and again when applied (a failure drops the order). It is a logged replacing order: it cancels what runs like a stop does (`cancelOrders`: interact, queue, door and carry steps, follow, attack, walk, drive) and leaves the character standing; it does not set `hold`.
- **The task:** `state::ProspectTask {ticks}` on the character, **20 ticks = 1.0 sim-second** (`prospect::kTicks`, whole ticks, so there is no float drift). It is saved (v40). The first tick counts on the tick the order applies, so a task ends 19 ticks after the apply tick. A replacing order (`cancelOrders`), a KO (also a `down` or a pick-up) and death remove it with no result and no event. A queued order waits for it; the jobs executors leave a character with one alone. No XP.
- **Who gets it:** every session of the server (the server has one player faction; JOBS and PLAYER_SQUADS go to every session the same way). Each session keeps only the latest finished task and sends it in the next `sendTick`; two prospectors that finish in one tick give one frame, the later one's, as the exe's singleton does. The frame is built once in the sim (`prospect::Result`) and shared by the sessions.
- **Science:** `FighterSkills::science` (new, STATS `science`, CharStats+0xe0; 1.0 for a character with no STATS record, UNSURE; saved, v40). It is clamped to 0..100 for `r` and `N` (a larger number only grows the frame); the `science` field is the raw number.
- **Zone:** the areas map (`AreaLookup`, the weather slice's lookup) gives the BIOME_GROUP `index` colour; the group is found among **all** BIOME_GROUP records (the weather model holds only the groups with seasons), and the field is its `name`. A colour that is no group's gives `""` and no environment record. **Grid:** the exe asks a 64 x 64 grid of 4608-unit cells (index `(coord + 147465) / 4608`) for a zone-info pointer; ours reads the areas map, 256 x 256 pixels of 1152 units (four per cell side), so a zone border inside a cell can differ from the exe's by up to one cell. The exe's 147465 against our 147456 is 9 units. UNSURE whether the exe's cell zone is the map's pixel at the cell's corner, centre or majority.
- **env:** the group's **first** `resources` entry, read from the merged data (`ENVIRONMENT_RESOURCES`). It confirms kinspect's live finding: The Great Desert (55621-rebirth.mod, `Desert` 44423-rebirth.mod: water alt 0-500 fade 200, min 0.1, mult 0.2; farming alt 0-800 fade 300, mult 0.75) gives Water 23 / Fertility 75 at altitude 570; the base file alone gives it nothing, rebirth.mod supplies the reference. 69 of the 74 groups have a record. The five without read 0 and "Dead": Rebirth, Wend and Berserker Country (on the areas map), Desert 44956-Newwworld.mod and Central 18002 (not on it). The test prints every group's record.
- **Water / Fertility:** `trap(alt) x mult + min`, not clamped, at the terrain height. `trap` is 0 below `altitude min - fade`, a linear ramp up to `min`, 1 on `[min, max]`, a ramp down to 0 at `max + fade`; a fade of 0 is a sharp edge (the exe's divide-by-zero is not copied).
- **Stone:** the exe's five-octave value noise (FUN_1406cc0e0), ported from kinspect `findings/stone26_static.md` (item 47; its Python was checked bit-exact against the exe on 1500 cases). Inputs are the raw world x and z at the prospector (and at each pixel centre), no seed, so it is the same in every world. Lattice: the Hugo Elias integer hash `n = x + 57z; n ^= n<<13; 1 - (((n*n*60493 + 0x0131071F)*n - 0x2DF722F3) & 0x7FFFFFFF) * 2^-30` (32-bit wraparound), smoothed by the 3x3 kernel (1/16, 1/8, 1/4), cosine interpolation with the literal 3.1415927. Five octaves, frequency x2 and amplitude x0.5, each sampled at `f*x/zoom, f*z/zoom` with `zoom = stone noise zoom x 1000` (a float multiply). `v = sum*0.5 + 0.5`; `v <= stone noise cutoff` gives 0, else `(v - cutoff)/(1 - cutoff)`; then x3.0 and clamped to 1.0 (the exe's globals shared with the grass painter, `kGrassBrightnessBoost` / `kGrassCap`; a FOLIAGE_LAYER can change them in the exe, not replayed), then x `stone mult`. The three fields come from the zone's ENVIRONMENT_RESOURCES record like the other prospect fields. The list value is that number, shown as `int(v x 100)` with no clamp (110 at mult 1.1). The Stone plane is `planeByte` of it like Water and Fertility, so the wire layout is unchanged; the exe paints a Stone pixel (v > 0.01, v clamped to 1) as `G = lerp(grey, 255, v^2)`, `R = lerp(grey, 255, (1-v)v)`, `B = grey`, which is the client's drawing from the plane and the grey plane. Test: `ksim_prospect` (12 + 12 worked values, 9 live points in The Great Desert, hash spot check).
- **Iron / Copper:** the deposits are the placed BUILDING instances with `building category` MINING and `is foliage` whose functionality's `world resource mining` is 1 (Iron Resource, 1,241 placements) or 3 (Copper Resource, 94), `World::depositSites` (the Stone Resource, mining 2, is noise, not a placement). The nodes have no geometry, so the node radius is 0 and the full-value radius is 25. The list value is the nearest placement of the kind within 1500, `richness x (1 - falloff)`. **UNSURE: richness.** The placements carry no number; the exe's `vf0x2e0` is not read. `depositRichness` is the zone record's `iron mult` / `copper mult` (kinspect's "M: they scale the deposit"), the prospector's zone record for every deposit on the map, as the exe uses one record for the whole map. The map dots carry that raw richness. A zone with no record has no dots (richness 0 would draw nothing). The Hub has no placement within 9,600 units.
- **Environment:** the classes come from the same record's floats `green`, `arid`, `swamp` (Green 1, Arid 2, Swamp 3, only those above 0). UNSURE: the exe's zone map (zone+0x60) may be built per cell from the terrain; the record's per-group numbers match the wiki's per-zone Arid/Green/Swamp table. No record gives no class ("Dead").
- **Altitude:** `Heightmap::terrainHeight` (the full-detail terrain, no -99 sentinel: the world is covered). **Map greys:** `round(min((alt - 500) / 9500, 1) x 255)`, 0 at or below 500 (UNSURE: round against the exe's truncation). Planes: `round(clamp(v, 0, 1) x 255)`, 0 at or below 0.01.
- **Wind:** the prospector's group region (`state::WeatherRegions`), `lerp(wind speed min, wind speed max, strength)` of the current WEATHER (WEATHER_CYCLE_LIVE.md section 5), raw units; 0 when the group has no weather (a group without seasons). UNSURE: the exe reads a float of the weather state that may also drift (`wind update time`).
- **Size:** at Science 100, 4 x 252^2 = 254,016 bytes of planes; one frame per finished task.
- **Not built:** the exe's per-tick progress is the frame dt, ours is whole ticks; the sound and the tutorial hook.

### UNLOCKS (type 36), to the owning session, on connect and on change (kenshi-b0, asked by kenshi-0f for the build window; implemented 2026-11-01)

Source: kinspect `findings/build_menu.md` §1 (and `build21_static.md`). UI: `docs/UI.md` `build.*`.

`u64 tick, u16 nResearch, strid research × nResearch, u16 nBuild, strid building × nBuild`.
- `research`: the player faction's completed RESEARCH records.
- `building`: the union of their `enable buildings`, deduplicated, in record order. The client lists exactly these, hiding a category or group with no member (the exe hides locked buildings; it never greys them).
- **Start set:**
  - a new game completes each research in the start-off's `research` list;
  - if that list is empty, it completes `_Default Start` (5359-gamedata.base);
  - so does a save with no completed research.
  - The Freedom Seekers come out as `_start- freedom seekers` + `_Low Start` from the data.
- The research is saved on the player faction (`PlayerFaction`).
- Sent with the connect snapshot (an empty list when the session has no player faction) and whenever the set changes. A research slice later adds completions; this record stays the same.
- Editor mode (list every building) is the client's own.

**Implemented (2026-11-01, kenshi-b0 sim worker).** Code: `Session::writeUnlocks` / `sendTick` (`cpp/src/ksim/simlink.cpp`), `Simulation::completedResearch` / `unlockedBuildings` / `stepUnlocks` and `startResearch` (`cpp/src/ksim/newgame.cpp`), `state::PlayerFaction::research` (save v39, RUNTIME_SAVE.md). Test: `ksim_newgame` (frame decoded by hand; the client has no type 36 decoder yet). Wire exactly as specified above (`u64 tick, u16 nResearch, strid x n, u16 nBuild, strid x n`; both strids kind 1, the records' stringIDs). Choices:
- **Who gets it:** every session of the server, like PLAYER_SQUADS (the server has one player faction; it picks the lowest-Id `PlayerFaction`). An empty-list frame is sent to a session when the faction does not exist.
- **When:** every `sendTick(initial)` (connect, a load, new_world, a resend), with empty lists when there is no player faction; later only when the completed-research list differs from what the session last sent (a new game, a scenario faction getting its set, a changed set). The building list derives from the research, so it is built only when a frame is sent. New game: the frame follows PLAYER_SQUADS in the same write. A different order of the same research counts as a change.
- **Start set:** `startResearch`: the start-off's `research` refs that are live RESEARCH records (unknown, wrong-type and REMOVED refs dropped, duplicates dropped, order kept); if none is left, `_Default Start`. `_Default Start` is found by its stringID `5359-gamedata.base` (`kDefaultStartResearch`), checked to be a live RESEARCH record; the test checks its name, so a data set that renumbers it gives an empty start set rather than a wrong one. Checked against the merged vanilla data: The Freedom Seekers = `_start- freedom seekers` + `_Low Start`; `_Default Start` enables exactly Small Shack, Small Research Bench, Stone Mine, Manual Stone Processor, Torch Post, Camp Bed, Campfire, Well, Vegetable Farm L / M / S. None of the four start-offs ready today (the Hub ones) lists research, so they all begin with `_Default Start`.
- **Empty set rule:** a player faction whose set is empty completes `_Default Start`: at `createPlayerFaction` (the scenarios), at the end of `Simulation::reload` (a load, so before the resend), and at every tick start (`stepUnlocks`, a state change like any other, so it is in the hash).
- **Buildings:** the union of the completed research's `enable buildings` refs, in BUILDING record order, REMOVED records skipped, a ref that is not a live BUILDING dropped.
- **Not built / UNSURE:** no completion after the start (no research bench, no cost, no time): the set only changes at a new game or a load; a research's own `prerequisites` are not checked (the start set is applied as listed); the exe's `improve buildings` / `upgrades to` play no part (build_menu.md section 1). Editor mode stays the client's.

### DISCOVERED (type 37), to the player faction's sessions, on connect and on change (kenshi-b0, asked by kenshi-0f for the world map; implemented 2026-11-01)

Source: kinspect `findings/map24_static.md` "Discovery" (`14092f6b0`). Client: `docs/CLIENT.md` world map.

`u64 tick, u16 n`, then n × `{strid town, u8 visited}`. `town` is a kind-1 strid of the TOWN record's stringID; `visited` is 0 or 1. It lists every place the player faction has discovered, in TOWN record order, as the whole list each time.

The rule is the exe's, run in the sim once per sim-second:
- The positions are all player characters plus each player session's view focus (`state::ViewFocus`, from set_view 51).
- **Discovered:** a place within 8500 of any position, 17000 for town type 2, 4250 for types 0 and 4 (camps, nests, ruins).
- **Visited:** a place within 400 horizontal. Visited implies discovered.
- Types 8 and 10 are never discovered.
- Both flags only ever turn on. They are saved on `PlayerFaction`.

A newly discovered place with type != 0 also sends **event 19 discovered** `{town strid}`, for the exe's "Discovered {1}" log line. The client fills in the placeholder name.

The client's share: the placeholder name and no tooltip for unvisited places, and camps (type 0) drawn only within 3258.4 of a player character. These are map drawing rules.

**Implemented** (`cpp/src/ksim/discovery.cpp`, `Session::writeDiscovered`; save v41, RUNTIME_SAVE.md; test `ksim_discovery`, plus a check in `ksim_newgame`). As specified, with these readings:
- **Check:** `Simulation::stepDiscovery`, after the commands, on ticks where the clock tick is a multiple of 20 (one sim-second), plus once at the end of a new game (the start position's first check, so the list is not empty for a second). Distances are horizontal and strict (`<`), as the exe's.
- **Positions:** every living `playerControlled` fighter plus the one `state::ViewFocus` (it is a world singleton; with several sessions it is the last set_view).
- **Faction:** the first (lowest Id) player faction, like UNLOCKS. `PlayerFaction::discovered` / `visited` are vectors of TOWN stringIDs in discovery order; the frame lists them in TOWN record order, built by `Simulation::discoveredPlaces`. The record is sent on connect (empty list included) and when the list differs from the last one sent; a visit alone changes it.
- **Event 19** `{town strid}` (kind-1 strid): the session sends it for a place that was not on its last list, not on connect, when `type != 0`. It is queued like the other events and is not dropped by the unknown-character filter.
- **Radius per type:** `Simulation::discoveryRadius` (17000 / 4250 / 8500; 0 for 8 and 10).
- **Several placements:** a TOWN with more than one placement is near when any placement is.
- **UNSURE: camps and nests (type 0).** The TOWN records of type 0 have no placement in the leveldata (nests are spawned by the wilderness spawner), so no type 0 place has a position and none is ever discovered. The radius, and the skip of event 19 for type 0, are in the code but no data reaches them. Placing nest TOWNs would need the spawner's nest positions.
- **UNSURE:** map items (`14075f550`) that reveal places are not built; the start-off's `town` refs (NEW_GAME) are not applied (the first check discovers what is around the start).

UNSURE: set_view is sent only when the focus zone changes (with hysteresis), so the camera term is as coarse as that. A finer camera position would need a new order; it's not proposed.

### FACTION_RELATIONS (type 38), to the player faction's sessions, on connect and on change (Implemented 2026-11-01, amended after kinspect 49; proposed by kenshi-b0, approved by kenshi-0f; asked by kenshi-0f for the FACTION page, docs/UI.md `faction.*`)

`u64 tick, u16 n`, then n x `{strid faction, str name, f32 value}`. Sources: kinspect `findings/faction29_static.md`, `findings/ui_captures/faction.md` (raw `faction_relations_live.txt`), all in ~/git/kenshi/kenshi-inspect.
- **`faction`:** a kind-1 strid of the FACTION record's stringID.
- **`name`:** its display name (the FACTION record's).
- **`value`:** THEIR relation toward the player faction (the other faction's `Relation` toward the player, else its `default relation`), not ours toward them (live: Dust Bandits -100 in ours, -11.02 in theirs). The raw float; the client truncates toward zero for the row (the exe's `(int)` cast) and may show the exact value in a tooltip.

**Which factions (kinspect 49):** the player faction's KNOWN set (`PlayerFaction::known`, saved, RUNTIME_SAVE v43), minus FACTION records with the bool `not real`, minus the player faction itself, minus `GENERATION_TEMPLATE_FACTION` by stringID. The rule is the one function `listsRelation` (`player_squads.cpp`). Rows are only ever added.
- **How a faction becomes known** (the exe's static is loose, UNSURE): (i) any `Relation` change involving the player faction, in either direction, adds the other (`noteRelationChange`); (ii) once per sim-second a living character of another faction within `perception::kBaseRange` (1500) of a player character adds its faction (the exe uses a distance threshold; the radius is ours); (iii) at a new game, and for a scenario's player faction, the set is seeded with every faction the player faction has a `Relation` pair toward (what the template's `relations` and the start-off's `faction relations` set explicitly; UNSURE). With only the template's relations a new game lists 1 faction (test `ksim_newgame` prints it); the live game had 52 known, 37 listed, grown by play.
**Order:** case-insensitive (ASCII) alphabetical by display name, ties shorter first. Not by value or record order.

**When:** the whole list each time. It is sent with the connect snapshot (an empty list when there is no player faction) and, checked once per sim-second (`tick % 20 == 0`, `Simulation::kFactionRelationsIntervalTicks`), when the set differs or any value differs by 0.5 or more from what this session last sent (each session keeps its own last send).

**Not on the wire:** the INFORMATION box text. It is built text in the exe (`---Active Contracts---`, then `---Upcoming Events---` with the campaigns targeting the player); the client fills it.

**Order 85 faction_rename:** `name str`. It sets `PlayerFaction::name`. PLAYER_SQUADS (`factionName`) and SAVES follow. Implemented as `SquadOrder` kind 85 (command type `SquadOrder` = 20; no new `CommandType`).
- It is a logged order of the owning session, like squad_rename (`OrderLogEntry` kind 85, `Command::squadOrder`). The name is saved state (`PlayerFaction::name`, already in RUNTIME_SAVE) and in the hash.
- Answers: event 9 on success; `bad_name`; `name_taken`; `not_allowed` with no player faction. A trailing byte is a malformed order (kick).
  - `name_taken` (kinspect 49 (e)): an exact, case-sensitive match with another FACTION record's display name (the player faction's own record excluded; the current name passes as a no-op, as in the exe). It is checked after `bad_name`.
  - `bad_name` uses NEW_GAME's rules (`checkFactionName`: 1-32 code points, valid UTF-8, no control characters, not only blanks; also a name over 4x32 bytes). **Deliberate deviation:** the exe has no length, trim or empty check; ours stays as wire safety.
  - Repeating the same rename is harmless: it sets the same value and PLAYER_SQUADS is not resent.
- Existing squad names (`<name>_N`) are not renamed (matches the exe, live). `squad_add` reads the current name, so the next squad is `<new name>_N`. UNSURE: kinspect 49 found no code that builds squad names from the faction name (new squads come from the "blank squad" template), while our `<factionName>_<N>` came from `docs/reference/FACTION_SQUADS_LIVE.md`; left as is.
- Test: `ksim_factionrel`. Cross references: FEATURES.md "Faction relations window", docs/UI.md `faction.*`, RUNTIME_SAVE.md v43.

### NEW_GAME (client -> server frame 111) and NEW_GAME_RESULT (type 62), order 84 new_world (kenshi-b0 + kenshi-0f, 2026-11-01)

The protocol is MULTIPLAYER.md section 4 (4a frames, 4b validation, 4c what the sim does); this is what the server built of it.

- **Order 84 new_world.** Admin session control like 80-83, never logged. Answer: event 9 (`appliedTick` = the fresh world's tick), the full resend in the same write (as a connect snapshot: no PLAYER_SQUADS, no characters, WEATHER, CLOCK, DETAIL_BOUNDS; the nav tiles only follow a player character), then STARTS last. `not_allowed` for a non-admin session (checked first), `unsupported` when the server has no fresh-world mark (`Simulation::markPristine`; `ksim_server` marks right after the world is built, before the scenario; not in `--dump-snapshot` runs), a body is malformed. Every other session gets the full resend at its next tick, like after a load. UNSURE: a NEW_GAME queued but not yet applied is dropped by the reset.
  - **The fresh world** is the world as `markPristine` saw it: the default world of the data and the server's seed (all FactionStates with their relations, towns, squads, wilderness, weather, the start clock 10:00 day 1), no player faction, none of the scenario's characters, items or storage. The Hub's mesh doors and containers are put back (they are the world's furniture; the scenarios place them too). `ServerContext::looks` is cleared. `Simulation::newWorld` = `reload` of the marked save. The same world every time (tested: same state hash).
- **Frame 111 NEW_GAME.** `u32 clientSeq, str name, strid startoff, u8 n, n x Spec` with Spec `strid race, u8 female, strid head, strid hair, strid beard, strid hairColour, f32 skin, u8 sliderCount, sliderCount x {str name, f32 value}, str name, i32 stance` (MULTIPLAYER 4a). **n = 0** = the start-off's squad as it is: the members of its `squad` refs (a SQUAD_TEMPLATE's `leader`, `squad`, `squad2` and `choosefrom list`, or a bare CHARACTER ref x v0), each built by `CombatData::makeFighter` from its CHARACTER record with its template as context. **n > 0 = character creation, Implemented** (below): n must equal the seeded squad's size, STARTS `m` (else `squad_size`) and each Spec replaces the look and name of the matching member, in member order, leader first. No AUTH: a client may send 111 right after HELLO (AUTH, JOIN, LEAVE, types 110, 112, 113, are not built and stay unexpected frame types).
  - **Optional advanced-options suffix (task BT, 2026-11-07; proposed with the UI's options page, kenshi-10, not built by any client yet).** After the Specs: `u8 nOptions, nOptions x {str key, f32 value}`; a frame without it is exactly the old one. Keys `cod` (0.5..4), `gdm` (0.5..4), `ht` (0.25..8) — `reference/ADVANCED_OPTIONS_STATIC.md`. Any other key, or a value outside its range, is a kick (`unknown new-game option 'x'` / `new-game option 'x' out of range`). An accepted frame saves them as `state::WorldOptions` (save v77) and applies them live.
- **Kicks (ERROR + close):** a frame-shape fault (truncated, an incomplete options block, trailing bytes after one), n above 16, sliderCount above 64, a string above 256 bytes, a non-finite float, an unknown or out-of-range option, a second NEW_GAME while one is pending (queued, not applied yet). It counts as one order against the rate limit.
- **Rejections** (type 62, result 1, nothing queued, nothing changed), checked in this order: `unsupported` (no combat arena, no navigation, no game data or no Nameless faction), `unknown_startoff` (the strid is not a start-off), `has_faction` (a player faction exists: V1 has one), `bad_name` (empty, over 32 code points, invalid UTF-8, a control character, only blanks), `squad_size`, `not_ready` (no ready place), then the Specs (the creator reasons below, the first faulty Spec decides). `spec_unsupported` is gone. `name_taken`, `server_full`, `no_faction` do not arise with one faction.
- **Applying:** an accepted frame becomes `Command::newGame` (`CommandType::NewGame` = 21, `text` = startoff sid + `\n` + name, `object` = the session's ticket; with Specs `\n` and the Specs encoded by `encodeSpecs` follow, length-prefixed, floats as their bits), stamped for the next tick and in `ServerContext::orderLog`, so replay and a save with it pending reproduce it. It is checked again when it applies (a failure sends the same type 62 rejection; `unsupported` also when a character record cannot be built, the whole start is refused and nothing changes). The randomness is `streamSeed(streamSeed(seed, kPlayerStartStream = 13), tick)`: the place among several ready ones, ranges and draws, the +-50 scatter, the stat rolls.
- **Reply 62 NEW_GAME_RESULT** to the asking session, in the write of the `sendTick` after the tick that applied it, before the snapshot that carries the members and PLAYER_SQUADS: `u8 result`; result 0 (ok): `u64 factionId, u16 n, n x u64 member` (living members, leader first; the shape of 4a's JOIN_RESULT), then appended `f32 x, f32 y, f32 z`, the leader's world position after placement (kenshi-0f, 2026-11-01: the members spawn at the start, outside the session's interest around the origin; the client moves its camera there and its set_interest follows; read only when present); result 1 (rejected): `strid reason` (kind 4, like event 10). (UNSURE: 4a's table put a reason strid and the ids in one fixed layout; this has one layout per result.) The session then controls the new faction: its members are `playerControlled` and in the player squad; PLAYER_SQUADS, CHAR_APPEARANCE and the rest follow through the normal stream. Their appearance faction is the Nameless FACTION (the session registers the look).
- **What the sim does** (4c): the FactionState of the FACTION record named "Nameless" gets `state::PlayerFaction` (`name` = the frame's, `money` = the start-off's, `startoff` = its sid, `research` = the start set, UNLOCKS 36; the save is v39, RUNTIME_SAVE.md); relations: the template's `relations` (spawnFactions already seeded them) plus the start-off's `faction relations`, both directions, value v0 clamped to -100..100; one player squad named `<name>_1` holds the members; each member has `Allegiance` = Nameless, `playerControlled`, team 0, placed at the start position (the Hub for the ready starts) scattered by +-50 and snapped to the mesh. **Not built (listed so it is not silent):** discovered towns (the start-off's `town` refs; the Discovery state exists now, DISCOVERED 37, but the refs are not applied), the `PlayerAreas` detail area, `force race` (character creation), "missing arm" and other `starting health` (Rock Bottom's Naked dude: LOCATIONAL_DAMAGE -150 on the Left Arm is not applied), animals of a squad template (the Wandering Trader's pack animal, the dog), `housemates` / `slaves` / `prisoners`, the wanted-criminal bounties (`bounty chance` / `bounty amount` on Holy Sword's Vagabond and the Freedom Seekers' Old Soldier are not applied to the player's characters), a re-roll of a memberless faction, a second faction.
- **Character creation (n > 0), Implemented 2026-11-01** (`cpp/src/ksim/creator.cpp`, `Simulation::checkNewGame` / `runNewGame`, `Session::handleNewGame`; save v42, RUNTIME_SAVE.md). Cross references: MULTIPLAYER.md 4a / 4b, FEATURES.md "New game: character editor".
  - **The creator's data** (`CreatorCatalog`, built from the merged data like the client's `Characters::choices` / `raceGroups`, kinspect `charedit.md`, `charedit8b_static.md`, `charsel23_static.md`): the creator's races are each RACE_GROUP's races with RACE `playable`, a body mesh and an `editor limits` file (vanilla + rebirth: Skeleton, Shek, Greenlander, Scorchlander, Hive Prince, Hive Soldier Drone, Hive Worker Drone). Per race: `heads male` / `heads female` (HEAD sids), `hairs` split by `attach slot` (2 hair, 13 beard) minus the race's empty records (Bald, No beard: the picker's none, strid 0), `hair colors`, and per sex the editor sliders (`<Range>` of `data/editor/editor_data_*.xml`: name, `<Category>`, min, max). A sex exists when its editor Config has sliders (the client's `bothSexes`). The Idle stance slider has 8 entries (the idle ANIMATION records the editor lists). The start-off's `force race` narrows the races; a list that leaves none allows every creator race (the exe's "Game start has no valid races" fallback), and the list applies to every member (the exe has no per-member lock).
  - **Strids.** A client can only name a record by the server's strid, so the STARTS write also puts every creator sid (races, heads, hairs, beards, hair colours; about 100) in the string table (kind 1, ordinary STRTAB_APPEND, no new frame). Spec strid 0 is none; a strid the table does not hold as a kind-1 sid names no record.
  - **What each Spec field maps to:** `race` -> the member is built by `makeFighter` with that race (its stats, parts, hunger: the race pick of the original), `Fighter::raceSid`; `female` -> `FighterLook::female` (CHAR_APPEARANCE `sex`); `head` -> the index of the HEAD sid in the race's list for that sex (`headIndex`); `hair`, `beard`, `hairColour` -> the sids (CHAR_APPEARANCE `hair`, `beard`, `hairColour`; none = strid 0); `skin` -> `skinTone` (0..1; the creator shows 0..100); `sliders` -> the `body` list as sent, in the order sent, `{strid key (kind 4), f32 value}`; `name` -> `Fighter::name` (CHAR_APPEARANCE `name`); `stance` -> the `body` list's last entry, key `Idle stance`, value = the stance number 1..8 (the creator's slider value; the client maps it to its stance list). The start-off's name for the faction is the frame's `name`, as for n = 0. The template keeps everything else (stats record, weapon, armour, squad context).
  - **Reasons** (type 62 result 1, kind-4 strid; the first faulty Spec decides; within a Spec this order):

    | Reason | When |
    |---|---|
    | `spec_mismatch` | (checked first, per Spec) a non-editable member (STARTS flags bit 0 clear) whose Spec does not echo its race and female with an empty look, skin 0, no sliders, stance 0 |
    | `unknown_race` | the strid is 0, not in the table, or not a RACE record |
    | `race_not_playable` | a RACE that is not in the creator's list (not `playable`, no mesh, no editor file or no group) |
    | `race_forced` | in the creator's list but not in the start-off's `force race` list |
    | `sex_not_available` | `female` above 1, or the race has no editor Config for that sex (a one-sex race) |
    | `bad_head` | not in the race's `heads male` / `heads female` list for that sex (strid 0 only when that list is empty) |
    | `bad_hair` | not in the race's hair list (strid 0 = none is fine; the race's empty Bald record is not accepted, the creator sends none) |
    | `bad_beard` | not in the race's beard list, or any beard on a female (the creator shows no beard picker for one) |
    | `bad_hair_colour` | not in the race's `hair colors` (strid 0 only when the race has none) |
    | `bad_slider` | a name that is not one of the race's editor sliders for that sex, or a name twice |
    | `slider_range` | a slider value outside [min, max] of its range, `skin` outside 0..1, or `stance` outside 1..8 |
    | `empty_name` | the character's name is empty or only blanks |
    | `bad_character_name` | invalid UTF-8, a control character, or over 32 code points |

    Sliders the Spec leaves out are at their mids (the creator's defaults). **Clamp or reject: reject** (`slider_range`; MULTIPLAYER 4b). The original's editor clamps a typed value, and so does the client's creator before it sends, so a valid client never trips it. `squad_size` and `not_ready` come before the Specs are read, so a Spec fault is never reported for a start that cannot begin; the `force race` starts of the vanilla + rebirth data are not ready on the Hub-only navmesh, so `race_forced` is only reachable once their zones have a mesh (tested on `CreatorCatalog::check`).
  - **Saved:** `state::FighterLook` on the Fighter (`custom`, `female`, `headIndex`, `hair`, `beard`, `hairColour`, `skin`, `sliders`, `stance`), save v42. The name and race were already saved. A character without a creator look (`custom` false: every n = 0 and scenario character) keeps the scenario look of `ServerContext::looks`.
  - **UNSURE / deviations:** (1) the Idle stance rides in the `body` list as the key `Idle stance`, because CHAR_APPEARANCE has no stance field; it is the slider's number, not the ANIMATION name. A dedicated field would be a wire change, not made. (2) The race's stats are applied through the race record (`makeFighter` with it: strength and dexterity multipliers, parts, hunger); the template's STATS record (skills) is kept. UNSURE: whether the original's race pick also re-rolls skills (not read). (3) The editor XML is found by its file name under `<data>/editor/` (mods with their own editor files elsewhere are not found; their races get no sliders and are unusable). (4) The Hive races have both sexes in the data (29 / 44 sliders), so no vanilla race is one-sex; `sex_not_available` for a one-sex race is tested on a synthetic catalog. (5) A female may have no beard; a male may. Hair for a race with no hairs (Shek, Skeleton, Hive) must be none. (6) The preview's clothes toggle, import / export and RANDOMISE are client-side only. (7) A character's name is not checked for uniqueness. (8) Posture, Shoulder set and Neck position are ordinary editor Ranges here (the client keeps them in Spec fields, but they are named sliders on the wire).
- **Test:** `ksim_newgame` (it decodes CHAR_APPEARANCE through the client's `SimLinkDecoder`). Cross references: MULTIPLAYER.md section 4 ("Implemented" marks), FEATURES.md "New game".

### RESEARCH_STATE (type 63), to every session, with UNLOCKS (approved by the client lead on kenshi-h2v.17.13, kenshi-h2v.17.15; implemented 2026-11-12)

The player faction's research page: what each RESEARCH record is, and what is unlocked per kind (`Simulation::researchState()`, CRAFTING_SLICE 9.8). UNLOCKS 36 is unchanged.

`u64 tick, u8 techLevel, u16 nRecords`, then per RESEARCH record in record order `{strid research, u8 state, u8 reason}`, then `u8 nKinds`, then per kind `{u8 kind, u16 n, strid ref x n}`.
- **state:** 0 completed, 1 listable (on the research page), 2 locked.
- **reason:** 0 none, 1 requirement (an unfinished requirement is not itself directly researchable), 2 tech level, 3 blueprint only, 4 nothing left to unlock: a record silently finished because everything it enables is already unlocked (CRAFTING_SLICE 9.2). It is sent as state 0 (completed) with reason 4, so a client can tell it from a researched one. Reason is 0 for a completed record that was researched and for a listable one.
- **kind:** 1 item, 2 weapon type, 3 weapon model, 4 armour, 5 crossbow, 6 backpack, 7 robotics, 8 building; each list is the refs the completed records enable (the unlocked set), in unlock order. Every kind is sent, an empty one with n = 0.
- **techLevel:** 1, or the highest finished `is level upgrade` record's `level + 1`.
- **When:** exactly when UNLOCKS is sent: the connect snapshot (also a load, new_world, a resend) and whenever the completed research changes. With no player faction: techLevel 1 and empty lists.
- **Trailing bytes:** a decoder ignores them. Later steps (bench level, startable, the research queue and progress) append fields without a new frame; the fixed part above never changes.
- Test: `ksim_research_wire`.

### TASK_PROGRESS (type 39), to the player faction's sessions, on connect and on change (Implemented 2026-11-01: kinds 1 lockpick and 4 prospect; 2026-11-11: a tail and more kinds, below; proposed by kenshi-b0, approved by kenshi-0f; asked by kenshi-0f for the floating progress bars)

Sources: kinspect `findings/ui28_static.md` part A and `findings/ui_captures/progbar.md` (item 21).

`u64 tick, u16 n`, then n × `{u64 char, u8 kind, f32 progress, f32 rate, f32 value}`. It is the whole list each time: every player character with a timed task that has a bar.

- **`kind`** gives the caption:
  - 1 lockpick: "Lockpicking", then "Lockpicking (Chance {1}%)";
  - 2 unlock: "Unlocking";
  - 3 lock: "Locking";
  - 4 prospect: "Prospecting";
  - 5 add materials: "Adding materials";
  - 6 train: "Training";
  - 7 cut: "Cutting", then "Cutting: {1,num}";
  - 8 restrain: "Restraining";
  - 9 operate (a producer, only with its functionality's progress-bar flag; Implemented, see INTERACTABLE kind 4).
  - The client owns the strings. Construction, repair and dismantle have no bar in the exe, so they get no kind.
- **`progress`** is 0..1 at `tick`. **`rate`** is progress per sim-second, so the client advances the bar between frames, with 0 for a stalled task. The fill is clamp(progress + rate × elapsed, 0, 1).
  - For a lockpick, progress is the elapsed fraction of the current attempt's interval, and it resets to 0 after each roll (the exe's bar). A container's lock reports it too (task AL): the timer is the box's, so an interrupted pick resumes.
  - For a prospect it is ticks done / 20.
  - For a cut (kind 7) see the kind list below: a cage's lock and a worn pair of shackles (order 60 action 16, kenshi-h2v.15.1) share the rule. A pick of worn shackles is kind 1 (action 15) with the same rule as a door's; their rows carry target = the wearer and flags bit 0.
- **`value`:**
  - lockpick: the chance, 0..0.9; the client shows `int(chance × 1000) × 0.1` with one decimal.
  - cut: the `{1,num}` number.
  - Otherwise 0.
- **When:** the list is sent on connect, with an empty list when there is no task or no player faction. After that it is sent only when it changes: a task starts or ends (including an interruption, after which the bar is gone), a lockpick roll resets progress, or rate or value changes. A character missing from a later list has no task any more. When the last task ends, the frame is sent with n = 0. Progress between changes is the client's extrapolation in sim time (CLOCK), so there is no periodic resend. UNSURE: a resend at most once per sim-second if the drift matters.
- **First users:** lockpick (interact action 3; its sim timer exists) and prospect (order 79). Unlock, lock and the rest come with their task timers. Today unlock and lock are instant in the sim, and train, cut and restrain are not built.

**Tail, kinds and levels (Implemented 2026-11-11, kenshi-h2v.16.1; proposed by the sim worker, approved by the client lead).** Additive, no protocol bump: after the n rows come `u8 flags[n]` and then `u64 target[n]`, in row order (not lists: no counts).
- **`flags`:** bit 0 = the original shows this bar; bit 1 = the character is the player faction's; bits 2-7 are 0 (reserved).
- **`target`:** the id of what is worked on: the door, container, building, producer, the body being restrained or processed, the patient, or the cage building. 0 when there is none (prospect).
- **Kinds sent now** (bit 0 in brackets):
  - 1 lockpick [1]: a door, a container, or a cage's lock (target = the cage key; the player's order 60 action 18, kenshi-k16.5.7), or a worn pair of shackles (order 60 action 15; target = the wearer).
  - 2 unlock [1] and 3 lock [1]: a door's `DoorStep` phase 0 / 6 over its 3.33 s (the lock's time is OURS, UNSURE).
  - 4 prospect [1].
  - 5 adding materials [1]: the 0..1 warm-up before a unit goes in (rate 0.2 per sim-second).
  - 7 cutting [1]: a cage's lock (target = the cage key; order 60 action 19) or a worn pair of shackles. **progress = 1 - hardness / start hardness, value = the remaining hardness, rate = cutRate(strength) / start.** The worn-shackle cut (order 60 action 16, h2v.15.1) is the same row with start hardness 30 (`kShackleHardness`, OURS) and target = the wearer; `Simulation::lockProgress` fills both.
  - 8 restraining [1]: a capture chain's step, `chainElapsed` / 12 s.
  - 9 operate [1]: a producer whose functionality has `has progress bar when used`.
  - 10 produce [0]: any other producer, with the producer's progress and rate (the same numbers as INTERACTABLE kind 4).
  - 11 building [0], 12 repair [0], 13 dismantling [0]: progress = hp / max hp (13: 1 - that), rate 0 (the rate is not constant), so a row is resent when its progress moves 1%, and only while the builder stands at the site.
  - 14 processing a new slave [0] (goal 182, 3 s).
  - 16 first aid [0]: the patient's bandage pools over the room they can take. The rate is the kit's, 0 while wounds are clotting.
  - 6 training and 15 crafting are never sent (training has no timer in the sim, benches are not built). Eating, crop work, chopping and smashing are not sent.
- **Who gets rows** is the session's level (order 98, `u8`, per session, default 1, reset to 1 on SCENARIO_STARTED and NEW_GAME, which the client answers by sending 98 again):
  - 0 = none (always n = 0);
  - 1 = rows with both flag bits (the original's bars of the player's characters: exactly what was sent before the tail);
  - 2 = every row of the player's characters;
  - 3 = plus the NPCs in the client's view (flags bit 1 clear).
  - A change of level sends the list on the next tick.
- **NPC pacing:** a change that is only in NPC rows is sent at most once per sim-second (20 ticks), and the frame is still the whole list; a change in a player's row is sent at once and carries everything.
- **Automatic producers** (`maxOperators` 0) have progress and rate on INTERACTABLE kind 4 but the rate is 0: the sim has no power model, so they never advance. Not part of this item.

**Implemented (2026-11-01), as built:**
- Built: kinds 1 lockpick and 4 prospect. Kinds 2, 3 and 5-8 stay defined above but nothing sends them (unlock and lock are instant in the sim; train, cut, restrain and add materials are not built). Code `Simulation::taskProgress()` (`cpp/src/ksim/interact.cpp`), the session's change test in `Session::sendTick`, test `ksim_taskprogress`.
- Lockpick: a living player character whose `DoorStep` is in the pick phase on a still locked door. progress = the lock's `lockElapsed / lockInterval`, rate = 1 / `lockInterval`, value = `pickChance(lockLevel, lockpicking)`, the function the roll uses. The timer is the door's (saved, shared between pickers, kept when a pick is interrupted), so a resumed pick starts with its old progress, and two pickers of one door show the same bar. Prospect: `ProspectTask` ticks / 20, rate 1 / (20 x 0.05 s) = 1.0, value 0.
- The list is sorted by character id. A KO'd or dead character has no row.
- Change test (per session, against the last frame it sent): the row count, id or kind differ, rate or value differ by more than 1e-6, or the progress differs from the client's extrapolation (last progress + last rate x elapsed sim-seconds) by more than 0.001. The last term makes a roll (progress back to 0) a change even when the new interval equals the old one. Nothing is sent otherwise: no periodic resend.
- On connect (and after a load, which resets the session's memory) the list is sent, n = 0 when nothing runs or there is no player character.
- Event 20 comes from the sim's interact observer at each roll (`InteractEvent::Kind::LockRoll`, result 1 = unlocked); the session passes it on only when `char` is a player character. It is exempt from the "character unknown to the client" filter (like 11 and 19), since the character may be out of the client's bounds. It arrives in the same tick as the reset frame 39.
- UNSURE: the walk to the door has no bar (the exe's bar belongs to the task; believed right). Two pickers on one door both tick the lock (section 13 "Lockpicking"), so the bar then advances twice as fast for both. After the successful last roll the row is gone (the door is opened next, with no bar).
- Save: nothing new. `lockElapsed`, `lockInterval`, `lockAttempts` on the door, `DoorStep` and `ProspectTask` were already saved; `ksim_taskprogress` saves mid-attempt, loads, checks the connect frame and continues to the same state hash.

**Event 20 lock_roll** `{u64 char, u64 door, u8 success}` follows every lockpick roll. It goes to the player faction's sessions when `char` is a player character. The client uses it for the floating "Pick success!" (green) / "Pick failed" (red) text, 15 units above the character.

### TRADE (type 53), to the trading session, on open and on change (**Implemented 2026-11-02**, branch `trade`; MINE_SELL_SLICE 3.2; approved by the client 2026-11-02; test `ksim_trade`)

`u64 tick, u64 trader, u64 actor, u64 stock, strid template, i64 traderMoney, u32 stockVersion, u16 n, n x {u16 index, i32 buy, i32 sell}, u32 actorVersion, u16 m, m x {u16 index, i32 sell}, u16 k, k x {u64 container, u32 version, u16 m, m x {u16 index, i32 sell}}, u16 b, b x {u16 index, u32 count, i32 price}`. (43 is NAV_PARAMS.)

- `trader` is the character talked to, `actor` the player character, `stock` the trader squad's Id: the stock's container id for orders 68, and the id its INVENTORY / ITEM_QUALITY / ITEM_GRID records go under. `template` is the squad template (a title). `traderMoney` is `state::TraderStock::money`.
- `n` prices every INVENTORY entry of the stock (the wire's flat order: `items`, then the worn backpack's): `buy` what the player pays for one unit, `sell` what the trader pays for one (equal for a `trade item`). An entry with no price (`value` 0: a blueprint or a map) has `buy` 0 and `sell` -1.
- `m` prices every INVENTORY entry of the actor: what the trader pays for one unit, -1 when he will not buy it (no price, a smuggler and a legal item, or the entry is equipped).
- **Chest prices (tail, always present; 2026-11-02, MQ12; approved by the client):** `k` = every container the actor's player faction owns within 1000 u of the trader: exactly the set order 68's `source` accepts (`Simulation::tradeSourceFault`, shared), sorted by container id; `k` = 0 when none. `container` is the id order 68's `source` takes; `version` is that container's current INVENTORY version; its `m` prices every INVENTORY entry of the container in the wire's flat order, same rules as the actor block minus the "equipped" case (what the trader pays for one unit, -1 when he will not buy it). Size of the tail: 2 + k x (14 + 6m). Client notes: (1) an entry is priced only while its `version` equals that container's current INVENTORY version; a mismatch means the price is stale and the client shows none until the resend. (2) A container in the set outside the client's interest still has its prices sent but no INVENTORY; the client ignores that block (no sim change).
- **Buyback tail (after the chest block; approved by the client 2026-11-12, kenshi-k16.20.6; implemented kenshi-k16.20.16, test `ksim_trade`):** `b` = the stock INVENTORY entries (index in the same flat order as `n`) that hold units this faction sold to this trader in this trade window; `count` how many of the entry's units are buyback (a stack merged with the trader's own units says how many), `price` what one costs. A buy by a character of that faction charges `price` for the first `count` units, then `buy`; the `n` block's `buy` stays the normal price. Only the selling player faction's own session gets entries: every other faction's TRADE has `b` = 0 and pays `buy`. The list is the sim's never-saved runtime, keyed (selling faction, trader squad) and bound to one window: it clears when the window closes (order 86, the trader leaving, event 40 for any reason, a load or `new_world`). A client that does not read the tail skips it by the frame length. Open: the same item sold twice at different prices in one window lists the latest price (UNSURE).
- Sent when the trade opens (the talk arrives) and again when either inventory's version, the trader's money or any listed container's version changes, or when a container enters or leaves the set. The player's money is PLAYER_SQUADS' trailing `i64 money` (it changes in the same tick).
- While the trade is open the stock is an INVENTORY + ITEM_QUALITY + ITEM_GRID under `stock`. When the trade closes the feed simply stops: no event 11 for the squad id; the client drops it on event 40.
- A trade is session state: the sim keeps only the stock (the squad's Inventory) and the two wallets. After a load or `new_world` every open trade is closed (event 40 reason 2).

### CLOCK (type 40)

`u64 tick, u32 day, f32 hourOfDay, f32 gameSpeed, u8 paused`. `paused` is a bit field (2026-11-04): bit 0 the idle or admin pause (the old 1), bit 1 the dialogue pause (a reply menu is open, DIALOGUE.md 13.2); a change sends CLOCK at once.

### WEATHER (type 41), 1 Hz and on change

`u64 tick, u16 n`, then n x `{strid biomeGroup, strid weather, strid previous, f32 fogBlend, f32 skyBlend, f32 strength}`
(agreed with kenshi-0f 2026-11-01, client fa18261; protocol stays 5; kinspect 37, `reference/WEATHER_CYCLE_LIVE.md`).

- One entry per BIOME_GROUP that has a weather region. It replaces `representativeWeather()` in the client.
- `previous` is the weather before the current one (0 = none, e.g. right after a new game).
- `fogBlend` goes 0 to 1 over the first 2.5 % of the current weather's lifetime, in game minutes.
- `skyBlend` goes 0 to 1 over 30 s of game-speed time.
- The server computes both blends, so pause and speed are already applied.
- `strength` is the instance's strength, 0 to 1, constant for its life.
- Sent once a second, and at once when any group picks a new weather. The camera-position blend across groups is the client's.
- **Wind (optional tail, 2026-11-06; client-approved 2026-11-06):** after the region list, `f32 windSpeed, f32 windYaw` — the wind of the group at the session's own camera, the last order 51 `set_view` this session sent (session state, so it survives a load or a new world; the world's `state::ViewFocus` is re-applied from it after a reload for residents and discovery), i.e. the group `WeatherSystem::groupAt` returns for that point. Absent when the session has sent no view or the point lies in no BIOME_GROUP (a client skips by the frame length; the pair is all-or-nothing). `windSpeed` is the raw `WeatherSystem::windOf` value (the same value as the PROSPECT frame's `wind`; the prospecting window prints it x 0.22369 as mph). `windYaw` is the direction the wind blows TOWARD, in radians [0, 2 pi), on the character yaw convention (CHAR_DYNAMIC): toward `(sin yaw, cos yaw)` in world XZ, 0 = +Z. Both are per weather instance (the speed from its strength, the direction from a seeded draw keyed on the saved instance), so they change only when the weather does. OURS/UNSURE: the exe keeps a vec3 on the instance (kinspect `findings/weather_effects.md` 4) but its pick is not traced.
- **Implemented (2026-11-01):** `Session::writeWeather` (`cpp/src/ksim/simlink.cpp`), regions in BIOME_GROUP record order, an empty string is strid 0 (`previous` right after a start; `weather` only if a season had nothing pickable). `fogBlend` and `skyBlend` are 1 when there is no weather. The immediate resend compares `WeatherSystem::picks()` (a metric, not saved) with the last frame. A server without game data (a bare `Simulation::load`) sends no WEATHER frame. Test: `ksim_simlink` decodes it with the client decoder and compares it with the sim's regions. Rules and the region model: `reference/WEATHER_CYCLE_LIVE.md` "Implemented". The wind tail is implemented in the same writer from the session's camera; `ksim_simlink` sets a view focus and matches the pair against the camera group's instance, and checks it still rides after a new game and after order 84 `new_world` without a second `set_view` (`ksim_weather` covers stability, save/load and change).

### BEAMS (type 58), to the sessions in range, on change (approved 2026-11-11 by the client lead; implemented, kenshi-h2v.14.1)

The wandering weather effects (EFFECT type 9; Venge's sky beams are the only live ones), planned by the sim a few seconds before they strike. Rules and findings: `reference/VENGE_BEAMS_STATIC.md`. Additive: a new type, no protocol bump.

`u64 tick, u16 nGroups`, then per group `{strid group, u8 flags, u16 n}` and n beams:

`{u32 id, strid effect, u64 spawnTick, u64 endTick, f32 x, f32 z, f32 speed, f32 radius, f32 strength, strid particles, strid sound, u16 nSeg, nSeg x {u16 ticks, f32 yaw}}`

- **`flags` bit 0:** forget every beam of this group the client holds before applying the list (a weather change, a pin, a load, the connect snapshot). Other bits 0.
- **`id`:** unique inside one weather instance of one group (`slot << 16 | k`); a reset (bit 0) starts a new id space.
- **`effect`, `particles`, `sound`:** the EFFECT record's sid, its `particle system` and its `sound` (strid 0 = none). The client owns the look.
- **`radius`:** the burn radius, u (200). **`strength`:** the core strength (0.75). The damage falls off linearly in distance squared from `radius` to `radius x sqrt(2)`; that is the sim's business and is not on the wire.

**The path, exactly (the client draws the sim's own path):**
- The beam exists for sim ticks `spawnTick <= t < endTick`. Its ground point at `spawnTick` is `(x, z)`; y is the terrain height under it (the client's snap).
- `speed` is in u per sim-second. One tick is 1/20 s, so a tick moves `speed / 20` u.
- The segments apply in order. Segment i covers `ticks` consecutive ticks and moves the point `speed / 20` per tick along the direction `(sin yaw, cos yaw)` in world XZ: yaw 0 = +Z, positive yaw turns toward +X (the character yaw convention, like WEATHER's wind yaw). Position at tick `spawnTick + n` is `(x, z)` plus the sum of the first n ticks' steps. Between ticks the client interpolates in time.
- The segments cover the beam's whole life: the sum of `ticks` is exactly `endTick - spawnTick`. There is no "after the last segment": at `endTick` the beam is gone. A beam that the weather's end cuts short has its last segment cut, so the sum still matches.
- The burn damage uses this same function: the sim computes the beam's position at each tick from the same `(x, z, speed, segments)` it sends (float values, rounded before use), so the drawn beam and its damage zone agree.
- The sim ticks are the clock (CLOCK frame): a paused server freezes the beams, their planning and their damage like everything else (intended).

**When, and for whom:**
- A beam is sent when `spawnTick - now` is 3 s (60 ticks) or less: the lead (a server constant) before it strikes. So a beam that is already in range goes out exactly 60 ticks before it spawns. A weather change at a group sends flags bit 0 with the beams alive or inside the lead.
- A session gets a beam when any point of its path lies within **R = 40000 u** of one of the session's reference points: its camera (order 51 `set_view`) and each of its player characters. **A beam planned while out of range of the session goes out when the camera, or one of its player characters, later comes within R of its path: on the first tick after, once (the lead is then less than 3 s if it spawns sooner, and a beam already alive is sent alive, so the client must accept a `spawnTick` in the past).** A beam is sent once per session and dropped from the session's set at its end; a replan (flags bit 0) starts the group's set afresh. R is a server constant, agreed with the client lead (the strip is 16 000 u tall).
- The connect snapshot (and the one after a load or a new world) carries every group with bit 0 set (n = 0 for a group with no beams, so the client forgets stale ones), and the beams of each that are in range and alive or inside the lead.
- Only the burning wandering effects (EFFECT type 9, `affect type` 3) are sent; the poison gas clouds are not (`reference/VENGE_BEAMS_STATIC.md` TQ1). The frame is omitted on ticks with nothing to say.
- Test: `ksim_venge_beams` decodes the frame by hand, checks the 3 s lead, once per session, the out-of-range-then-in-range send, the reset on a replan and that the wire is the sim's beam.

### DETAIL_BOUNDS (type 42), on change

`u64 tick, u16 n`, then n x `{f32 minX, minZ, maxX, maxZ}`: the full-detail area as boxes (the view
zones and the character bubbles; they may overlap). Sent after the hello and whenever the set
changes.

### EVENT (type 50), the reliable event stream

`u64 tick, u16 kind`, then a kind-specific body:

| Kind | Body |
|---|---|
| 1 hit | victim u64, attacker u64, part strid, pos f32 x3, dir u8 (hit direction in the x-z plane: atan2(z, x) of the attacker-to-victim vector, 256 steps per turn; ground blood orientation), damage f32 x4 (cut, blunt, pierce, stun), result u8 (0 hit, 1 blocked, 2 dodged). **The damage values are final:** after armour (`applyArmour`), the toughness factor and `damageScale`, so they are what the part took (`medicalHit` in combat.cpp). **Tail (approved 2026-11-01 by kenshi-0f, 4adf8c9; implemented 5a27b7d):** `f32 cutAbsorbed, f32 bluntAbsorbed` appended after `result` (additive; a decoder reads them only when present). They are what `applyArmour` removed in `medicalHit`: the pre-armour value minus the post-armour one, before the toughness factor and `damageScale`; the pierce absorption is merged into cut. A blocked or dodged event 1 sends 0, 0. The event is 68 bytes with the tail (60 before).
- These are what the armour layers removed: the pre-armour value minus the post-armour one, before toughness. Pierce absorption is merged into cut, as the exe does when > 1.
- The exe's floater (kinspect `findings/ui_captures/floating_text.md`): option `floaters`, default Simple, shows "37 (-12)" = the int damage after armour plus "(-M)" absorbed when > 0. Full mode shows "Left Arm: 14 (-6) + 9 (-3)". It appears for every visible hit within 4000 of the camera, NPC vs NPC included.
- UNSURE: whether the exe's shown number includes the toughness factor. |
| 2 bleed drip | pos f32 x3, amount f32. **Redefined 2026-11-01 (kinspect bleed_pool.md, branch 3):** `amount` = the character's bleed rate; the client draws a type-2 drop of size min(0.1·amount, 0.5). Sent only when the drop counter (+= rand·rate·dt·10) passes 1 AND the character moved dist² > 1 since the last drop; standing still sends nothing. Body layout unchanged. The server derives it in the Session (`ksim/bloodtrail.hpp`, no sim state): the roll is a hash of (id, tick), the counter dt is the 1 s medical interval, and a drop is sent only for characters in bounds/interest. |
| 16 bleed pool | id u64 (the character), pos f32 x3 (pool origin), g f32 (0..2; pool width = 72·g units). Branch 1: the pool grows every sim tick at the near-camera cadence (`BloodTrail::kPoolStepsPerSecond` = 54 / 5 = 10.8 exe steps per second, BLOOD_DECALS.md "Pool growth cadence"); sent only after g grew by 0.005 since the last event 16 (`kPoolSendDelta`, about 3 per second at the start of a 0.35 bleed, fewer later, and on reaching the cap), so the volume stays near 3 Hz per pooling character instead of 20 Hz. While a down/KO'd character with speed 0 grows its pool (g += 0.2·rate·dt·speed / max(1, 10g), cap 2). The client keeps one decal per (id, pos): same id near the same pos resizes; a pos > √10 from it is a new pool; growth stops when events stop (death, wake, carried). Server: `speed == 0` is the character's own movement (position unchanged since the last tick), the game speed in the exe's formula is 1 per sim-second, frameDt = 1/54; the 0.005 rule replaced "sent only when g changed". Additive |
| 17 blood smear | pos f32 x3, dir u8 (as kind 1), alpha f32 (clamp(rate, 0, 1)). Branch 2: a down, not-carried character that moved dist² > 16 since its last smear; decal type 3, size 0.2. Additive |
| 18 dismissed | id u64 (the character), faction strid (the new faction's name from the merged data, e.g. "Tech Hunters"), squadId u64 (its new squad). Sent to the session that dismissed it, after order 75 is applied, for the client's toast. Additive (player squads, 2026-11-01) |
| 19 discovered | town strid (kind 1, the TOWN record's stringID). A place newly on the DISCOVERED (37) list with type != 0; not sent on connect. The client fills in the placeholder name for its "Discovered {1}" log line (2026-11-01) |
| 20 lock_roll | char u64, door u64, success u8. After each lockpick roll by a player character, for the float text (approved 2026-11-01, see TASK_PROGRESS in section 6; implemented 2026-11-01) |
| 20+ | reserved for dialogue (speech, reply menu, conversation end, window, notice; see the dialogue design). DIALOGUE.md §13.7 proposes the block 41-46 and orders 87 / 88 (18-20 and 71 / 72 are taken); the lifecycle of a reply menu / dialogue window over disconnect, reconnect resend, load and the V1 sim pause is DIALOGUE.md §13 |
| 47 limb severed | id u64 (the character), part strid (kind 4, the LOCATIONAL_DAMAGE name: the same strid as that part's CHAR_MEDICAL row). Reliable, **once** per sever, at the moment it happens: a hit past -(max - wear) (Dismemberment >= 1) or the Dismemberment 2 sink path; routed like KO event 3 (to a session that knows the character). The CHAR_MEDICAL `severed` bit is set from the next frame on. **Implemented 2026-11-02.** The setting is `ksim_server --dismemberment 0|1|2` (default 1, Rare; 2 also severs through the passive sink, which is how to see severs in a test fight). Numbering: 18 is dismissed (taken), 41-46 are proposed for dialogue, so this one is 47 |
| 48 ragdoll | id u64, cause u8 (0 KO, 1 death, 2 drop/put-down, 3 launch = a hit that moves a body that was already KO'd or dead; BG2: a conscious down crawler is **not** ragdolled - it animates, its blows send no 48 and its KO is cause 0 - and since the hit target scan rejects unconscious bodies a launch cannot happen today, kept for the later knockback, PHYSICS 3.3), dir f32 x3 (unit, world frame: the hit's attacker -> victim direction; 0,0,0 = just collapse), strength f32 (the velocity change at the hit part, u/s; 0 = collapse), part strid (kind 4, the LOCATIONAL_DAMAGE name, 0 none), seed u32. **Client-approved 2026-11-06, implemented 2026-11-06 (task BG; `reference/PHYSICS.md` 2-3).** These are the client's ragdoll input: from them it flops the body and pulls it toward the rest point (CHAR_DYNAMIC `restYaw` below, and the logical position at state ends). Sent on the same tick as event 3 / 5 when there is one, after it; routed like event 3 (to a session that knows the character). **No event for the get-up** (the cleared CHAR_DYNAMIC flag bit 5 is the signal) and **none for a death while already ragdolled (KO'd or dead) with no hit** - a crawler's no-blow death is cause 1 - and a limb collapse without a KO sends none either (bit 5 alone). `dir` and `strength` are OURS/UNSURE: the exe's impulse is not decoded (`CombatArena::kRagdollHitStrength` = 3.0 u/s). `seed` is a draw of the fighter's own deterministic stream (`kRagdollStream`), so a replay and a save/load flop alike. Test `ksim_simlink` (all four causes, the get-up, the tail) |
| 41 speech | speaker u64 (who says it: the conversation's owner, or a squad mate interjecting), target u64 (whom it is said to, 0 none: a bit 2 line names the conversation's owner), dialogue strid (kind 1, the DIALOGUE root's sid), line strid (kind 1, the DIALOGUE_LINE sid), text strid (kind 4, the resolved wording: a variant picked by the server, WORD_SWAPS /TOKEN/s replaced, other tokens as written), duration f32 (seconds the bubble shows: max(speech timer `length x 0.12 + 2`, DA_FORCE_SPEECH_TIMER)), flags u8 (bit 1 interjection; bit 3 large: the conversation's event is one the original draws in the large font at twice the range, EV 2 / 11 / 17 / 26 / 31 / 34 / 36 / 40 / 45 / 74, kinspect findings/speech_bubble.md; bit 2 spoken by the target (the player character's reply, or a monologue's target answering by itself; slice 2, 2026-11-04); bit 0 conversation is 0 now; bit 4 important: range x1.33, the large events and EV 6 / 13 / 30 / 46 / 48 / 51 / 54, also set on every large line). One per spoken line; the next line of the same conversation follows when the duration has run out. Routed like the other character events (a session that knows the speaker or the target). **Implemented 2026-11-03** (DIALOGUE.md slice 1 step 5, monologues; `ksim_server --dialogue 0|1`, default on) |
| 42 speech_end | speaker u64: the bubble closes early (the speaker or the interjector went down or left mid-line). A line that runs its duration sends nothing. **Implemented 2026-11-03** |
| 43 reply_menu | speaker u64 (the NPC whose conversation waits), char u64 (the player character who answers), menuSeq u32 (low 32 bits of the tick it opened), flags u8 (bit 0 resend: after a connect, reconnect or load, same seq and texts, the client replaces its panel; bit 1 dismissible, always in V1; bit 2 freezes: the sim is paused until it closes), n u8, n x {line strid (kind 1, DIALOGUE_LINE sid; 0 = the "[...]" continue entry), text strid (kind 4)}. Sent after the speaker's line when the root is not a monologue or the event is the player's talk (DIALOGUE_SELECTION_STATIC Q8: every passing child in order, a T_ME child ends the list as "[...]"); a target already answering another menu gets none. Sent whether or not the speaker is in view. Answer with order 88 or 89. **Implemented 2026-11-04** (DIALOGUE.md 13) |
| 44 conversation_end | speaker u64, char u64, reason u8 (0 finished, 2 interrupted (the player's talk opened the interrupt root), 3 speaker or answerer gone, 4 walked away / end_talk, 5 no_client: no session answered within 30 s). Only for a conversation that showed a menu. **Implemented 2026-11-04** |
| 40 trade_closed | trader u64 (the character), reason u8: 0 the client (order 86), 1 an order of the actor's: **only when the server runs with `ServerContext::tradeCloseOnMove` (DIALOGUE 13 `window.close_on_move`, default off since 2026-11-02, MQ11)** a non-queued order that moves it; and always a new loot window (`interact` loot with the open flag) of the same character, 2 a party is gone (not in the arena) or the trader is out of the detail bounds, or the world was reloaded. A KO, a death or a fight closes nothing (MQ11: the original removes nothing then). **Implemented 2026-11-02.** Sent whether or not the trader is still in view (the usual rule that drops events about unknown characters does not apply to 40). Distance never closes a trade (MQ8 / MINE_SELL_SLICE 3.2): a transfer past 1000 units is refused `out_of_range`. A talk to the trader whose trade is open keeps it open (and sends TRADE again) |
| 3 KO / 4 wake / 5 death | id u64. **KO (3) fires once per KO transition, whatever the cause (2026-11-02):** a hit's KO blow, and a KO with no hit (a vital part sinking past its KO point, blood at 0, hunger, a shallow KO woken and put down again). In process `CombatEvent::Knockout` carries the attacker in `other` (0 = no attacker / the medical step); the wire body has no attacker field and is unchanged, the cause is the Hit event of the same tick (none for a medical KO). A KO that is also a death sends 5 only. A KO set from outside the combat step (a carry put-down's forced timer, a scenario) sends none. |
| 6 item dropped / 7 picked up | id u64, item strid, pos f32 x3 (id = the character; 7 is sent by pick_up and the item's INTERACTABLE id leaves with event 11; 6 is sent by order 61 drop (the new ground item arrives as an INTERACTABLE record)) |
| 8 drive blocked | id u64 |
| 9 order applied | clientSeq u32, appliedTick u64 |
| 10 order rejected | clientSeq u32, reason strid (always the clientSeq of the rejected order; reasons in section 10, `no_route` in section 12) |
| 13 interact done | id u64 (the actor), target u64, action u8, result u8: (action 6 talk, result 0: the actor arrived and the trade opened, **Implemented 2026-11-02**) 7 full (a producer reached its cap: every operator stops; Implemented), 0 done, 1 refused (the target's state changed while walking), 2 unreachable (the walk ended short), 3 target gone, 4 actor KO'd or dead, 5 cancelled (the running or queued interact was replaced or cleared by an order that is not queued; protocol unchanged, new value), 6 partial (a pick-up or loot took some of the stack and the rest did not fit the pack; nothing fitting is 1; additive, new value). Sent when a walk-and-act order ends, **exactly once per interact order** (a replaced or cleared one sends result 5; a KO'd actor sends 4 for the running order and 5 for each queued one) |
| 15 noticed | observer u64, target u64. A hostile `observer` became aware of `target` (its sensor record went from unaware to aware: it saw or heard it, the meter passed 1, or it was targeted by it), when one of the two is player-controlled. It fires again if the observer forgot the target (30 s unseen, 60 s in combat) and notices it later. For the client's barks (the original's EV_I_SEE_ENEMY_PLAYER family). Additive, protocol unchanged; a client that does not know kind 15 skips it |
| 14 theft | id u64 (the thief), container u64, owner strid (the container's owning FACTION record). Sent when a character takes from a container whose owner faction is not its own (a transfer order with take 1, or loot): once per transfer order and once per loot that moved something. Putting an item in is never theft. Crime itself (witnesses, bounty) is out of scope: this is the hook. Additive, protocol unchanged |
| 11 left interest | id u64 (the client drops the character or interactable; it comes back with a new CHAR_APPEARANCE) |
| 12 position corrected | id u64, distance f32. The server found the character off the navmesh and snapped it to the nearest mesh point; `distance` is how far it moved (-1: no mesh within 256 units, so it was not moved). A bug state, logged and counted on the server (section 12). The next CHAR_DYNAMIC has the corrected position. |
| 49 shot | shooter u64, target u64, from f32 x3, to f32 x3, result u8 (0 hit, 1 blocked, 2 missed), weapon strid, turret u64. **Approved by the client 2026-11-10, implemented 2026-11-10** (`CombatArena::rangedFire`, `Session::writeEvent`; test `ksim_simlink`; additive, no protocol bump; a client that does not know kind 49 skips it). Sent once per shot that leaves a gun: a hand crossbow or a turret's gun; `from` is the muzzle (the shooter's point lifted by the muzzle height), `to` the target's point; `weapon` the gun's CROSSBOW record (kind 1 strid, as `item` in event 6); `turret` the `kworld::doorKey` of the manned turret's placement (the CHAR_DYNAMIC `building` key space, equal to the gunner's mounted tail), **0 = a hand crossbow**. `result`: 0 the shot landed (event 1 follows from the same tick, per hit, with the damage), 2 the bolt missed (the deviation put it wide, or the target was no longer a legal victim), 1 blocked: **reserved** for a fired bolt that the line stops. UNSURE: the sim has no producer for it: a shot an ally or a hull / the terrain blocks is held (`rangedFire` returns Blocked, the gun retries 0.3 s later, no bolt is spent), so it sends no event 49 at all; event 49 is sent only for a fired bolt. Routed like event 1 (a session that knows the target or the shooter gets it; `who` is the target, as event 1's victim), once per shot not per bolt of a `num shots` burst. UNSURE (default: mirror event 1's conventions): `result` 0 for a burst where some bolts missed (any bolt landing = 0). |
| 50 stealth_ko | assassin u64, victim u64, result u8 (0 KO landed, 1 roll failed, 2 aborted because the victim was aware), chance f32 (0..100; the roll's chance, 0 for an abort). **Approved by the client 2026-11-10, implemented 2026-11-10** (`CombatArena::stealthKoStep` / `resolveStealthKo`, `Session::writeEvent`; tests `ksim_simlink`, `ksim_stealth_ko`; additive, no protocol bump; a client that does not know kind 50 skips it). Sent once per attempt of the stealth KO (order 60 action 14, STEALTH_KO.md): at the roll (result 0 or 1, with `chance`) or at the abort (result 2, chance 0: the victim's sensor already knew the assassin, no roll, no assault, no XP). Routed like event 1 (a session that knows the victim or the assassin gets it; `who` is the victim). A task that ends for another reason (the victim dead, KO'd or carried, the assassin hurt or carried, a cancelling order) sends nothing. |

### BUILDING (type 47), to the owning faction's sessions, on first sight and on change (**Implemented 2026-11-01, Phases 1-2**: sent for every building of the first player faction)

`u64 tick, u16 n`, then n x:

| Field | Type | Meaning |
|---|---|---|
| id | u64 | the building's instance Id (what orders name) |
| record | strid kind 1 | BUILDING stringID |
| owner | u64 | owner faction Id (0f, approval 2026-11-01: a name strid goes stale on every faction_rename 85; the client names factions from PLAYER_SQUADS / FACTION_RELATIONS 38) |
| x, y, z | f32 x3 | origin |
| qw, qx, qy, qz | f32 x4 | rotation |
| floor | u8 | |
| status | u8 | section 7 |
| flags | u32 | bit 0 built, 1 paused, 2 broken, 3 switchedOn, 4 runOffBattery, 5 repairWanted, 6 dismantling, 7 hasItems, 8 upgradeAvailable, 9 downgradeAvailable, 10 on the grid, 11 fully automatic |
| progress | f32 | construction progress 0..1 at `tick` (hp / maxHp while !built; 1 when built) |
| rate | f32 | progress per sim-second (builders at work; 0 when stalled, paused or built). The client advances `clamp(progress + rate * (now - tick), 0, 1)`, as TASK_PROGRESS |
| hp, maxHp | f32 x2 | the Condition bar: fill `min(1, hp / maxHp)`, number `(int) hp` |
| nMat | u8 | then nMat x `{strid item, u16 have, u16 need}`; empty once built |
| powerValue | f32 | effective `power output` (negative = consumption) |
| powerSupplied | f32 | delivered / produced at the last solve |
| powerDemand | f32 | the machine's demand at the last solve |
| stored, capacity | f32 x2 | battery (0, 0 when none) |
| workers, maxWorkers | u8 x2 | maxWorkers 0 = fully automatic |
| efficiency, maxEfficiency | f32 x2 | fractions (the client shows int percent) |
| quality | f32 | mines: the location richness 0..1 at the building's site (+0x46c, what the exe shows x 100); 1.0 otherwise. The client derives the label (Iron Quality / Stone Richness ...) from the record's `world resource mining` |
| town | u64 | PlayerTown Id (0 none) |
| version | u32 | the building's change counter |

**Coming in Phase 7 (approved, not faked by the client):** generators append the fuel level and burn rate for the panel's "Fuel Consumption" bar, either as appended fields or as a flag-gated tail; the exact layout is fixed with 0f when Phase 7 is proposed.

**When**: with the connect snapshot (every building of the session's faction), then a record only when something in it changed, coalesced per sim-second. Status, flags, materials and workers go out at once (they are step results); power fields follow the 1 Hz solve; HP at most 2 Hz while damaged or repaired; `progress` is resent only when `rate` changes or on a jump (a material arrives, completion, pause) because the client extrapolates. No periodic resend (UNSURE: at most one per sim-second if drift shows, as TASK_PROGRESS). A removed building: EVENT 11 `left interest {id}` (existing kind).

**A producer's row (focus panel, implemented 2026-11-12):** sent only to the session whose order 87 focus is that producer (Order 87, "Producer focus"), never in the connect snapshot. `id` is the
producer id, so the row needs no EVENT 11. record = the producer's BUILDING; flags bit 0 built (bit 11 fully automatic when `max operators` is 0); status 0 Working
(`prodState` 2), **5 Output full** (1), **6 No workers** (3, idle); progress 1, rate 0; hp = maxHp = the BUILDING record's HP (the exe shows Condition from it);
nMat 0; the power fields, stored, capacity and town 0; workers / maxWorkers = operators / `max operators`; quality = the site's richness; maxEfficiency =
richness x the BUILDING's `output rate` (the field behind +0x1a8) x research 1.0; efficiency = power 1 x HP fraction 1 x worker fraction (operators / max;
1 for a fully automatic one) x that max (hud_building.md "Stats list"). UNSURE: no research model, so the research multiplier is 1.0 owned or not (deferred:
playtesting); the HP fraction is always 1 (a producer has no damage model). `version` = the producer's version plus its stock's.

**Who**: the owning faction's sessions only in this slice. Other players' buildings come with the interest rules; static zone-file buildings stay the client's own computation.

**Phases 1-2 fill:** id, record, owner, transform, floor, status (0 Working, 1 Broken, 2 UnderConstruction, 3 NeedsMaterials, 4 Paused; a paused site shows 4 before 2 / 3), flags bits 0-6, progress, rate, hp, maxHp, the material rows (empty once built), quality (always 1.0: mines are `unsupported_site`), town, version. **Neutral until their phases:** powerValue, powerSupplied, powerDemand, stored, capacity, workers, maxWorkers, efficiency, maxEfficiency are 0 (workers on the wire stays 0 while the saved count is the scripted builder count). A new site is `NeedsMaterials` at once (hp 0 is at the 0 material cap). `version` bumps on a change of status, flags, builder count or delivered materials, never on hp alone. The connect snapshot (and any full resend after a load) carries every building; later only the changed ones. Test: `ksim_buildings_place`, `ksim_buildings_build` (decoded by the client's own decoder).

### BUILDING_GRID (type 48) (approved; Phase 6, not implemented)

`u64 tick, u16 n`, then n x `{u64 town, str name, f32 supply, f32 demand, f32 stored, f32 capacity, u16 generators, u16 consumers, u16 batteries}`: the player-town list with each town's totals at the last solve (the panel's "Town:" line uses `name`). On connect, on change (a new town, a rename), totals at most 1 Hz. The generators' fuel tail comes in Phase 7.

## 7. Client -> server orders

Each order frame (type 100) is `u32 clientSeq, u16 kind`, then the body. Orders are reliable and
ordered. The server turns each into a sim `Command`, stamped for the next tick (the command queue
is the only writer of sim state). It answers every order with event 9 or event 10, so the client
knows the exact tick its order took effect.

**Applied while paused (2026-11-12, kenshi-k16.20.5; user: the original lets you trade with the game paused).** While the clock is paused
(the admin or idle pause, a dialogue freeze) an accepted order **68 against a trader's stock** (a buy, a sell, a sale from an owned
container) and order **86 trade_close** take effect at once, between ticks: the command runs through `Simulation::applyNow` (the code a tick
runs for it, no tick), the event 9 ack carries `tick + 1` as always, and the frames that changed (INVENTORY and ITEM_GRID of both sides, TRADE,
PLAYER_SQUADS with the money, and the player characters' CHAR_STATS that changed, encumbrance included, outside their 1 Hz slot: kenshi-k16.20.15) follow the ack in the same reply. The order is logged for `tick + 1`, **ahead of** the commands already logged for that tick (they were queued and run at its start) and after earlier
paused-applied ones (`OrderLogEntry::immediate`), so a replay that enqueues in log order applies them in the order they ran live. The list is one function, `appliesWhilePaused` (simlink.cpp). Every other order (walking, attacking, talking, the other
inventory orders) waits for the tick as before; which window moves beyond trade the original allows while paused is kenshi-k16.20.9.

| Kind | Body |
|---|---|
| 1 move_to | n u16, ids u64 x n, x f32, z f32, y f32, flags u8 (protocol 3; section 7 notes) |
| 2 attack | n, ids, target u64 |
| 3 follow | n, ids, target u64 |
| 4 stop / hold | n, ids, hold u8 |
| 10 toggle | n, ids, which u8 (0 block mode, 1 stealth, 2 run, 3 ai_off; 3 is still `unsupported`; 4 jobs = JOBS `enabled`, the client sends the opposite of the shown character's for all selected, as the exe does; implemented 2026-11-01, kenshi-b0 sim worker: value 1 = on, 0 = off, applied to every id, creates the list if it has none; section 6 JOBS), value u8. Run (value 1 = run, the default; 0 = walk) caps move and drive speed at the race's walk speed (RACE `walk speed`, 15 for humans: MEASURED only as the data value); chases and follows always run. It does not replace the current order. CHAR_DYNAMIC flag bit 12 shows it. **`which` 5 = speed mode (approved by the client 2026-11-03, implemented 2026-11-05; ROADMAP sim queue 8; kinspect `findings/ui_captures/hud_floor_speed.md` §2, `findings/move_kinematics.md` s70 "Speed modes"):** `value` 0 Walk (RACE `walk speed` cap), 1 Jog (cap 55), 2 Run (no cap; the default for every character), 3 Group (each member capped at the slowest live, conscious member's own run speed); above 3 `bad_value`. The exe's CharMovement+0x20 numbering = the client's diamond index. Applied to every id, never replaces the order; chases and follows always run. Group: the order's ids become one group (keyed by the lowest id), replacing their earlier groups; another mode leaves it; dead or departed members stop counting. `which` 2 stays an alias (1 = Run, 0 = Walk). Save v65 (the conversations work took v57): `run` becomes `moveMode u8` + `speedGroup u64`, and a v64 save still loads — its `run` 0 is Walk, 1 is Run (RUNTIME_SAVE.md). Ours caps move orders and drives; a chase or a follow task always runs (the exe's Char+0xbc is one cap under the mover's top speed, but its combat and follow movement under a speed mode is not measured: UNSURE). Left out (FEATURES rows): the 0.8 slow-down for a member behind, the Char+0xe0 lock; the cycle key (Numpad 6) is client-only. Test `ksim_speed_modes` (top speeds, the group cap and its release, both saves); the wire flags and `bad_value` in `ksim_simlink` |
| 20 drive | id u64, drive u8 (0 stop, 1 forward, 2 backward), heading f32 (section 12; coalesced per tick, no resend) |
| 30 pick up / 31 carry / 32 drop | id u64, target u64 (still `unsupported`; 60 interact covers pick up, **carry (lift: action 8) and put down (action 13)**, 61 covers drop) |
| 62 equip | id u64, item str, equip u8 (1 equip a carried WEAPON or ARMOUR record, 0 unequip the wielded or worn one) (additive; section 6 INVENTORY). Answers `bad_value` (equip above 1, empty or over-long item), the section 11 id reasons, `bad_target` (not a WEAPON or ARMOUR record), `not_allowed` (not carried when equipping, not equipped when unequipping; the character is down, mid-swing, defending or staggering). Instant (UNSURE). Does not replace the running order |
| 63 transfer | id u64, container u64, item str, count u16, take u8 (1 container -> character, 0 character -> container) (additive; section 13). Answers `bad_value` (count 0, take above 1, empty item), the section 11 id reasons, `bad_target` (no such container), `out_of_reach` (farther than 16 units from the container's use point), `locked` (the box is locked), `item_type` (a stand's `itemtype limit` refuses the kind), `full` (the box's grid has no room for the stack), `no_room` (the taker's pack has none; for a put into a producer, order 68's over-the-cap answer), `not_allowed` (the character is down, the container is a producer and the item is not the one it produces, or the source holds fewer than `count` carried). Instant; the client UI opens after a walk-and-loot or a plain walk. Does not replace the running order |
| 61 drop | id u64, item str (u16 length + bytes: the item record's stringID, as sent in INVENTORY), count u16 (protocol 4; section 13). With several stacks it takes from the first matching entries in pack order |
| 60 interact | n u16, ids u64 x n, target u64, action u8, queue u8, **optional trailing flags u8** (section 13). **Loot open (approved 2026-11-01 by kenshi-0f, implemented 3748429, save v45):** with action 5 (loot), flags bit 0 = open: the walk ends without taking anything; on arrival the server sends the body's INVENTORY + ITEM_GRID to the looter's session(s) and keeps sending them on change while a looter of that session stays within 16 units of the body (the container use-range rule); event 13 result 0 marks the arrival. flags 0 or absent = take-all as before. Other bits must be 0 (`bad_value`). Orders 63 / 64 / 65 / 68 accept a KO'd or dead character as the `container` / `id` under the same range rule; taking from a body of another faction reports theft (event 14) as for containers. A living, conscious character is `bad_target` (for loot open; take-all on a standing character keeps `not_allowed`, and 63 / 68 on one answer `bad_target`). Orders 64 / 65 carry no actor: on a body they need any conscious player character within 16 units (`out_of_reach` otherwise). The open feed stops when no looter that opened the body stays in range (a later open sends it afresh); take-all still strips equipped pieces as before. **Stripping (approved 2026-11-01, kenshi-0f; implemented 3748429):** with take = 1 from a body, 68 accepts the body's equipped entry (`index` points at a section-255 entry in the body's INVENTORY). It unequips the entry and puts it into the looter (`section` 255 = autoplace in the looter's pack, else the rect), all or nothing, with the same `no_room` / theft / range rules. 63 counts the body's carried entries first, then its equipped ones of that item. Take-all (flags 0) is unchanged from before loot open, and it **strips equipped pieces too**: the existing `lootBody` and its test take everything. The earlier "carried only" text was b0's error, corrected 2026-11-01 from the loot worker's report. **Answered (kinspect 721b348, `findings/ui_captures/loot.md`, `findings/s55_static.md`): the original has NO take-all.** Looting a body opens its character inventory beside the looter's, and items move one at a time (RMB = 1 unit, Shift+RMB = the stack, or drag). Worn gear is an ordinary section and comes off the same way, which is our 68 / 63 stripping. KO and dead bodies are treated the same. The only bulk loot in the exe is the AI's Task_Loot_AI (hip / back weapons). So action 5 take-all is **our extension (MINE)**, kept for convenience and for AI use. UNSURE, for the crime slice: a non-player body logs crime 10 "Looting" unless exempt, counted only if witnessed (M). The exe's per-transfer range is distance² < 1e6, against our 16-unit rule (unit scale UNSURE; kept at 16). Giving (take = 0) never equips onto a body |
| 64 move_item | id u64 (a character or a container), version u32, index u16, section u8, x u8, y u8, **optional trailing count u16** (grid inventory; section 6 ITEM_GRID). `version` / `index` are the INVENTORY's the client saw. Answers `stale` (version differs), `bad_value` (index past the end, section above 1), `no_room` (the rect is not in the grid, the section does not exist or does not accept the entry, a non-empty container into a bag, a stack over the destination's cap), `occupied` (the rect holds 2+ entries, or one entry that cannot be merged or swapped with: onto exactly one entry of the same stackable kind with room the stacks merge up to the cap and the remainder stays at the source; a full stack of the kind is `occupied`; onto exactly one entry of another kind the two **swap**: the other entry goes to the moved entry's old section and cell (the exe parks it on the cursor), `occupied` when it does not fit there after the move, is not allowed in that section or is over its stack cap), `not_allowed` (an equipped entry: equip / unequip are 62 / 67), the section 11 id reasons, `unknown_id` (no character or container). **count** (no protocol bump; the decoder takes a payload with or without it): absent, 0, or at least the entry's count moves the whole stack; otherwise `count` units are split off to the target cell (an empty rect, or a merge up to the cap with a stack of the kind; never a swap) and the rest stays at the source; a refusal (`occupied`, `no_room` over the cap or outside the grid) leaves both cells unchanged. Instant. A container may be rearranged from anywhere (UNSURE: the original only rearranges an open window) |
| 65 arrange | id u64 (a character or a container), section u8. The original's repack (INVENTORY_GRID_STATIC (e)): the section's entries are ordered by x then y, stable-sorted descending by max(w, h), then by area, then by w, each pass re-adding every entry with the autoplace rule (stacks merge); the first pass that fits everything is kept; if none does the grid stays as it was (applied, version unchanged). Answers `bad_value` (section above 1), `bad_target` (section 1 and no backpack worn). A character's `main` and its backpack are arranged separately. The exe arranges only `main`; arranging the backpack (section 1) is **our extension (MINE)** |
| 66 drop_entry | id u64, version u32, index u16, count u16. Drops `count` units of that carried entry (1..its count) at the character's feet (event 6); `stale`, `bad_value` (count 0, index past the end), `not_allowed` (equipped, more than the entry holds). A dropped stack keeps its quality, material and contents |
| 67 equip_entry | id u64, version u32, index u16, equip u8. Order 62 for one entry: equip a carried WEAPON, ARMOUR or a CONTAINER with `slot` 12 (a backpack: it becomes the worn backpack and its grid appears), or unequip an equipped one. The displaced piece (or the unequipped one) goes back to the pack by autoplace; if it has no cell the order is `no_room` and nothing changes. Answers `stale`, `bad_value`, `bad_target` (not a WEAPON, ARMOUR or backpack), `not_allowed` (wrong state, down, mid-swing) |
| 68 transfer_entry | id u64 (the character), container u64, version u32, index u16, count u16, take u8, section u8, x u8, y u8 [, source u64 (**optional trailing, 2026-11-02, MQ12**: a sale from an owned container; absent = 0)]. Order 63 for one entry: `version` / `index` are the **source's** INVENTORY (the container's when take = 1); `section` 255 = autoplace in the destination, else the rect must be free (or hold one entry of the same kind with room for all of it) or the answer is `no_room`; all or nothing. Within the container's reach of its use point (`out_of_reach`; the shared reach rule, MQ13: a storage container 25, doubled once the actor stands still; a producer its `use range`; a body 16.6 / 30.6); A locked box refuses both ways (`locked`); `stale`, `bad_value`, `bad_target`. Taking from a foreign container is theft (event 14). **`section` 255 into a character is the quick move (implemented 5a27b7d; approved by kenshi-0f, 2026-11-01, 4adf8c9; kinspect `findings/ui_captures/loot.md`):** the destination character tries, in order, a free matching worn slot (the equip path of order 67, so the fighter's weapon / armour numbers follow; refused while the fighter is attacking, defending or staggered, or when the instance cannot be built), then `main`, then the worn backpack (section 1; what does not fit main goes there whole or as the remainder). A container destination (take = 0) stays `main` only. `grid::autoplace` itself is unchanged (main only, or the one section named); the quick move lives in order 68. **Order 63 (by record) takes the same quick move into a character (implemented, kenshi worker T3b1 rider):** `take` = 1 sends each unit through the same path (a free matching worn slot via the equip path, then `main`, then the worn backpack), judged all or nothing; `take` = 0 into a container stays `main` only. Shared code: `Simulation::quickMoveInto` / `wearOnto` (inventory_orders.cpp); test `ksim_inventory_grid`. **Against a merchant's stock (`container` = the TRADE `stock` id; Implemented 2026-11-02, MINE_SELL_SLICE 3.2, TRADE 53):** `take` 1 buys, `take` 0 sells, at the TRADE prices; the rect / `section` rules are 68's, `version` / `index` are the source's. Needs the actor's trade to be open with that stock (else `not_allowed`). **`source` (optional trailing u64; only with `take` 0 against a merchant's stock, else `bad_value`):** a storage container of the actor's own faction (its `ownerSid` is the actor's Allegiance faction; a body, a producer, another's or an unowned container is `not_allowed`) that the goods come from instead of the actor's pack: `version` / `index` are then the **container's** INVENTORY, and the range is the XZ distance container to trader (`out_of_range` at 1000 or more, the actor may be anywhere; the window stays open on every refusal). The TRADE frame's chest-prices tail prices the entries of every container in this set (TRADE 53; MQ12). A short wallet moves `min(count, floor(payerMoney / unit))` units and the rest stays at the source; the money moves in the same tick (`PlayerFaction::money`, `TraderStock::money`). Refusals, only when nothing moved: `out_of_range` (the XZ distance between the actor and the trader character is 1000 or more; never a close), `no_room`, `cant_afford` (code 3), `shop_cant_afford` (4), `not_for_sale` (0xc: the entry has no price), `illegal_only` (0xe: a smuggler's `buys illegal` template and a legal item; UNSURE reading), plus 68's usual reasons. Orders 63, 64 and 65 on a stock are `not_allowed`. |
| 70 squad_add | (no body). A new empty squad named `<factionName>_<N>`; N is the faction's running counter (saved, never reused; FACTION_SQUADS_LIVE). `not_allowed` at maxSquads. Faction size is not checked (the exe doesn't either). Player squads, section 6 PLAYER_SQUADS |
| 71 squad_delete | squadId u64. `not_allowed`: not empty, or the faction's last squad. `bad_target`: not one of the session's squads |
| 72 squad_rename | squadId u64, name str (up to 32 characters, "" allowed). `bad_value`: longer, or bad UTF-8. The exe commits on every keystroke; the client may send on each change, the last one wins |
| 73 squad_move | char u64, squadId u64, index u16 (0xFFFF = append, 0 = leader), swap u8. swap 0 inserts at the index (`not_allowed` if the target squad is full; within the same squad it moves the member). swap 1 swaps with the member at the index, in the same squad or across squads, and is allowed when full (`bad_value` if there is no member at the index). The exe's Shift drop can push a squad to max + 1; ours never exceeds max (deliberate, FEATURES) |
| 74 squad_reorder | squadId u64, newIndex u16 (clamped to the last index). Reorders the faction's squad list, which is also the HUD tab order |
| 75 dismiss | char u64. `not_allowed` if it is the faction's last living character. The character joins faction `1233-gamedata.base` (resolved in the merged data; "nofac" if missing) in a new squad at its position with its orders cleared; event 18 follows. The confirm box is the client's |
| 76 job_add | n u16, ids u64 × n, type u16, target u64 (0 = none) (section 6 JOBS; implemented 2026-11-01, kenshi-b0 sim worker). This is Shift + an order: the client maps the order to the exe's type (attack → 4, follow → 31, a FirstAid interact → 25, a bodyguard follow → 45). The server applies the exe's `addTask` rules for each id: replacement type, duplicate dropped (applied, no change), companion rows, and `enabled` set on. Answers `unsupported` (a type with no executor yet, or `kind` 0: those are plain orders), `bad_value` (an unknown type), `bad_target` (the type needs a target and it is 0 or unknown, or the target is not a valid kind), and the section 11 id reasons. Does not replace the running order |
| 77 job_remove | id u64, jobId u32 (implemented 2026-11-01). Removes that one row (the X button). A running goal from it is dropped at the next replan. `bad_target`: no such jobId (already gone, so stale clients are harmless) |
| 78 job_move | id u64, jobId u32, index u16 (implemented 2026-11-01; 0xFFFF = last). The exe's drag rule: the row is taken out, then inserted at `index` of the shortened list (A onto C in ABCD gives BCAD). `bad_target`: no such jobId; an index past the end means last |
| 79 prospect | id u64 (section 6 PROSPECT; implemented 2026-11-01). The client picks the one character, as the exe does: the selected character nearest the camera pivot. If the window is showing, the button only hides it, and the client sends nothing. Replaces the character's orders. The character prospects where it stands. When the task ends the server sends PROSPECT (35) to the session. Answers the section 11 id reasons, and `not_allowed`: the character is indoors (the exe says "I need to go outside to do this."), not on the ground (carried, or riding), down, or in combat. A replacing order, a KO, or death cancels it with no result (a carry or a pick-up too). Duration: **MEASURED: 1.0 sim-second** with no skill term. Progress += frame dt until it reaches 1.0. That is about 33 seconds on the HUD clock (kinspect `findings/prospecting.md` "Live timing", item 45: two Science levels finished together, three runs). Two prospectors finishing together: the later finisher's result is the one shown, as in the exe |
| 40 pause / 41 set speed | paused u8 / speed f32 |
| 50 set_interest | x f32, z f32, radius f32 (not a sim command, see 5) |
| 51 set_view | x f32, z f32: camera focus; a sim command, sent only when the focus zone changes after hysteresis (see 5) |
| 80 save_game | name str. **Session control** (like 40 / 41): not a sim command, not in the order log, run at a tick boundary; **admin session only** (see SAVES). Writes the runtime save to `<saveDir>/<name>` atomically (temp file + rename); an existing name is overwritten (the confirm box is the client's). `bad_value`: empty, over 64 characters, bad UTF-8, a control character, `/`, `\`, `:`, or a leading `.`. Then SAVES (32) to every admin session |
| 81 load_game | name str, resetPositions u8 (accepted and ignored; UNSURE what the original's checkbox maps to). Session control, admin only. Restores the save at a tick boundary, then every session gets the full resend as on connect (snapshot, PLAYER_SQUADS, WEATHER, ...); the tick jumps to the save's tick, and the client treats that as a reset. `bad_target`: no such save; `bad_value`: unreadable or a newer save version. **Approved 2026-11-01 by kenshi-0f, implemented 66a6a0a (the client already maps it):** `old_version` for an intact save of an older version (our magic and a version number below the current one; a newer version, a bad head or anything unreadable stays `bad_value`; test `ksim_saves`, files `old` and `v45`). The loader has no migrations (SAVES status 2), so the save panel can say "made by an older build" instead of "unreadable". Save v46 (merchant refresh, `reference/TOWN_LIFE_SLICE.md` 3.6) is the first bump this answers for v45 files. |
| 82 delete_save | name str. Session control, admin only. `bad_target`: no such save. Then SAVES (32) to every admin session |
| 83 list_saves | (no body). Session control, admin only. Answered with SAVES (32) |
| 84 new_world | (**implemented 2026-11-01**, see "NEW_GAME" in section 6) (no body). Session control, admin only. Resets the server to a fresh world built from the data and the server's seed, with **no player faction** (the scenarios' squads are not spawned). Answered like a load: event 9 (the new tick), then the full resend, in one write, then STARTS (33). Destructive: the client checks STARTS before sending it. Then the client sends NEW_GAME (frame 111, MULTIPLAYER section 4a; n = 0 = the start-off's template members as they are), answered with type 62 |
| 85 faction_rename | (approved 2026-11-01; the client sends it on commit, Enter or focus loss, only when the name changed; see FACTION_RELATIONS in section 6) name str. Renames the player faction (`PlayerFaction::name`); `bad_name` as NEW_GAME, `name_taken` (another faction's display name), `not_allowed` without a player faction (Implemented) |
| 88 reply | id u64 (the player character), speaker u64, menuSeq u32, option u8 (index into the menu). Event 10: `bad_target` (no open menu of the speaker for id, an unknown id), `not_allowed` (not player-controlled), `stale` (another seq, answered already this tick, or no such option). The answer is spoken as event 41 by the player character (flags bit 2) with the menu's text, then the speaker goes on with that line's children; "[...]" lets the speaker go on at once. Lifts the dialogue freeze; applies on the next tick (Command::DialogueReply). **Implemented 2026-11-04** |
| 89 end_talk | id u64, speaker u64. Closes the open menu: event 44 reason 4; the NPC says nothing more. Refusals as 88 (no `stale`). Command::EndConversation. **Implemented 2026-11-04** |
| 86 trade_close | (**implemented 2026-11-02**; no body, a body is malformed) Closes the session's open trades: event 40 reason 0 for each, then the stock feed stops. Answers applied also when none is open. Session state, not a sim command (not in the order log) |
| 90 build_check | record str, x f32, y f32, z f32, qw qx qy qz f32, floor u8 (**Implemented 2026-11-01**, BUILDINGS_SLICE.md sections 3, 10). A dry run: event 9 when a placement would apply, else event 10 with the reason; changes nothing. The client sends at most every 100 ms. Reasons: `no_faction`, `locked` (not in the UNLOCKS set), `bad_value` (floor above 3, a quaternion off unit length by more than 0.05, outside the world, y more than 40 from the terrain, an empty record), `unsupported` (no game data or heightmap), `unsupported_site` (a mine, farm, well-like piece, wall / gate, snapping or interior piece), `too_close_town` (the nearest town's radius x 2.5 or the 2000 floor; item50 Q2 / Q3), `characters_in_way`, `invalid_ground` (a LAND part with Y below 98, Q4), `not_level` (Q5), `overlap` (another player building's footprint, or a placed building with doors; other static shapes are not tested) |
| 91 build_place | the same body as 90 (**Implemented 2026-11-01**). One per ghost at Confirm. The same checks, again when it applies (a failure then drops it with no event: the first of two overlapping ghosts in one tick wins). No stock check, no charge. The new building arrives as a BUILDING record; the first building with `creates player town` out of reach of the faction's towns also makes the town "Your Outpost" |
| 92 build_dismantle, 93 build_repair | (approved, not implemented; bodies in BUILDINGS_SLICE.md section 10) |
| 94 build_pause | building u64, paused u8 (**Implemented 2026-11-01**). Toggles construction pause of an unbuilt building (`paused` above 1 is `bad_value`; an unknown, foreign or built building is `bad_target`). A paused site gains no hp and shows status 4 |
| 95-97 build_* | (approved, not implemented): 95 build_upgrade, 96 build_power, 97 build_work. Rotation, Undo and Cancel All are client-local; worker assignment reuses 76 job_add. 86-89 stay free for session control |
| 120 profile_query | (no body; **Implemented 2026-11-12**, PROFILE below, approved by the client lead on kenshi-k16.6.88). Admin session only, like orders 80-83; session control, not logged. Answered with PROFILE (70). Refusals: `not_allowed` (a non-admin session, checked first), `unsupported` (a server built without KSIM_PROFILE). A body is a malformed frame |
| 99 menu_query | id u64 (the asking player character), target u64 (**Implemented 2026-11-12**, MENU_ENTRIES below; approved by the client 2026-11-12, kenshi-k16.9.17). A read: answered to the asking session only (reply MENU_ENTRIES 61, then event 9), no tick, not in the order log or the state hash. Refusals (event 10): `bad_target` (no such character, `target` not in the session's view, or `id` not a player character), `bad_value` (`id == target`) |
| 98 set_progress_level | level u8 (**Implemented 2026-11-11**, TASK_PROGRESS below). Session control, not logged. `level` above 3 is `bad_value` |

**move_to `flags`** (protocol 3): bit 0 = queue (the move waits behind the character's running order and queue; it is routed to the clicked point, as snapped when the order was given, when it starts. An order without the bit replaces the running order and empties the queue; see section 13), bit 1 = `yValid`: `y` is the clicked surface
height (the pick ray's hit on the terrain, a deck, a floor). Bits 2-7 must be 0. `y` must be finite always (send 0 when not
valid), and within +-100000 when valid; otherwise `bad_value`. With `yValid` the server snaps the click to the closest
navmesh point within 340 units in 3D of (x, y, z), so the right floor or deck is chosen (section 12); without it the click is
snapped by horizontal distance, any height, as in protocol 2. A group order applies `y` to every member's scattered point.

Selection stays on the client, so there is no message for it.

**Determinism:** the server logs every applied order with its tick. A recorded input log plus
the start save replays the session exactly. Pause, speed and interest are not sim commands.

## 8. Debug dump

`ksim_server --dump-snapshot FILE` (or `ksim_run --dump-snapshot`) writes the same content as
key/line text, one record per line, for the scene tests and for eyeballing. It is not a wire
format.

## 9. Open

- Appearance field encodings (heads, hair, sliders): to be fixed with kenshi-0f once the sim
  carries appearance.
- Deltas behind `full = 0`.
- Building and town state (destroyed variants, gates, doors).
- Authentication / remote play: out of scope for version 1 (loopback only).
- **BOUNTY (type 46), proposed, not implemented (crime foundation, 2026-11-01).** The highest type in use is 45 (NAV_POLY_FLAGS; EVENT is 50),
  so 46 is free. Additive, no protocol bump (a client that does not know it skips the frame by its length). Sent for **the session's own
  player-controlled characters only** (bounties are per character and per faction, so one session never sees another player's), on first
  sight and on change, and once with an empty list when the last bounty goes: `u64 tick, u64 id, u8 crime, f32 timer, u8 n`, then `n` x
  `{u32 factionName (string table), i32 amount, u32 crimes, u8 claim, f32 hoursLeft}`. `crime` is the pending CrimeEnum (0 none) and `timer`
  its seconds left; `hoursLeft` is `amount x 0.04 - age` (-1 = never expires, from 10000) so the client does not need the clock rule.
  The bounty is held by the faction named, so the client can show "Wanted by The Holy Nation: 2500 (60 h left)". A sentence would come
  later with the cages. The server work is small (a dirty check on `state::CrimeRecord::bounties` in the per-tick snapshot pass).
- **ITEM_GRID (type 28) and orders 64-68: implemented (2026-11-01)**, see section 6 ITEM_GRID and section 7 (approved by kenshi-0f). Swap is covered by order 64 (a drop onto one other entry swaps; the exe parks the swapped item on the cursor, ours puts it in the source cell); order 64 has an optional trailing `count` (split). Still open: equipment sections as grids, a carried backpack's contents window, animals' grids, robotic limbs, trade windows, colour (COLOR_DATA).

## 10. Server first cut: decisions that fill gaps (kenshi-b0, 2026-10-31)

**Player squads (type 31, orders 70-75, event 18): implemented 2026-11-01** (`Session::writePlayerSquads`, `Simulation::checkSquadOrder`, save v35; rules and the deliberate differences in `reference/FACTION_SQUADS_LIVE.md`; test `ksim_squads`, which decodes frame 31 by hand because `client/src/simlink.*` had no type 31 decoder). The record is sent on connect and whenever its content differs from the last one sent. `CHAR_APPEARANCE.squadId` is the player squad (or a dismissed character's new squad), and SQUADS (30) lists the player squads after the others. A client that does not know type 31 skips it.

Code: `cpp/include/ksim/simlink.hpp`, `cpp/tools/ksim_server.cpp`, test `ksim_simlink` (uses the client's
`simlink.cpp` decoder unchanged). Samples: `simlink_sample.txt` (6 ticks of the duel, text dump),
the byte stream `simlink_sample.bin` (400 ticks, about 244 KB; not in the tree: it carries game-data strings, kenshi-379.14.9) is recorded at test time into the build dir by the CTest fixture `ksim_simlink_sample` (`ksim_server --no-world --dump-snapshot T --dump-bin B --dump-ticks 400 --dump-script simlink_sample_script.txt`), so it is always the current protocol; `ksim_simlink` decodes it. The text sample is the same with `--dump-ticks 6`; it holds the tile stream, an INVENTORY, a CHAR_ROUTE, a walk and a `no_route`; since the weather slice it also carries WEATHER (type 41) frames, 66 regions, one per 20 ticks and on a new pick; the `--no-world` server turns the weather on with `Simulation::enableWeather` and the clock at Day 1 10:00), `simlink_sample_script.txt` (the orders fed).

- **Handshake order.** The server sends its HELLO as soon as the connection is accepted; the client's HELLO
  follows. On a protocol, dataHash or role mismatch the server sends `ERROR` and closes. A first frame that is
  not HELLO gets `ERROR`. On success: the string table so far, then a full snapshot (bracketed, with every
  appearance, CLOCK, SQUADS and DETAIL_BOUNDS inside it), then one bracketed snapshot per tick.
- **Tick field.** `STRTAB_APPEND`, `HELLO` and `ERROR` carry no tick (they are sections 2 and 3, not records of
  section 6). Every section 6 record does, once.
- **Within a tick:** `STRTAB_APPEND` (if new strings) first, then `SNAPSHOT_BEGIN`, `DETAIL_BOUNDS` (on change),
  `CHAR_APPEARANCE` (first sight or version change), `CHAR_DYNAMIC`, `CHAR_ROUTE` (when a route changed),
  `CLOCK` and `SQUADS` (every 20th tick),
  `SNAPSHOT_END`, then that tick's `EVENT`s. A KO event can precede the hit event that caused it.
  `CLOCK` is also sent outside a bracket right after a pause or speed order.
- **Conventions:** `yaw` is the sim's: radians, 0 = +z, increasing towards +x. `y` is the navmesh surface height
  and `yValid` is set (protocol 2; the surface follows the terrain on open ground, within about 3 units, and
  the decks and stairs of buildings). `legR`/`legL` = min(100, 100 x (health - stun) /
  (maxHealth - wear)) of the Right/Left Leg part (no bonus term, as MedicalSystem 140646860 -> 14051ccb0;
  LOCOMOTION.md "Leg values"; healthy = 100, can go negative). `dataHash` is `kworld::dataHash(LoadOrder)`
  (`kworld/loadorder.hpp`), shared by server and client.
  `action` = the technique's ANIMATION name (string kind 3), `actionStart` = tick the clip started (derived from
  the clip time), `actionSpeed` = the clip play speed. `flags`: combat = has a target or plays a technique, and is neither KO nor dead (2026-11-06), drawn
  = the weapon is in the hand (a sheathed one, Inventory::kSheathed, is not; rwh.105), prone = down, KO or dead. `drive`, `heading` and `driveSeq` are the direct-drive
  state (section 12); `building`, `floor` and `carriedBy` are the derived membership and carry fields (section 6), 0 when nobody is in a building, outside a building or carried.
- **CHAR_APPEARANCE (first cut):** race = the RACE record sid; faction from the scenario; `squadId` 0; sex, head
  from the scenario's seed (the sim carries none: male, head 0); hair, beard, hairColour, skinTone 0; no body
  sliders; items = the worn ARMOUR records (by `slot`: AttachSlot hat 3 -> head 3, body 5 -> armour 5, legs 6 ->
  legs 6, shirt 8 -> shirt 4, boots 9 -> boots 7, back 1 -> back 0, backpack 12 -> backpack 8, belt 14 -> hip 2;
  hair, eyes, gloves, neck, beard have no section and are skipped) and the wielded WEAPON in section 9 unless
  it is fists; a sheathed weapon is listed in section 0 (back; the sim keeps no hip / back2 choice) and `appearanceVersion` bumps on draw / sheathe. `appearanceVersion` bumps when the gear changes.
- **Events sent:** 1 hit (result 0 hit with damage after armour; 1 blocked, 2 dodged with damage 0 and part 0),
  3 KO, 4 wake, 5 death, 8 drive blocked (once per blocked spell), 9, 10, 11, 12 position corrected.
  Hit `pos` is the victim's x, y, z; `dir` as defined above.
- **Orders and answers.** A group order (`n` ids) is atomic: if any id is unknown, not player-controlled or
  dead, the whole order is rejected. Sim orders (move_to, attack, stop, toggle, set_view) become `Command`s
  stamped for tick clock+1; event 9 carries `appliedTick` = that tick and the event's own `tick` is the
  clock when the answer was written. Pause, speed and interest are applied at once (`appliedTick` = the current
  tick) and logged but are not sim commands. Rejection reasons (string kind 4): `unknown_id`,
  `not_player_controlled`, `dead`, `no_characters`, `bad_target`, `same_team` (the attack target is the attacker's own party, a
  player-controlled character: the player may attack anyone else, whatever its Fighter::team), `bad_value` (non-finite or
  outside the world, speed outside (0, 64], drive mode above 2 or a heading beyond 1000 radians), `no_route`
  (move_to the server cannot route: no navmesh loaded, the character or the destination is not within reach of
  the mesh (the destination may be up to 40 units from it and is snapped), or no complete path joins them; the
  whole group order is rejected if any member has no route), `unsupported` (pick up/carry/drop, toggles run and ai_off; follow and toggle stealth are supported since 2026-10-31, section 12; follow answers `bad_target` for itself, a dead target or a loop of followers
  other than block mode, drive on a server without a navmesh), `unknown_order`. A malformed or truncated order
  frame is a protocol error (`ERROR`, close). Every event 10 carries the order's clientSeq, `no_route` included.
  The `queue` bit of move_to is ignored (the order replaces; PROVISIONAL). `stop` with hold = 1 stands the character
  still (no chasing or footwork) until the next order.
- **move_to** is routed by the server on its navmesh (section 12) and walked along the route at the
  character's run speed (UNSURE: no walk/run choice, no avoidance); the route goes out as CHAR_ROUTE. The
  route is asked at the character's own clearance radius (the race's `pathfind footprint radius`,
  `Simulation::pathRadius`; NAVMESH_CACHE.md "Clearance"), so a click whose only way is a gap narrower than
  the body is `no_route`.
  character does not fight until it arrives, except that a running attack clip finishes first. A route that
  is blocked mid-walk (a door closed, an edge) is abandoned (n = 0 clear) and counted. **attack** keeps the
  target at any distance until it is down; the chase is footwork steered straight at the target but moved
  through the same navmesh step, so it slides along edges and cannot leave the mesh (UNSURE: it does not
  route around obstacles yet); beyond 50 u the chase follows a route at the same clearance radius. All are sim state (`state::Fighter`, save v11). The server routes with one
  Detour search over the whole mesh; the navigator's local/long split (`localRouteFor`) does not exist on
  a single mesh.
- **Navmesh at startup.** The duel scenario takes orders, so the server loads a navmesh (no navmesh, no nav):
  by default it bakes the Hub zone (cell 20,32; 324 tiles, 6,703 polygons) from the game data at startup, about
  3.5 s on 4 threads (a full server start with the default world takes about 6.5 s, of which ~3 s is the world).
  `--navmesh FILE` reads a `knav_bake --out` cache instead (about 0.1 s). Tests bake or use the knav
  fixture; nothing outside the repo and the game install is read.
- **DETAIL_BOUNDS:** the view box is the focus zone (4608 units) plus its 8 neighbours (set_view, a logged sim
  command saved as `state::ViewFocus`); each living player-controlled character adds a bubble box of half-width
  390 (340 + 50) centred on its position snapped to a 100-unit grid, so the set changes rarely. The server
  sends only characters inside the boxes AND inside the interest filter (default: 1000 units round any
  player-controlled character; `set_interest` replaces it with the circle given). A character leaving either
  gets event 11. The sim itself does not yet change its simulation detail with these bounds: the duel's
  characters are always simulated.
- **SQUADS:** every squad with a position, by Id (`--squad-limit N` keeps the first N, for small samples), 1 Hz.
  The squads move only when the server runs with `--bake` (the squad AI needs a navmesh bake); otherwise they
  stand where the default world put them.
- **Order log:** `--order-log FILE` writes one line per accepted order (`tick= seq= sim|nosim text`). The sim
  commands in it plus the start state replay the session exactly (test `ksim_simlink`).

## 11. Untrusted input (kenshi-b0, 2026-10-31)

Client bytes are never trusted. Any client fault gets that client one `ERROR` frame and a close (the
server waits at most 2 s for the client to read it); the process, the sim and the listener carry on and
accept the next client. The sim only ever sees fully validated `Command`s, and a frame that fails
validation changes nothing (pause, speed and interest take effect only after the whole frame validated).
An exception thrown while handling a client's frame or building its snapshot is logged to stderr and
treated as that client's fault. Code: `simlink.cpp` (`Session::onFrame`, `handleOrder`), `simlink_server.cpp`.

**Limits (protocol errors, kick):**

| What | Limit |
|---|---|
| Client frame (`len` field) | 2 .. 65536 bytes (`kMaxClientFrame`). Server frames stay up to 16 MiB. |
| Ids per order | 256 (`kMaxOrderIds`) |
| Any float in a client frame | must be finite; NaN or Inf is "malformed order" |
| Trailing or missing bytes in an order | "malformed order" |
| Orders per wall second (all kinds; PING excluded) | 100 (`kDefaultOrderRate`); the 101st in a one-second window kicks |
| NEW_GAME (111): n | at most 16 (`kMaxSquadSpec`), a Spec's sliderCount at most 64 (`kMaxSliders`), each string at most 256 bytes (`kMaxClientString`); more, a bad shape, or a second NEW_GAME while one is pending = kick |
| Unknown frame type, or a second HELLO | kick |

**Out-of-range values are answered, not kicked** (event 10, reason `bad_value`; the order has no effect):
x and z outside +-294912 (`kWorldExtent`) in `move_to`, `set_view`, `set_interest`; `set_interest` radius
not in (0, 100000]; `set_speed` not in (0, 64]; a u8 flag (`hold`, `paused`, toggle `value`) above 1, move_to `flags` with a bit above 1 set, or a non-finite `y`.
Ids that are unknown, dead or not player-controlled stay `unknown_id` / `dead` / `not_player_controlled`.

**Timeouts (wall clock):**

- HELLO: 10 s from connect to a valid client HELLO, else kick ("HELLO timeout").
- A frame that has started must complete within 10 s ("frame timeout").
- Keepalive: **clients must send `PING` (type 4, empty payload) every 5 s.** After HELLO the server kicks a
  client from which it has received no frame of any type for 30 s. There is no idle timeout otherwise: a
  watching client may send nothing but PINGs for minutes. The server answers PING with nothing. PINGs do
  not count towards the order rate. A PING with a payload, or before HELLO, is a protocol error.
- A client that stops reading is dropped once 64 MiB of output is queued.

Only one client is served at a time; the timeouts are what stop a silent socket holding that slot.
Test: `ksim_simlink` (socket faults, then 800 fuzzed frames: truncated, oversized, NaN, bad id counts,
random types; rejected frames must leave the state hash, order log, pause and speed unchanged).

## 12. Movement authority and direct control (kenshi-b0, 2026-10-31)

**Streaming mesh (2026-11-02):** with `ksim_server --navworld DIR` the server's mesh follows the players' areas, so tiles come and go
over a session: a new tile is NAV_TILE, a zone that left is NAV_TILE size 0 for each of its tiles, a tile that returns has a higher
version (whole tile, no patch chain); `meshId` is unchanged by a swap, NAV_PARAMS is not re-sent. Details and costs:
`docs/formats/NAVMESH_CACHE.md` "Streaming coverage".

**Implemented (2026-10-31):** server routing and `no_route`, the movement step library
(`knav/walk.hpp`, `knav::Walker`), the off-mesh correction with event 12, CHAR_ROUTE, y/`yValid`, and order 20
drive end to end on the server (state, `driveSeq`, AI-off hold, blocked event 8, stop on disconnect). Not done:
the navmesh tile stream and an `AI off` toggle (`follow` and the stealth toggle: below) (`hold` stands in for it). Attack chases are routed
and blocked walks re-route (below). Notes on the code:

- `knav::Walker` (`cpp/include/knav/walk.hpp`) is the shared movement step: `step` (slide by a displacement),
  `drive` (heading, gait speed, backpedal 0.5 UNSURE), `follow` (a route, reached exactly), `snap`, `onMesh`,
  `surfaceY`. It needs only a `knav::NavMesh`, no flecs and no sim world, so the client can link it. Deterministic
  for one mesh and one platform (Detour floats).
- Correction: at the start of every combat step (twice per tick) a living character not within 1 unit of the
  mesh is snapped to the nearest mesh point within 256 units, and event 12 is emitted; `CombatArena::
  positionCorrections()` counts them (the server prints the count when it stops and logs each one). Normal play
  needs none (tests assert zero).
- Disconnect: the server enqueues a drive stop (logged in the order log) for every driven character.
- Drive orders count against the order rate once per character per tick (coalesced); a hard cap of 20 x the
  rate on raw drive frames per second applies (`kDriveRawFactor`).
- **Turn in place (agreed 2026-11-02, b0 + client). Implemented 2026-11-02 with the walker kinematics slice (ARCHITECTURE.md "Walker kinematics"; `Fighter::driveTurn`, save v51; test `ksim_walk_kinematics`).** Drive 0
  means "don't translate"; its heading is still honoured. A character in the drive hold turns toward `heading` with no
  movement, at the original's facing rate (20 rad/s; the 4 rad/s combat-ready rate is not used by a drive yet; kinspect `findings/move_kinematics.md`). The
  turn needs no new wire value: the heading rides on the drive 0 order as before, and a drive 0 only turns a character that is in the drive hold
  (a drive forward / backward put it there; a character never driven ignores the heading). Walking and driving forward also face at 20 rad/s now.
  A drive 0 at the current heading is a no-op, so a plain stop is as before; the small settle turn when W is released with
  the camera's heading is expected. The client sends held A/D as headings at 2 rad/s, at most once per tick. No new value.

The rules below are the design this implements.

**Click destinations** (2026-10-31; MEASURED, `docs/reference/MOVE_CLICK_LIVE.md`). `MeshNavigator::routeFrom` snaps the
click to the closest walkable point within `kClickReach` = 340 units (the original finds 347 diagonal / 330 vertical and refuses
354 / 340: bracket 335-347; 340 is the pick). With `yValid` set on the order the distance is 3D from the clicked surface point
(so the deck or floor under the click is chosen); with it clear the box is horizontal and any height counts. The original
accepts only a complete path: a goal the search cannot reach, or one in another connected area (hilltop, island), is
`no_route` (the original drops the order silently; the client flashes red). The old "walk to the closest reachable point of a
partial path" is retired (`cpp/src/ksim/retired/`). CHAR_ROUTE `flags` bit 1 (approach) is still set whenever the route's end is
more than 0.75 units (xz) from the click (the snap). The Detour pool for these queries is 32768 nodes (the original runs
100000 A* iterations); a search that still runs dry is `no_route`. **Group orders:** only the first id gets the exact point;
each other member gets click + r (sin a, cos a), r = u1 sqrt(N x 60) + 5, a random angle; the draws are seeded with the
member count (the original's srand(count), kinspect 0789d56: deterministic per count and selection order, identical on every click), snapped like a single click. **Deliberate deviation:** the original's vertical-ray fallback (a click
340+ above the mesh keeps its y and the character loiters under it) is not copied; ours refuses such a click. Test: `ksim_move`.

**Chases and reroutes** (2026-10-31). An attack chase (the attacker steering at a target beyond its ring) follows
the follow task's rule (`docs/reference/SQUAD_FOLLOW.md` s4): straight steering below 50 units, a navmesh route
beyond 100, and in the 50 to 100 band whichever it was doing (hysteresis; the flag starts set). The chase route
is sent as CHAR_ROUTE with `clientSeq` 0 and the approach bit, and cleared when the chase goes direct or ends.
It is re-planned when the target is 30 units from where it was at the last plan (at most every 0.5 s), when the
route runs out short of the target, and after a block. A walk (move order or chase) blocked mid-way re-routes at
most 3 times; then a move order is cleared (the old abandon), and a chase stands for 2 s before it plans again.
**UNSURE:** the 50/100 rule is borrowed from follow, the 30 and 0.5 s are guesses. Test: `ksim_move`.

**Follow and fight join** (2026-10-31; `docs/reference/SQUAD_FOLLOW.md` s10). Order 3 starts the follow task (slot
in the leader's frame, speed cap, direct/path steering, give-up monitor); toggle 1 (stealth) sets sneak mode, shown as CHAR_DYNAMIC
flag bit 1, which followers copy from their leader. Any other order replaces the follow. A player-controlled character does not pick
fights itself: it joins one only when idle and not sneaking, as PROTECT_ALLIES does. Option `squad.follow_heading_fix`
(`ksim_server --follow-heading-fix`, default off = original). Scenario `--scenario squad`. Scenarios `duel`, `duel_katana`, `duel_armour` (the duel with the servant in full-coverage plate, one heaviest plate piece per hat / body / legs / boots chosen from the data by fields; event 1's absorbed tail is then non-zero) and `squad`: see RUNBOOK "ksim_server scenarios".

**Pause when nobody is connected** (`SimLinkServer`, `ServerContext::idle`; the single-server default of MULTIPLAYER 6.2): the sim does not tick while no session has completed HELLO. The server starts paused (CLOCK says paused), resumes at the first ready session (before its initial snapshot, so that CLOCK says running), and pauses again when the last ready session disconnects or is dropped. The speed is never touched, so resuming restores it (1 on the first start). An admin's explicit pause order (`ServerContext::paused`) is separate and wins: a joining session does not unpause it, and CLOCK `paused` is the OR of the two. `ksim_server --run-without-clients` (`ServerOptions::runWithoutClients`) keeps it ticking with no client; a server config will replace the flag later. Tools that do not use `SimLinkServer` (`ksim_soak`, `ksim_server --dump-snapshot`) are unaffected. Test: `ksim_simlink`.

**Pathfinding is the server's alone.** A `move_to` is routed by the server on its navmesh. The answer
is either the route (waypoints, sent to the client so it can draw the path line and interpolate) or
event 10 `no_route`.

**No navmesh, no nav.** There is no straight-line fallback anywhere. An order the server cannot route
is rejected. A character found off the navmesh is in a bug state. A correction step snaps it to the
nearest navmesh point and logs a `position_corrected` event with the distance; it is counted in soaks
and never silently absorbed. Scenarios that take orders (the duel included) load a navmesh.

**Direct drive** (decided 2026-10-31; replaces the speculative notes in ARCHITECTURE.md rule 4 and
CLIENT.md where they differ). The model is a sticky drive state sent on change, not a per-tick input
stream. This fits the rest of the protocol: orders are reliable logged sim Commands applied on the
next tick, and the link is TCP.

- **Order 20 `drive`:** `id u64, drive u8 (0 stop, 1 forward, 2 backward), heading f32`. One order
  carries both, so a turn and a start are never split. The client sends it when either changes. Held
  W/S maps to forward/backward and release to stop; a toggle key gives the original's sticky autorun;
  A/D or mouse turning sends new headings. The old `tick u64` field is dropped, because the server
  stamps every order with the next tick (event 9 reports which).
  **Strafe (approved by the client 2026-11-14, kenshi-k16.8.15; implemented k16.8.17; no protocol bump):** an
  optional trailing `strafe i8` (-1 left = towards heading - 90 degrees, 0 none, +1 right; any other value is
  `bad_value`). The body length decides: the old 13-byte body is strafe 0. `heading` stays the FACING, held (a
  strafe, like forward, turns the body to it). Moving = drive != 0 or strafe != 0, so strafe with drive 0 is a pure
  sidestep (still the AI-off hold; no turn in place). The local velocity is (forward +1 / back -0.5, side +-0.5
  (`knav::kStrafeFactor`, MINE / UNSURE like the backpedal)) x gait, normalised to length <= 1: W+A = 1.0 x gait,
  S+A = 0.707 x gait, strafe alone 0.5 x gait. Sticky and coalesced like the rest of order 20. CHAR_DYNAMIC echoes
  it in flags bit 22 (strafe left, `kFlagStrafeLeft`) and 23 (strafe right, `kFlagStrafeRight`); `drive`, `heading` and
  `driveSeq` are unchanged. The client picks the clip from (drive, strafe) and predicts with `knav::Walker::drive`'s
  `strafe` parameter. Saved from save v111.
- **Rate:** at most one drive order per character per tick is applied; later ones in the same tick
  replace it (coalesced, last wins). Drive orders count towards the section 11 order rate only after
  coalescing, so mouse steering at 20 Hz is fine. A client should still not send faster than the
  tick rate.
- **No resend loop.** TCP does not lose orders, so "resend until the snapshot matches" is not needed.
  Event 9 acknowledges each order with its `clientSeq` and tick. CHAR_DYNAMIC's `drive, heading`
  show the applied state, plus a new `driveSeq u32` (the last applied drive order), which the client
  uses to reconcile its prediction. On disconnect, the sim stops that client's driven characters.
- **AI and orders:** forward or backward puts the character in the non-terminal AI-off hold (the same
  hold as toggle 3). `stop` leaves it standing in that hold. Any ordinary order (move_to, attack,
  follow) clears drive and runs as usual; toggling AI back on resumes goal selection.
- **Speed:** forward uses the character's current gait (walk, run, sneak toggles). Backward is a
  backpedal: facing unchanged, at a reduced speed. The factor and the clip come from game data; until
  measured, use 0.5 and mark it UNSURE.
- **Prediction:** both sides step the character with the **same movement step code**, a shared library
  with no sim-world dependency (like `kworld::dataHash`), from (tick, position, drive, heading,
  speed). The client applies its own changes at once and smooths toward the server's state.
- **Navmesh:** a driven character moves on the navmesh like any other mover. It slides along edges,
  never leaves the mesh, and stops at walls and cliffs, which the server reports with a `blocked`
  event.
- **Navmesh tile stream (scheduled; layout decided 2026-10-31):** the server sends the navmesh for the
  full-detail area (section 5: the camera zone plus 8 neighbours). The client uses it only to predict
  sliding and blocking and for previews (cursor validity, path preview, route y on decks and stairs),
  never as an authority. A modified client cannot walk through walls, because the server's step
  decides. Frames (server to client):
  - **NAV_PARAMS (type 43)**, once after HELLO and whenever the baked mesh is replaced: `meshId u32`
    (identifies the bake; tiles from another meshId are dropped), `detourVersion u32`
    (DT_NAVMESH_VERSION; the client ignores the stream and logs once on a mismatch), `origin f32 x3`,
    `tileWidth f32`, `tileHeight f32`, `maxTiles u32`, `maxPolys u32` (the dtNavMeshParams the server
    used). A new meshId clears all of the client's tiles.
  - **NAV_TILE (type 44)**, on a tile entering the full-detail area and on a rebuild: `meshId u32,
    tx i32, tz i32, layer i32, version u32, size u32, data u8 x size`, where data is the Detour tile
    blob exactly as baked (dtCreateNavMeshData output, little-endian). `size 0` removes the tile (it
    left the area or no longer exists). `version` counts rebuilds of that tile (construction, terrain
    edits); a lower or equal version than the client holds is ignored. Tiles are paced at no more
    than 256 KiB of tile data per tick, nearest the camera zone first.
  - **NAV_POLY_FLAGS (type 45)**, on a cheap change that is not a rebuild (doors opening, closing or
    locking: knav::paintDoor): `meshId u32, tx i32, tz i32, layer i32, version u32, n u16,
    n x {poly u16 (index in the tile), flags u16, area u8}`. `version` is the tile's version after
    the patch: the client applies it only when it holds exactly `version - 1`, and its copy then takes
    `version`. Anything else is dropped (a NAV_TILE is coming). Rebuilds and patches share one
    per-tile counter on the server, so every change bumps it by one.
  **Implemented (2026-10-31).** `knav::NavMesh` owns the counter (`tileVersion`, 1 as baked; +1 per door repaint that changed the tile
  and per `replaceTiles` rebuild) and a log of the last 64 patches per tile (`tilePatches`). NAV_TILE carries the tile's current
  version and its blob with the current door paint on the door polygons (`tileBlob`), so a tile sent late is already painted. A
  session remembers the version each client holds; every tick it sends, per held tile that moved on, one NAV_POLY_FLAGS per version
  step in order (the polygons that changed in that step: `poly u16, flags u16, area u8`), or the whole tile again (NAV_TILE with the
  current version) when the chain is cut (more than 64 steps behind, or a rebuild). Updates to held tiles go before new tiles
  in the pacing. `layer` is 0. The server paints with `NavMesh::setPolyFlags(tx, tz, layer, poly, flags, area)`, which the client
  may use as well (also `NavMesh::fromParams(DetourParams, tiles)` builds the client's mesh straight from NAV_PARAMS).
  The tile stream follows the detail bounds; it is not sent to a client that has not been sent
  DETAIL_BOUNDS. No protocol bump: older clients skip unknown types (section 2).
- **Combat while driving** uses the AI's rules: an attack picks a technique by the measured
  attack-start rule (MEASURED item 18), and the impact box decides the hit (item 17).
- The follow view is 3rd person only (decided 2026-10-31; a full 1st person view does not suit Kenshi). It is only a camera and changes nothing in the sim. It has two uses:
  following a character while its AI (or the last click order) drives it, which is scenic and needs no
  orders; and direct drive. Unlike the original's follow camera, ours rotates with the character, so
  the view stays from that character's perspective. It steers by CHAR_DYNAMIC yaw (and the route for
  look-ahead), so yaw must stay smooth and stamped. Camera position still drives detail through
  `set_view` (section 5).

## 13. Picking and interaction (kenshi-b0, 2026-10-31; implemented for doors and ground items)

**Implemented (2026-10-31):** order 60 `interact`, INTERACTABLE records (section 6), events 7 and 13 (section 6), code
`cpp/include/ksim/interact.hpp`, test `ksim_interact`. Order `n u16, ids u64 x n, target u64, action u8, queue u8`.
Actions: 1 open, 2 close, 3 lockpick (implemented 2026-10-31), 4 use*, 5 loot, 6 talk (**implemented 2026-11-02**: a merchant's character opens a trade, TRADE 53; since 2026-11-04 any other conscious character hears EV_PLAYER_TALK_TO_ME on arrival (a dialogue, maybe a reply menu 43; a busy speaker whose running line set an `interrupt` opens that root instead), and when nothing opens, a second answer to the same clientSeq follows event 13: event 10 `nothing_to_say` (`not_trader` is gone); `not_allowed` for one that is down, asleep, or a merchant whose faction is hostile to the player faction (relation at or below -30), `bad_target` for a non-character or yourself; event 13 result 0 on arrival), 7 pick_up, 8 carry (lift, implemented 2026-10-31, see below), 9 sleep (bed: target = an INTERACTABLE kind 5 id; **implemented 2026-11-06**: the refusals `occupied` / `bad_target` / `not_allowed` (down, carried, carrying, in a fight), the walk to the bed's use point, the rules checked again on arrival, then the lie-down -- `Fighter::asleep`, flag 16 with the pose tail, event 13 result 0; any replacing order wakes the sleeper and clears the bed target; test `ksim_player_sleep`), 10 first_aid, 11 unlock (only a character of the door's building faction, isObjectMine; 3.3 s), 12 bash (a shut door, locked or not: hits until it breaks), 13 put_down (new 2026-10-31: target = the character the actor carries) (4 use is reserved:
answered `unsupported`; 3 lockpick: a locked door or locked container (task AL), any hands, attempts as below, `not_allowed` on one that is not locked, `bad_target` on anything else; 5 loot also takes everything from a **container**, see below; a locked container refuses it with `locked`). `queue` must be 0 or 1. **Order queue (implemented 2026-10-31, save v20):** queue 1 (interact), and move_to flags bit 0, append the order to the character's queue (`state::OrderQueue`, saved); a queued order is routed from where the character stands when it starts, and it starts when the last one has ended (no interact task, walk, drive, follow or attack order running). Queued orders run in order. An order without queue clears the queue, and event 13 result 5 (cancelled) is sent for the running interact and for each queued interact, so every interact gets exactly one event 13; a character that is KO'd or dies loses its queue the same way (4 for the running order, 5 for the queued). A queued interact that has nothing to do when its turn comes ends at once with 1 (refused), 2 (no route) or 3 (target gone); a queued move with no route is dropped without an event. Validation of a queued interact skips `not_allowed` (the target's state at order time proves nothing: close a door after opening it). **CHAR_ROUTE needs no queued count:** it shows the route being walked; the client keeps its own list of what it queued and learns of each end from events 13 (a queued move has no end event). Test: `ksim_interact` (two characters, Katana queue 0 then Bread queue 1: each takes one and gets target_gone for the other). Validation (event 10):
`bad_value` (action 0 or above 20, queue above 1; actions 14 to 20 also with queue 1), the section 11 id reasons for the actors, `bad_target` (no such id, the
action does not fit the target's kind, loot on yourself), `not_allowed` (open on an open, locked or broken door, close on a shut or broken one, unlock a door that is not locked or not the actor's faction's, bash an open or broken door, loot on a standing character, first aid on an unhurt one), `unsupported` (no navmesh, or a reserved action),
`no_route`. A damaged door still opens; a locked door refuses (lockpick it, or unlock it as its owner). The server routes each actor to the use
point (CHAR_ROUTE, approach bit) and checks the rules again on arrival (within 12 units, UNSURE); the actor then acts and
event 13 follows. Any other order replaces a running interact. Picked-up items go to the character's inventory (INVENTORY,
section 6). **First aid (action 10, implemented 2026-10-31):** the actor walks to the patient (itself allowed: the target is its own id) and then treats tick by tick until nothing is left to bandage; event 13 (result 0) follows when the treatment ends, or 4 if the actor is KO'd. It uses the first carried first aid kit (ITEM `item function` 1, or 12 for robotic parts; UNSURE: the original kept the first one in the inventory), opens one kit at a time (its charges drain at lerp((100 - skill) / 100, 0.1, 2) per second), fills each part's bandage pool in the order head, torso, arms, legs at lerp(min(skill, kit quality) / 100, 1, 15) x 3 per second (skill x0.8 on oneself), and clots wounds faster. Health follows from the pool at 0.0125 HP/s per 100 max health (docs/reference/MEDICAL_HUNGER_LIVE.md c, d). Without a kit nothing happens and the order ends at once (result 0). **Loot (action 5, implemented **Loot (action 5, implemented
2026-10-31; containers 2026-10-31):** on arrival at a KO'd or dead character the looter takes everything (take all, no looting UI): every entry of the
body, equipped pieces too, goes into the looter's pack, the body is left with bare hands and no armour (its CHAR_APPEARANCE
changes), then event 13 done. On a **container** (INTERACTABLE kind 3) loot takes every stack (a walk to its use point, then everything goes into the looter's pack; the container's INVENTORY version bumps; event 13 done; an empty container is allowed and moves nothing; a **locked** one refuses at once with `locked` (event 10), until order 60 action 3 opens it). Taking from a container whose owner faction is not the looter's (`state::Allegiance`) is theft: event 14 after the take. The owner is the building's own owner faction, else the nearest town within 1,500 units (UNSURE, as for doors). **Transfer (order 63)** moves `count` of one item between a container and a character within 16 units of its use point, either way; the same theft rule applies to a take; a locked container refuses both ways (`locked`), a box farther than 16 units is `out_of_reach`, a box with no room for the stack is `full`, and a box with an `itemtype limit` (2 weapon, 3 armour) refuses a put of any other kind (`item_type`; see the kind 3 section). **Drop (order 61, `id u64, item str, count u16`):** the character puts `count` carried units of
`item` on the ground at its feet at once (no walk): event 6, a new ground-item INTERACTABLE, INVENTORY version + 1. Answers:
`bad_value` (count 0, empty or over-long item), the section 11 id reasons, `not_allowed` (fewer than `count` carried; an equipped
piece cannot be dropped yet). Does not replace the current order.

**Stealth KO (action 14; approved by the client 2026-11-10, implemented 2026-11-10; `Simulation::orderStealthKo`, `Simulation::checkStealthKo`, `Command::stealthKo`; test `ksim_simlink`; STEALTH_KO.md; the client's `knockout` cursor 0x19).** Not an interact: the target is a character, no walk is routed by the order (the assassin walks up behind it itself, the task of row 228) and `flags` and `queue` must be 0 (`queue` 1 is `bad_value`). Answers, per actor, at once like the other refusals: `bad_target` (no such character, or itself), `not_allowed` (the actor is down, KO'd, carried, mounted, caged or has no working arm; the victim is dead, KO'd, down or carried), the section 11 id reasons for the actors. An accepted order is a replacing order (a running walk, interact, queue and follow end) and is applied at the next tick (event 9); the outcome is the usual KO event 3 (and EVENT 48) when it succeeds. Any action above 14 is `bad_value`. UNSURE (default: mirror the other order-60 reasons): the attempt's outcome and chance are event 50 (the event table).

**Shackle orders (actions 15 / 16 / 17; client-approved 2026-11-11, kenshi-h2v.15.1, implemented; `Simulation::orderShackleAction`, `Simulation::checkShackleAction`, `Command::shackleAction`, `lock_tasks.cpp`; test `ksim_shackle_orders`).** Not interacts: `target` is the character wearing the locked shackles (the actor itself, or another character the actor walks to; reach 4 u), `ids` the actors. 15 pick_lock runs task 201 PICK_LOCK_ON_SHACKLES (the lock's timer, a roll every interval at `pickChance(level, lockpicking)`), 16 use_tools cuts (185 CUT_SHACKLES: needs a carried TOOL item, drains the hardness at `cutRate(strength)`, removes the shackles when it is through), 17 use_strength breaks (186 BRUTE_FORCE_SHACKLES: instant, strength above 3 x hardness, the shackles stay worn and open). A picked or broken pair stays on, open; a cut pair is gone. Order-time answers (event 10): `bad_value` (queue 1, any flag, action above 17), the section 11 id reasons, `bad_target` (the target is not a character), `not_allowed` (the actor is dead, KO'd, down or carried, or the target has no **locked** shackles: none worn, or already open). A replacing order (any order that is not queued) cancels it. Event 13 (action 15 / 16 / 17, `target` = the wearer) ends it, exactly once: result 0 done, 1 refused at run time (a cut with no tool, a break with too little strength), 3 the wearer is gone, 4 the actor KO'd or dead, 5 cancelled. The bars are TASK_PROGRESS kind 1 (pick) and kind 7 (cut; used only for the shackles, see there), shown once the actor stands beside the wearer. The lock state is ITEM_LOCK (type 57). Freeing another's shackles is crime 7 (slave freeing) against the captor, every tick, as for the planner's own 201 / 185 / 186. UNSURE: the level 20 and hardness 30 of the shackles' lock; the wire shows the **wearer's** chance, so a rescuer's own chance is not shown. Not saved: a load drops a running order (the lock's timer is saved, so the pick resumes). No save bump.
**Cage orders (actions 18 / 19 / 20; client-approved 2026-11-13, kenshi-k16.5.6, implemented kenshi-k16.5.7; `Simulation::orderShackleAction` (the same entry, `target` = a cage key), `Simulation::cageOccupant`, `lock_tasks.cpp`; test `ksim_cage_wire`).** The shackle orders' twins on a **cage's lock**: 18 pick_cage (task 76 PICK_LOCK), 19 cut_cage (285 CUT_LOCK, needs a carried TOOL item), 20 break_cage (286 BRUTE_FORCE_LOCK, strength above 3 x hardness). `target` is the **cage key** (CAGES `cage`), not a character: 15 / 16 / 17 name the wearer of shackles, and a prisoner can be both caged and shackled. `ids` are the actors; the actor walks to a character caged there (reach as for a character, `lockReach`) and works the lock from beside it; **a caged actor works its own cage from inside**. A caged picker's skill is 10 lower (`kCagePickSkillPenalty`), as the planner's own 76. Order-time answers (event 10): `bad_value` (queue 1, any flag, an action above 20), the section 11 id reasons, `bad_target` (`target` is not a cage key: no character is caged in that building, which a character id, a door or a container is not), `not_allowed` (the actor is dead, KO'd, down or carried, or the cage's lock is not locked). A replacing order cancels it. Event 13 (action 18 / 19 / 20, `target` = the cage key) ends it exactly once: result 0 done (the lock is open; CAGES is resent), 1 refused at run time (a cut with no tool, a break with too little strength), 3 gone (nobody is caged there any more), 4 the actor KO'd or dead, 5 cancelled. The bars are TASK_PROGRESS kind 1 (pick) and kind 7 (cut) with **target = the cage key**, once the actor stands at the cage (the kind list above). Freeing another faction's prisoner is crime 7 against the captor, every tick, as for the planner's own 76 / 285 / 286 (a building lock someone else owns, with no captor, is crime 2). Not saved: a load drops a running order (the lock's timer and hardness are saved, so the pick or cut resumes). Order 60 action 3 stays doors and containers only. No save bump.

**Carry (action 8 lift, action 13 put down; implemented 2026-10-31, save v26; GATES_CARRY_LIVE.md c, GATES_CARRY_STATIC.md 4 and 5; `ksim/carry.*`).** Lift: the target is a KO'd or dead character nobody carries (`bad_target`: not a character, yourself; `not_allowed`: it stands or is carried already, the actor is down, carries somebody or is carried; an ally may be lifted). The actor walks up (run, the usual route and approach), stands still and lifts for **2.343 game-s** (the "shoulder lift left" clip, 2.4667 s; rwh.114): the body attaches at 40 % of the clip (0.987 s: `carriedBy` is set, the carrier's flag bit 7 comes up) and the task ends at 95 % (event 13, action 8, result 0). A new order that replaces the lift stops it (before the attach nothing happened; after it the carrier keeps the body). The carried character has no movement of its own: its position is the carrier's every tick, it is nobody's target and in no building; a carried person that wakes up is dropped on the spot (UNSURE). A carrier that is KO'd, killed or knocked down drops the carried at once, with no timer. Put down: action 13 on the carried character (`not_allowed` when the actor does not carry it, `bad_target` when it is not a character), **1.973 game-s** (the 2.4667 s clip played backwards at rate -1, released at 20 %), then event 13 (action 13, result 0); the carried lies where the carrier stands, and a **non-ally** (different team, faction not allied at relation 50) gets a forced KO timer `((maxBlood - blood) x mult x 2 + 2 x knockout time base) x param x 4`, at least 3 s (`mult` from the carried's toughness, `param` = lerp(0.1, 0.4, carrier strength / 100): UNSURE which stat), never shortening a longer one; an ally gets none. **Encumbrance:** a carrier weighs its pack (`weight kg` of every inventory entry, equipped too) plus a fixed `carry person weight` (30, from GLOBAL CONSTANTS) whoever it carries; capacity = `carry weight mult` (1.2) x effective strength x vf10 (UNSURE: 1) + `encumbrance base` (15); ratio = clamp01((capacity - 0.05 W) / (0.95 W)) goes into `Fighter::encumbrance` (also read by dodge / damage / hunger) and the run speed becomes (base - 11) x ratio + 11 (`knav::Gait::encumbrance`, 11 the exe's floor; the gait ramp is unchanged). Strength 30 with an empty pack: ratio 1, the unladen run speed. Only carriers are weighed; the ratio of an ordinary pack stays 1 (OPEN). An NPC carrier asked to walk goes at move mode 1, capped at 55. **NPC take-out** (the original's goals 223 PICKUP_INTRUDERS -> 224 TAKE_INTRUDER_OUTSIDE, `Simulation::stepIntruders`): once a second a non-player fighter inside a building whose doors belong to its own faction (`state::Allegiance` equals the doors' `ownerSid`), not fighting, finds the nearest KO'd, non-ally, non-member character in the same building within 60 units (UNSURE: the original reads its sensed list), walks up, lifts (action 8, event 13 as above), walks it out at the walking gait through the nearest door of the building (the walker's door steps open or unlock it as for any NPC; nobody closes it) to `outside + 1.1 (outside - inside)` of that door (the points 14 units either side of its centre: ~45 units beyond it, as item 15 measured) snapped to the mesh, and drops it once outside the building and idle, after 5 game-minutes outside, or at once when its walk ends inside, it is in a fight or 15 minutes have passed (no-terminal-state caps). That drop has no animation and starts no forced KO timer. Not done: the town version (273 / 274), cages and jailing, crime / trespass events, a fighter's initial building (scenarios set it).

**Doors as the original does them (implemented 2026-10-31, save v21; DOORS.md section 6 is the measured source).**
`state::Door` is the owner of a door's state (0 open, 1 closed, 2 locked, 3 broken) in a navmesh scenario;
`Simulation::syncDoorPaint` (after the commands and after the interact step, every tick) builds a `knav::DoorBook` from the door
entities and repaints the door polygons when it differs (areas DoorOpen / DoorClosed; a locked door denies every traveller class;
a broken door is painted open). Rules (UNSURE marks what the measurement does not give):
- **Closed unlocked door:** the search treats it like an open one (`kDoorClosedCost` is 1; the old 11 per unit is retired), and the
  walker adds the open step: a character walking a route stops in front of a shut door whose centre lies within 14 units (plus two
  ticks of its speed) and within 8 units of the rest of its route (UNSURE: the original's approach points are not read), opens it
  (item 19: openness grows by `door move speed` per game-second (0.5 when the record has none), so an open or close takes
  1 / speed seconds; the door's state and the navmesh flip when openness crosses 0.6, the step ends at 1.0; a speed of 0 in the
  data is floored at 0.1) and walks on. INTERACTABLE `health` / `maxHealth` are the record's HP (Σ `construction` counts, else
  `build materials`: 16, 12, 90, 15 ...), not a fixed 16; the layout is unchanged. While it opens, CHAR_ROUTE shows a clear (n = 0) and then the
  resumed route (a new routeVersion); CHAR_DYNAMIC shows the character standing. A character whose interact order targets that very
  door acts on it instead (a timed open, close, unlock or bash; event 13 when the step ends).
- **Locked door:** locked for everyone. A **player's** move or interact order never plans an unlock: a goal behind a locked door is
  `no_route` unless a detour exists, and a walk blocked by a door that locks meanwhile re-routes or ends. A character that plans
  its own walks (not player controlled: an AI move; the route is computed with `MeshNavigator::routeFrom(..., planned = true)`,
  locked doors passable) behaves by who it is: a member of the building's faction (isObjectMine: the door's `ownerSid` equals the
  character's `state::Allegiance`; the "faction owns it" branch for bought buildings is not modelled) **unlocks** (3.3 s, MEASURED)
  and opens; every other AI character plans against the door like the original's GOAP (DOORS.md section 6 "Bash vs pick vs wait",
  `ksim/door_plan.hpp`, the plan with the highest utility wins, utility <= 0 dropped, no dice):
  - BASH_DOOR 1.0: a melee weapon in hand (a carried one is equipped first, instantly: UNSURE), an owner that is not allied (relation
    >= 50), and: a hostile owner (relation <= -30, a factionless actor counts as hostile), or an owner faction whose FACTION
    `fundamental type` (CharacterTypeEnum) is 1 law enforcement, 2 military, 6 slave, 7 slaver or 8 bandit; traders (3), civilians
    (4, the data default), diplomats (5) and adventurers (9) score 0, and so does a door nobody owns. The reading of "the faction"
    as the door owner's faction is UNSURE (DOORS_LIVE.md says "a non-hostile door's faction"). Verified against the merged
    data: Mercenary Guild 2, Mercenary Police and Police 1, Trade Ninjas 3, Holy Nation and United Cities 4, Vagrants and the
    Starving Bandits 8, Nameless and Drifters 9; the table of all 103 factions is 38 bandit, 25 civilian, 11 none, 11 adventurer, 8
    military, 4 slaver, 2 law, 2 trader, 2 slave.
  - BASH_GATE (a door of an `is wall gate` / `is gateway` building, `state::Door::gate`): 1.0 / (squad-mates already bashing it + 1), only with
    fewer than 8 mates and while no melee attack runs against the actor; the same owner rules; no weapon needed (UNSURE, so unarmed raids
    keep bashing gates at 0.3 per hit). The squad is the faction here (UNSURE).
  - PICK_LOCK 0.1: empty hands (no weapon wielded) and lock level - Lockpicking < 41.7. It wins only when every bash is dead.
  - Otherwise wait (up to 60 s, UNSURE, then the walk is given up), planning again once a second.
  The old rule (any hostile non-owner bashes, with or without a weapon) is retired (`src/ksim/retired/interact_hostile_bash_rule.txt`).
- **Lock level** (`Door::lockLevel`, saved): the **building's** faction's `doors lock level` (default 40; merged data) plus the door record's `lock level
  bonus`, plus a deterministic draw in +-`lock level random` (default 10) from the door's key, clamped 1..100. The building's faction is the placement's `owner faction ID` (its local GAMESTATE_BUILDING in the zone file; `Door::ownerSid`, also what isObjectMine reads), else the nearest owned town's within 1,500 units (UNSURE: the data has no building -> town link), else "nofac" (40 / 10). Rolled once, never re-rolled when the owner changes (GATES_CARRY_LIVE.md b). Hub: Holy Nation Outlaws 30 / 15 -> storm house doors 15..45, shack -10 -> 5..35; the Hub also has Trade Ninjas buildings (40 / 10).
- **Lockpicking** (the AI's PICK_LOCK step and interact action 3): one roll per attempt, chance = 0.9 / 2^((level - skill) / 10)
  while level > skill, else 0.9 (the exe's Task_PickLock tick, FUN_140297e50; constants 0.9, 2 and 0.1 read from the exe; the 41.7
  limit is where that chance falls to 5 %). The skill is STATS `lockpicking` (`FighterSkills::lockpicking`, saved). **UNSURE: the
  time between attempts.** The exe waits a random interval between two bounds the lock record stores (the lock constructor takes
  them from the save record, so the data does not give them); ours draws 0.5-1.0 s (`kPickIntervalMin/Max`), which makes the live
  Vagrants thug's level-22 lock (about 360 frames, ~3.4 s at ~107 frames/s) come out near 3.5 s at skill 1 (chance 0.21, 4.7 attempts).
  The dice use the tick, the actor and the door (reproducible, different each time). A success unlocks (state 1); the AI then
  opens the door and walks on, the order ends with event 13. No crime is recorded (out of scope). Lockpicking XP (the exe adds the
  stat's rate x 1.2 on success, x 0.24 on failure) is NOT done.
  **Updated (item 19):** the interval is measured: the first attempt comes after 5 game-s, each failure re-rolls the interval
  uniform in [5, 20) (`kPickFirstInterval`, `kPickIntervalMin/Max`; `tickLock` in `door_plan.hpp`). Elapsed time, the current
  interval and the attempt count live on the door's lock (`state::Door` `lockElapsed`, `lockInterval`, `lockAttempts`): they
  survive an interrupted pick, are shared between pickers and are saved. Every picker's tick advances the lock once (UNSURE: the
  original ticks the lock from each Task_PickLock, so two pickers may count double). The dice use (door key, picker, attempt
  count). XP per attempt: `addXp(lockpicking, 1.2 / 0.24)` = ((101 - s) / 101)^2 x 1.2 (success) or 0.24 (fail); OPEN: the race
  multiplier is not read (1.0). The paragraph above about 0.5-1.0 s and "no XP" is superseded.
- **Interact reach of a wide door:** the reach (`kInteractReach`) is measured to the door slab's footprint (`INTERACTABLE` `use`
  stays the polygons' centre), not to its centre: a shut door's polygons are a wall to the walk, which ends at their edge. A
  lockpick order on the Defensive Gate used to end 12.4 units from the centre (`kInteractReach` 12) with event 13 result 2
  "unreachable" (kenshi-0f); `state::Door` `axisX`, `axisZ`, `halfLong`, `halfDeep` carry the footprint.
- **Bash:** each hit takes clamp(2 blunt + 0.5 cut, 20, 60) x 0.015 hp (the basher's weapon `blunt1` / `cut1`; unarmed = 0.3, as
  measured), one hit per basher every 2.7 s; the first hit comes 2.7 s after it starts. Door hp is 16 for every door (the Hub bar
  door; the data has no hp field, UNSURE). At 0 the door is **broken** (state 3): open for ever, cannot be closed or unlocked,
  its lock cleared; repair is out of scope. Every hit bumps the door's stateVersion (an INTERACTABLE record).
- **Interact actions** 11 unlock and 12 bash (see above); open (1) and close (2) are timed steps too. Closing a door whose record has
  `always locks` locks it again (gates).
- Door changes reach the client as NAV_POLY_FLAGS (section 12). Tests: `ksim_interact` (owner unlock + open timing, a stranger
  opens a closed door, a non-owner waits, a hostile non-owner bashes in the expected number of hits, a player's route refuses a
  locked door, unlock by order, always-locks re-lock, broken doors refuse close and unlock, patches in step, a closed door is
  walked through).
- **Owner** (`Door::ownerSid`): the faction of the nearest town within 1,500 units (UNSURE; the data has no link). `state::Allegiance`
  is set from the appearance faction by the duel and squad scenarios.
**Gates in the whole-world sim** (`ksim/doors.*`, no `state::Door`): same model. A closed gate is opened by any traveller unless it
is locked; an `always locks` gate is locked whenever it is closed, so on an Alert only those refuse strangers (hostile and neutral);
a plain closed gate refuses nobody. Nobody opens a gate for a traveller. A raid that the gates refuse **bashes** instead of failing
its order: it routes ignoring the gates, stops at the gate for the bashing time (a lone unarmed basher, 54 hits x 2.7 s = 145.8 s;
squad size, weapons and the per-gate hp are not read, UNSURE) and the gate is then broken for ever (a `DoorOverrides` entry with
state 3, SetDoorState accepts 3). The old rule (a refused raid route failed the order with a cooldown) is retired
(`src/ksim/retired/ai_raid_door_refused.txt`). Test `ksim_doors` (Hub bake: the refused raid keeps its order, stops, breaks the gate).
**OPEN:** gate crews (a man-the-gate crew reopens a closed gate about 600 frames after a close, DOORS.md section 6): the world sim has no guard squad stationed at a gate, so there is no crew to run it; the intruder carry-out (a guard carries a KO'd enemy out, unlocks while carrying, leaves the door open) needs lift and
carry, which do not exist; the original's approach side for doors; doors outside the Hub scenarios in the
character layer. Save v22 holds `Door` (with `alwaysLocks`, `moveSpeed`, `ownerSid`, `gate`, `lockLevel`), `Container`, `Allegiance`, `DoorStep`, `OrderQueue`,
`GroundItem`, `Inventory`, `InteractTask`. The duel and squad scenarios drop a Katana and 3 Bread by the player and spawn every door
of the Hub mesh.

**Navmesh tile stream (section 12)** is implemented: NAV_PARAMS, NAV_TILE with size 0 on leaving, nearest-first pacing at
256 KiB per tick; test `ksim_interact` rebuilds a mesh from the received tiles and its Walker steps equal the server's.
NAV_POLY_FLAGS is sent (section 12, "Implemented"): see the door rules below.

User requirement: everything clickable must feel natural from a UI point of view. That covers
characters (including downed bodies and animals), doors, machines (crafting benches, storage,
beds, turrets, gate controls), items on the ground, and the ground itself.

**Client: one pick pass, one answer.** The mouse ray is tested against everything pickable, and the
nearest hit wins. Pickables: character capsules, interactable shapes (doors, machines, containers,
ground items: their collision or bounds), then the static collision shapes and terrain for a ground
point. The same pass drives:
- hover feedback: the cursor icon for the default action (attack, talk, open, use, loot, pick up,
  move). The original has no per-object outline and no hover name tooltip (measured): only dropped
  item labels recolour on hover, and names show in the RMB-hold menu;
- RMB: the default action; hold RMB opens the action menu (FEATURES "RMB default action").
A small screen-space tolerance (a few pixels) makes thin or small things (items, door edges) easy
to hit. Characters win ties against the ground behind them.

**The original** (docs/reference/CURSORS.md, kenshi-0f 23ea476): hover dispatch 140800250 tries
ground items (1407fc440), then characters (1407fb410), then the building under the ray (1407fc6e0),
then the ground; if nothing takes it, the cursor is `invalidmove`. Every branch sets the cursor
(setCursor 1406e3d60, 26 pointers from `kenshi_pointers.xml`) **and** records the RMB default action
in the same decision, which is what our single pick pass does. Measured live (kenshi-inspect,
docs/reference/CURSORS_LIVE.md): the dispatch order is not a hit priority; the nearest ray hit wins,
with no bonus for characters or items (a guard in front of barrels is picked on his body, the
barrels just below him). The live table, with cursor ids and RMB task codes:
- neutral NPC without dialogue: no cursor; with dialogue: talk (RMB TALK);
- selected char sneaking, over a non-hostile: knockout (RMB STEALTH_KNOCKOUT); the predicate is the
  selected char's sneak mode;
- hostile in a fight: attack; KO'd enemy: loot (RMB LOOT); bleeding own char: first aid; own
  healthy chars: no cursor;
- foreign storage or shop: red loot; plain foreign buildings: no cursor;
- doors: open/closed -> door cursor, RMB CLOSE/OPEN; locked -> lockpick; damaged -> repair cursor,
  but RMB still OPEN (cursor and RMB come from two building virtuals, +0x410 checks health first,
  +0x418 only open/closed). Ours derives both from one decision; whether to keep that mismatch is
  a fidelity call (UNSURE, default: copy it).
Static only (not staged live): free/owned items, cages, lifting a KO'd ally, machines, unbuilt and
damaged buildings; the static rows (C1-C22, I, B0-B6) are in CURSORS_LIVE.md.

**Server: interactables are entities.** Doors, machines, containers and ground items get u64 ids
in the same id space as characters, so orders and events name them the same way. Their state
(door open/locked, machine in use, container contents version) arrives as records once the sim owns
it (section 9 "Building and town state").

**Orders.** One generic order replaces per-thing kinds where it can:
`interact` = `n u16, ids u64 x n, target u64, action u8, queue u8`. Actions include open/close,
lock-pick, use, loot, talk, pick up, carry, and sit/sleep. The server validates the action against
the target and answers event 10 (`bad_target`, `not_allowed`, `no_route`) when it doesn't apply.
It routes the character to the target's **use point** (the approach point, CHAR_ROUTE bit1) and
performs the action on arrival. Orders 30-32 (pick up, carry, drop) stay until `interact` covers
them, then are retired.

**Default action rule** (which action RMB means): the client picks it from the target's kind and
state plus the selection's relations (hostile -> attack, neutral character -> talk, closed door ->
open, machine -> use, item -> pick up). The server stays the authority on whether it is allowed.
The original's exact rules and cursor icons are to be read from its data (cursor textures, layouts)
and measured.

## ITEM_STATS (implemented 345eba3; approved by the client 2026-11-02)

**Approved by the client (2026-11-02), with these answers:**
- (a) Order 69 / reply type 29: OK.
- (b) `cutEfficiency` and `harpoon` go out as raw sim values. The client formats them per kinspect tooltips.md ("Cut resistance efficiency", "Harpoon resistance"), and the UNSURE stays.
- (c) Base damage is fine; keep `cutMult` / `bluntMult` in.
- (d) `sellValue` is -1 outside TRADE; inside TRADE the client uses TRADE's per-entry prices (MINE_SELL_SLICE 3.2). `value` ignores quality, so the client keeps weapon / armour Value hidden until the sim prices quality.
- (e) No condition: OK.
- (f) The order may address containers, and also a TRADE `stock` id (the trader squad), so trader stock tooltips get stats.
- (g) Added 2026-11-02 (UI session via the client; kinspect `findings/s66_static.md` part B): the armour block gains six trailing fields after its current rows. They are the runtime values after the loader:
  - `f32 stealthMult` (+0x25c, scaled by material / class, < 0 clamped to 1.0);
  - `f32 athleticsMult` (+0x248, 1.0 for weather-protection types 5 / 8 / 3);
  - `f32 combatSpeedMult` (+0x258, 1.0 for type 3);
  - `f32 dexterityMult` (+0x264, two load paths, l.342 / 376);
  - `i32 meleeAttack` (`combat attk bonus`);
  - `i32 meleeDefence` (`combat def bonus`); both zeroed for type 6.

  A value of 1.0 or 0 means "no row"; the client formats them. The builder must check which of these the sim's armour model already derives, and report the rest as UNSURE.

**Implemented (345eba3).** `Simulation::itemStats` (`inventory_orders.cpp`), `case 69` and `Session::writeItemStats` (`simlink.cpp`), test `ksim_item_stats`. Deviations / notes: the order is answered through the same path as 80-83 (not in the input log, no tick); the visibility rule is a player character, a body this session opened, the stock of a trade it has open, or a container / producer in the detail bounds; an entry that cannot be built (a weapon or armour without a quality) is answered as kind 0 with its own grade / level. **(g):** `stealthMult` (clamped < 0 -> 1.0), `athleticsMult` and `dexterityMult` are the record's raw values, UNSURE: the exe's material / class scaling, the weather-protection type overrides (athletics 1.0 for 5 / 8 / 3, combat speed 1.0 for 3) and the type 6 zeroing of the melee bonuses are not modelled; `combatSpeedMult` repeats row 8 and `meleeAttack` / `meleeDefence` are the armour's `attackBonus` / `defenceBonus` truncated to i32 (sent as two's-complement u32). The sim derives none of the other (g) fields from the exe tables.

Status (was): draft. The client approved the shape (STATUS "Client asks", 2026-11-02). Purpose: the armour / weapon tooltip rows (UI.md "Item tooltips as data") with numbers the sim owns (`CombatData::makeArmour`, `ArmourTables`, `makeWeapon`; mods change them), so the client carries no copy of the tables.

**Numbers (both free):** order kind **69 item_stats** (the gap between 68 transfer_entry and 70 squad_add; no `case 69` in `simlink.cpp`), reply record type **29 ITEM_STATS** (the gap after 28 ITEM_GRID; unused in `simlink.hpp`). Additive: no protocol bump, a client that does not know 29 skips it by its length (section 11).

### Order 69 item_stats

| Field | Type | Meaning |
|---|---|---|
| id | u64 | a character or a container (as order 66) |
| version | u32 | the INVENTORY `version` the index belongs to |
| index | u16 | the entry's position in that INVENTORY's list |

Answer: the ITEM_STATS frame below, to the asking session only (never broadcast, never in the state hash), and event 9 (applied) for the order. Failures answer event 10: `bad_target` (no such character / container, or not one the session may see: the INVENTORY rule, player characters and containers in the detail bounds), `bad_value` (index past the end, or a `version` that is not the current one: the client re-reads the INVENTORY and asks again). An entry that is neither weapon nor armour (kind 0) is answered, not refused. Read-only and rate-limited like any order; it takes no tick and logs nothing (UNSURE: if it is logged it must not shift replays, so it is better left out of the input log, like 80-83).

The client caches the reply by (id, version, index); the sim never pushes it.

### ITEM_STATS (type 29)

Frame body: `u64 tick, u64 id, u32 version, u16 index` (the echo of the order), `u8 kind`, then the kind's rows. All f32 are per unit (the client multiplies `value` by the stack count). Percent-like values are fractions 0..1 as the sim holds them; the client prints x100.

| kind | rows |
|---|---|
| 0 | none (a plain ITEM, a container, a record that is neither weapon nor armour); the common block below still follows |
| 1 armour | armour block, then the common block |
| 2 weapon | weapon block, then the common block |

**Armour block (kind 1)**

| # | Field | Type | Sim source |
|---|---|---|---|
| 1 | blunt | f32 | `state::FighterArmour::blunt` (+0x22c), rebuilt with `CombatData::buildArmour(record, entry.quality)`; 0..0.8 |
| 2 | cut | f32 | `FighterArmour::cut` (+0x228); 0..0.9 |
| 3 | cutEfficiency | f32 | `FighterArmour::cutIntoStun` (+0x238, ARMOUR `cut into stun`). **UNSURE**: that this is the tooltip row "cut efficiency" (RESOLUTION.md 264 only names the stun conversion). If the original's row is something else the field is renamed, the slot stays |
| 4 | harpoon | f32 | `FighterArmour::pierce` (+0x230; ARMOUR `pierce def mult` x `ArmourTables::kPierce[material]` lerped by level; the table is 0..90, NOT a fraction, unlike 1-2). **UNSURE**: that "harpoon" is the pierce resist; FEATURES 201 says "pierce/harpoon resist" |
| 5 | nCoverage | u8 | `FighterArmour::coverage.size()`; 0 = `[No Armour Coverage]` |
| 6 | coverage | nCoverage x { strid part, f32 fraction } | `FighterCoverage{part, coverage}` (`part coverage` v0 / 100; the part name is the anatomy part name `AnatomyPart::name`, as event 1's `part` strid) |
| 7 | weight | f32 | the ARMOUR record's `weight kg` (the carry slice weighs inventory entries by it; `FighterArmour` has no weight member, so the writer reads the record). UNSURE: no manufacturer / material factor is modelled for armour |
| 8 | combatSpeedMult | f32 | `FighterArmour::combatSpeedMult` |
| 9 | dodgeMult | f32 | `FighterArmour::dodgeMult` |
| 10 | damageOutputMult | f32 | `FighterArmour::damageOutputMult` |
| 11 | attackBonus | f32 | `FighterArmour::attackBonus` (`combat attk bonus`) |
| 12 | defenceBonus | f32 | `FighterArmour::defenceBonus` (`combat def bonus`) |
| 13 | unarmedBonus | f32 | `FighterArmour::unarmedBonus` |
| 14 | armourClass | u8 | ARMOUR `class` as the data enum (0 cloth, 1 light, 2 medium, 3 heavy; `makeArmour` clamps 0..3) |
| 15 | material | u8 | ARMOUR `material type` as the data enum (0 cloth, 1 leather, 2 chain, 3 plate; clamped 0..3) |

Rows 8-13 are "stat mods": the client prints the ones that differ from the neutral value (1 for mults, 0 for bonuses). UNSURE: which of these the original's tooltip lists, and their wording (kinspect `tooltips.md`).

**Weapon block (kind 2)**

| # | Field | Type | Sim source |
|---|---|---|---|
| 1 | cutDamage | f32 | `FighterWeapon::cut1` (base cut at skill 0; skill and `cutMult` lerp it up, combat_rules.cpp:560). **UNSURE**: whether the tooltip prints the base, the skill-99 value or a range; the sim can also send `cutMult` (row 3) and `CombatData` knows the k.cutDamage99 end, so the client can be given a lo / hi on request |
| 2 | bluntDamage | f32 | `FighterWeapon::blunt1` (0 for a cutting weapon; a blunt weapon's `cut1` is 0.4 x its blunt) |
| 3 | cutMult | f32 | `FighterWeapon::cutMult` (+0x19c: level lerp 0.3..2 x record `cut damage multiplier` x manufacturer `cut damage mod`) |
| 4 | bluntMult | f32 | `FighterWeapon::bluntMult` (+0x1a0) |
| 5 | bleed | f32 | `FighterWeapon::bleedMult` (+0x1a4, `bleed mult`) |
| 6 | armourPenetration | f32 | `FighterWeapon::armourPenetration` (+0x1c0, `armour penetration`) |
| 7 | attackMod | f32 | `FighterWeapon::attackMod` (record `attack mod` + the material spec's) |
| 8 | defenceMod | f32 | `FighterWeapon::defenceMod` |
| 9 | weight | f32 | `FighterWeapon::weight` (record `weight kg` x manufacturer `weight mod`, at least 40 x `bluntMult`) |
| 10 | length | f32 | `FighterWeapon::length` (`length`, default 14) |
| 11 | weaponType | u8 | `FighterWeapon::weaponType` (skill category: 0 katana, 1 sabre, 2 blunt, 3 heavy, 4 hackers, 5 unarmed, 8 polearm); picks the skill the "skill mods" belong to |

"Skill mods" = rows 7-8 (attack / defence); the original's wording and whether a weapon lists a stat requirement are UNSURE (no strength / dexterity requirement is modelled as a tooltip row).

**Common block (kinds 0, 1 and 2)**

| # | Field | Type | Sim source |
|---|---|---|---|
| 1 | value | f32 | the record's `value` (`trader.cpp recordValue`: `record.intOr("value", 0)`), per unit. **The sim does not price quality or condition**: `priceStock` is "value x count" and its own comment marks the exe's value as also reading quality and condition (UNSURE). So `value` is the same for a Prototype and a Masterwork piece until that is modelled |
| 2 | sellValue | f32 | **not modelled** (MINE_SELL_SLICE MQ6: the trader's buy / sell factors, ITEMS_CULTURE, the "books about equal" spread are open). Proposed: the writer sends **-1** ("unknown") until MQ6 lands, and the client keeps its Value / 4 stand-in for -1. The field is in the frame now so the layout does not change later |
| 3 | grade | u8 | `ItemQuality::grade` of the entry (0 prototype .. 5 masterwork; 255 = none, e.g. clothing without part coverage and plain items); set by `ItemQuality::gradeOf(level)` |
| 4 | level | f32 | `ItemQuality::level` (0..1; -1 when the entry has no quality). The same values ITEM_QUALITY (27) already carries for this index; repeated so the reply stands alone |
| 5 | condition | not sent | **Not modelled.** The sim has no item wear or durability (the only `wear` is a body part's, `fighter_state.hpp` line 93). When it exists, a trailing `f32 condition` is added behind a new `kind` revision, never by growing this block |

**Byte layout (little-endian, the section 2 conventions: `strid` is the table index of section 3)**

| Offset | Bytes | Field |
|---|---|---|
| 0 | 8 | tick u64 |
| 8 | 8 | id u64 |
| 16 | 4 | version u32 |
| 20 | 2 | index u16 |
| 22 | 1 | kind u8 (0 / 1 / 2) |
| 23 | ... | kind 1: blunt f32, cut f32, cutEfficiency f32, harpoon f32, nCoverage u8, nCoverage x (part strid, fraction f32), weight f32, combatSpeedMult f32, dodgeMult f32, damageOutputMult f32, attackBonus f32, defenceBonus f32, unarmedBonus f32, armourClass u8, material u8, then (g): stealthMult f32, athleticsMult f32, combatSpeedMult f32, dexterityMult f32, meleeAttack i32, meleeDefence i32 |
| 23 | ... | kind 2: cutDamage f32, bluntDamage f32, cutMult f32, bluntMult f32, bleed f32, armourPenetration f32, attackMod f32, defenceMod f32, weight f32, length f32, weaponType u8 |
| next | 4 | value f32 |
| +4 | 4 | sellValue f32 (-1 = not modelled) |
| +8 | 1 | grade u8 (255 = none) |
| +9 | 4 | level f32 (-1 = none) |

Kind 1 body is `16 + 1 + 28 + 2 + 24 = 71` bytes plus nCoverage x (strid + 4); kind 2 is `40 + 1 = 41` bytes; kind 0 is 0. The common tail is 13 bytes. The `part` strids are interned before the frame, as for event 1.

Open for the client's approval: (a) the numbers 69 / 29; (b) whether `cutEfficiency` and `harpoon` are the right rows; (c) cutDamage as base vs a range; (d) `sellValue` -1 until MQ6; (e) no condition; (f) whether a reply to an entry of a **container** is allowed (proposed: yes, same rule as order 66's addressing, the INVENTORY of a container in the detail bounds).

## MENU_ENTRIES (type 61; the answer to order 99 menu_query; implemented 2026-11-12, approved by the client 2026-11-12, kenshi-k16.9.17)

The original's right-click menu offers an entry only when its gate passes (the Talk To gate is `14067aa50`, DIALOGUE_TRADE_STATIC.md). The client never guesses a gate, so on a right-click it sends order 99 and opens the menu when the reply arrives. Frame body: `u32 clientSeq` (the order's seq), `u64 id`, `u64 target` (the echo), `u32 entries`: a bitset of the entries that apply.

| bit | entry | rule |
|---|---|---|
| 0 | Talk To (order 60 action 6) | a dialogue root passes for EV_PLAYER_TALK_TO_ME from `target` (the speaker) to `id`: the same selection a talk runs (`DialogueSystem::evaluate`; rolls are hashes), without speaking or changing anything. A busy speaker whose running line has an interrupt root: that root passes. With `--dialogue 0`: 1 for a merchant (the direct open), else 0 |
| 1-15 | reserved | 0 now; each later **character** gate (trade, recruit, ...) gets a bit and a row here first |
| 16 | pick lock a cage (order 60 action 18) | `target` is a **cage key** (CAGES): its lock is locked and `id` can act (not dead, KO'd, down or carried). The skill is not tested: the trailing pickChance says what it is |
| 17 | use tools on a cage (action 19) | as 16, and `id` carries a TOOL item (ITEM `item function` 9; UNSURE as for ITEM_LOCK) |
| 18 | use strength on a cage (action 20) | as 16, and `id`'s strength beats 3 x the lock's hardness now (`bruteBreaks`) |
| 19-31 | reserved | 0 now; later non-character targets |

**Range rule** (client ask 1, 2026-11-13): bits 0-15 gate a **character** target, bits 16-31 a **non-character** target (a cage key today), so a reply is unambiguous whatever `target` was. Order 99 with `target` = a cage key needs a character in the session's view caged in that building (else `bad_target`); `id` is any player character, caged or not. A cage that is not locked answers 0.

**Trailing `f32 pickChance`** (0..0.9), present **only when bit 16 is set** (a decoder reads it when the body is 4 bytes longer than the fixed part): `pickChance(level, lockpicking)` of `id`, less the caged-picker penalty (10 skill points when `id` itself is caged: `kCagePickSkillPenalty`), the very roll order 60 action 18 makes. The client shows `int(chance x 1000) x 0.1` as "90%". A pure query as before.

A talk sent anyway still answers event 10 `nothing_to_say` when no root passes (the world may change between query and click). A query is pure: it queues no event, starts no conversation, and shifts no hash.

## PROFILE (type 70; the answer to order 120 profile_query; implemented 2026-11-12, approved by the client lead on kenshi-k16.6.88, kenshi-k16.6.90)

The per-system tick timings of a KSIM_PROFILE build (docs/PROFILING.md), for the client's profile overlay. Admin session only (the SAVES rule, MULTIPLAYER 6.2); one-shot, to the asking session only: the overlay polls about once a second, there is no subscription and an idle client costs nothing. Order 120 is not a sim command and never enters the order log; it runs between two ticks. An OFF server answers `unsupported`, a non-admin session `not_allowed` (checked first, so it learns nothing); the reply to an accepted one is PROFILE, then event 9, in one write (the client accepts either order; kenshi-k16.6.170).

Frame body, like every record starting with `u64 tick` (the server's clock tick when written): `u32 window` (ticks in the profiler's ring behind the summary, at most 4096), `f32 tickMeanUs`, `f32 tickP50Us`, `f32 tickP99Us`, `f32 tickMaxUs` (the window's tick time), `f32 tickBudgetUs` (the server's tick period, 50000), `f32 characters`, `f32 squads`, `f32 pathQueries`, `f32 pathUs` (means per tick over the window; the path time is inside the sections; `characters` = live Fighters, `squads` = every SquadInstance in the world, spawned or not: a placement makes one per placed squad, so a full world reads about 4800 and a scenario a handful; kenshi-k16.6.170), `u16 n`, then n x `{str name, f32 meanUs, f32 maxUs, u64 totalNs}`: the window's mean and max per tick and the total since the server started, one row per section of docs/PROFILING.md (setup ... save), in the same order for the whole server run. The client keys rows by name; a new section is a new row, not a protocol change. Trailing bytes are ignored and new fields are only appended. A window summary only: no per-tick series (the client keeps its own history). About 0.7 KB per answer. Test: `ksim_profile_wire`, in both builds.

## CHAR_STATS and the inspect focus (kenshi-b0 2026-11-03; **approved by the client 2026-11-03** with changes 1-3, folded in below; ROADMAP item 7)

**Implemented 2026-11-03** (`Simulation::charStats`, `char_stats.cpp`; `Session::writeCharStats`, `case 87`, `simlink.cpp`; test
`ksim_char_stats`). Notes: skill `bonus` is 0 for now (the gear part is not split out, UNSURE); the run speed comes from the same
gait the fight moves with (`CombatArena::runSpeed`); `athleticsXp` is 1 (no constant); the KO point is the toughness lerp whatever
`--ko-gate` says.

Purpose: the bottom-left character panels (State / Goal / encumbrance / extra line / stats rows / extended stats / bounty) and the
Stats window, for the player's characters and for the session's **inspect focus** (an NPC clicked in the world). Specs: kinspect
`findings/inspect_focus.md` (panel fields, focus rules), `findings/ui_captures/sneak_icons.md` (tag state, light),
`findings/ui_captures/stats_window.md`. Numbers: order **87 focus**, record type **49 CHAR_STATS** (both free; 46 is left alone
because events 41-46 are proposed for dialogue). Additive, no protocol bump.

### Order 87 focus

Body: `id u64` (a character; 0 = none). **Session control** like 50 set_interest: not a sim command (the order log keeps it as a non-sim line, never replayed), no
effect on the world or the state hash (the original's focus changes nothing but the panels: inspect_focus.md result 3). Answers
event 9; `bad_target` when `id` is not a character in the session's detail bounds. The client sends it on every focus change
(world LMB on an NPC, portrait / own character, number keys, `select_all`, box drag: the table in inspect_focus.md result 2; the
client owns those rules). The server keeps one focus per session; it never clears it. While the focus is outside the detail
bounds the server sends nothing for it (the client keeps the handle and shows empty frames, as the original does on unload); when
it comes back the frames resume. A player character needs no focus: its CHAR_STATS always flows.

**Producer focus (client-approved 2026-11-12, implemented 2026-11-12, kenshi-k16.19.14):** `id` may also be a producer's id (the u64 its INTERACTABLE kind 4
record carries: a wild deposit or a placed mine), accepted while the producer is in the detail bounds, else `bad_target`. One focus per session: a producer
focus replaces a character focus and the reverse; `87 0` clears either. While a producer is the focus the session gets **one BUILDING (47) row for it**, any
owner (`owner` 0 = wild; the client keeps an owner-0 row out of its building lists and drops it when the focus moves), on focus and then whenever the
producer's version moves (an operator arrives or leaves, the state changes). Nothing is sent when the focus moves away. Values: BUILDING section.

**Answered at once while paused (2026-11-12, kenshi-k16.5.5; client-approved timing, no layout change, kenshi-h2v.11.7.1; user: "Unable to change inspect target while paused").** When the clock is
paused (`pausedNow`: the admin or idle pause, a dialogue freeze) an accepted order 87 is followed in the same reply by the frames the new focus needs, stamped with the paused tick: the focus's
CHAR_STATS (49, with its goal line) or, for a producer focus, its BUILDING (47) row. Nothing else is per-focus: CHAR_MEDICAL (body parts, health) and CHAR_APPEARANCE already go for every
character in the detail bounds, and an NPC's inventory is not sent at all, so a paused client already has them. The frames come from the same write as a paused trade (`sendTick`), so
anything else that changed since the last tick can ride along. While the clock runs nothing changes: the next tick's slot sends the frame within a tick. A focus outside the detail bounds is still `bad_target`.

### CHAR_STATS (type 49)

To one session: **every** player character of the session (in view or not: the Stats window and the status panel show far members;
client change 3), and its focus while that is in the detail bounds. Sent on connect / focus change, then on change at most once per
second per character (staggered by id like CHAR_MEDICAL); never broadcast. The medical panel needs nothing new:
CHAR_MEDICAL already goes for every character in the detail bounds. Name plate: the name from CHAR_APPEARANCE, the faction from
CHAR_APPEARANCE (its faction strid).

| # | Field | Type | Sim source / rule |
|---|---|---|---|
| 1 | tick, id | u64, u64 | |
| 2 | state | u8 | first match wins (inspect_focus.md "State"): 4 Dead, 5 Locked / 6 Unlocked (cage), 3 Unconscious (the client prints Rebooting for a robot race), 2 Sleeping, 1 Defensive (`Fighter::blockMode`), 0 Normal. 5 / 6 for a prisoner in a cage (`Fighter::cageBuilding != 0`; 5 while the cage door is locked, which a prisoner put in auto-locks, 6 unlocked; kenshi-k16.6.36); the cage also feeds `crime::tick`'s `inCage`. 2 is a sleeper in its bed (`Fighter::asleep`, as flag bit 16). The special-state label is not modelled (UNSURE) |
| 3 | lockLevel | u8 | the cage lock level (`cageLockLevel`, clamped 0..255) with 5 / 6, else 0. UNSURE: the cage's level is rolled as a door's, not the exe's own (deferred: playtesting) |
| 4 | goal | strid | the character's current goal as a display name; "" = the client prints "Aimless". Table below |
| 5 | encumbrance | f32 | percent, 100 = at capacity: pack weight / capacity x 100 with `carry.hpp`'s capacity. UNSURE: the exe's percent may be the ratio's complement; the sim's ratio only slows a carrier (carry.hpp OPEN) |
| 6 | extraKind, extra | u8, f32 | 1 `Stealth: N` while sneaking (the stealth skill), 2 `Athletics: N` while moving faster than 9 u/s, 0 none |
| 7 | weaponClass | u8 | 0 MARTIAL ARTS (nothing wielded or in the weapon slots), 1 ATTACK + DEFENSE, 2 CROSSBOWS (never yet: no ranged combat) |
| 8 | n, skills | u8, n x {u8 skill, f32 value, f32 bonus} | every skill the sim has, by the fixed id table below (never the struct order; client change 1); `value` is the total, `bonus` the armour / weapon part (`DEXTERITY: 18 (+2)`; 0 = plain row). The Stats window uses `value - bonus`. Ids 19-32 (2026-11-11) are sent for every character, bonus 0 (UNSURE: weapon / armour bonuses on crossbows); a skill the sim has no id for is not sent, and the client shows no row for it. UNSURE: only the bonuses the sim's armour model derives (ITEM_STATS (g)) |
| 9 | athleticsXp, strengthXp, toughnessXp | f32 x 3 | the extended rows' XP rates (`CombatData` `xpRate*`, encumbrance applied). UNSURE: no skill training in the sim yet, so these are the static rates |
| 10 | koPoint | f32 | `-koPoint(toughness)` (`combat.hpp` koThreshold) |
| 11 | runSpeed, runPenalty | f32, f32 | u/s after encumbrance and sneak (`knav::Gait`), and the loss vs unencumbered; the client converts to mph and prints the penalty red |
| 12 | hungerRate | f32 | RACE `hunger rate` x the encumbrance / bed mults, as a fraction (the client prints %) |
| 13 | light | f32 | 0..1, only meaningful while sneaking (the tag's light icon frame). Sim: daylight > 0.5 -> 1, else 2 x daylight (no lamps: perception.hpp OPEN) |
| 14 | n, bounties | u8, n x {strid factionSid, i32 amount} | `CrimeRecord::bounties` with amount > 0, a claimed one included (row 32 says which); n = 0 hides the bounty panel |
| 15 | sentenceHours | f32 | remaining sentence, 0 = none |
| 16 | goalEnum | u16 | the AI_TASK enum behind row 4's goal, 0xFFFF = none (a KO'd / dead character, an NPC on its template's behaviour only). Names: `docs/formats/AI_TASK_ENUMS.md` (all 291, generated). **Implemented 2026-11-07** (spectator mode): the same walk fills rows 4 and 16 (`LiveGoal`, char_stats.cpp), so they never disagree |
| 17 | source | u8 | where the goal came from: 0 none, 1 a player order, 2 a package / job row (`state::Jobs::running`), 3 the routine slot (the town scorer, a world task; a dialogue's individual goal too), 4 the group AI (`state::GroupAI::goal`), 5 the fight (`Fighter::combatGoal`), 6 a carry chain, 7 fleeing. **Implemented 2026-11-07**; UNSURE: 1 and 2 are never sent yet (row 4 does not read orders or the job queue: a player's job shows through its own routine / interact) |
| 18 | stepEnum, stepIndex, stepCount | u16, u8, u8 | the G4 plan step being executed (`state::Routine::plan` / `::step`, save v82), leaf first; 0xFFFF / 0 / 0 when there is no plan (a fight plans per tick and stores none) |
| 19 | targetId | u64 | a character id, else a building / site object id (a routine site's `object`), else 0 |
| 20 | targetX, targetZ | f32 x 2 | the point the goal is about (the target's position, a routine site, the group goal's patrol point); NaN when there is no point |
| 21 | n, progress | u8, n x {u8 skill, f32 progress} | the Next Level bar of every skill in row 8 (`n` equals row 8's, same ids): the fractional part of the row-8 value, 0..1. UNSURE: the exe's bar formula (deferred: playtesting); the sim trains nothing yet, so it is static. Row 9's XP rates stay as they are |
| 22 | carryKg, carryMaxKg | f32 x 2 | the pack's weight in kg (`Simulation::packWeight`, without the fixed 30 `carry person weight`) and `carry.hpp`'s capacity; the client prints `19 / 101` |
| 23 | damageResist | f32 | a fraction: `1 - CombatRules::toughnessFactor` = lerp(`damage resistance min`, `damage resistance max`, toughness / 100) (FUNCTIONS.md F8 getDamageResistance `140644770`; rebirth -0.65..0.65, RESOLUTION.md 5.3); the client prints a percent |
| 24 | tagFlags | u16 | the HUD tag row's booleans: b0 squad leader (the head of a player squad's list, else `liveRole` Leader), b1 army commander (0: not modelled, kenshi-k16.6.211), b2 captive (`Fighter::captive`), b3 diplomat in its territory (0: kenshi-k16.6.212), b4 slaveSeen, b5 escaped prisoner (0: kenshi-k16.6.214), b6 obedient (`Fighter::obedient`). **b4 slaveSeen is meaningful only when row 28 slaveLook != 255**: while "seen" is not modelled the sim sends b4 = 0 and slaveLook = 255, and the client must not show "Undiscovered" (0 cannot tell false from unknown) |
| 25 | slaveState | u8 | `Fighter::slaveState`: 0 free, 1 slave, 2 escaped, 3 ex-slave |
| 26 | captor | strid | `Fighter::captorSid`, the faction that chained it; "" = none |
| 27 | slaveHoursLeft | f32 | state 2: stamp + 12 h - now; state 3: stamp + 96 h - now (never below 0); -1 for any other state or an unstamped one |
| 28 | slaveLook | u8 | the percent the slave has been looked at; **255 = not modelled** (always 255 today, kenshi-k16.6.213). Read with b4 (row 24) |
| 29 | crime, timer | u8, f32 | the pending crime's `CrimeEnum` (0 none; the client keeps its own name table) and its timer in seconds. N = (int)timer; caught vs witnessed = timer > 3.0 (`crime::kMinTimer`) |
| 30 | escapeHoursLeft | f32 | the escaped prisoner's expiry; -1 = none (not modelled, kenshi-k16.6.214) |
| 31 | localFaction | strid | the owner of the town circle the character stands in (`townStateAt`), "" = none / outdoors |
| 32 | n, bountyTags | u8, n x {u32 crimes, u8 claim, f32 hoursLeft} | `n` equals row 14's, same order. `crimes` = the bounty's CrimeEnum bits (bit n = crime n), `claim` 1 = the reward was collected, `hoursLeft` = stamp + `crime::expiryHours` - now (never below 0), -1 = never expires (amount >= 10000). Row 14 now keeps a claimed bounty (amount > 0), so the client sees `claim = 1` on its row |

The HUD tag row tail (rows 24-32, kenshi-k16.6.215, approved by the client lead on kenshi-k16.6.209 2026-11-14) is additive after row 23: **no
protocol bump**; 27 bytes fixed + 9 per bounty, only while CHAR_STATS is sent (own characters + the focus, on change, at most 1 Hz). Unmodelled
values go as 0 / 255 / -1 until they are (kenshi-k16.6.211 .. .214).

The AI tail (rows 16-20) is additive after row 15: **no protocol bump** (spectator mode, agreed by the client coordinator and the
client, 2026-11-07). `goalEnum` / `source` come from the same walk as row 4 and `stepEnum` from the stored plan; a goal planned per
tick (a fight, a join) stores none, so row 18 is empty there.

The stats window tail (rows 21-23, kenshi-k16.6.54, client lead OK 2026-11-11) is additive after row 20: **no protocol bump**; an older
client skips it by the frame length.

**Skill ids** (row 8; the wire table, fixed: a new skill takes the next free id): 0 strength, 1 dexterity, 2 toughness, 3 athletics,
4 dodge, 5 melee attack, 6 melee defence, 7 katanas, 8 sabres, 9 hackers, 10 blunt, 11 heavy weapons, 12 martial arts, 13 polearms,
14 field medic, 15 lockpicking, 16 stealth, 17 science, 18 labouring, 19 engineer, 20 crossbows, 21 turrets, 22 thievery, 23 assassination (the
sim's `FighterSkills`), 24 perception, 25 swimming, 26 robotics, 27 weapon smith, 28 armour smith, 29 crossbow smith, 30 farming, 31 cooking, 32 precision
shooting (2026-11-11; read from the character's template STATS at send time, keys `perception`, `swimming`, `robotics`, `weapon smith`, `armour smith`,
`bow smith`, `farming`, `cooking`, `ff`; 1 for a character with no STATS record; nothing trains or uses them yet, so they move into `FighterSkills` with a save
bump when a system first does. Deferred: playtesting).

**Sneak tag state:** not a field. The client derives it as sneaking (CHAR_DYNAMIC flags) + CHAR_DETECT `state` (1 -> Seen, 2 ->
AlmostSeen, none / 0 -> Hidden), which is the exe's worst-state-wins rule over the 4 s records (sneak_icons.md item 4). Question (e).

**Goal names** (row 4; task AT). The original shows the current GOAL's display name -- never the primitive task it is running (a
Bonedog on goal 230 with task 220 read "Eat corpse") -- and "Aimless" when it has no goal (kinspect `findings/inspect_goal.md`).
Ours reads the goal the character is running, most specific first (`liveGoal`, char_stats.cpp): the fight (`Fighter::combatGoal`,
the goal that started it: 5 a player's attack order, 32 self preservation, 16 / 15 / 143 / 34 / 21 the join goals); fleeing (280
"Running away"); carrying (its chain's goal, else 68 "Lifting"); the routine slot (`state::Routine::goal`: 153 "Going to bed", 214
"Relaxing", 38 "Static guard", 190 "Worker", ...); the individual goal (19 "Going home", 67 "Running"); and the group goal
(`state::GroupAI::goal`: TravelTo 53 "Travelling", Patrol 36 "Patrolling town", Raid 18 "Raiding" (UNSURE), Roam 24 "Wandering",
ManGate 180 "Gate guard", GoHome 19). Only with no goal at all does an NPC show its squad template's behaviour (the first cut:
`shop_keeper` Shopkeeper, `bar_hangout` Relaxing, `patrol_local` Patrolling, `man_the_gate` / `guard_shift` Guarding,
`trade_circuit` Trading, `travel` Travelling, `go_home` Going home, `raid` Raiding, `animal_territory` Wandering, `stay_home` /
`fallback` ""). A KO'd or dead character has no goal (the original clears it) and reads "Aimless"; down but conscious keeps its goal.
An enslaved character (slave state 1) shows the committed goal's name (k16.6.116, rwh.99 section 4): following its captor 171; any carried character reads Aimless, as the original (the user on kenshi-8x6, kenshi-k16.6.204); never "Obedient Slave" (a do-nothing row is not a goal).
A player character with no order: "" (Aimless). A player's move order is 29 "Move order" until arrival, under attack too (kenshi-k16.6.343; docs/reference/COMBAT_MOVE_LIVE.md).

The strings are the exe's compiled display names, not the data's AI_TASK record names ("Patrolling town", where the record reads
"Patrol hometown"); the ids the sim can emit are in ORIGINAL_AI.md §5 with their source. An id with no exe string falls back to the
data record's `name` (UNSURE), then to "" (Aimless). The strings go through the string table, so a wrong name only changes the server.

**Client answers (2026-11-03):** (a) 87 / 49; (b) session control, unlogged, the client sends 0 when the focus returns to the selection; (c) not needed; (d) unsent, no row; (e) derived, no field; (f) fine as a first cut.

## DEBUG VIEW (Sim side built 2026-11-05; shape agreed with the client coordinator, revised after a Fable review. The client side
(`KCLIENT_DEBUG_VIEW`) is its own session's.)

Why: the user wants to see the sim's squads and their AI on the client's map, so that what the sim tests check can be judged by eye.
Debug only, not part of the game protocol: frames 200-219 are reserved for it and carry their own `debugVersion`.

**What it builds on.** SQUADS (type 30) already sends every squad's id, faction, x, z and member count world-wide at 1-2 Hz, and
the client draws its squad overlay (Ctrl+F4) from it. So the debug channel carries only the AI fields, keyed by squad id, in the
same write as SQUADS: no area subscription, no deltas, no gone list (a squad that is no longer in SQUADS is gone).

**Gating and code layout (no bloat, user 2026-11-05).**
- Sim: all code in `cpp/src/ksim/debugview/` + `cpp/include/ksim/debugview/`, built with the CMake option `KSIM_DEBUG_VIEW`
  (**ON in every build type**, Release included — the user's normal server is Release; `-DKSIM_DEBUG_VIEW=OFF` drops the code
  and the frames are then accepted and dropped silently). The runtime gate is the server's `--debug-view` plus a loopback
  peer. Hooks outside that directory:
  1. frame dispatch: frames 200-219 from the client are **always** accepted and dropped when the view is compiled out or off
     (one unconditional line; elsewhere an unknown client frame is a protocol error, section 11);
  2. the write in `Session::sendTick`, inside its two-pass `emit` (so new strids go through `flushStrings` first, section 3),
     after `SNAPSHOT_END`, outside the bracket (like events);
  3. session close / load resend: a load (`resendDue_`) drops pending queries;
  4. one optional decision sink in `AiSystem::Config` (next to the `log` sink): `decision(squadId, chosen, topCandidates)`,
     called only when set. debugview keeps the last decision per squad in its own map (not saved, not in the ECS);
  5. one optional decision trace sink on `Simulation` (`setDecisionTrace`, `decision_trace.hpp`, kenshi-k16.6.43): the two
     `goalsel::select` callers fill a verdict vector and report one `Pick` per member pick only while it is set. debugview keeps the
     last 8 picks per member in its own map (same rules: not saved, not in the ECS).
  It reads `const Simulation&` only and never recomputes AI choices: no state, RNG stream or tick changes, so a run with and without
  a debug client is identical. Exception: `DEBUG_SPAWN_SQUAD`, `DEBUG_SET_WEATHER` and `DEBUG_SET_TIME` (below) are commands and
  do change the world (`DEBUG_SET_OPTION` too). Safe while the write stays inside `sendTick` (server thread, region workers joined in `step`).
- The server answers only with `--debug-view` and only to a peer on loopback.
- Client: `client/src/debug/` behind `KCLIENT_DEBUG_VIEW`; its hooks are the dispatch and one draw call.

**Client -> server (control frames, unlogged, never in a replay):**
- `DEBUG_SUBSCRIBE` (frame 210): `u8 on`. Answered in the next tick's send with `DEBUG_INFO` (frame 200: `u8 debugVersion` = 1,
  `u8 enabled`; 0 = the server has no debug view or refused the peer). Every subscribe is answered, a repeat with the same
  value included. While on, every SQUADS write is followed by its `DEBUG_SQUADS`, so the first one comes with the next SQUADS
  write: **one SQUADS cadence (1 s of sim time), not one tick**. The SQUADS stream is never moved for the debug view (a debug
  client must not change what the client sees). On a server running slower than real time that cadence can be several seconds
  of wall clock — measured 2.5 Hz on the dev box with 4,855 squads, 8 s per cadence (client report 2026-11-05, item 1).
- **Paused:** no tick runs and no SNAPSHOT is sent, but the queued DEBUG_QUERY / DEBUG_INFO / DEBUG_SPAWNED / DEBUG_ACK answers go out at once (kenshi-k16.2.104.1), describing the paused tick. A subscribe answered while paused also carries one `DEBUG_SQUADS` (the paused tick; the id set of the last SQUADS, since nothing ticked): no SQUADS cadence runs while paused, and a paused F12 shot learns which squads to query from it. Otherwise DEBUG_SQUADS only follows a SQUADS write. No new frame or field.
- `DEBUG_QUERY` (frame 211): `u8 kind` (0 squad, 1 character, 2 world), `u64 id`. Answered in the next tick's send with one `DEBUG_TREE`. Up to 160 queries per send are kept (a screenshot's 64 characters plus their squads); more in one tick are dropped. A character tree carries `combat` (state, technique, clip time, cached reach), `velocity`, `engaged`, `encumbrance` besides the body; the squad tree has `detail`. Kind 2 (world, task BT) answers flat rows at depth 0 — `cod`, `gdm`, `ht`, `medicalClock`, `saved` (yes/no) — and ignores `id`.
- `DEBUG_SPAWN_SQUAD` (frame 212, **implemented 2026-11-05, approved by the client**): `u32 clientSeq, str templateSid, f32 sx,
  sz, tx, tz`. The one debug frame that changes the world: it places a squad mid-run, for judging the AI by eye without restarting
  the server. The squad is the one `ksim_server --place-squad` puts down (`Simulation::placeWildernessSquad`: a homeless wilderness
  squad of the SQUAD_TEMPLATE, template size, at (sx, sz)), and it runs the ordinary AI from then on. `templateSid` empty = the
  default `1302-gamedata.base` (Dust Bandits). The goal: a town within 2,000 u of (tx, tz) is raided — the squad gets the same
  `state::GroupAI` raid order the faction planner writes (`RaidOrder` Issued, the town as target, the spawn as origin; the group
  layer's Raid candidate routes and walks it, `ai.cpp`), and the ordinary AI resumes once the raid ends; with no town there it
  walks to (tx, tz) (a `TravelTo` goal on a route from the spawn) and then runs its own AI. It is applied between ticks (in the
  frame handler, not a `Command`), so the reply is in the next tick's send. When the view is compiled in (`KSIM_DEBUG_VIEW`, ON by
  default) it is answered always — server without `--debug-view`, not a loopback peer, and a refusal included — with
  `DEBUG_SPAWNED` (frame 203). With `-DKSIM_DEBUG_VIEW=OFF` the whole range is accepted and dropped silently (the section's
  rule above), so 212 gets no answer at all.
- `DEBUG_SET_WEATHER` (frame 213, **implemented 2026-11-06, approved by the client**): `u32 clientSeq, str weatherSid, str biomeGroup,
  f32 strength`. Pins `weatherSid` (any WEATHER record of the merged data: the WeatherModel's set) as the current weather of the
  BIOME_GROUP's region; `biomeGroup` "" = the group under the session's first player character, "*" = every group with a region.
  `strength` 0..1 (the WEATHER frame's `strength`; below 0 = 1.0). The pin holds (past a season end and its own lifetime; the
  record's hour window ignored) until released or a server restart; a save stores it as the current weather, the hold is not
  saved. `weatherSid` "" releases: a fresh pick from the current season next tick. The change goes out as a normal WEATHER frame
  at once (`previous` = the replaced weather, sky and fog blends from 0). Answer: `DEBUG_ACK`.
- `DEBUG_SET_TIME` (frame 214, **implemented 2026-11-06, approved by the client**): `u32 clientSeq, f32 hour, u8 extraDays`. Forward
  only (the clock is the wire's tick): jumps to the next time the clock reads `hour` (0 <= hour < 24; today if still ahead, else
  tomorrow) plus `extraDays` (0..7). A plain jump, no catch-up over the span (hunger, bleeding, healing, walking do not run); what
  is keyed on absolute game time comes due at once next tick (routine switches, weather lifetimes (a pin holds), raid deadlines,
  restocks, perception timers); light and sight follow the new hour. Answer: `DEBUG_ACK` and the CLOCK frame in the same send
  (the debug frames follow SNAPSHOT_END, so the CLOCK precedes the ACK on the wire).
- `DEBUG_SET_OPTION` (frame 215, **implemented 2026-11-07, task BT; proposed to the client with the UI's options page**):
  `u32 clientSeq, str key, f32 value`. Sets a new-game advanced option in the running world — `cod` (0.5..4), `gdm` (0.5..4),
  `ht` (0.25..8), saved from then on as `state::WorldOptions` (save v77) and live in the combat arena — or `medicalClock`
  (0.25..4), the medical clock's dev override, never saved (`COMBAT_OPTIONS.md` 4). `reference/ADVANCED_OPTIONS_STATIC.md`
  "Ours". Answer: `DEBUG_ACK`; reasons as for 213/214 plus "unknown option 'X'", "'cod' out of range 0.5..4" (and the like)
  and "value is not finite".
- `DEBUG_CHEAT` (frame 216, **implemented 2026-11-10, kenshi-h2v.6.3; for the client's cheat panel h2v.6.4**): `u32 clientSeq, u64 characterId,
  u8 op`. One character: id 0 = the session's first player character (the first member with a fighter of the first player squad). `op` 0
  **heal** (every part to its max, stun and collapse cleared, blood full, wounds and bleeding stopped, KO and unconscious cleared, stands up;
  chains and cages untouched), 1 **knock out** (the KO timer a damage KO sets, `CombatRules::knockoutTime`; the arena's next update makes the
  body unconscious and sends the same Knockout event and ragdoll a KO with no hit sends, so perception and the slavers see a normal downed
  character), 2 **wake** (KO cleared, wounds kept), 3 **kill** (blood below its death line: the next medical update kills it, the ordinary
  Death event and the last attacker's credit as any bleed-out). Applied between ticks in the frame handler; answered with `DEBUG_ACK`
  (frameType 216). Reasons: "no such character", "no player character" (id 0 with no living player character), "unknown op N", "already
  dead" (every op on a dead body), "already down" (knock out), "not down" (wake), and the view-off refusal as for 212-215. The server logs one
  stderr line per cheat (`debug cheat: op N character C: applied|<reason>`). Nothing is saved: no flag, no field, no save change (a KO timer
  and the blood are the ordinary fighter state). Gated as 212-215 (`KSIM_DEBUG_VIEW`, `--debug-view`, a loopback peer).
  Tests: `ksim_debug_weather_time` (the frame, every op, every refusal), `ksim_cheat_slavers` (the player of `slavers_downed` knocked out by
  the cheat at tick 100 gets down > disarm > cuff > lift like the player downed in the scripted fight). Open (deferred: playtesting): a heal does
  not touch hunger, so a starved character (hunger < 0.1) is knocked out again by the next update; a wake leaves a vital part below its KO
  point to knock the character out again (wounds are kept by design); the kill is a bleed-out, not a blow, so it has no hit event and its
  ragdoll is the no-blow collapse; id 0 after the only player character died answers "no player character", not "already dead".

**Server -> client:**
- `DEBUG_ACK` (frame 204, **implemented 2026-11-06, approved by the client**): `u32 clientSeq, u16 frameType, u8 accepted, str reason`, the generic
  answer to 213, 214, 215, 216 and later debug commands (203 stays for 212: it carries the squad). Reasons: "the debug view is off (...)",
  "unknown weather 'X'", "unknown biome group 'X'", "group has no weather region", "no player character", "hour out of range",
  "extraDays out of range" (a non-forward target answers "backwards jump", a guard the target rule cannot reach), "unknown
  option 'X'", "'cod' out of range 0.5..4" (and the like), "value is not finite". Always
  answered while the view is compiled in, like 212.
- `DEBUG_SPAWNED` (frame 203, answering `DEBUG_SPAWN_SQUAD`): `u32 clientSeq, u64 squad` (the new squad's `state::Id`, the key
  SQUADS and DEBUG_SQUADS use; 0 = refused), `str reason` (empty on success; otherwise why: the debug view is off, the template
  is not a SQUAD_TEMPLATE, there is no navmesh at the spawn point, or the wilderness spawner's own error, e.g. the point is in
  no wilderness slot). One reply per order, in the next tick's send (it is a world change, so it is not coalesced like the
  queries).
- `DEBUG_SQUADS` (frame 201): `u64 tick, u32 n`, then per squad (the SQUADS set; a squad without a position is in neither):
  `u64 id, strid template, strid package` (the template's AI package / squad routine, 0 none — **0 also for a player squad**:
  a `state::PlayerSquad` has no SQUAD_TEMPLATE, its members are the PLAYER_SQUADS list, leader first),
  raw sim enums, the client owns the colour key and the names: `u8 goalKind` (`state::GroupAI` GoalKind: idle, go home, travel to,
  patrol, roam, raid, man gate), `u8 fallbackReason` (`AiReason`), `u8 orderStatus` (raid order status), `u8 detail` (detail level),
  vitals (`SquadVitals`): `u8 members, u8 effective, u8 dead, u8 intact`,
  target: `u8 targetKind` (0 none, 1 point, 2 town, 3 squad, 4 character), `f32 tx, f32 tz, u64 targetId` (a town's `state::Id`),
  `strid townSid` (0 unless a town; matches DISCOVERED), route: `u8 routeValid, f32 along, f32 length`,
  members: `u8 m, u64 member x m` (the client shows loaded members on its 3D characters).
  Enum tables (value -> name) are in `cpp/include/ksim/debugview/enums.hpp`: header-only, standard library only, so the client
  includes it as it does kworld headers (no copy). A new value is appended, never renumbered. All names, templates and packages
  in DEBUG_SQUADS are strids; only DEBUG_TREE rows are inline (one frame per query).
- `DEBUG_TREE` (frame 202): `u64 tick, u8 kind, u64 id, u16 nRows`, then per row `u16 depth, str key, str value` (flat rows in
  pre-order; children follow at depth + 1; inline strings, not strids). Content is the sim's and may grow without a version bump:
  the squad (package, goal, last decision and its top candidates from the sink, home, target, raid origin, timers, fallback;
  a player squad answers too, with its name, vitals and members) and per member (stats, skills, goal stack, routine, target,
  hitBy, carry, inventory summary). An unknown id answers `nRows` 0.
- **Decisions subtree (kenshi-k16.6.43, 2026-11-10).** A character query (kind 1 only, not the members inside a squad tree) with
  picks in the ring ends with `decisions` (depth 1, value `N picks`, N <= 8), then the picks newest first, each a `pick` row (depth 2)
  with its candidate rows under it (depth 3, in offer order):
  `pick` = `tick T <Source> running R hold <Hold> -> <Result> TASK target ID score S tier N`;
  `candidate TASK` = `<Verdict> tier N target ID utility U weight W crowd C score S` (numbers %.2f).
  `Source` WorldSelector (a built world squad's member, world_tasks.cpp), TownScorer (a resident, routine_scorer.cpp) or Combat (a
  fighter's combat goal / target, below); `running` the
  goal in hand (0 none), `Hold` None / Inside (its minimum duration runs) / Expired (its re-scored copy is capped at 0.05);
  `Result` Chosen / DefaultIdle (nothing scored: the member keeps what it does) / Fallback (the town scorer's STAY_IN_HOME, nothing
  plannable; TASK = 43). `Verdict` (`goalsel::Why`): **Chosen**; **NoTarget** (scored 0: the resolver found nothing, or the crowd
  decay zeroed it); **OutOfWindow** (the row's hour window is shut); **LowerTier** (a higher tier had a candidate, this one was
  never compared); **Outscored** (same tier, a higher score, or a tie that went to the later offer); **HoldCapped** (the running goal
  past its hold, capped at 0.05, and it lost). The ring exists for every member while `--debug-view` is on; a member that never
  picked has no `decisions` row. The refusal verdicts of an order check (the design's Refused) are not recorded yet.
  Candidates the town scorer's resolver dropped before they became offers (no target found, a blocked pair) are not listed.
- **Combat source (kenshi-k16.6.59, 2026-11-11).** The 1 Hz passes skip a fighting squad (a fight plans per tick, row 18), so the fight's
  own pick records itself: `CombatArena::pickTarget` (combat.cpp) reports to the same ring with `Source` **Combat**, only when the
  member's (combat goal, target) CHANGES from the last one reported (a target held for N ticks adds one row; a goal flip on the same
  target, or a let-go, is a change). Same row shape; the pick is one rule, not goalsel's offers, so the fields read: `pick` task = the
  combat goal that admitted the target (5 attack order, 15 / 143 protect, 16 / 21 attack enemies (and neutrals), 32, 34 troublemaker, ...),
  `running` = the (goal, target) last reported, `hold` None, `Result` Chosen, or DefaultIdle with TASK 0 target 0 when it let go (nothing
  admitted); `candidate` rows = every enemy the picker scored at that moment (arena order): TASK = the goal that admitted it, `target` the
  enemy, `utility` the distance term (1 - d/200) x 2, `weight` 1.00, `crowd` the engaged-plus-refusals COUNT (not goalsel's factor, which
  is a multiplier), `score` the picker's (utility - crowd, +5 / +10 for the held target, +10 for a charge's attacker), `tier` 0, `Verdict`
  Chosen (the target) or Outscored. A player's attack order is a pick with no candidate rows (the order owns the target). Picks that
  other paths make (the selector's `orderFight`, a down or caged fighter dropping its target) are not reported; the next scan's change
  is. Costs nothing while no sink wants the member (the candidate list is built only for a traced member). Read only: test
  `ksim_fight_trace` (spectate_fight, 3000 ticks: picks with one Chosen candidate = the target, no pick inside a 1662-tick hold, hash
  identical with the sink on and off), `ksim_simlink` the row text.
- **`ksim_server --trace-member ID`** (repeatable): one stderr line per pick of that member, with or without `--debug-view`:
  `ksim_server: trace tick T member ID <Source> running R hold <Hold> -> <Result> TASK target ID score S tier N | TASK tN uU wW cC
  sS <Verdict> | ...` (one `| ...` group per candidate in offer order, u utility, w weight, c crowd factor, s score; the numbers
  as `<<` prints a double). Both are read only: no RNG draw, no state or tick change (tests `ksim_world_follow` sink on vs off hash,
  `ksim_goal_select` verdicts, `ksim_simlink` the subtree).
- Deferred: playtesting - the ring is 8 picks (the town scorer re-picks about every 5 s per resident, so it covers a minute or so; a fighter's Combat picks are rarer); the
  Refused verdict, `GroupLayer` and order sources, and a per-source pick filter wait for a use.

**Ids:** squad `state::Id`s are per world, never reused, and survive save / load and promote / demote, so a marker is stable.
**Size:** ~5,700 squads x ~60 bytes + 8 per member, about 0.5 MB/s on loopback; the walk is the one SQUADS already does.

## SCENARIO: playtest scenarios (task CE, 2026-11-07; the client's wire, accepted verbatim; PLAYTEST.md 1-3)

The client's Scenarios window lists the server's `scenarios/<name>.kscen` files, loads one (the server swaps the world, no restart),
plays it, and reads the run's metrics at the end. Session controls like load_game: applied between ticks, not in the order log. **Any
session may send them**: a playtest runs on the user's own client, which is a normal session, not the loopback admin (UNSURE for
multiplayer, where MULTIPLAYER 6.2's pause policy decides). A refusal is an answer (a frame or an ERROR), never a silent drop.
`cage_prisoner` (kenshi-k16.6.36) is the file with a caged character (`cage=near bounty=500`, CHAR_STATS state 5 / lockLevel); `ksim_cage_scenario` checks that no shot, KO or melee target picks it while the slavers fight the player. UNSURE (deferred: playtesting): whether the original's slavers ever pick a caged character through some path ours does not model. kenshi-k16.6.98 (playtest h2v.11.6): on the server's world the Heft warden squad streams in after the file is applied and `cageDoorLocked` read its un-run lock task as "never locked", so CHAR_STATS sent 6 (Unlocked) a second in; the scenario's prisoner now keeps a transient locked mark (`Simulation::setScenarioLocked`, cleared by `newWorld`, not saved), pinned by `ksim_cage_capture` (the server-like world, 45 s). Known, not changed: a world character on LIBERATE_ALL_THE_PRISONERS 276 (an Outlaw Farmer, ~58 s in) frees the prisoner by the decoded rule, which ignores the lock. The client's debug tree reads "state: up" for it whatever CHAR_STATS says, and the portrait mark is the client's to draw.

| Frame | Dir | Body |
|---|---|---|
| 102 `SCENARIO_LIST_REQ` | c->s | `u32 clientSeq` |
| 54 `SCENARIO_LIST` | s->c | `u32 clientSeq, u16 n`, then per scenario `str name, str title, str setup, str lookFor, u64 fileHash, u8 m, m x str metric`, then the 2026-11-10 frame tail `u16 n, n x str bead` |
| 103 `SCENARIO_LOAD` | c->s | `u32 clientSeq, str name, u32 seed` (0 = the server picks) |
| 55 `SCENARIO_STARTED` | s->c | `u32 clientSeq, u8 status` (0 ok), `str error, str name, u64 fileHash, u32 seed, str simHash, f64 startGameTime`, then the 2026-11-08 tail `f32 centreX, f32 centreZ`, then the 2026-11-10 tail `str bead` |
| 104 `SCENARIO_END` | c->s | `u32 clientSeq` |
| 56 `SCENARIO_METRICS` | s->c | `u32 clientSeq` (0 = unrequested), `str name, f64 elapsedGameS, u8 m, m x {str metric, f64 value, u8 valid}` |

- 54, 55 and 56 are answer frames: **no `tick` head** (like NEW_GAME_RESULT, 62), every string inline (`str`, not strid). The
  trailing-field rule (section 1) applies as usual; a client skips a `56` tail it does not know by the frame length.
- **List.** `name` is the file's stem. Only files that carry the `scenario` header record are listed (a fixture KSCEN without one
  is not a playtest, section 1 of SCENARIO_IMPORT.md); the list is sorted by name. A file that does not parse is an ERROR
  (`scenario_list: <path>:<line>: <reason>`), never a silent skip: a scenario the client lists must load.
- `fileHash` is FNV-1a 64 over the file's bytes (`scenario::fileHash`, the dataHash constants), so the client can stamp a run's
  `versions.md` with the exact scenario file it played (`playtests/<name>/versions.md`).
- **Load.** `name` must be a plain file name (no `/`, no `..`). The server reloads its pristine world (`Simulation::newWorld`, the
  world of order 84: no player faction, no scenario) and applies the file on it (`scenario::apply`, SCENARIO_IMPORT.md "The
  scenario file"): the clock, the player faction (none for a player-less file, spectator mode), every other squad as a world squad
  that runs the normal world AI, the file's own centre as the Full-detail area where no player stands, and the file's
  orders (the player's). It sets the run's seed and start tick, acks with event 9, then answers `SCENARIO_STARTED`, then sends
  the full resend as after a load (every other session at its next tick). The world stays as the scenario left it until the next
  `SCENARIO_LOAD` or `NEW_GAME`; saving it is an ordinary save.
- `seed` 0 means `splitMix64(clockTick ^ 0x5CE7A110)` (the tick mixed), so two plays of one scenario differ; any other seed
  reproduces the run. The seed feeds the fighters' own dice (`scenario::apply`), not the world's.
- `simHash` is the server build's git hash, embedded at configure (`git rev-parse --short=8 HEAD`, `KSIM_GIT_HASH`), `unknown`
  outside a git tree. `startGameTime` is the scenario's clock in game seconds (tick x 0.05).
- **The centre tail (2026-11-08, client-approved):** `f32 centreX, f32 centreZ`, the file's `source centre=` (the spot the scenario
  stands on), so a client can frame a player-less scenario with the spectator camera. NaN when the file names no centre and on every
  refusal (`status` 1). A tail like the others: an older client skips it by the frame length.
- **The bead tail (2026-11-10, client-approved):** the header's `bead=<feature id>` (the beads feature item the playtest proves; empty
  when the file has none). `SCENARIO_LIST` carries it as a frame tail after the `n` entries, `u16 n, n x str bead` in list order
  (a per-entry field cannot take a tail: the entries are packed back to back, so an older client would misparse the next one);
  `SCENARIO_STARTED` carries `str bead` after `centreZ` (empty on every refusal). Older clients skip both by the frame length.
- **Refusals** are `SCENARIO_STARTED` with `status` 1 and the reason in `error`: `no scenario 'X'`, `the server has no scenario
  directory` (started without `--scenarios`), `the server has no pristine world` (`--dump-snapshot`), `bad scenario name`, or the
  parser's / `scenario::check`'s message (an unknown stringID, a malformed record). The session stays up.
- **End.** `SCENARIO_END` acks with event 9 and answers `SCENARIO_METRICS` with the run's numbers in the header's metric order,
  and leaves the world as it is. The counters stop sampling at that answer, so a second `SCENARIO_END` repeats the same numbers
  (on the same game time); a `SCENARIO_LOAD` starts a new run. `clientSeq` 0 marks a `56` the server sends unasked (none does yet).
- **Metrics.** Every time is in game seconds since the run's start (tick delta x 0.05). `valid` 0 means the run never produced the
  metric (the value is then 0.0, not a real reading). The vocabulary, extended only by appending:

| Metric | Meaning |
|---|---|
| `targeted_player` | characters that ever had a player character as their target |
| `crew_join_s` | game seconds from the provoke (the first player fight) to a bystander fighting a player's attacker (the Bar's crew) |
| `bodies_down` | fighters that were in the fight and are down (KO, unconscious or dead) at the end |
| `fight_s` | game seconds from the first engagement to the last |
| `player_downed_s` | game seconds from the start to the first player character down |
| `captured` | 1 when a player character was carried or shackled during the run |

- **Observation only.** The run is sampled by the server after every tick (`ScenarioRun::sample`, `simlink_server.cpp`); it issues
  no commands, so a scenario plays the same with and without a client.
- **Implementation:** `cpp/src/ksim/simlink_playtest.cpp` (the handlers, the vocabulary, `buildHash`) and
  `cpp/include/ksim/scenario_run.hpp` (the counters); `ksim_server --scenarios DIR` (default `cpp/../scenarios`; unset or empty
  answers an empty list and refuses every load). Tests: `ksim_simlink` (all six messages over a live session, the fixture
  `cpp/tests/data/scenarios/wire.kscen`) and `ksim_scenario_load` (every shipped `scenarios/*.kscen` parses, checks, loads by name
  and answers every metric its header names).
