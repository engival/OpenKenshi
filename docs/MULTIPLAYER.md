> **Status: proposal (Fable review, 2026-11-01). Nothing here is implemented.** Decisions 6.1-6.12 were answered (user, 2026-11-01, REPORT_2026-11-01 §4); see §6 (answers marked **DECIDED**, reversed recommendations kept as `was:`). **V1 = a single co-op lobby session**; the world model stays a server option (ARCHITECTURE.md). Dialogue side of "the player" in co-op: DIALOGUE.md §6 (one dialogue event per player faction). Server game data: the server reads an original Kenshi install, and there is no server-data export tool (LICENSING.md "Server data", user 2026-11-04).

# Multiplayer: one player faction per user — architecture notes (fable, 2026-11-01)

Scope: the decision in ARCHITECTURE.md "One player faction per user". Read-only survey of
committed code (`git show HEAD:`), SIMLINK.md, RUNTIME_SAVE.md, CRIME_STATIC.md,
DIALOGUE_ACTIONS_STATIC.md, FACTIONS.md, IMPORT_GAME.md, SAVE_FORMAT.md, and the new-game
notes (`new_game_spec.md`). Nothing here is implemented; sections 1-5 are the design, 6 the
calls the user has to make.

Vocabulary used below:

- **PlayerFaction**: a FactionState entity with the player flag (the exe's `faction+0x250 != 0`).
- **User / account**: the identity behind a connection (a name + key, later). One user owns one
  PlayerFaction. A **client** is one connection; a user may reconnect, so the sim binds state to
  the user, not the socket.
- **Control group**: the set of characters a client may command = the members of its
  PlayerFaction's squads (plus the carry/prisoner cases in section 2).

## 0. Where we stand (committed code)

The sim has no faction-level notion of "player" at all. What exists:

- `state::Fighter::playerControlled` (bool, save v10, wire flag bit 6): the only "player" test the
  character layer has. Set by the scenarios, never by a faction. It is read in: `combat.cpp` (fight
  join rule, PROTECT_ALLIES, chases), `medical.cpp` (auto-eat 2.1 vs 2.4), `perception.cpp`
  (player-pair sight 1000), `interact.cpp` (a player's route never plans an unlock), `carry.cpp`
  (intruder take-out skips player residents), `simlink.cpp` (order authority, CHAR_ROUTE,
  CHAR_DETECT, INVENTORY, event 15 filter).
- `state::Fighter::team` (int): the arena's enemy test (`combat.cpp:155 enemy = team != team`).
  Scenarios put the player on team 0 and enemies on team 1. There is no derivation from factions.
- `state::Allegiance{factionSid}` per character (save v21): door ownership (isObjectMine), theft
  (event 14), `Simulation::alliedTo` (same team, same faction sid, or relation >= 50).
- `state::FactionState{templateSid}`: one entity per FACTION record; `Relation` pairs between them
  (f64, -100..100); `FactionAI` for the faction layer. No flags, no money, no bounties.
- `state::ViewFocus {x, z}` singleton (one camera), `state::PlayerAreas` singleton (a list of
  centres, already a vector: multi-area capable), `SetView` command (not tied to a subject).
- `simlink_server.cpp`: **one client at a time** (`Client client;`, "Only one client is served at a
  time" SIMLINK.md section 11). `Session` holds the per-client string table, known-set, interest.
  Disconnect = drive stop for *every* driven character (comment: "every driven character is its").
- `ServerContext::looks` (fighter Id -> CharLook with factionSid) is the server's appearance table,
  per process, not per client. Fine.
- The scenarios' player faction is the FACTION record `Nameless` (`simlink_scenario.cpp:170`), the
  original's default player faction.

So the multiplayer work is: (a) a PlayerFaction flag and per-faction player state, (b) team
derived from faction, (c) a `Session -> user -> PlayerFaction` binding and an authority test,
(d) N sessions, (e) the NEW_GAME/JOIN handshake, (f) per-player replication.

## 1. Every single-player assumption, and what it becomes

### 1a. In our code

| Where | Assumption | Becomes |
|---|---|---|
| `state::Fighter::playerControlled` | "the player's character" is a per-fighter bool set by scenarios | Keep the bool as a **cache** ("belongs to some PlayerFaction"), derived from `Allegiance` -> FactionState flag when the character spawns or changes squad/faction. Add `std::uint64_t owner` (state::Id of the PlayerFaction) next to it so hot code answers "which player" without a flecs lookup. Both are mirrors of the faction graph, re-derived on load; saved for the hash. |
| `state::Fighter::team` (arena enemy test) | two hard-coded teams | `team` = the FactionState Id (or a dense index of it). `enemy(a, b)` = `team != team && relation(a.team, b.team) <= -30` (hostile) OR a temp-enemy tag. Two player factions start neutral (0) so they do not fight on sight; co-op is `Relation >= 50`. Needs a per-tick relation cache (section 1c). |
| `simlink.cpp checkIds` | any `playerControlled` living fighter may be ordered | `fighter->owner == session.playerFaction` (section 2) |
| `simlink_server.cpp` disconnect | "every driven character is its" | stop only the characters whose `owner` is the gone user's faction |
| `state::ViewFocus` singleton + `SetView` | one camera | `ViewFocus` becomes per PlayerFaction: a component on the PlayerFaction entity (or `std::vector<{faction Id, x, z}>` singleton). `SetView` gains `subject` = the PlayerFaction Id (the server fills it from the session; the client never names its faction). Detail bounds = union over players. |
| `state::PlayerAreas` | a bare list of centres | already a list; tag each centre with the PlayerFaction Id so a leaving player's area is removed, and merge overlapping areas (ARCHITECTURE already says so) |
| `Session::sendTick` interest filter "1000 units round any player-controlled character" | any player | round the **session's** faction's characters only; another player's characters are ordinary characters to this session (sent when in interest, with flag bit 6 clear for them, see 1d) |
| `Session` CHAR_ROUTE / INVENTORY / CHAR_DETECT / event 15 "player-controlled" filters | any player | the session's own characters only (a route or an inventory of someone else's squad is private, section 5) |
| `medical.cpp` auto-eat 2.1 (player) / 2.4 (AI) | per-fighter bool | unchanged, reads the cached bool |
| `perception.cpp` player-pair sight 1000 | both player-controlled => sighted | **decide**: the exe's rule is "both in the player faction" (one faction). For two different player factions it should be the ordinary NPC rule (they are strangers), so test `owner == owner`, not the bool. |
| `combat.cpp` fight join (PROTECT_ALLIES) | player characters join fights of "allies" by team | unchanged once team is the faction and `alliedTo` is relation-based; a co-op ally's fight is joined (relation >= 50), a neutral player's is not |
| `carry.cpp:401` intruder take-out skips `playerControlled` residents | | should skip residents of the **building's owner** faction (any faction), not "the player": a player faction's guard throws out another player's character |
| `interact.cpp:1198` a player's move never plans an unlock | per-fighter bool | unchanged (any player-owned character) |
| `combat_data.hpp:198` squad formation "of the player faction" | one value | read per PlayerFaction (FACTION `squad formation` of the player's own record; each PlayerFaction can be its own FACTION record, section 1c) |
| `spawn.cpp:92-101` `player ally` / `player enemy` / `player involvement` on WORLD_EVENT_STATE | one player | these decide whether a world-state outcome counts for "the player". With N players: evaluate per PlayerFaction (relation to the named faction) and resolve by the **server's** rule (recommend: a world state flips when *any* player faction satisfies it; it is world truth, not per-player) |
| Scenarios (`simlink_scenario.cpp`) `Nameless` + team 0/1 | | keep as test fixtures; they become "one PlayerFaction flagged Nameless" |
| `ServerOptions` / `ksim_server --scenario` | the server starts with the squad already spawned | a server starts with **no** player squad; squads arrive through NEW_GAME (section 4). Keep `--scenario` for tests. |
| Order log (`--order-log`, `OrderLogEntry`) | one seq space | add the user id (or faction Id) to every entry; `clientSeq` stays per connection. Replay needs (user, seq). |
| `Command` has no author | | add `std::uint64_t author` (PlayerFaction Id, 0 = server/AI/script) so the apply step can re-check authority at apply time (the frame was validated at enqueue; the character may have changed hands in between: a recruit left, a character was enslaved). Also needed by the log. Save trailer change (RUNTIME_SAVE). |

### 1b. In the documented original behaviour (exe), with the flag test

Everything the exe tests as `faction+0x250 != 0` ("is a player faction") stays a **flag test**, as
ARCHITECTURE decided. The cases where the exe uses *the* player faction pointer
(`*(142134690+0x2a0)`, "THE player faction") need a "which one" rule; those are the real design
points:

| Original behaviour (doc) | Single-player form | Multi form |
|---|---|---|
| CRIME: NPC class forced to 9 adventurer for player chars (CRIME_STATIC +0xDC) | flag | flag (per character via owner) |
| CRIME: trespassing (0xb), shoo (EV_SHOO), intruder found: only for player-faction characters | flag | flag; the *observer's* faction may itself be a PlayerFaction (a player's guard in a player's building): then the observer is an AI-driven member of a player faction, allowed |
| CRIME: wanted-reaction detection boost for player criminals | flag | flag |
| CRIME: bounties are a map `Faction* -> Bounty` **per character** (`CrimeRecord +0x18`) | already per faction | unchanged: a bounty on a character is keyed by the faction that wants him; a player faction can also be a *bounty-issuing* faction if it is "legal" (+0x38) and "offers bounties" (+0x40) — recommend: **no** for V1 (player factions do not issue bounties; **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4), 6.9) |
| CRIME: bounty reward on cage delivery goes to "the player wallet" (`faction+0x80 -> +0x88`) | one wallet | the **carrier's** PlayerFaction wallet; conditions "carrier is a player faction, cage owner is non-player, prisoner is non-player" become: carrier.owner is a PlayerFaction, cage owner is not a PlayerFaction, prisoner.owner is not a PlayerFaction **or is a different PlayerFaction** (decision 6.4, **DECIDED yes** (user, 2026-11-01, REPORT_2026-11-01 §4): you can cage another player's character, and the exclusion of player prisoners is dropped; *was: player prisoners excluded*) |
| CRIME: bail price `2 x bounty` for player prisoners vs `max(2000, 2x)` | flag | flag, debited from the **payer's** wallet |
| CRIME: enslaving requires the target's owner be a player faction or `+8` | flag | flag |
| CRIME: prisoner release messages / UI refresh for player criminals | one UI | an EVENT to the **owning** session only (section 5) |
| DIALOGUE: when T is NULL, factions default to **the** player faction pointer | one | the dialogue's **initiating** player (the one whose character opened the conversation) — every dialogue session carries the PlayerFaction Id of its player participant; a dialogue with no player participant (NPC-NPC barks) has none and those branches are no-ops |
| DIALOGUE `change relations` list: listed faction's relation toward THE player faction | one | toward the initiating player's faction |
| DIALOGUE `DA_AFFECT_RELATIONS` "if TF is a player faction, flag thresholds" + HUD text | flag + one HUD | flag; the HUD message goes to TF's session |
| DIALOGUE `PlayerFactionRelations` stubs (vf2..vf5 ret): the player faction's own opinion never changes by reasons | one | the same stub on **every** PlayerFaction (a player faction's opinion of others is not driven by the reason machinery; only others' opinion of it). Between two PlayerFactions: both sides are stubs, so **player-player relations are set only by explicit command** (alliance / war, section 3). |
| DIALOGUE `DA_JOIN_SQUAD_*` recruits S into "the player's squad", squad-full check | one | into the initiating player's **talking** squad (T's squad), its size cap |
| DIALOGUE `DA_TAKE_MONEY` pays from TF's wallet; `DA_PAY_BOUNTY` credits "the player" | one wallet | TF's wallet (T is the player's character); credit the player who delivered |
| DIALOGUE `DA_MASSIVE_ALARM`: EV_I_SEE_ENEMY_PLAYER to every faction member | targets "the player" via T | per T's faction |
| DIALOGUE `DA_CHARACTER_EDITOR`, `CHOOSE_*` trading windows, `TALK_TO_LEADER` | open a UI | an EVENT to the initiating session (`dialogue window open`, section 5); the server waits for that session's reply order |
| DIALOGUE CONDITIONS (DIALOGUE_CONDITIONS_STATIC.md section 2) null-target fallback: `whoChar == NULL` takes the character nearest the speaker from **the** player's list, else the first member of the player's active squad, and writes it as the dialogue target | one | the nearest character **of the initiating PlayerFaction** (the dialogue knows its initiator, R2); if the dialogue has no initiator (a bark started by an NPC with no player near), the nearest character of *any* PlayerFaction within the speaker's perception, else the condition is false. Never "the first squad member of some player" (that picked a character across the map). |
| DC 2 PLAYERMONEY: reads `world+0x2a0` cash, ignores `who` (data: T_TARGET 455, T_ME 81) | one wallet | the **initiating player's** wallet (= TF's wallet, what T_TARGET means in 455 of 536 lines); the 81 T_ME lines also mean the player's cash in practice (a trader asking "can you afford it"). Same answer as DA_TAKE_MONEY, so the pay and the test agree. |
| DC 77 IS_RECRUITABLE: global roster cap `DAT_142133f50` over **the** player faction's members | one roster | the cap applies **per PlayerFaction** (the initiating player's member count). The cap stays the server's squad-size option; it is per faction, never a server total. |
| DC 74 PLAYERS_BEST_TOWN_LEVEL: max town level over towns owned by `world+0x2a0` | one | over towns owned by the initiating PlayerFaction. (Used by campaign gates too, FACTIONS.md: per candidate PlayerFaction there.) |
| DC 12 PLAYER_TECH_LEVEL: `*(world+0x38)+8`, a world-global progress object | world-global | per PlayerFaction `Research.techLevel` (section 3): the initiating player's. The exe keeps it global because research is global per save; ours is per player, so this follows. |
| DC 36 IS_PLAYER, 31 IN_A_NON_PLAYER_TOWN, 75 IN_A_PLAYER_TOWN, 73 TOWN_LEVEL_CURRENT_LOCATION, 66 CAN_AFFORD_BOUNTY: `faction+0x250 != 0` | flag already | flag; CAN_AFFORD_BOUNTY reads W's own faction's cash (it already names the faction) |
| DC 1 RELATIONS / 3 REPUTATION with no target: player faction as "who faction" via the fallback | one | the initiator's faction, through the same fallback rule |
| FACTIONS: campaign target = "the towns of the player faction inside the territory", gate on the **player's** population and town level | one | roll **per PlayerFaction** that has a base in the territory (each is a candidate target; weight by distance as the exe does). A faction with two player targets rolls each separately with the same `repeat limit` (one campaign at a time per faction, as the exe). |
| FACTIONS: `trigger player ally` campaign chains | one | per PlayerFaction with relation >= 50 to the faction |
| FACTIONS: relation reasons (vf5) move the victim faction's opinion of the aggressor's faction | works | works per PlayerFaction: killing a UC guard with faction A's character lowers UC -> A only. **Guilt is per faction**, which is the point of the decision. |
| SQUAD_FOLLOW: "Player faction `Nameless` has one AI goal PROTECT_ALLIES"; formation from the player faction's record | one record | each PlayerFaction is instanced from a FACTION template: V1 all from `Nameless` (same AI goals, formation). Later: a NEW_GAME_STARTOFF may name one. |
| PERCEPTION: per-frame updates "player faction always, others within camera 3250" | one camera | full detail = union of all players' areas (ARCHITECTURE table). Already the plan. |
| SAVE_FORMAT: `CAMERA.player money`, `pfaction name`, `selected_character`, `usedUniques` are one block | one | per PlayerFaction in our save (section 3); `usedUniques` is **world** state (a unique recruited by A cannot spawn for B) |
| IMPORT_GAME ("import": keep player squads, reset world) | one | per-player import is meaningless on a shared server; **import = server operation** that keeps every PlayerFaction's squads/money/research and resets the world (decision 6.8) |
| WORLD_MODEL `player involvement` ambiguity | one | see 1a |
| UI messages ("Relations with X improved", "Received c.N reward", "X has been enslaved", "Alarm raised" if the town is the player's) | one HUD | every such message becomes an EVENT addressed to one PlayerFaction (section 5 "addressed events") |

### 1c. New state (sim)

- **Partly built (2026-11-01):** `state::PlayerFaction` exists with `name`, `squadCounter` and the ordered `squads` list only (player squads: `reference/FACTION_SQUADS_LIVE.md`, SIMLINK PLAYER_SQUADS 31 and orders 70-75). The squad caps are the GLOBAL CONSTANTS `max faction size` / `max squad size` / `max squads`, applied per PlayerFaction. The server has one session, which owns the first PlayerFaction; the fields below are still to come.
- `state::PlayerFaction` component on a FactionState entity (saved):
  `userId` (str, the account), `displayName` (the pfaction name), `money` (i64 cats),
  `startoffSid`, `createdTick`, `squadOptions` (ditch/sleep/rescue/share/shootfirst/eject/formation,
  the CAMERA record's), `view {x, z, set}` (replaces the ViewFocus singleton), `lastSeenTick`
  (for the disconnect policy), `present` (bool, derived, not saved).
- `state::Discovery` component on the PlayerFaction (saved): the per-town `discovered`,
  `explored` bits (the original's GAMESTATE_TOWN flags, which are per *save* = per player). Store as
  a sorted vector of TownState Ids with two bits, or a bitset over town index.
- `state::Research` component on the PlayerFaction (saved): current, progress, finished (sorted
  sids), enabled buildings. (The exe's research is global per save; it must be per player.)
- Bounties: `state::Bounties` on the **character** (sorted vector of {faction Id, amount, claimed})
  = the exe's CrimeRecord. Already per faction; nothing player-specific.
- `Relation` pairs: already per faction pair; PlayerFaction <-> faction pairs are created on
  NEW_GAME from the FACTION template's `relations` and the start-off's `faction_relations`
  (new_game_starts.tsv column; e.g. "Son of a Captain": Holy Nation; United Cities). Player <->
  player pairs start at 0 (neutral).
- Team cache for the arena: a dense `factionIndex` per Fighter plus a per-tick
  `hostile[i][j]` bit matrix rebuilt at the strategic step and whenever a Relation crosses -30 /
  50 (we already emit `RelationCrossed`). Keep `team` as that index so `enemy()` stays one compare
  + one bit test.
- `Command::author` (PlayerFaction Id, 0 = system) in the trailer encoding (save version bump).

### 1d. Wire flag bit 6 "player-controlled"

Today it means "someone's". Per session it must mean **mine** (what the client may select and
order). Redefine bit 6 = owned by this session's PlayerFaction. Add bit 13 = owned by *some* player
faction (so the client can draw other players' squads with a distinct marker and name their
faction). CHAR_APPEARANCE already carries `faction` (strid), so the client can show whose it is.
Protocol bump (5) is justified anyway by the handshake change (section 4).

## 2. Command authority

### 2a. The rule

A session may command a character iff `fighter.owner == session.playerFaction` **and** the
character is not in a state that strips control. Evaluated twice: at the order (reject with event
10 `not_yours`, a new reason; keep `not_player_controlled` for AI characters) and again when the
`Command` applies (`Command::author` vs the subject's owner at that tick: a mismatch drops the
command silently and counts in `badOrders`; the client already got event 9, and sees nothing
happen — acceptable, rare).

Group orders stay atomic: one foreign id rejects the whole order (`SIMLINK.md` section 10).

### 2b. What strips or transfers control

| Case | Original | Ours |
|---|---|---|
| **Squad transfer** between two players (give a recruit to a friend) | n/a (one player) | a new order `transfer_character {id, toFaction strid}` from the owner; the receiver must **accept** (order `accept_transfer {id}` within 60 s; else it reverts). Both are logged commands (two-phase so a replay is deterministic). Changes `Allegiance`, `owner`, `MemberOf` -> a squad of the receiver (new squad if none within 50 units; the exe's `SEPARATE_TO_MY_OWN_SQUAD` is the model). Uniques: allowed; `usedUniques` is world state so nothing changes. |
| **Recruits** (dialogue JOIN) | join THE player's squad | join the initiating player's talking squad; control passes at the line's effect tick. Owner = that PlayerFaction. The server fills `author`; no order from the client beyond the dialogue reply. |
| **Prisoners / slaves** (a player's character caged, cuffed or enslaved by NPCs) | the player keeps control of the character (can order it to struggle, lockpick, escape); the character is "slave state 1" | same: ownership does not change. Control is limited by state, not by owner: a caged character accepts only the escape/lockpick/wait actions (the server answers `not_allowed` for the rest), a cuffed one is carried. Slave state 3 (ex-slave) keeps the owner. **Enslaved by another player** (decision 6.4, **DECIDED yes, like any NPC** (user, 2026-11-01, REPORT_2026-11-01 §4)): the character stays the original owner's and is limited by state like any NPC captive; the captor commands the *cage*, not the person. *was: no, `not_allowed` in V1.* |
| **Enslaving an NPC** (a player buys or captures a slave) | the slave is `slave state 1`, owned by the player faction, in the player's ownership list; player orders it like a member | owner = buyer's PlayerFaction with a `slave` state bit; freeing it (release) returns it to its template faction (or a new "Rebirth escapee" squad). |
| **Carrying another player's character** (a friend carries your KO'd character to a bed; or an enemy player kidnaps it) | a carried character takes no orders (`char+0x3d4` being carried, orders ignored) | the carried one stays owned by its player, but **its orders are refused while `carriedBy != 0`** (`not_allowed`, reason `carried`); the carrier's owner commands the carrier. Putting down restores. A put-down inside a cage = imprisonment by a player (6.4, allowed (user, 2026-11-01, REPORT_2026-11-01 §4); *was: `not_allowed` for another player's character*). |
| **KO'd / dead** | a KO'd character takes no orders; dead = gone | unchanged (`dead` already rejects). A dead member is removed from the control group; the body is lootable by anyone (theft rules by faction). |
| **AI-off toggle** (order 10 which 3) | per character | per character, owner only |
| **Buildings, turrets, gate controls, machines** (not yet in the sim) | owned by the player faction | `Allegiance`-style owner on the building; the interact rule "isObjectMine" already compares faction sids, so it works per PlayerFaction. A co-op ally may *use* (beds, benches) but not *reassign* (decision 6.5). |
| **Camera / set_view, set_interest, pause, speed** | per client | `set_view` and `set_interest` are per session. **Pause and speed are global** (one clock): see 2d. |

### 2c. Disconnected player: two world models, both kept open (user, 2026-11-01: TBD)

> **Update (user, 2026-11-01, REPORT_2026-11-01 §4) (6.1 / 6.1b):** both models stay server options and neither is built now; **V1 = a single co-op session** (join a lobby, start together, like any other game). The offline-faction question is deferred (late). Stay open to MMO style; it does not block.

Which model a server runs is **not decided** and must not be decided by the code. The two:

| | **Session model** | **Persistent model** (MMO-like; the existing Kenshi multiplayer mod's reading) |
|---|---|---|
| World runs when | all expected players have joined (a lobby; the host starts) | always, while the server is up |
| Join into a running world | not allowed (late join = next session) or allowed only as a reconnect of a listed player | the normal case: NEW_GAME or JOIN at any time (section 4d) |
| A faction while its player is offline | never happens: a disconnect pauses the session (`pause_on_disconnect`), or the host decides to carry on for the reconnect grace only | the hard problem; options below (keep running / dormant) |
| Saves | one save per session, taken by the host; load = everybody rejoins the same save | the server's autosave of the whole world; a player never "loads" |
| Region threads | sized to the group; most of the time all players share one or two areas | N areas spread over the map; the honest coarse levels matter (ARCHITECTURE "Why not the whole world") |
| Interest management | mostly shared bounds; privacy is a nicety | privacy (routes, inventories, camera leash) is the anti-cheat; SQUADS leak matters |
| Pause / speed | `anyone` or `host`; speed up is normal (that is how Kenshi is played) | `none`: fixed 1x; pause impossible |
| Failure mode of the model | one AFK player blocks the table | an offline faction gets wrecked (below) |

Both are server options (`world_model = session | persistent`), and the single-player integrated
server is the session model with one player (pause and speed as today). Nothing in the sim is
different between them except (1) the join gate (lobby vs open), (2) the pause policy, (3) the
offline-faction policy. All three live in the server layer (`simlink_server`, options), not in
the sim: the sim only ever sees commands.

#### Offline faction in the persistent model: the options

| Policy | Effect | Notes |
|---|---|---|
| A. **Stays in the world as NPCs** (PROTECT_ALLIES only: finish orders, stand, eat, bleed, get raided, get jailed) | honest; the original's alt-tab behaviour | the user's objection stands: a player returns to half a squad dead or in a cage. In a persistent world raids are rolled against bases and hunger runs for days of game time. Acceptable only with short offline spans (co-op friends) or per-server choice. |
| B. **Dormant faction** (the user's idea): the whole faction leaves the world while offline, its towns stay as ghost towns | the player resumes where he left; the world keeps running without him | analysed below; recommended as the persistent default, pending the research items |
| C. AI takeover (the faction gets NPC-faction goals) | squads wander and die | no |
| D. Freeze in place (invulnerable, no hunger, characters stay visible) | the base cannot be raided but is visible and in the way (blocks doors, gates, navmesh); other players walk into statues | no; it is B without the benefits |

#### B in detail: the dormant faction

**Pulling the faction out.** A PlayerFaction's state is already an entity subtree: the
FactionState entity, its `PlayerFaction`/`Discovery`/`Research` components, its squads
(`SquadInstance` with `faction` ref), the members (`MemberOf`), their `Fighter`, `Inventory`,
`OrderQueue`, `Bounties`, `Allegiance`, and the pairs `Relation` to and from it. Our save format
is already an entity list keyed by `state::Id` with save-local indices; a **per-faction save
subtree** is the same writer restricted to the entities reachable from the faction entity, plus
the pairs whose source or target is in the set. Dormancy = write that chunk into a
`DormantFactions` singleton (or a sidecar file keyed by userId), delete the entities from the
live world, keep the faction entity itself (money, relations, research, name: those are world
facts other factions reason about), keep the towns (below). Resume = read the chunk back with
fresh Ids? **No: Ids must be kept** (ids are never reused, so a dormant chunk's ids stay
reserved; `Ids.next` only grows). Bounties on the dormant characters stay in the chunk; bounties
**issued by** the faction do not exist (6.9).

Cross-faction references are the whole difficulty. Each one, and the rule:

| Reference | Rule at dormancy |
|---|---|
| A member **carried by** an NPC or another player (`carriedBy != 0`) | the carrier drops him (a put-down at its feet) before extraction; the carrier's `CarryStep` ends with event 13 result 3 "target gone". Abuse-safe because the linger timer (below) already ran. |
| A member **carrying** someone (`carrying != 0`): an NPC prisoner, an ally's character | put down in place; the carried one stays in the world |
| A member **in a cage / shackled** (prisoner of an NPC town or of another player) | **stays in the world**, not extracted: a jailed character is the jailer's business (the exe's sentence model); extracting it is the jail-escape exploit. The chunk records it as "left behind: caged at town X"; on resume it is still there (or dead, or released and wandering: policy A for that one character). Same for a member with slave state 1 held by an NPC slaver. |
| **Prisoners / slaves held by** the faction (NPCs in its cages, bought slaves) | owned NPC slaves (owner = the faction) are extracted with the chunk (they are faction property); NPC prisoners in the faction's cages are **released** (the cage is a ghost; nobody feeds them) with the exe's release effects (bounty entries erased with the releaser's faction: none, since the faction issues no bounties) |
| **Recruits** mid-dialogue, pending squad transfer, pending alliance (two-phase orders) | cancelled (revert) |
| **Bounties on** members | in the chunk; they come back with the character (the exe never expires them either) |
| **Relations** to / from the faction | stay live on the faction entity. NPC factions keep hating it; the war manager keeps its grudges. Campaigns, however, must not **target** a dormant faction's towns (ghost rule below), so the campaign roll checks `present`. |
| Another player's character **inside** the dormant faction's building | he is not thrown out; the building is a ghost (below), doors keep their state |
| **Items** the members dropped, containers in the faction's buildings | dropped items are world; containers go with the ghost town (frozen, see below) |
| An **ally** (co-op) currently following / being followed by a member | the follow ends (give-up path) |
| **Dialogue** in progress with a member | ended as "walked away" |
| Region engine `Mover` / stand-in group membership | removed like a `RemoveSquad` (the engine already handles removal); re-added on resume like a spawn |

**Combat logging and jail escape.** A disconnect does **not** extract at once. The faction gets
a **linger timer**: it stays in the world under policy A for `linger` seconds of wall clock
(server option; default 60 s, extended to e.g. 5 min while any member is "in danger": in combat
(`CombatClass` state not idle, or an enemy within 100 units with a hostile relation), bleeding,
being chased (someone's `orderTarget` is a member), carried, or inside a non-own non-public
building with an alarm active). Only when the timer expires with nobody in danger is the chunk
extracted. A character in a cage is never extracted (above), which closes the jail escape; a
character mid-fight stays for the fight (and may die), which closes combat logging in the way
every MMO does it. A **voluntary logout** (`LEAVE` frame) with nobody in danger extracts after a
short fixed delay (10 s) so a player can leave cleanly. The exact linger numbers and the "in
danger" predicate are an **A/B / playtest item**, not a decision now.

**Rejoin placement.** On resume the chunk's characters come back at their saved positions.
Checks, in order: (1) the position is on the navmesh (else the off-mesh correction, event 12,
already exists); (2) the spot is **occupied** (a building was built there, a different player's
squad stands there): shift to the nearest free navmesh point within 50 units (the group-order
scatter already has the code); (3) the spot is now **hostile** (an enemy squad within the
perception range, the town changed owner to a hostile faction, a raid is on site): offer the
client a choice before placing (`RESUME_OPTIONS` reply: here anyway / at your nearest own town /
at the start-off position); never auto-place into a fight. (4) The world time has moved: hunger
and healing are **frozen** in the chunk (below), so no catch-up. The resume is a logged command
(`ResumeFaction`), deterministic.

**Ghost towns.** A dormant faction's towns and buildings stay in the world (they are placed
buildings with navmesh and doors; removing them would rebuild the mesh and strand other players).
Two readings:

| | Frozen ghost | Live ghost |
|---|---|---|
| Raids / campaigns | cannot target it (`present` check); a campaign already marching to it arrives and finds nobody: resolves as "no defenders, nothing to loot"; recommend it **retreats** without damage (the exe's "target gone" cancel) | can be raided; buildings damaged, turrets unmanned |
| Looting by other players / NPCs | containers **locked** (the exe's "locked" state, isObjectMine refuses; lockpick allowed? **no**: an unpickable lock level 999 while dormant) | lootable |
| Doors / gates | keep their state; an `always locks` gate stays locked, a public building stays open | as live |
| Crops, machines, research benches | frozen (no progress, no decay) | run on (who collects?) |
| Honesty | a visible exception to "honest world": a town nobody can touch | honest but the owner comes back to a ruin |

Recommend **frozen ghost** as the default (`ghost_towns = frozen`), live as an option for PvP
servers that want base raiding of offline players (then the owner's own turrets and defenders
stay under policy A: the faction's *garrison squads* could be left in the world as NPC guards
while the *field squads* go dormant; that split is an OPEN design question, not needed for V1).
Frozen is not fully honest, but it is the honesty exception the user is willing to buy, and it is
cheap: a `dormant` flag on the TownState and on the door/container owner tests.

**Time freeze for the dormant characters.** The chunk is bytes; nothing ticks. Hunger, bleeding,
healing, KO timers, XP all stop. On resume the wounds are what they were. This is what the user
wants ("resume where you left"). Note the one exception: a member **left behind** (caged) lives
on world time. Food in frozen containers does not rot (we have no rot). Research does not
progress (a bench is a ghost).

**What must be designed in now so B stays possible** (none of it costs the session model
anything):

1. The per-faction save subtree: the save writer takes an entity filter (reachable from a faction
   entity) and the reader accepts a chunk with **pre-assigned Ids** into a live world. Today the
   writer and reader are whole-world only; make `writeEntities(filter)` / `readEntities(into
   live world)` the primitives now, before more components pile up. Every component that holds
   a cross-faction `state::Id` (today: `carrying`, `carriedBy`, `orderTarget`, `followTarget`,
   `sight[].target`, `Raid.squads`, `attack slots`) must be in a **documented list** so the
   extraction step can cut them; add the rule to RUNTIME_SAVE.md: "a u64 Id field that may point
   outside the owner's faction is listed in `kCrossRefs`".
2. `PlayerFaction.present` and a `dormant` bit on TownState, read by: campaign targeting, the
   isObjectMine / theft / lockpick tests, the perception of the dormant town's doors (none).
3. The linger predicate needs "in danger" to be computable per faction: a per-faction list of
   members (we have `MemberOf` by squad) and the arena's per-fighter `engaged` list (exists).
4. Removal and re-add of a faction's squads through commands that already exist in spirit
   (`RemoveSquad`, spawn): make `ExtractFaction` / `ResumeFaction` commands with the chunk in a
   `Command::blob`, so the order log replays dormancy deterministically.
5. Interest management must tolerate a faction appearing and vanishing: every session's `Known`
   gets `left interest` (event 11) for the extracted characters, exactly as leaving the bounds.
   No new wire.
6. Region threads: extraction and resume happen at the tick boundary (commands apply there), so
   no region ever holds a pointer into the chunk. Already the rule for commands.
7. The session model needs none of 1-6 but is not harmed; the dormant path is behind
   `world_model = persistent` and `offline_faction = dormant | npc`.

**Open research / A/B items (not decisions):**

- O1. Linger timer values and the "in danger" predicate (playtest).
- O2. Whether a campaign marching at a town that just went dormant should retreat, hold at the
  gate until the owner returns (ambush), or be re-targeted (playtest; the exe cancels on "no
  target").
- O3. Garrison split (guards stay, field squads go dormant) for live ghosts: needs the base
  building slice first.
- O4. Rejoin into a hostile spot: how often it happens on a real server (telemetry) before
  building the `RESUME_OPTIONS` UI; V1 can refuse to place and ask the client to pick a town.
- O5. How the existing Kenshi multiplayer mod treats offline players and time (someone should
  read its docs/source; it is the only prior art for this world).
- O6. Whether player-built towns of a dormant faction should age at all (decay, weeds) when the
  world has a decay model (none yet).

#### Common details (both models)

On disconnect the server enqueues `FighterDrive stop` for that faction's driven characters only
(today's code does it for all), clears nothing else (routes and queued orders keep running).
`PlayerFaction.present` false; `lastSeenTick` set. The detail bubble is removed with the session
after a **grace** of N seconds (server option, default 30 s) so a reconnect does not demote and
re-promote the fight the player was in; under the persistent model the grace is the linger timer
(the area must stay at full detail while the faction lingers "in danger": a fight at squad level
is not what the player would have had).

A faction with **no living members and no buildings** is "lost": the exe's answer is game over
(or the import salvage). Ours: the faction entity stays (money, research, relations), and the
client may NEW_GAME again *into the same faction* (keeps the name, money, research; a fresh squad
from a start-off). Decision 6.6.

### 2d. Pause and speed with N players

The clock is one. The original has no multiplayer. Rule: pause and speed are **server-wide and
owner-arbitrated**: a `pause_policy` option: `anyone` (any client may pause/unpause; good for
co-op), `host` (only the listed admin user; good for public servers), `none` (the clock is fixed
at 1x; pause disabled, play like an MMO). Speed above 1x: `host` only, and never while more than
one player is present unless `anyone`. Event 10 reason `not_allowed` otherwise. Recommend default
`anyone` for <= 2 players, `host` beyond (one option, server picks by player count: too clever;
just default `host` and let the config say `anyone`). **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4) (6.2): co-op default `host`, persistent forced `none`.**
Separate from `pause_policy` (who may pause): the single-server default is that the sim idles while no ready session is connected and an admin pause wins over it; implemented in `SimLinkServer`, see SIMLINK.md "Pause when nobody is connected" and ARCHITECTURE "Server loop".

## 3. Per-player state and the save format

| State | Where | Scope |
|---|---|---|
| Money | `PlayerFaction.money` | per player. Co-op sharing is a trade (give money order between allied players), never a shared wallet. |
| Relations | `Relation` pairs (exist) | per faction, so per player. Player <-> player: explicit only (`ally`, `neutral`, `war` orders: both sides must agree to ally (two-phase like the transfer), one side may declare war; sets ±: ally = 50 both ways, war = -35 both ways, as `DA_DECLARE_WAR` does). |
| Bounties | on the character (CrimeRecord map) | per issuing faction; a player's bounties are visible to that player (the exe shows your own bounties in the UI) |
| Crime records / pending crime / alert timer | on the character | unchanged; a crime by A's character stains A's faction (relation reasons) and A's character (bounty) only |
| Discovered / explored map | `Discovery` on the PlayerFaction | per player. **Co-op shares discovery?** The exe cannot say. Recommend: per player; allies may `share map` (an order that copies bits) — cheap, explicit, no magic. |
| Research | `Research` on the PlayerFaction | per player; research benches belong to a faction and progress the owner's tree |
| Squad options, formation, pfaction name | `PlayerFaction` | per player |
| Camera, selected character, tracking | **client** state, not saved by the server (the original saves them in CAMERA; ours: the client keeps its own prefs). `view` is saved only because `set_view` is a sim input (determinism). |
| Used uniques | world singleton (exists implicitly today via spawn) | world |
| World states, town ownership, NPC squads, doors, items on the ground | world | world |
| Player-built towns and buildings | TownState with `owner` = the PlayerFaction (already an `EntityRef owner`) | world, owned |

RUNTIME_SAVE.md implications (one version bump, v29, all additive in the component list):

- new components `PlayerFaction`, `Discovery`, `Research` (on FactionState entities), `Bounties`
  (on characters), `Fighter.owner` (u64) after `playerControlled`; `Allegiance` exists.
- `ViewFocus` singleton retired into `PlayerFaction.view`; `PlayerAreas.centres` gains a faction Id
  per centre.
- trailer: `Command.author` (u64) after `tick` (the trailer is not hashed; still a version bump).
- The header already records the load order; add nothing (users are in the body).
- **Identity in the save:** `userId` is a string the server trusts from its auth layer (V1
  loopback: the client's hello `name`; later a key). A save moved to another server keeps the
  strings; the admin maps them.
- The **state hash** must stay independent of which clients are connected: `present` is not
  saved, the view is (it is a logged input), the grace timers are wall-clock and live in the
  server, not the sim.
- `usedUniques`: today spawn decides from the world; make sure a recruited unique is marked so
  a second player's start-off does not spawn him again (check `spawn.cpp` uniques rule when the
  NEW_GAME path is written).

Save per player or one save? **One save = one world.** A player cannot "save and load" alone on a
shared server; the server admin saves (and autosaves). The single-player client is its own admin. The tick profile (SIMLINK PROFILE, order 120, KSIM_PROFILE builds) is admin only the same way.
The original's quick.save / per-slot UI maps to the integrated server only. The persistent model
adds the dormant chunks (section 2c): they are part of the world save (a `DormantFactions`
singleton holding the chunks as byte blobs with their userId, or sidecar files next to the
save); the state hash covers them, so a dormant faction is still state. Both models use the one
writer; the per-faction subtree writer is the piece to build now.

## 4. NEW_GAME / JOIN protocol sketch (SIMLINK, the protocol after 5; 5 = CHAR_APPEARANCE `portraitSerial`)

Principle: the HELLO stays as is; after it the client is **attached but owns nothing**. It then
either joins its existing faction or creates one. Until then it is a spectator: it gets the string
table, clock, weather, SQUADS, DETAIL_BOUNDS and anything in its `set_interest` (section 5 says
what a spectator may see; recommend: nothing at full detail: no CHAR_DYNAMIC until bound, except
when the server option `spectators` is on).

### 4a. Frames

Client -> server (new types; all `u32 clientSeq` first like orders, answered by event 9/10 or by
the typed reply below):

| Type | Body | Notes |
|---|---|---|
| 110 `AUTH` | `str userId, str token` | V1 loopback: token empty, userId from the client config. Sent right after HELLO. The server answers `AUTH_OK` (type 60: `u8 state` 0 new, 1 has a faction, `u64 factionId`, `strid factionSid`, `str displayName`, `i64 money`, `u32 members`) or `ERROR` + close (bad token; a second AUTH; AUTH before HELLO). Malformed = kick. |
| 111 `NEW_GAME` | `str name (the pfaction name, 1..32 UTF-8), strid startoff (NEW_GAME_STARTOFF sid), u8 n, n x Spec` | Spec as in `new_game_spec.md`: `strid race, u8 female, strid head, strid hair, strid beard, strid hairColour, f32 skin, u8 sliderCount, sliderCount x {str name, f32 value}, str name, i32 stance`. Only valid in state 0 (or state 1 with `members == 0` and no buildings: a re-roll, decision 6.6). Reply `NEW_GAME_RESULT` (type 61): `u8 result` (0 ok, 1 rejected), `strid reason`, `u64 factionId`, `u16 n, n x u64 characterId`. |
| 112 `JOIN` | (empty) | state 1 only: binds the session to the existing faction. Reply `JOIN_RESULT` (type 62): `u8 result, u64 factionId, u16 n, n x u64 characterId (living members)`. Then the full snapshot as today. |
| 113 `LEAVE` | (empty) | detach without closing (back to spectator). Rare; optional. |

**Update 2026-11-01: n > 0 is built** (character creation: SIMLINK.md "NEW_GAME", reasons `unknown_race` ... `bad_character_name`, FEATURES.md "New game: character editor"); the text below describes the first cut. **Implemented 2026-11-01 (single player, kenshi-b0; SIMLINK.md "NEW_GAME"):** frame 111 NEW_GAME with **n = 0 only** (the start-off's template members as they are); n > 0 parses (shape faults kick, n above 16 / 64 sliders / 256-byte strings) and is rejected `squad_size` or `spec_unsupported`; the reply is **type 62** (single player, agreed with 0f; not the 61 of the table), `u8 result`, then for ok `u64 factionId, u16 n, n x u64 member`, for a rejection `strid reason`. Order 84 new_world and STARTS (33) are in. **Not built:** 110 AUTH (a client may send 111 right after HELLO), 112 JOIN, 113 LEAVE, AUTH_OK (60), a state 1 re-roll.

Strings on the wire today are limited to strtab, hello and error; NEW_GAME adds `str` fields on
the client side: keep the same `u16 len` form and cap each at 256 bytes (`kMaxClientString`), the
frame at 64 KiB as now (a 10-member squad x 39 sliders x ~24 bytes ≈ 10 KB: fits).

The reply carries the start-off's fixed outcome: n is **fixed by the start-off** (the
`starting_squads` SQUAD_TEMPLATE size; "Rock Bottom" 1, "Nobodies" 5...). A Spec count that does
not match the start-off's squad size is a rejection (`squad_size`), not a kick: the client knows
the record and should have sent the right count, but it is a data question (mods), not a malformed
frame.

### 4b. Validation and reply/kick rules (the standing rule)

**Kick (ERROR + close)**: any frame-shape fault, as section 11: wrong length, truncated fields,
non-finite float, a string longer than its cap, n > 16 (`kMaxSquadSpec`), sliderCount > 64,
AUTH twice, NEW_GAME/JOIN before AUTH, order frames before JOIN/NEW_GAME succeeded (an order from a
spectator is a protocol error: the client has no characters yet; recommend kick, it is a client
bug, not a data mismatch), a second NEW_GAME while one is pending.

**Reject (reply result 1 + reason strid; nothing changes)**: unknown `startoff` sid; unknown race,
head, hair, beard, hairColour sid; a head/hair/colour that is not in the race's lists; a race the
start-off's `force_race` list excludes; a slider name not in the race's editor ranges; a slider
value outside its range (clamp? no: reject, the client has the ranges); a name that is empty,
all-whitespace or already taken on this server (`name_taken`); state mismatch (`has_faction`,
`no_faction`); `squad_size`; the server is full (`server_full`, max player factions = the
server's slot count, ARCHITECTURE "8-16"); the start location has no navmesh loaded yet
(`not_ready`, retry later: the server bakes the start zone on demand, see risks).

Reasons are strtab kind-4 strings like the event 10 reasons. The client shows them.

**Implemented 2026-11-01:** kicks for every frame-shape fault listed (and a second NEW_GAME while one is pending); rejections `unknown_startoff`, `has_faction` (any player faction in the world: one per server in V1), `bad_name` (empty, over 32 code points, invalid UTF-8, control characters, blank), `squad_size` (Spec count != the template's members), `not_ready` (no place of the start-off has navmesh ground), plus `spec_unsupported` (n > 0, character creation not built) and `unsupported`. Not built: unknown race / head / hair / slider checks (no Specs are read), `name_taken`, `server_full`, `no_faction` (no JOIN).

**Determinism**: a successful NEW_GAME is a logged sim command `CreatePlayerFaction` (new
CommandType, body in `text` as the serialized Spec list, or better: a new `Command::blob`
field) applied at tick+1; the reply is sent when it applies (the ids are allocated in the sim
tick, not in the session). The order log then replays the join. Same for JOIN (a `PlayerPresent`
command: it changes `present`, which is not state; so JOIN needs **no** sim command, only
`set_view`, which is one already).

### 4c. What NEW_GAME does in the sim

**Implemented 2026-11-01 (n = 0):** 1 done (the FactionState of the FACTION named Nameless, `PlayerFaction` with name, money, startoff; no `userId` until AUTH; relations = template + `faction relations`, both directions); 2 done except the Spec part: the `squad` refs' members as they are, at a ready place (Hub), `SpawnSource PlayerStart` is not added (the members are full-detail fighters, not SquadInstance members), research not built; 3 done (`Allegiance`, `playerControlled`, team 0, `owner` n/a); 4 built 2026-11-02 for the player's own characters (`Simulation::stepPlayerAreas`, once a second, one area per region a player-controlled character stands in; trade slice, MINE_SELL_SLICE 3.3), not yet per faction; 5 not built (no `Discovery`). The command is `CommandType::NewGame` (text = startoff sid + `\n` + name, applied at tick + 1, in the order log and in saves). Details and the list of unbuilt parts: SIMLINK.md "NEW_GAME".
1. Instance a FactionState from the FACTION template (`Nameless` for V1; `startoff.faction`
   later), with the `PlayerFaction` component (`userId`, name, `money` = start-off money,
   startoff sid). Relation pairs from the template's `relations` + the start-off's
   `faction_relations` list; player<->player pairs 0.
2. Spawn the `starting_squads` SQUAD_TEMPLATE as one squad (`SpawnSource` new value `PlayerStart`)
   at `start_pos` (or the `starting_towns` town's spawn point; "Rock Bottom" forces the position),
   members built from the Specs (race, sex, look, name) over the template's stats/items; "missing
   arm" and such come from the squad template's character record (LIMB_REPLACEMENT / hidden limbs
   already on the wire). Research = `starting_research` or `_Default Start`.
3. Every member: `Allegiance{faction}`, `owner`, `playerControlled = true`, team index.
4. The detail area for the new faction is added (`PlayerAreas`); the navmesh for the start zone
   must be available (risk R3).
5. Towns in `starting_towns` get `discovered = explored = true` in the player's `Discovery`.

### 4d. Joining a running world

- The join gate is the server's model (section 2c): session model = a lobby; the server answers
  `AUTH_OK.state` 2 "waiting for players" and admits NEW_GAME/JOIN only for listed users, and
  the host's start opens the clock; persistent model = open. The frames are the same.
- JOIN into an existing faction: the full snapshot is the normal one; the characters are wherever
  the sim left them (policy A) or come back from their dormant chunk (policy B, via
  `ResumeFaction`, possibly after a `RESUME_OPTIONS` exchange). Nothing rewinds.
- NEW_GAME into a running world: the squad spawns now, at the start-off position, at the current
  world time and state (day 300 of a server means raids and world states have moved on; that is
  the nature of a shared world). The start-off's `faction_relations` apply to this faction only.
- Loopback single player: the client sends HELLO, AUTH (its own name), then NEW_GAME or JOIN
  according to `AUTH_OK.state`. A saved game has state 1. "Load game" = the client (as admin)
  tells the integrated server which save to load before connecting (a launcher argument today:
  `ksim_server --load FILE`), then JOIN. No "load" frame in V1.
  **Single player new game (agreed b0 + 0f, 2026-11-01):** order 84 new_world (an admin control: a fresh world with no player faction), then NEW_GAME 111 as in section 4a, with n = 0 = the template's members, and the type 62 reply. AUTH is optional in V1. STARTS (33) tells the client which start-offs are ready. See SIMLINK.
  **Revised 2026-11-01 (b0 + 0f):** save / load / delete / list are SIMLINK session controls (orders 80-83, frame SAVES 32), allowed only to an admin session (loopback / author 0 for now), like pause; see SIMLINK SAVES.
- Admin frames (save, load, kick, pause policy) are **not** in this sketch; recommend a separate
  admin channel (a local socket or the launcher) rather than privileged SIMLINK frames, so the
  game protocol never carries authority above "my faction".

## 5. Interest management and replication hooks to design in now

Per-session filtering already exists (`Session::Known`, interest circle, detail boxes). What
must be shaped now so it does not need a rewrite:

1. **Addressed events.** Every event the sim emits gets a `audience` field in the sim-side event
   struct (not on the wire): `all`, `faction X`, `faction X and allies`, `observers of position`.
   The session filters on it. Today's "one of the two is player-controlled" filter (event 15) is
   the first such rule written per session: it becomes "one of the two is **mine**". The HUD
   messages of section 1b are addressed events (new kind 20 `message`, `strid text, u8 severity`),
   not strings in the sim.
2. **Private records.** CHAR_ROUTE, INVENTORY (of characters), ITEM_QUALITY, CHAR_DETECT, the
   order queue, hunger numbers: **own faction only**. CHAR_MEDICAL for everyone in view (visible
   wounds; the original shows enemy health bars). Container INVENTORY: anyone in reach (the
   original shows a foreign crate's contents when you open it). Other players' characters appear
   as AI characters do: appearance, dynamic, medical.
3. **Fog = interest circle, not discovery.** A session's interest = the union of (a) 1000 units
   round its own characters, (b) its `set_interest` circle **clamped to the union of its own
   detail bubbles plus its view zone** (today the circle is unclamped: a modified client could
   watch anywhere). Decision 6.7: whether the free camera may roam the world (the original lets
   it, there is no leash) — in multiplayer that is wall-hacking another player's base. Recommend:
   server option `camera_leash` (the "DECISION PENDING" in SIMLINK section 5): off for co-op,
   on (radius ~ one zone round own characters and own buildings) for PvP/public.
4. **Detail bounds per faction, full detail union.** `DETAIL_BOUNDS` is sent per session with
   that session's boxes only (what it may draw at full detail), while the sim runs the union.
   Already the design; make the box list carry the owner so the merge/unmerge is cheap.
5. **SQUADS (type 30) at 1 Hz world-wide** is a global leak (every squad's position). Keep for
   the own faction and allies; for others send only squads inside the interest or inside
   discovered towns (the original's map shows squads only where you can see them). Flag it now;
   it is the one record that bypasses interest.
6. **String table per session** already; the strtab is append-only per connection: fine.
7. **Per-session clock of what has been sent** (`Known`, `knownInventory_`): fine; add the
   faction Id to the key where ownership can change (a transferred character must get a fresh
   INVENTORY on the receiver's side and a `left` on the sender's private records).
8. **Bandwidth**: N sessions x snapshot encoding. Encode each record once per tick into a scratch
   and let sessions copy slices (CHAR_DYNAMIC is identical for every viewer; only the flag bit 6
   differs: put "mine" in a separate per-session bitmap record `OWNED {n, ids}` sent on change,
   and keep CHAR_DYNAMIC viewer-independent). That removes the only per-viewer field from the hot
   record and makes a shared encode possible.
9. **Threading**: sessions run on the network thread, reading the frozen tick mirror; the sim
   never waits for a session (today the loop is single-threaded and sends inline: fine for 1-2
   clients, make the mirror the boundary before N > 2).
10. **Deltas** (`full = 0`) stay reserved; design the per-session `Known` to support "last sent
    version per field" later.

## 6. Risks and decisions for the user (with recommendations)

> Dialogue interactions in co-op (a reply menu or a dialogue window pauses the sim for everyone in V1; `pause_policy = none` forces the real-time hold; disconnect lifts the pause and ends the menu after the grace): DIALOGUE.md §13.

**R1. `team` as a faction index changes the arena's enemy test everywhere.** Today
`enemy = team != team`. Replacing it with a relation lookup touches combat, perception, follow,
PROTECT_ALLIES, and the tests' fixtures (team 0/1). Risk of behavioural drift in the measured
combat tests. Mitigation: keep `team` an int, derive it = faction index, and make `enemy()` read
a per-tick `hostile` bitset; the duel/squad scenarios set relation -100 between their two
factions and the tests pass unchanged.

**R2. Dialogue "default to the player faction".** (Answered (user, 2026-11-01, REPORT_2026-11-01 §4): the dialogue fans out one event per player faction, DIALOGUE.md §6, decisions 7 / 12.) Every dialogue branch that reads the global
player pointer needs an initiator; the dialogue executor (not yet in the sim) must be written with
`initiator` from day one. No risk if done now; a rewrite if the dialogue slice lands first without
it. Tell the dialogue worker.

**R3. Start zone navmesh.** The server bakes the Hub at start (3.5 s). A start-off at Rebirth or
Bast needs that zone's mesh; "no navmesh, no nav". NEW_GAME must either bake on demand (`not_ready`
then retry) or the server pre-bakes the 13 start-off zones (~45 s, or cached with `knav_bake`).
Recommend: pre-bake start zones from the cache at startup; `not_ready` as the fallback reply.

**R4. Determinism with N input streams.** Orders from N sessions are enqueued in arrival order on
one thread: deterministic given the log, as today, provided the log carries the author and the
order within a tick is the enqueue order (it is). Keep it single-threaded at the queue.

**R5. The perception "player pair sight 1000" and the fight-join rules were measured on one
player faction.** Treating two player factions as strangers is the only consistent reading, but
untested. Mark UNSURE.

**R6. Order log replay and the save trailer change** (author) touch `ksim_simlink` and
`ksim_continue` fixtures. One version bump, regenerate samples.

Decisions for the user:

| # | Question | Recommendation |
|---|---|---|
| 6.1 | World model: session (all join, then play) vs persistent (MMO-like) | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4): not now; both stay server options**, the sim is identical. Decide per server when there is one. What to decide now: the four design-in items of section 2c, which keep both open. |
| 6.1b | Offline faction under the persistent model: NPCs in the world (A) vs dormant faction (B) | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4): deferred, a late question. V1 = a single co-op session** (join a lobby, start together, like any other game); stay open to MMO style, it does not block. *was: B recommended as the persistent default (frozen ghost towns, linger timer, caged members left behind); A as `offline_faction = npc`; numbers O1.* The B design stays in section 2c for later. |
| 6.2 | Pause/speed authority with N clients | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4):** server option `pause_policy` = `anyone` / `host` / `none`; co-op default `host`, persistent forced `none` |
| 6.3 | Player-player relations: automatic (reasons) or explicit only | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4): explicit only** (ally needs both, war needs one); the exe's PlayerFactionRelations stub says the same |
| 6.4 | Can a player cage / enslave / sell another player's character? | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4): yes, like any NPC** (players can cage, enslave and sell each other's characters). Diverges from the earlier "no in V1"; the cage-bounty exclusion of player prisoners goes too (section 1b CRIME rows updated). *was: V1 no (a put-down into a cage of a character owned by another PlayerFaction is `not_allowed`; the cage-delivery bounty excludes player prisoners); revisit for PvP servers as an option.* |
| 6.5 | Co-op allies may use each other's buildings (beds, benches, storage)? | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4): use yes, demolish no, with the game's own rules now.** A late research area and a non-blocker: it depends on MP scope. An MMO-like community server (like the existing Kenshi MP mod) needs more cross-faction security; two friends in co-op can relax it. Implement the game rules now, extend later. Taking from an ally's storage is not theft (relation >= 50): matches the exe's isObjectMine + theft rule only partly; UNSURE. *was: use yes, reassign/demolish no.* |
| 6.6 | A wiped-out faction: game over, or re-roll into the same faction? | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4): re-roll**, keeping name / money / research / relations: NEW_GAME allowed when members == 0 and no buildings (the exe's import salvage is the nearest thing) |
| 6.7 | Camera leash (SIMLINK 5 pending) | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4) (as recommended):** server option `camera_leash`, off in co-op, on for PvP/public; interest clamped to detail bounds in every case |
| 6.8 | "Import" on a shared server | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4) (as recommended):** a **server** operation keeping every PlayerFaction (squads, money, research, buildings per the import flags), never per client |
| 6.9 | Player factions issue bounties / count as "legal"? | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4) (as recommended): no** in V1 |
| 6.10 | Spectator (attached, no faction) sees what? | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4) (as recommended):** clock + weather, SQUADS of nothing, no characters; `spectators` server option for more |
| 6.11 | One FACTION template for every PlayerFaction (`Nameless`) or per start-off | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4) (as recommended):** V1 `Nameless` for all; the start-off's `faction_relations` give the flavour |
| 6.12 | Max player factions per server | **DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4) (as recommended):** the slot count (8-16); `server_full` reject |

## 7. Suggested order of work (each a slice that keeps tests green)

1. `PlayerFaction` component + `Fighter.owner` + team-from-faction + hostile bitset (save v29);
   scenarios create one PlayerFaction; `checkIds` tests owner. Single client still.
2. `Command.author`, per-faction `view`/areas, disconnect stop per faction.
3. AUTH / NEW_GAME / JOIN frames + `CreatePlayerFaction` command + spec validation (protocol 6; 5 is `portraitSerial`);
   start-off spawn; navmesh pre-bake for start zones.
4. N sessions in `simlink_server.cpp` (vector of Client), per-session private-record filters,
   `OWNED` record, addressed events, SQUADS filter.
5. Money / discovery / research components as the features that use them land.
