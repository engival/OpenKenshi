# Follow stream (side-by-side: the original drives, kclient follows)

Agreed protocol between the inspector agent (the original's side) and kclient's follow mode. Proposal: client lead,
kenshi-cym.40.2.1 (2026-11-20); agreed by inspect with 15 amendments the same day (that item's comment). Dev-only: the
client side lives behind `KCLIENT_DEV_TOOLS` and needs `--debug`. Order bodies: `docs/formats/SIMLINK.md` (1 move_to,
2 attack, 60 interact, 40 pause / 41 set speed). Id map: `docs/formats/IMPORT_REPORT.md`.

Contents: 1 Transport · 2 Grammar · 3 Session and state · 4 Agent -> client records · 5 Client -> agent records ·
6 Order mapping · 7 Ids · 8 Launch sequence · 9 Who produces the records

## 1. Transport

- The agent listens on `127.0.0.1:PORT`, plain TCP; kclient connects out (`--follow-stream 127.0.0.1:PORT`; not `--follow` as proposed: that flag already names the
  character spectator mode follows, so the client lead renamed it when placing this doc). 127.0.0.1 only.
- One peer at a time: a new connection with a valid `hello` replaces the previous one. When the client dies the agent
  keeps listening and the original keeps running; a reconnect sends `hello`, then after `ready` gets the full state again.
- Versioning: both sides skip unknown verbs and keys, so a lower `v` works and a higher `v` degrades the same way.

## 2. Grammar

- A line is a verb, then zero or more `key=value`, separated by single spaces. UTF-8, ends with `\n`, at most 4096 bytes
  (the receiver drops a longer line).
- A value runs to the next space, unless it starts with `"`: then it runs to the closing `"` and may hold spaces; `\\` and
  `\"` escape inside quotes.
- `ids=` is comma separated, no spaces, no empty items; a bare `ids=` is the empty set.
- Numbers are C decimal floats, exponents allowed. `pos` and `zoom` carry at least 3 decimals, `rot` at least 6.
- Unknown verbs and keys are skipped. A known record that is malformed is skipped whole, never half-applied.

## 3. Session and state

- The client sends `hello` at once on connect, and `ready` when its sim session is on the imported world and paused.
- On a reconnect the sim may be running: the client **pauses it first** (order 40, repeated each second until the clock shows paused), then sends
  `ready`, so `ready` always means paused (kenshi-cym.40.2.9). The agent's `speed` after `ready` unpauses it again.
- While the mirror lock is on (connected and ready) the client locks the mouse, the camera keys and every key that sends an order or changes the
  selection or camera (pause, speed keys, stop, select keys, Tab, focus, sneak, run speed, floor keys, direct drive, quickload); the dev keys and the F8
  panel stay free. A bare `select ids=` empties the client's selection and the client keeps it empty until the next `select`, a selection of its own
  (lock released from the F8 panel) or a world load.
- The agent sends nothing but `hello` before `ready`; what the original does between them is dropped, except the state.
- `cam`, `speed` and `select` are **full state**, never deltas. Right after `ready` the agent sends each one even if
  nothing changed, then only on change. The client may replace its state from any of them at any time.
- `clock` is the liveness beat (a still camera sends no `cam`).

## 4. Agent -> client records

| record | fields |
|---|---|
| `hello v=1` | first line |
| `align t=` | once per session: the original's clock at the instant of the quick-save, game hours since day 1. The sim's imported clock is the save's whole minutes (ksim save_import), so it can sit up to 59 game-s behind; the client keeps the offset and compares at matched time |
| `cam pos=x,y,z rot=w,x,y,z zoom=Z floor=N` | `pos` = the rig's `camera_center` orbit pivot (not the eye), world frame. `rot` = that pivot node's own world orientation, Ogre order w,x,y,z; the view is rot times a constant local offset the client does not need (as CAMERA_START). `zoom` = orbit distance, **positive**, 10..2000 (the save and the camera node store minus the distance; CAMERA_START uses the same positive number). `floor` = the visible floor slice (PlayerInterface+0x1e4), 0..3, the save's CAMERA `floor num`. Sent when anything differs from the **last sent** record by more than pos 0.5 u, rot 0.05 deg, zoom 0.5 u, or floor at all; at most once per original frame, latest wins. The client interpolates between records, never snaps. Optional future key `track=<our id or 0>` |
| `speed v=S` | the original's world speed float, 0 = paused (HUD buttons 0, 1, 2, 3, 5; any 0..10). The client turns it into order 40 paused=1 (S = 0) or order 40 paused=0 + order 41 speed=S, and sends nothing when its sim is already there. The agent never talks to the sim |
| `select ids=` | the player selection set (PlayerInterface+0x208, count +0x228), in the set's bucket-walk order, which is the order the game fans orders out in. Ids absent from the map are dropped. Empty = a real empty selection |
| `order kind=move ids= to=x,y,z [queue=1] src=N` | one record per **player dispatch**, every affected id in select order. `to` = the clicked ray point (y = surface height; move_to's yValid). `queue=1` = a shift order |
| `order kind=attack ids= target=T [queue=1] src=N` | |
| `order kind=interact ids= target=T action=N [queue=1] src=N` | `action` = the sim's interact action byte (section 6) |
| `order kind=unmapped ids= what="..."` | anything the agent cannot map, e.g. `what="issue_task type=12 target=<class>"`. The client logs it and never drops it silently. Expected often at first: only the ground-move path hands over a click point |
| `orig id= pos=x,y,z yaw=Y t= [state=]` | the original's position of one of our ids at game time `t`: mapped characters within 3250 u of the camera pivot (SIMLINK section 5's others-detail radius) plus the player's squad always, about once a second. `pos` = the logical point (body node; the same point CHAR_DYNAMIC carries). `yaw` in the CHAR_DYNAMIC convention (radians, 0 = +Z, toward (sin yaw, cos yaw)). `state` one of up, down, ko, dead, carried; omitted when up or unknown |
| `clock t= day=D hours=F` | once a second: the original's clock (t = day * 24 + hours) |

Orders are relayed while the original is paused too; the sim queues what it allows.

## 5. Client -> agent records

| record | fields |
|---|---|
| `hello v=1 client=<build hash>` | on connect |
| `ready` | the sim session is on the imported world, paused |
| `drift id= d= t=` | per followed character, once a second: the distance in world units (3 decimals) between the original's `orig` position and our character's position **at the same game time** (the client interpolates its own snapshots at `t` minus the align offset), and the sim's game hours `t` at which it was measured |
| `clock t=` | optional, once a second: the sim's clock |
| `bye` | optional, before a clean close |

## 6. Order mapping (the original's task type -> our order)

The client side owns this table (it owns the action numbers). `src` carries the original's task type
(`findings/ai_enum_map.tsv`), so a wrong mapping shows itself.

| src (task type) | record | our order |
|---|---|---|
| 29 MOVE_CUS_ORDERED | `kind=move` | SIMLINK 1 move_to (flags bit 0 = queue) |
| 5 FOCUSED_MELEE_ATTACK (character target) | `kind=attack` | SIMLINK 2 attack |
| 26 LOOT_TARGET | `kind=interact action=5` | SIMLINK 60 interact, action 5 loot, flags bit 0 = open (the original opens the body's inventory; it has no take-all) |
| 72 OPEN_DOOR | `kind=interact action=1` | 60 interact, action 1 open |
| 73 CLOSE_DOOR | `kind=interact action=2` | 60 interact, action 2 close |
| 76 PICK_LOCK | `kind=interact action=3` | 60 interact, action 3 lockpick |
| anything else (talk 12, follow, jobs, HUD toggles) | `kind=unmapped` | logged only |

The client issues a mapped order as if the player had clicked it, with the ids in the record's order: the original gives
the first member the click point exactly and offsets the rest (findings/msmove).

## 7. Ids

The agent speaks our sim ids only, never the original's handles. The map is the server's import report
(`--import-report`, IMPORT_REPORT.md format 1: squads pool -> id, members (pool, serial) -> id). The live character
hand's id ints at hand+0xc..+0x18 are the save handle's C, CS, I, S (SAVE_FORMAT.md section 3); the agent maps (C, S) to
our id and drops anything absent. The exact order in the live hand is asserted on the first live run.

Launch contract: the agent chooses the report path; kclient passes `--import-save <dir>` and `--import-report <path>`
through to its local server child.

## 8. Launch sequence

Both games must sit at the same clock at the import moment, so the original stays paused across the save and the load:

1. The button is pressed; the agent pauses the original with the game's own setter and notes the speed.
2. Quick-save; the original's clock at that instant becomes `align`.
3. The agent launches `kclient --debug --follow-stream 127.0.0.1:PORT --import-save <savedir> --import-report <path>`.
4. The client connects, sends `hello`, starts its server child with the import, asks for a paused start, then `ready`.
   The first camera comes from SIMLINK CAMERA_START; the first `cam` may replace it.
5. The agent reads the report and sends `align`, `cam`, `speed v=0`, `select`.
6. The agent restores the noted speed; it arrives as a `speed` record and unpauses the sim.

## 9. Who produces the records

The frame-rate records come from the inspector plugin's per-frame hook inside the original, pushed to the agent over the
existing kinspect local port; the agent relays them and translates the ids. A polling agent is not viable: one kq query
costs about one render frame (findings/camera_rts.md, Method). For the client this only means `cam` arrives at the
plugin's frame rate.

Refs (kenshi-inspect findings): camera_rts.md (rig, distance, zoom range), rwh150_save_floor_static.md (save camera
block, zoom sign, floor num), inspect_focus.md (selection set, hand layout), cursors.md (issue_* hooks, task 29's point),
msmove/README.md (selection order, formation offsets), anim_root_travel.md (the body node is the logical position).
