> **Status: proposal (Fable design review, 2026-11-01). Nothing here is implemented.** User decisions are in §11; all of them except #3 were answered (user, 2026-11-01, REPORT_2026-11-01 §4), marked **DECIDED** below. Where an answer reverses the original recommendation, a `was:` line keeps it. Multiplayer side: MULTIPLAYER.md §6; FEATURES "Dialogue" rows. **§13 (addendum 2026-11-02): client interaction lifecycle**: V1 freezes the sim per open menu / dialogue window (user, 2026-11-02; decision 5 updated), the pause as one switchable policy, disconnect / reconnect / save-load rules, windows as sessions, wire deltas and tests. Inputs: docs/reference/DIALOGUE_DATA.md, DIALOGUE_ACTIONS_STATIC.md, DIALOGUE_CONDITIONS_STATIC.md, TRESPASS_*, CRIME_*; multiplayer side in docs/MULTIPLAYER.md.

# Dialogue engine: design review and proposal (Fable, 2026-11-01, read-only)

Scope: how the original's DIALOGUE / DIALOGUE_LINE / DIALOG_ACTION / DIALOGUE_PACKAGE data runs inside
ksim. Sources read: ARCHITECTURE.md (layering, regions, runtime state, AI layers, Lua, "one player faction
per user"), STATUS.md, FEATURES.md "Dialogue", DIALOGUE_DATA.md (census, 16 questions),
DIALOGUE_ACTIONS_STATIC.md (executor), CRIME_STATIC.md / CRIME_LIVE.md, SIMLINK.md, and the sim headers
(`ai.hpp`, `behaviours.hpp`, `perception.hpp`, `command.hpp`, `state.hpp`, `fighter_state.hpp`,
`factions.hpp`, `script.hpp`, `interact.hpp`, `simlink.hpp`, `carry.cpp::stepIntruders`).
`DIALOGUE_CONDITIONS_STATIC.md` landed while this was being written (commit 12ae41e) and is folded in
(§1.3, §2.3, §4, §6). Everything not sourced to a doc is my proposal.

Numbers to keep in mind: 1,890 dialogues, 22,783 lines, 10,951 condition refs (69 kinds used), 5,011 effect
refs (51 kinds), 293 packages (203 inherit, one cycle), 69 events. 99% of effect use is covered by ~16
actions decoded at H confidence; the operand matters for only 9 of them.

---

## 0. The shape in one paragraph

A **DialogueBook** (compiled once from `kdata::GameData`, like `FactionBook` / `BehaviourRegistry`) holds
flat arrays of dialogues, lines, conditions, actions and an index `(packageSet, event) -> candidate roots`.
Every character carries a `packageSet` id (derived from its template; rebuilt on load). Engine systems that
already exist (perception, buildings, crime hooks, medical, combat, interact orders) produce
**DialogueEvent** messages. A **DialogueSystem** step, run per owning region after the engine's phase 1,
consumes the events for its speakers, evaluates root conditions (C++, memoised per evaluation, early-out),
picks a root, and opens a `state::Conversation` on the speaker. The conversation is a small timer-driven
state machine that speaks a line (a SIMLINK SPEECH event), runs its effects through an **effect sink** that
only ever sets individual goals, squad tags, faction relations, crime records, money, locks and outgoing
events (never touching a layer it is not allowed to), waits for the speech timer or a player reply
(a Command), and advances. Memory (repetition, locks, tags) is saved; the permanent-chance roll is a hash
and needs no save.

---

## 1. Runtime data model

### 1.1 Compiled book (`ksim/dialogue/dialogue_book.hpp`, built from kdata, immutable, shared_ptr like FactionBook)

```
struct CondDef  { u8 kind; u8 who; u8 cmp; i32 value; i8 tag; }             // DC_* and the list kinds (§1.3)
struct ActDef   { u8 kind; i32 operand; }                                  // DA_*, operand only where the exe reads it
struct LineDef  { u32 textFirst, textCount;  u8 speaker; bool interjection;
                  f32 chancePerm, chanceTemp; u8 repetition; i16 scoreBonus; u8 targetType;
                  span<CondDef> conds; span<ActDef> effects; span<ChildRef> children;
                  span<RelChange> changeRelations; span<ItemGive> giveItems;
                  span<u32> locks, unlocks, unlockKeepMe; u32 interrupt; u32 crowdTrigger; u8 crowdPct;
                  span<AiContract> contracts; u32 changeAi;  span<CampaignTrigger> campaigns; }
struct ChildRef { u32 line; u8 edgeV0; }                                   // v0 kept, unused until Q2 is answered
struct DialogueDef { strid sid; u8 flags (forEnemies, monologue, locked, oneAtATime, onceOnly);
                  f32 chancePerm, chanceTemp; u8 repetition; i16 scoreBonus; u8 targetType;
                  span<CondDef> conds; span<u32> firstLines; StaticMask staticMask; }
```

- **Text**: all `text0..N` variants go into one string pool; WORD_SWAPS compiled into a token table
  (token -> list of alternatives, `persistent use` flag). Built-in tokens (`NAME`, `YOURNAME`, `MYNAME`,
  `MYTOWN`, `COST`, `HIM`...) resolved at speak time (Q15: the list is from the census; unresolved tokens are
  left verbatim and counted).
- **Drop at compile**: the 788 orphan lines, 5,174 unreferenced DIALOG_ACTION records, stale fields
  (`speaker` on DIALOGUE, `action value`, `delivery AI package`, `set AI package`, `Leader AI Goals`), the
  one DIALOG_ACTION pretending to be an AI package (log it). `once only` bool = DR_ONCE_ONLY (Q6, assumed).
- **A DIALOG_ACTION's role** is decided by the referencing list (`conditions` vs `effects`), never by the
  record (census §2): the compiler reads `condition name` under `conditions` and `action name` under
  `effects`.
- **Operand**: `v0` of the referencing entry. Stored only for the 9 actions where the exe reads it
  (AFFECT_RELATIONS, AFFECT_REPUTATION, FORCE_SPEECH_TIMER, GIVE/TAKE_MONEY, ASSIGN_BOUNTY,
  INCREASE_FACTION_RANK, KNOCKOUT, CHOOSE_*); zeroed elsewhere so nobody is tempted (ACTIONS_STATIC §1).
- **Dead actions** (22, 26, 27, 31, 44-46, 50, 51, 59) compile to `DA_NONE` with a warning count.

### 1.2 Package sets and the event index

Effective package of a character (fcs.def order, Q5 unverified; union semantics):

1. SQUAD_TEMPLATE `dialog leader` (leader) / `dialog squad` (others) / `dialog animal` overrides,
2. else CHARACTER `dialogue package` (`dialogue package player` when the character is in a player faction),
3. else FACTION `dialog default`;
4. plus CHARACTER `dialogue` refs as extra `EV_PLAYER_TALK_TO_ME` roots (always added, they are per-character).

"Overrides" vs "adds" between 1 and 2 is Q5; I propose **override** as fcs.def says ("overrides anything
personal") and record it PROVISIONAL. Inheritance is a DFS over `inheritsFrom` with a visited set (kills
the `Empire Bounty Hunter ` cycle); the result is the **union** of `dialogs` entries, deduplicated by
dialogue (the 112 duplicates collapse). A set is hashed; identical sets share one `PackageSet` id (there are
at most a few hundred distinct sets for 5,709 characters). Per set: `events[EV] -> sorted span<DialogueIdx>`.

Per character: `state::CharacterInstance::packageSet` (u16, **derived**, rebuilt from the template after a
load; not saved) and a `staticPass` bitset over the set's roots (§4).

### 1.3 Conditions: what the exe actually does (CONDITIONS_STATIC 1.2-1.3), compiled in

- **`who` collapses to two values**: `T_ME` = speaker, anything else = the line target (for an interjector,
  T_ME = the interjector, target = the original target). Compile `who` to one bit `onTarget`.
  Ten "relation" ids (16, 18, 20, 50, 52-55, 61, 65) never compare the speaker with himself: a T_ME on them
  is redirected to the target; compile that redirect in (`onTarget = true`).
- **Comparator normalised at compile time** into one `op` per CondDef: booleans ignore `cmp` entirely
  (`actual == (value > 0)`); strict 3-way ids (SQUAD_SIZE, NUM_BACKPACKS, FACTION_RANK, PLAYER_TECH_LEVEL,
  NUM_DIALOG_EVENT_REPEATS, TOWN_LEVEL_*, PLAYERS_BEST_TOWN_LEVEL) keep `<` / `==` / `>`; the "at-least"
  ids (RELATIONS, PLAYERMONEY, REPUTATION, BOUNTY_AMOUNT_*, TARGET_LAST_SEEN_X_HOURS_AGO, HAS_ILLEGAL_ITEM)
  map `==` and `>` to `>=`. So `DC_RELATIONS > 15` is really `>= 15`; the compiler says so once.
- **HAS_TAG and PERSONALITY_TAG are not conditions at run time**: they fold into four bitmasks on the line
  (required / forbidden personality, required / forbidden long-term tag; `v0 == 1` -> required, else
  forbidden; several required entries are an **OR**). PERSONALITY_TAG always tests the **speaker**
  (77 data entries say T_TARGET and are wrong); HAS_TAG tests the speaker's squad memory about the target.
  The 2,125 + 276 entries become four `u16` masks per line.
- **The gate order** (one function for roots and lines; the DC list is **last**): masks, one-at-a-time,
  locked, speaker reservation, chance temporary, the list conditions (`in town of`, `my faction`,
  `target faction`, `world state`, `is character`, `target carrying character` (retargets), `has package`,
  `target has item` / `item type` (retarget), `target race` (retargets to a squad member) / `my race` /
  `my subrace`), `target is type` (OT_SLAVE also accepts slave states 1-2), repeat delay (hours),
  chance permanent (rolled once per (speaker session, line) and cached), then the DC list as a
  short-circuit AND **in list order, side effects included** (a retarget inside HAS_ILLEGAL_ITEM changes
  the target for the conditions after it). Our evaluator keeps this exact order so selection matches.
- **Ids with no handler evaluate true** (0 NONE, 6 FACTION_VARIABLE, 15 DEATHROW, 34 disabled slot,
  51 MY_MISSION_IS_FRIENDLY, 80 END): compile them to `Pass`, not to fail-closed. Fail-closed is only for
  kinds we have not implemented yet (a per-kind counter, one log line), so a dialogue is never offered on
  a guess. Decision 8 below.
- Facts the doc settles for slice 1: TALKING_RANGE = 260 units and the same floor (`char+0xa4`);
  IN_MY_BUILDING = target's building == speaker's **home** building, else speaker's current building
  (risk 1 confirmed); RELATIONS = S's faction -> W's faction, truncated, same faction 100, temp tags
  excluded; IS_ALLY / IS_ENEMY = faction flag or >= 50 / <= -30 (our FactionBook thresholds); LOVE =
  LT bits {2, 3}, HATE = LT bits {4, 5, 7, 9}; SQUAD_SIZE counts everyone incl. KO and animals; COPS_AROUND
  has **no radius** (threat-manager flags; we need an equivalent: "a law / in-town-military character in
  S's sight records", UNSURE); STRONGER_THAN_ME = `W.stat > 1.5 x S.stat` (toughness-like, Q7 open);
  LAST_SEEN uses a squad-memory timestamp (none = 9999 h).

### 1.4 Implemented: `ksim/dialogue/dialogue_book.hpp` (slice 1 steps 1-2, 2026-11-02)

`DialogueBook::build`, `packageSetFor` / `packageSetOf` (the event index), `staticPass`; tool `ksim_dialogue_coverage`
(census, `--sets`, `--package`); tests `ksim_dialogue_book` and `ksim_dialogue_compare` (the sets and the static pass against
`tools/dialogue.py`, 77 packages). Choices where this doc was silent or the data differs (all UNSURE unless noted):

- **Census vs this doc.** Reproduced: 1,890 / 22,783 / 293, 21,135 actions, 5,174 unreferenced, 10,938 (+13 in SINGLE_DIPLOMATIC_ASSAULT =
  10,951) condition refs over 69 kinds, 5,011 effects over 51 kinds, Manhunters 63, 279 / 267 text tokens. Differ: events used are **68**
  (the census doc's own unused list is 8 of 76; it says 69). The "788 orphan lines" are lines with no DIALOGUE / LINE parent; WORD_SWAPS
  records adopt 503 of them, so only **285** have no parent at all; **1,109** lines are unreachable from a DIALOGUE or WORD_SWAPS record
  and are the ones dropped (21,674 compiled). Also DIALOGUE_SELECTION_STATIC says 0 package cycles; the data has **1** (a package that
  inherits itself); the walk visits a package once.
- **"Unfinished" packages** (9 in the data) are skipped with what they inherit, as the exe does (`14067fab0`); `tools/dialogue.py package`
  does not skip them, so its effective counts differ for those. Package `dialogs` entries naming no event (14) register nowhere.
- **Per-event dedupe** follows the exe: a dialogue listed by a parent under event 3 and by the child under event 66 is rooted under both
  (the python helper keeps only the child's events).
- **Faction `dialog default`** is the last fallback of a human with no other list (fcs.def; the exe text for it was not found).
- **Sets are interned lazily** under a mutex (`PackageSetId` depends on call order: never save or hash it; set contents are
  content-addressed). Attaching the id to `CharacterInstance` is step 3 (needs the squad link and the rolled personality).
- **staticPass only excludes.** A static condition that holds is still evaluated at run time (DC_IS_FEMALE retargets when true, so folding
  a pass would lose the side effect). Static kinds: personality masks, `my faction` / `my race` / `is character` / `has package`
  (= any package the speaker's walk visited, inherited included), and T_ME DC_IS_FEMALE / HAS_ROBOT_LIMBS / IS_A_TRADER / IS_LEADER.
  `my subrace` is not derivable from the data (exe: race `+0x40`), so it never excludes. An empty (unknown) speaker fact never excludes.
  Personality, female and robot limbs are rolled at spawn and are the caller's inputs (the sim has no personality yet);
  IS_LEADER changes with squad leadership, so the set and the pass are recomputed then.
- `once only` (bool on one DIALOGUE) compiles to `DR_ONCE_ONLY`; 3 folded HAS_TAG / PERSONALITY_TAG entries have a tag outside 0..31
  and are dropped (counted); 2 dead-kind actions on compiled lines become DA_NONE.

**Steps 3-4 (events, gate, root selection; `ksim/dialogue/dialogue_events.hpp`, `dialogue_events.cpp`, `dialogue_sim.cpp`; test `ksim_dialogue_events`, 2026-11-02).**
Off by default: `Simulation::enableDialogue(book)`. Producers push `DialogueEvent`s, `stepDialogue` (after `stepCrime`) sorts the queue
(tick, speaker, event, target, seq), drops events older than 2 s, evaluates at most 64 per tick and logs a `DialoguePick` (root, first line,
pool size) per event in `DialogueSystem::log()`. Nothing is spoken, saved or hashed; the package set and static pass of a speaker are derived
per character from (CHARACTER, squad template, faction, leader, animal, player flag) and cached in the system, not on the instance.
Events: SHOO (EV 9) from `stepDialogue`'s producer (an NPC in a building it owns, a player-faction stranger in it that it is aware of, relation
strictly between -30 and 50, house not locked; every 2 s per pair), I_SEE_ENEMY / ALLY_PLAYER / NEUTRAL_SQUAD (66 / 67 / 3; hostile <= -30,
friendly >= 50; on awareness, then every 30 s), and every `fireDialogue` the crime layer already had (INTRUDER 17 via the trespass sensor,
THIEF_CAUGHT 8, WITNESS_LOOTING_ALLY 42, WITNESS_THIEF_OR_LOCKPICK 43, WITNESS_GENERIC_ASSAULT 41, BOUNTY_SPOTTED 44) with the crime kind as
`reason`. The minimal `CrimeRecord` already existed (`crime.hpp`); nothing was added to it. Conditions: 36 of the 69 kinds the data uses are
implemented (7,022 of the 10,938 refs); the other 33 fail closed and are counted per kind (`DialogueCoverage`). Choices where this doc was silent
or the data differs (UNSURE unless noted):

- **Theft goes to "Law witness looting ally" (13657), not "Law spotting a generic crime" (13643).** The exe maps theft to EV 42 (CRIME_STATIC)
  and the law package roots 13643 under EV 41 (WITNESS_GENERIC_ASSAULT: assault, murder, terrorism ...) only. The "Stop!" arrest lines are
  therefore the assault path, not the theft path. The test covers both (theft -> 13657, an assault record -> 13643).
- **`for enemies` is not a gate** (TRESPASS_STATIC: the exe never reads it); section 2.3 step 2 is wrong about it.
- **Selection**: candidates sorted by static score + best child score (descending, stable), scores as DIALOGUE_SELECTION_STATIC Q1.4 (the masks
  count one bit per distinct tag, so a tag listed twice counts once), +2 for a matching target race; a root-like line recurses; the pool
  holds the lines with the best cumulative score (own + parents'), the pick is uniform from a hash of (seed, speaker, tick, event). The doc rates
  the pool composition for root-like candidates M; cumulative scores are the simplest reading. Interjection nodes and lines spoken by the target
  (reply options) are skipped and counted (steps 5 / slice 2). "Would speak line Y" is therefore the first plain T_ME line.
- **Rolls are hashes**, never streams: chance temporary per (speaker, tick, record), chance permanent per (speaker, record) (D7), the pick per
  (speaker, tick, event); `kDialogueStream` = 19 seeds them.
- **Not modelled yet, treated as passing and counted**: one at a time (needs the squad's sessions), step 5. **The DIALOGUE record's own
  `repetition limit` (DR_SHORT_2, …) is not enforced by the exe either**, so treating it as passing is faithful, not a gap: the gate
  (`140678f30`) reads the limit from the *line* (`line+0x268`) against that line's last-play stamp (`+0x270`), and a DIALOGUE container has no
  text, so it is never stamped and its limit is never consulted (kinspect findings/dialogue_pacing, 6a13297; live: all 9 shoo lines and their
  root read `+0x268 = 0`). The per-line gate is the exe's (step 5 below). **Locked** roots and lines fail (no unlock memory). A retarget by
  IS_FEMALE lasts for the rest of that gate evaluation only.
- **Truthful constants for state the sim lacks**: IS_IMPRISONED, IS_SLAVE, IS_ESCAPED_SLAVE are false (the cage / slave fields exist since T3e2, but the conditions are not wired yet: the slave-dialogue slice, FEATURES 248); sex, personality
  and robot limbs are `DialogueInputs` (default male, PT_NONE, no limbs: a root that requires a personality never passes a character with none);
  the squad memory (HAS_TAG, LOVE, HATE) is `setMemory`, empty by default; `my subrace` fails closed (the item and carrying lists
  are live since task BS). **`world state` is
  live** (task AN): each entry's WORLD_EVENT_STATE is evaluated by `worldstate::stateHolds` over `Simulation::worldStateFacts()` — the
  dead / imprisoned status of the unique characters, the player faction's relations, the involvement set — and compared with the entry's v0
  (WORLD_MODEL.md a). Only a state record missing from the data fails closed, counted as `worldStateFailClosed`.
- **IN_A_NON_PLAYER_TOWN** = the character's CURRENT town (DECODED by address: `Character vf+0x78` = getTown, 1405c89b0 / GOG 1405c8350,
  CharStatus+0x88, which the state block carries at +0x88 - DIALOGUE_CONDITIONS_STATIC) owns the point and is not a player faction. The town test
  is the one shared geometry (`towncircle.hpp`, task town-circle): the exe's Town radius vf+0x2a0 (base by `type` x `town radius mult`, not the
  record's `size radius`) and the nearest centre among overlapping circles; player outposts keep their own radius. The exe's
  town-type exclusion 0 / 8 and the type-4 home rule are not modelled. **TALKING_RANGE** = same building and < 260 units. **IS_RUNNING** (W != S) =
  running gait and moving (the exe: running away from S). **DAMAGED_HEAD** = the head part's health - stun < 0.2. **COPS_AROUND** = a law /
  in-town-military character S is aware of (no radius). **OUTNUMBERED** = enemies S is aware of > 1.3 x able allies it is aware of.
  **STRONGER / WEAKER** compare toughness (Q7 open). **SAME_RACE** = the same RACE record (the exe: the race group, so subraces count as same).
- **Producers**: I_SEE_* only for player-faction targets (NPC-NPC sightings are squad-level, slice 3). SHOO's "owned building" is the observer's
  `homeBuilding`, or a door of the building owned by its faction. A hostile or friendly stranger draws no SHOO; a locked house is the trespass
  sensor's INTRUDER path (stepCrime), as TRESPASS_LIVE has it. **SHOO requires the building to be non-public**: the predicate (`1408525c0`)
  reads vf+0x290, the building's `+0xc8` "open to public", which is the owning SQUAD_TEMPLATE's `public day` byte (the exe never reads
  `public night`, no hour and no door state; forced false only for building type 1 / 0x22 while the squad's AI package `+0xe8` "house locked"
  is set, which the shoo already excludes). Live: a player at the Holy Barman's counter in Stack's public bar for 2.93 game hours drew zero
  dialogue; `sim->world().openToPublic` is the shared read, and `ksim_dialogue_events` "shooGates" holds a public bar for a game hour with no
  EV 9 and a private home still shoos. **UNSURE:** a keeper in a *public* shop still fired the EV 9 closing chain at 24:00 in the same capture,
  and its predicate then queried a different building object (unidentified), so the gate is decoded for the trespass sensor but does not
  explain that closing line (open question below).

**Step 5 (conversations, monologues; `dialogue_sim.cpp`, `state::Conversation`, `state::DialogueShown`; SIMLINK 41 / 42; save v57; test `ksim_dialogue_events` "conversations", 2026-11-03).** A Picked event opens a `state::Conversation` on the speaker and speaks the first line; a busy speaker drops new events (result `Busy`). When the line's timer runs out, `DialogueSystem::next` runs the selector over the line's children (T_ME lines; reply lines stay slice 2), and an empty pool ends it. A speaker (or interjector) that goes down or leaves the arena ends it early with SPEECH_END. `ksim_server --dialogue 0|1` (default on; the book builds in 0.2 s). Choices (UNSURE unless noted):

- **Repetition** is implemented (Q4, H): a gate within `repetitionHours` of its record's last stamp fails. Speaking a line stamps the line and its root (the exe also stamps the intermediate parents; ours does not).
- **Interjections:** the interjector is the nearest able, non-player squad mate within 260 that is not in a conversation. The node's own gate is the speaker's; its children are gated for, and spoken by, the mate. They join the pool at their cumulative score.
- **Speech timer** `(length x 0.12 + 2.0)` **hundredths of a game hour** (14067fd80; kinspect findings/dialogue_pacing: live `"..."` = 2.36 -> 0.0236 game h, and the 9-line closing chain's gaps 0.024-0.087 game h match), on the resolved text; the line holds it in game ticks (`kTicksPerGameHour`) and the SPEECH event carries wall seconds at 1x (hours x 109.09). Text variant and WORD_SWAPS pick are hashes (a `persistent use` swap per voice, else per utterance). Name tokens the sim does not fill yet stay as written. UNSURE: the live per-line hold is not exactly the formula on the displayed text (up to 0.029 game h off, lines 8-9), so a per-line component beyond the length may exist; the replay in `ksim_dialogue_events` "pacing" is 8.2 % long over the nine lines.
- **Not yet:** effects (step 6, so FORCE_SPEECH_TIMER and locks too), one at a time, the EV 3 / 4 / 0x1b / 0x42 / 0x43 1 s cooldown, the interrupt wrappers, and ending on being attacked (only down / gone ends it).
- **Throttle state** (`DialogueSystem::due`) is transient, so a run replayed from a save can re-fire producers at other ticks than the original run did (the save itself round-trips: the test checks the hash).

**Step 6a (effects, the dialogue-side and fight part; `dialogue_sim.cpp` executor, `state::DialogueMemory`, `CombatArena::markHostile`; save v58; test `ksim_dialogue_events` "effects", 2026-11-03).** A spoken line runs the executor in the exe's order (DIALOGUE_ACTIONS_STATIC 1): lock maps, `change relations`, the `effects` loop; SPEECH is sent after it, its duration max(speech, forced) and flags bit 3 for the large-bubble events. World effects act as the line's voice (S), the conversation's target (T); the lock map and the forced timer belong to the conversation's owner (UNSURE for interjections). Coverage counts every DA_* kind applied / skipped and the line lists not applied. Applied:

- **Locks**: `unlock but keep me` (+ the running root locked), `unlocks`, `locks`, DA_LOCK_THIS_DIALOG into the owner's `DialogueMemory` (sid -> bool); the gate's step 3 reads it (an entry wins over the record's `locked`). FORCE_SPEECH_TIMER: the line holds `max(speech, operand s)`.
- **Relations**: DA_AFFECT_RELATIONS (S faction -> T faction += operand, clamped; no-op from a player faction), `change relations` (listed faction -> T's faction when it is a player faction). Direct writes + `noteRelationChange`; a side with no FactionState is skipped (counted).
- **Fights**: ATTACK_CHASE_FOREVER / STAY_NEAR_HOME / IF_NO_COEXIST and ARREST_TARGET = TEMP_ENEMY (`markHostile`: S's squad lists T's squad in `hostileSquads`; 6 skips it when S's faction is friendly to T's) + `orderTarget` = T (a replace order: the follow and route end). STAY_NEAR_HOME has no leash yet; ARREST is always the melee branch (no FIND_CAGE). FLAG_TEMP_ENEMY = markHostile; FLAG_TEMP_ALLY = forgiveHostile (UNSURE: no ally tag).
- **FOLLOW_WHILE_TALKING**: `followTarget` = T, `Conversation::follower`; the follow ends with the conversation.
- **CRIME_ALARM**: TRESPASSING when T has no crime and stands in a town of S's faction, then T's crime timer to 30 s (live). The town alarm manager is a HOOK. **ASSIGN_BOUNTY**: flat add when T has no pending crime, else commit it when the speaker's CHARACTER `assigns bounties` (UNSURE: the exe reads it through the faction).
- **Not yet**: the STAY_NEAR_HOME leash; interrupt, crowd (slice 3). AI contract and change AI landed with the package Blackboard (task BY): the line's `AI contract` records push timed contracts on the speaker's platoon, and `change AI` replaces its table with one priority-0 package (non-player speakers) - docs/reference/RUNTIME_SYSTEMS.md G1.

**Step 6b (individual goals; `individual_goals.cpp`, `state::IndividualGoal` on the Fighter, save v61; test `ksim_dialogue_events` "individual goals", 2026-11-04).** kinspect `findings/dlgtasks_static.md` decoded the three tasks. A goal is a replace order: the follow, route, attack order and sleep end. The routine scorer, the world-squad follow and the leader carry skip a fighter with a goal. Each goal ends by itself (no terminal state), and a sim with no navmesh gives none. DA_RUN_AWAY (34) = **Flee** (task 280 Task_Runaway, on the speaker S): runs to a point 2000..2400 past T (the exe uses the centroid of S's known threats; ours uses T, UNSURE) and takes no target. The exe then wanders 1000..1400 at a time and never ends the task by itself; ours ends at the point, when knocked down, or after 120 s (OURS). DA_GO_HOME (7) = **GoHome** (task 19): the squad's home building (SquadInstance home point), else its home town. It ends inside the building, or within 100 of the town point, or after 600 s (OURS: the exe has no timer). With no home it is not given (the exe: impossible). DA_SUMMON_MY_SQUAD (52) = **Summoned** (task 67) on every other member of S's squad: runs to S, re-routed as S moves, and ends within 100 or after 120 s (OURS: the exe gives up like Task_Move). Not kept: the AGGRESSOR tag (2) that 34 and 7 set on T (its readers are not decoded).

**Step 7 (the scenario tests; `ksim_dialogue_events`, 2026-11-04).** All four §8 scenarios run end to end in the sim. A Holy Citizen shoo re-fires every 2 s and reports no crime (CRIME_LIVE d). The thug follows, then attacks while the intruder is still inside. A witnessed theft gets "Law witness looting ally" (13657), the arrest (melee branch) and a 2500 bounty (CRIME_LIVE a). A starving mugging threatens 4871 -> 4880 -> 4881 (15 s each) and stops at the player's reply. Its attack (98118 "Fine. Have it your way...") hangs under the player's "..." (98039), a reply menu, so it waits for slice 2. Save / load mid-conversation keeps the hash ("conversations"; the lock map in "effects").

**Slice 2, reply menus (`dialogue_sim.cpp`, `DialogueSystem::menu` / `interrupt`; SIMLINK 43 / 44, orders 88 reply / 89 end_talk (87 is the inspect focus, so §13.7's 87 / 88 moved up one); CLOCK `paused` bit 1; save v64; test `ksim_dialogue_events` "reply menus", 2026-11-04).** In automatic selection a target line is spoken by the target, a player character too (140672bf0: T_TARGET / T_TARGET_WITH_RACE the target, T_TARGET_IF_PLAYER only a player one, T_WHOLE_SQUAD the speaker), so the starving mugging ends with the player's "..." (98039) and the attack. A menu opens after the speaker's line when the root is not a monologue or the event is the player's talk (Q8 order, "[...]" for a T_ME child), only with `Simulation::setReplyMenus` (the server's; never in a soak). A running line's `interrupt` opens on the player's talk to the busy speaker (the mugger's 97949 "Drop your weapons..."). Server: the open menus freeze the tick (`ServerContext::menuHolds`), a reply / end_talk lifts it, a gone session clears it, the initial snapshot resends with the resend flag, a menu with no ready session for 30 s ends by a logged EndConversation (reason 5). The §13.4 leash ends an open menu past 2 x TALKING_RANGE (reason 4). UNSURE / not yet (FEATURES): a hit on T and `in_conversation` for T's moves (moot under V1 Freeze: the tick stops while a menu is open, and no client is attached in the grace), several clients (one observer per sim), T_TARGET_WITH_RACE's squad-mate pick, OPEN_WINDOW 45, which end of a bedroll is the pillow. The interjector slots 3..5 are spoken since task CA: inside an interjection node's subtree a T_INTERJECTOR1..3 line is voiced by the interjector (Q10; the manhunters' 54946 / 55096), where slice 2 skipped them.
- Test rigs pin their hand-made house ids every tick (`Rig::house`): `stepBuildings` re-derives a moved character's building, and a follow moves the owner.

---

## 2. Event pipeline

### 2.1 The message

```
struct DialogueEvent { u8 event; Id speaker; Id target; u8 targetKind (character/none);
                       u8 reason (crime kind, EV family detail); u64 tick; u32 seq; }
```

Producers (all already have a natural home; none of them is new state):

| Event | Producer (existing code) | Note |
|---|---|---|
| EV_I_SEE_ENEMY_PLAYER / ALLY_PLAYER / NEUTRAL_SQUAD / ANIMAL_SQUAD / BOUNTY_SPOTTED / ESCAPED_* | `perception::scan` `Noticed` transitions (already drives event 15 on the wire) | classify by the target's faction flags + relation (hostile <= -30, ally >= 50, FactionBook) and crime record. The original re-fires while the sensor sees; we fire on the aware transition and **re-fire every `kSeeRefire` = 30 s** (UNSURE) while `aware` holds, so repetition limits have something to limit |
| EV_SHOO_FROM_MY_BUILDING, EV_INTRUDER_FOUND | `Simulation::stepBuildings` (sets `Fighter::building`) + the resident rule of `stepIntruders` (Allegiance vs door owner) | SHOO: private building of the observer's side, target in a player faction, not friendly (CRIME_STATIC "How a character becomes an intruder"; the live run says shoo is the common outcome). INTRUDER: the full trespass check; one gate still unknown (CRIME_LIVE d). Re-fire every 2 s while inside (measured) |
| EV_THIEF_CAUGHT_STEALING_FROM_ME, EV_WITNESS_LOOTING_ALLY, EV_WITNESS_THIEF_OR_LOCKPICK, EV_WITNESS_GENERIC_ASSAULT, EV_KIDNAPPING_MY_ALLY, EV_EATING_MY_CROPS, EV_I_SEE_UNIFORM_IMPOSTER | the witness reaction (CRIME_STATIC §"Dialogue event each witness fires"); today only the event-14 theft hook exists | needs the minimal crime record (§8) |
| EV_KIDNAPPING_MY_ALLY (58), EV_WITNESS_GENERIC_ASSAULT for the carrier | **perception observing carried characters** (our fix, §2.4, D1): `perception::scan` today skips a fighter with `carriedBy != 0` ("not a target, in no building"); it must keep scanning the carried one *and* attribute the sighting to the carrier, so a witness who is a friend of the carried character (CRIME_STATIC: "witness must be friend of the victim") gets crime 15 Kidnapping on the carrier and fires 58; the enslaving variant (crime 1) likewise | needs the minimal crime record (§8) |
| EV_HEALING_OTHER_START/FINISHED, EV_BEING_HEALED_*, EV_FIRSTAID_KIT_EMPTY | `Simulation::treat` / medical | trivial hooks |
| EV_GET_UP_PEACE / FIGHT / UNNECCESSARY_FIGHT, EV_I_DEFEATED_SQUAD, EV_SPEECH_INTERRUPTED_* | combat arena (KO / wake / fight end) | |
| EV_PLAYER_TALK_TO_ME | interact order 60 action 6 Talk (reserved today) on arrival | the one event a player order raises directly |
| EV_ANNOUNCEMENT, campaign `dialog announcement`, BIOME_GROUP `arrival dialog` | faction layer (campaign lifecycle), group layer (zone entry) | slice 3 |
| EV_CROWD_TRIGGERED, `interrupt`, DA_MASSIVE_ALARM -> EV_I_SEE_ENEMY_PLAYER | the dialogue system itself | |
| events 5 and 7 (unnamed), EV_ACID_*/WINDY/POISON_GAS/BURNING, LOST_LEG/ARM, slavery family | later; no producer until the trigger is known (Q12) or the system exists | |

### 2.2 Threading and determinism

- Producers run inside region phase 1 (perception, buildings, combat all read frozen state) and push
  `DialogueEvent`s onto the **speaker's owner region** queue as tick-boundary messages (ARCHITECTURE:
  cross-border effects are messages). The target is read from the frozen previous tick; nothing writes it.
- The DialogueSystem step runs in phase 2 of the speaker's region: it writes only the speaker (its
  `Conversation`, `DialogueMemory`, its orders) and its squad's memory **if the squad is owned by the same
  work unit**; otherwise the tag goes out as a message to the squad's work unit (squads are their own unit,
  assigned by squad id). Relation and crime changes are global state: they go through the existing command
  path (`applyRelationReason`, a new crime command) at the tick boundary, one tick late, exactly like a hit.
- Queue order is sorted `(tick, speaker, event, target, seq)` before consumption; the roll is
  `hash(seed, speakerId, tick, dialogueSid, salt)` like `UtilityGroupLayer::jitter`. Results do not depend
  on thread count (the 1 = 4 workers test covers it).
- **Budget**: at most `kDialogueEvalsPerTick` (64, tunable) root evaluations per region per tick; the rest
  stays queued and ages (an event older than 2 s is dropped with a counter, since barks about a stale
  sighting are wrong anyway; player-talk events are never dropped). No terminal state: a dropped event only
  means no bark.
- Today the sim is single-threaded for these steps (`Simulation::tick` calls them serially); the design
  above is what the region engine will need, and nothing in it assumes a thread.

### 2.2b The event gate is a switchable policy (TRESPASS_LIVE.md)

The exe's event fire (`140684990`) **drops events from indoor speakers unless the interior is streamed
in** (building+0x1f0 = BuildingInterior, its +0x10 set while streamed; CARRY_KIDNAP_LIVE.md e, kinspect 29c): any player-faction character (PI+0x2b0 = all player-faction chars, corrected by kinspect 7cd47e3; not the selection) is, or was within ~10 s, inside that building. In normal play this only silences buildings with no player in them. Outdoor speakers are not
gated. **DECIDED (decision 14, (user, 2026-11-01, REPORT_2026-11-01 §4)): the default is `FullDetail`**, "get it working now; refinement is a late second pass" (`Exe` stays selectable as the fidelity reference; *was: `Exe` first*). The rest of this paragraph is the original reasoning: whether we keep the exe gate was TBD (user): lean, but mods may want dialogue-driven behaviour (a jailer's
routine, a shopkeeper's crime response) to run with no player around. So the gate is a **policy** applied
at the producer side, before the event is queued, never baked into the book or the state machine:

```
enum class DialogueGate : u8 { Exe,          // indoor speaker needs a player char inside (now or <= 10 s ago); outdoor free
                               FullDetail,   // any speaker at full detail (the sim's own notion of "near a player")
                               Always };     // every instanced speaker, indoors or out, player or not
```

- Settings: a global default (`DialogueTuning::gate`, **`FullDetail`** per decision 14; *was: exe-like `Exe` at first*), overridable **per
  package** in the book (a column of our overlay / the behaviour TSV: `package -> gate`), and so by a mod's
  manifest. The producer looks up `book.gate(packageSet)` for the speaker, which is one array read.
- `Exe` needs "a player character was inside this building within 10 s": `state::Building`-level
  `lastPlayerInside` tick, written by `stepBuildings` when a player-controlled fighter's `building`
  changes; one u64 per building, cheap.
- The policy gates **event production**, not conversations in progress (a bark already running finishes)
  and not EV_PLAYER_TALK_TO_ME (a player is by definition present).
- Cost of `Always` (§4 item 7): the indoor population is most of a town's residents, so this is where the
  budget earns its keep.

### 2.4 Deliberate divergences from the exe (fidelity tests: these differences are on purpose)

| # | Exe | Ours | Why |
|---|---|---|---|
| D1 | Dialogue observation triggers never notice a character being carried or kidnapped (the carried one is dropped from perception; FEATURES "Original gap", 986aa51) | Perception keeps observing the carried character and its carrier; witnesses who are friends of the carried get crime 15 Kidnapping on the carrier and EV_KIDNAPPING_MY_ALLY (58); the crime path (tag, bounty by law, alarm) runs. Law and guards judge by ARCHITECTURE "Guards notice what you carry" (prisoner flag / wanted here), not by friendship. The legitimate uses must not fire it: a carrier who is an ally of the carried (rescue), law moving its own prisoner or a wanted character (arrest, delivery). The original does detect kidnapping (carry inspection `1408549f0`, CRIME_NOTICE C21-C28; the witness event needs a friend of the victim), so the failure in play is two broken links of that chain (CARRY_KIDNAP_LIVE.md: plain carry never a kidnapping, bandit victims refused); with a player in the building a real kidnap does reach dialogue and attack (29c, CARRY_KIDNAP_LIVE e; the indoor gate §2.2b only silences unstreamed interiors); the legitimate uses are the user's reading of why it keeps the carried one out of perception (2026-11-01); the exemption list is OPEN (§11) | a gap, not a design; the user decided to fix it |
| D2 | Indoor speakers fire events only while the interior is streamed in (TRESPASS_LIVE) | A policy (§2.2b); `Exe` reproduces it, `FullDetail` / `Always` do not | streaming artefact; mods want NPC-only behaviour |
| D3 | `world+0x2a0` = the one player faction | A dialogue that targets "the player" **fans out: one dialogue event per player faction** (§6, decisions 7 / 12 (user, 2026-11-01, REPORT_2026-11-01 §4)); `playerContext` is the per-event resolution inside each fan-out copy; NPC-NPC exchanges with no player party see false / no-op. *was: one resolved `playerContext` per event* | one player faction per user |
| D4 | HUD strings composed in the engine | Addressed structured NOTICE events (§7) | the sim never calls the view |
| D5 | DA_TAKE_MONEY burns the cats | Credit the speaker's faction (decision 10, **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4); low stakes, may change late; FEATURES divergence row). **In the code** (`ksim_dialogue_econ`): `state::FactionState::cash` (save v70) takes the payment; a faction with no instance is the exe's burn. A plain counter — not the economy overlay's `TownStock`, which may read it later | real economy |
| D6 | Unknown / unimplemented condition kinds | Fail closed and are counted (exe's own no-handler ids still pass, as in the exe) | never offer a line on a guess |
| D7 | Permanent-chance roll cached in the live session (lost on reload) | A hash of (seed, speaker, line): stable across saves | determinism |
| D8 | A second player talking to a busy NPC: undefined (one player) | Refused `busy` (decision 6) | multiplayer |

Everything else is meant to match; a scenario test that finds a difference not in this table has found a
bug.

### 2.3 Root selection (Q1 / Q3 PROVISIONAL)

**Measured (kinspect 29, DIALOGUE_SELECTION_LIVE / _STATIC), supersedes the "highest wins" rule below:** one selector (Steam 14067a0c0) for roots and children; candidates are sorted at load by own score + best child score (descending, stable); the selector pools the lines that pass the gate and stops at the first plain line scoring below the best; it picks **uniformly at random** from the pool (no weights); a root-like candidate contributes its picked child. A busy character **drops** the event (no queue, no priority number) except through the interrupt wrappers (140685660 / 140685730), which end the current conversation first; 1 s cooldown for EV 3, 4, 0x1b, 0x42, 0x43. Slice 1 follows this (the uniform pick from a seeded stream).

For an event on speaker S with target T:

1. candidates = `book.events[S.packageSet][ev]` filtered by `S.staticPass`;
2. for each: `for enemies` gate (skip if T hostile to S and not set), `target is type` (T's faction
   `fundamentalType` from `tmpl::Faction`), repetition memory, `locked` without an unlock, `one at a time`
   token on S's squad, permanent chance (`hash(seed, S.id, sid) % 100 < chancePerm`), temporary chance
   (per-event roll), then the root `conds` AND-list;
3. score = the line's stored score (`line+0x278`, i.e. `score bonus` as loaded; whether the FCS
   adjustments are baked in is Q1) **+ 2 if the target's race is in the line's `target race` list**
   (CONDITIONS_STATIC 1.4 `140671bb0`); the highest wins, ties broken by the roll. Interjectors are the
   squad mates that pass the same gate, scored by distance (nearest). PROVISIONAL only on the tie rule.

Note the gate is the same function for roots and lines (1.3), so steps 2-3 of this list are the gate of
§1.3 applied to the root, then `pickChild` (§3.3) applies it to each child.

---

## 3. Conversation state machine and timing

### 3.1 State (`state::Conversation`, one per speaker; the original keeps one `dlg` per character)

```
dialogue u32; line u32 (0 = at root); variant u16; mode u8 (normal, interrupt 0x4a);
target EntityRef; playerContext Id (the player faction "the player" resolves to in this conversation, §6; 0 for NPC-NPC. Not a trigger, not required: the EV_* event is the trigger);
speechTimer f64; forcedTimer f64 (DA_FORCE_SPEECH_TIMER, seconds);
pendingReplies vector<u32> (line idx shown to the player); menuSeq u32; replyWait f64;
interjectors array<Id,3>; crowdToken bool; startedTick u64;
```

Saved (a conversation survives a save mid-sentence; cheap and it keeps the hash honest).

### 3.2 Tick (every tick at full detail, after combat and interactions)

```
if speechTimer > 0 or forcedTimer > 0: decrement, return           (the exe: both must run out)
if pendingReplies not empty: replyWait -= dt; if replyWait <= 0 -> end(TimedOut); return
next = pickChild(current)                                           (§3.3)
if none: end(Finished); return
speak(next)                                                         (§3.4)
```

**Speech timer** (`dlg+0x230`): `length x 0.12 + 2.0` seconds (`14067fd80`, DIALOGUE_SELECTION_STATIC; live Q8 H).
The forced timer is seconds (H).

### 3.3 pickChild

Children of the current line, in order of `children`: evaluate `conds` (who = T_ME / T_TARGET resolved
against S and T), chance temporary roll, repetition memory on lines, `target race` lists; keep the passing
ones.

- If T is in a player faction and any passing child has speaker `T_TARGET` or `T_TARGET_IF_PLAYER` (and the
  root is not a monologue, or the event was EV_PLAYER_TALK_TO_ME, census §3 and Q11): those become the
  **reply menu**; send REPLY_MENU to T's controlling client, **freeze T in the conversation** (it cannot
  walk off; **no timeout**, decision 5 (user, 2026-11-01, REPORT_2026-11-01 §4); V1 pauses the sim tick while the menu is open (decision 5 update 2026-11-02, §13.2); *was: `replyWait = kReplyWait` 30 s, UNSURE*). `T_ME` siblings that also pass are the NPC continuing without waiting; the exe's rule is Q11,
  I propose: menu first.
- `T_TARGET` line with T not a player: spoken by T (NPC-to-NPC talk in monologue trees) if T is at full
  detail and idle; else skipped (Q9).
- `interjection` node: pick an interjector = the nearest living, un-busy squad mate of S within talking
  range (Q10, PROVISIONAL), roll the node's `chanceTemp`, then speak its child as that mate (the child's
  effects act with S' = interjector). No mate: skip the node.
- Among the rest: highest score, ties by the roll (same rule as roots).

### 3.4 speak(line)

1. Choose a text variant (`hash(seed, S, tick, line) % textCount`), resolve tokens (WORD_SWAPS by the
   speaker's hash when `persistent use`, else per utterance), append "c.N" for TAKE_MONEY lines.
2. Emit SIMLINK EVENT 18 SPEECH (§7) to every session whose interest covers S.
3. Run the effect pipeline **in the executor's order** (ACTIONS_STATIC §1): lock maps, `change relations`,
   `interrupt`, `give item`, `AI contract`, the `effects` loop, then `change AI`. A JOIN_SQUAD that fails
   aborts the rest (H). Effects fire when the line starts, as in the exe.
4. Set `speechTimer` = `(2.0 + 0.12 x characters)` **hundredths of a game hour** (`14067fd80`; the line holds that many
   game ticks, the SPEECH event carries the wall seconds at 1x); clear `pendingReplies`.

### 3.5 Player replies

Order 71 `reply` -> `Command::DialogueReply{subject = player char, object = speaker, value = option index,
x = menuSeq}` -> validated at apply (the speaker still waits, the menu seq matches, the subject is the
conversation's target and belongs to the sending client's control group) -> `speak(pendingReplies[i])`
on the **speaker's** conversation with the line spoken by T (the SPEECH event names T as the speaker).
Answered with event 9 / 10 like every order (`bad_target`, `not_allowed` when stale).

### 3.6 Interrupts and ending

- A new event for a busy S: `EV_PLAYER_TALK_TO_ME` while S barks a monologue -> the running root's
  `interrupt` list if any (mode 0x4a on the squad, per the executor), else refuse the talk order with
  event 10 reason `busy` (decision 6). Being attacked -> end the conversation, then
  `EV_SPEECH_INTERRUPTED_ATTACKED_BY_TARGET/STRANGERS` as a normal event.
- `end(reason)`: emit EVENT 21 CONVERSATION_END to the target's client when a menu was open; stamp
  `DialogueMemory.lastFired[dialogue] = tick`; drop the `one at a time` token; remove the component.
  `DA_FOLLOW_WHILE_TALKING`'s goal ends with the conversation (FOLLOW_WHILE_TALKING is a goal that
  expires; the no-terminal-state rule).

### 3.7 Memory (`state::DialogueMemory`, per character, saved)

- `lastFired: vector<(dialogueSid idx, tick)>`, pruned on save to entries younger than the longest limit
  (1 week). Repetition units (Q4, measured, DIALOGUE_SELECTION_LIVE): game hours 2 / 6 / 48 / 168 / 5000 / 0.33; the exe's stamp is global per line record, not per speaker.
- **Keys (decisions 4 / 16, (user, 2026-11-01, REPORT_2026-11-01 §4)):** locks per NPC + per player faction (player-gated roots); repetition is global per line, as the exe (so the `lastFired` stamp above is the one global table, not per speaker).
- `locks: vector<(idx, bool)>` (DA_LOCK_THIS_DIALOG, `locks` / `unlocks`): per speaker (the exe's map lives
  on `dlg`, so per character; Q16 answered by the executor's `dlg+0x30`).
- `eventRepeats: (event, target) -> count` for `DC_NUM_DIALOG_EVENT_REPEATS` (window unknown; 1 h UNSURE).
- Permanent chance: **no storage**, it is a hash of (seed, speaker, line) (§2.3); live: cached in dlg+0x120 and never cleared, 19 keys never flipped over up to 712 evaluations (DIALOGUE_SELECTION_LIVE). The exe rolls once per
  (session, line) and caches it on the speaker's session (gate step 12), so per speaker per line is the
  exe's scope too; ours is reproducible after a load and does not depend on when the character was first
  asked. The "speaker reservation" flag (gate step 4) falls out of the same hash.
- Squad memory (`state::SquadMemory` on the squad, saved): short-term tags (INTRUDER, AGGRESSOR, TEMP_ALLY,
  TEMP_ENEMY, PRISONER, CRIMINAL; with a timestamp, decaying) and long-term tags (LT_*, `DC_HAS_TAG`),
  remembered characters (`DA_REMEMBER_CHARACTER`, `DC_MET_TARGET_BEFORE`, `DC_TARGET_LAST_SEEN_X_HOURS_AGO`).
  The crime system needs the same component; build it once.

Save: one version bump for Conversation + DialogueMemory + SquadMemory (v29 at the time of writing).

---

## 4. Condition evaluation cost

The data is 10.9k condition refs, but an evaluation never touches more than one package set's roots for
one event. The levers, in order of payoff:

1. **Event-gated, never polled.** Nothing evaluates without a DialogueEvent (ARCHITECTURE "event
   subscription, not polling"). Event volume is bounded by perception transitions (+ the 30 s re-fire),
   building entries and crime hooks: tens per second across a full-detail area, not thousands.
2. **Index by (packageSet, event).** `EV_I_SEE_NEUTRAL_SQUAD` has 312 entries over all packages; one set
   sees ~5-30.
3. **Static prefilter per character** (`staticPass` bitset, computed when the instance is spawned or
   loaded): everything in the gate that depends only on the speaker's template or on facts fixed at spawn
   is decided once: the **personality masks** (always the speaker, personality rolled at spawn and never
   changed: the single largest kind, 2,125 entries, becomes a bit test done once per character per root),
   `my race`, `my subrace`, `my faction`, `is character` (speaker), `has package`, `DC_IS_FEMALE` /
   `DC_HAS_ROBOT_LIMBS` / `DC_IS_A_TRADER` / `DC_IS_LEADER` (speaker). Roughly 40% of root condition
   entries vanish at spawn, and whole roots with them.
4. **The gate's own order is the cheap-first order.** Masks (bit ops), one-at-a-time, locks, chance,
   the list conditions (set lookups on interned ids), target type, repeat delay, chance permanent (a
   hash), then the DC list: a root that fails the preamble never reaches a relation lookup. The DC list
   itself must stay in **data order** (side effects: retargeting), so there is no reordering inside it; the
   memoised `CondContext` makes order irrelevant for cost: each fact (relation S->W, W's faction flags,
   squad size, in-combat, imprisoned, in my building, talking range, bounty) is resolved lazily once per
   (event, speaker, target) and reused across every root and line of that evaluation. The common preamble
   (`IS_IMPRISONED == 0`, `IN_COMBAT == 0`, census §9) is paid once per event. `CondContext` carries a
   **mutable target** (the retarget rule of HAS_ILLEGAL_ITEM, `target carrying character`,
   `target has item`, `target race`) and invalidates the per-target facts when it changes.
5. **Budget** per region per tick (§2.2) with ageing, so a crowd event cannot stall a tick.
6. **Full detail only (V1).** Dialogue runs for speakers at full detail. Squad-level and strategic NPCs do
   not bark; the *outcomes* that matter there (a mugging that turns into an attack, a guard arresting) come
   from the group layer's scenario rules, and later from a compiled "outcome table" per dialogue (reachable
   effects within depth 2 with their chances) so the coarse level stays honest. Decision 1.

7. **The gate policy sets the volume** (§2.2b). `Exe`: indoor speakers fire only with a player inside,
   so the live population is the outdoor NPCs near players plus the one interior being visited: tens of
   speakers. `FullDetail`: every full-detail NPC indoors too (a town of ~60 residents, ~40 indoors), so
   SHOO re-fires every 2 s from every resident of every entered house, not just the visited one: still
   tens of events/s. `Always`: every instanced speaker in the world (5,700 characters). What saves it is
   that **events need a producer**, and the producers are themselves gated: perception scans run only at
   full detail; `stepBuildings` entry/exit transitions are rare; crime hooks fire on acts. So `Always`
   does not mean "evaluate everyone": it means indoor and far speakers are not dropped *when something
   happens to them*. The cost that does scale is the **re-fire** rules (SHOO every 2 s, I_SEE every 30 s)
   over NPCs whose building holds an intruder; bounded by the number of occupied-by-a-stranger buildings,
   not by the population. Worst case (a raid in a town, every house entered): ~40 buildings x 3 residents
   / 2 s = 60 events/s, 0.2 ms/s with the figures below; the per-region budget caps it anyway. For squad-
   and strategic-level speakers `Always` has nothing to produce events in V1 (no perception, no building
   state); it becomes meaningful with the coarse-level outcome tables of slice 4.

Estimate: 20 candidates x ~8 remaining conditions x ~20 ns ~ 3 µs per event; 100 events/s is 0.3 ms/s.
Memory: the book is ~60k small structs plus the string pool (~3 MB).

Lua: the DC_/DA_ kinds are **native builtins** and the gate above is native; the per-line `when` logic is
generated Lua (§12), woken only for candidates that survive the gate. The numbers above are the native
reference; §12.6 gives the scripted overhead (~1 µs per candidate call).

---

## 5. The action-execution boundary

`DialogueEffects` is a sink the conversation writes to; the engine applies it. Every action maps to one of
six kinds of write, none of which crosses a layer:

| Write kind | Actions | Where it lands |
|---|---|---|
| **Individual goal / order** on S (or S's squad members) | ATTACK_CHASE_FOREVER (DefeatSquad vs T, no range), ATTACK_STAY_NEAR_HOME (ranged), ATTACK_IF_NO_COEXIST, FOLLOW_WHILE_TALKING, THUG_HUNTER (town stalker), RUN_AWAY (forced), GO_HOME, ARREST_TARGET (home-town gate -> lift + cage, else TEMP_ENEMY + melee), ARREST/ENSLAVE_TARGETS_CARRIED_PERSON, PAY_BOUNTY (the cage part), SUMMON_MY_SQUAD (members move fast to S), TALK_TO_LEADER, MAKE_TARGET_RUN_FASTER, OPEN_NEAREST_GATE | `Fighter` order fields (`orderTarget`, `followTarget`, routes) exist; `TakeOut` / `CarryStep` exist for the arrest-carry; **new**: `state::IndividualGoal` on the fighter (DefeatSquad / Follow / Flee / GoHome / Arrest / Stalk, with a target, a leash and an expiry) that the Individual layer (no interface in `ai.hpp`; `individual_goals.cpp` runs them) executes and re-evaluates. Each goal has an end condition (target down / gone / out of leash / timer) and falls back to ordinary life: the no-terminal-state rule enforced by construction |
| **Squad memory tag** | FLAG_TEMP_ALLY / ENEMY, TAG_ESCAPED_SLAVE (slave state), the PRISONER / AGGRESSOR tags that other actions set; REMEMBER_CHARACTER writes a met entry (built, task BB: `state::Fighter::met`) | the transient squad-memory tags (§3.7) and the saved met memory; a message when the squad is another work unit |
| **Faction relation / war / rank / reputation** | AFFECT_RELATIONS (S faction -> T faction, += operand, clamped; no-op on a player faction's own relations), `change relations` (listed faction -> **T's player faction**), DECLARE_WAR / END_WAR, INCREASE_FACTION_RANK, AFFECT_REPUTATION | the existing `state::Relation` + `applyRelationReason` path (new reason `Dialogue` with no floor) and `FactionEvents` (RelationCrossed fires as today). War flag and reputation: new fields on `Relation` / `FactionState` |
| **Crime / bounty** | CRIME_ALARM (alert list, crime timer 30 s live (DIALOGUE_ACTIONS_LIVE d), TRESPASSING if none, town alarm), ASSIGN_BOUNTY (flat add, or commit the pending crime if the speaker's faction `assigns bounties`), PAY_BOUNTY (the claim flag, halved when the prisoner is dead; built, task AY), CLEAR_BOUNTY (built), MASSIVE_ALARM (faction-wide EV 66 + move; live: reaches only the squads on the faction's squad list, 15 of 54 loaded squads missed, DIALOGUE_ACTIONS_LIVE e) | a `CrimeRecord` component on the criminal + a `Bounties` table per (character, legal faction) + a town `Alarms` list (CRIME_STATIC data model). All three are needed by the crime milestone regardless of dialogue |
| **Economy / membership** | TAKE_MONEY (T's faction pays, S's faction is credited: D5 / decision 10, `FactionState::cash` v70; a payment over the payer's cash is refused and counted, the rest of the line still runs), GIVE_MONEY (T's wallet credited; the exe debits nobody), `give item` (each entry into T's `Inventory`; the stack count is 2..49 or 1), JOIN_SQUAD_WITH_EDIT / FAST (membership change: off `MemberOf`, on `PlayerSquad::members`, `Command::ChangeSquad`), SEPARATE_TO_MY_OWN_SQUAD (a player member gets a new player squad, an NPC a new `SquadInstance` of its own side; already alone = no-op), GIVE_TARGET_MY_SLAVES (every non-carried `captorSid` slave to T's side; the carried are dropped), REMOVE_SLAVE_STATUS (slave state and captor cleared, out of the cage slot, orders cleared), KNOCKOUT (the fighter's KO timer forced to at least the operand, in sim seconds: UNSURE, the exe's field units are unverified) | `PlayerFaction::money`, `FactionState::cash`, `Inventory`, squad instancing (`instancesChanged`), `Fighter::slaveState` / `captorSid` / `cageBuilding` / `cageSlot` (T3e2), `Fighter::koTimer` / `unconscious`. Tests: `ksim_dialogue_econ` (TAKE_MONEY and the give item on real lines; SEPARATE and GIVE_TARGET_MY_SLAVES on real lines; REMOVE_SLAVE_STATUS on real line 59278 since task AY, its cage part direct; KNOCKOUT direct — its line 1533728 hangs under a biome arrival root) |
| **Group layer** | `change AI` (whole-squad package swap, non-player speakers), `AI contract` (hours), ASSAULT / RETREAT / VICTORY_PHASE (campaign phase), `trigger campaign`, CLEAR_AI | `GroupAI` gets an **active package override** (sid + expiry) that the behaviour registry resolves exactly like the default package; campaign phase -> `FactionAI` raid entry; `trigger campaign` -> `state::PendingTrigger` (exists) |
| **Client-side windows** | TRADE, CHARACTER_EDITOR, CHOOSE_SLAVES_*, CHOOSE_ANIMALS_BUYING, CHOOSE_PRISONER_BAIL | SIMLINK EVENT 22 OPEN_WINDOW to T's client; the window's *transactions* come back as ordinary orders (transfer, a new `recruit` order). The sim never opens a window; it says one may be opened |
| **Dialogue itself** | LOCK_THIS_DIALOG, locks / unlocks / unlock but keep me, FORCE_SPEECH_TIMER, `interrupt`, `crowd trigger` (v0 % of squad mates start the dialogue, EV_CROWD_TRIGGERED) | `Conversation` / `DialogueMemory` |

Layer rule check: dialogue is an **Individual-layer input** (a reaction), so it may set the speaker's
current goal and the squad's memory. It must not set a *squad* goal directly: ATTACK_CHASE_FOREVER puts a
DefeatSquad goal on the speaker and a TEMP_ENEMY tag on the squad memory; the group layer's scenario
assessment ("a squad mate fights a tagged enemy") brings the rest in, as fight-join does today. `change AI`
is the one squad-level write, and it goes through the registry as a package override, not as code.

Reuse: `Command` factories and `Simulation::apply` for the cross-tick writes; `applyRelationReason` for
relations; `perception::notice` for "T learns of S" when S shouts; `stepIntruders`'s resident rule for
"my building"; `interact.hpp` reach constants for talking range (UNSURE: `DC_TARGET_IN_TALKING_RANGE`
radius unknown, I would start at 300, the slave-selling list radius).

**Live reference for the group layer (kinspect, 2026-11-02):** the Error Code robots
(`~/git/kenshi/kenshi-inspect/findings/errcode_robots.md`, logs `findings/errcode/`). `errorcode sees player` fires only on
a player character in the robot's own building (indoor sight, no stat / relation check); its 999999 h Bodyguard
`AI contract` lands squad-wide (goal 44 FOLLOW_PLAYER_ORDER, player squad tagged TEMP_ALLY, faction unchanged);
`errorcode protect` runs DA_ATTACK_STAY_NEAR_HOME (248) on the first non-player neutral noticed, which started a
24 vs 23 war at Bad Teeth with the player never targeted; marching robots that fall > 8 k u behind unload and never
regroup. A fidelity test once slice 2 reaches contracts. Saves: ki_errcode_base2 / _imprinted2 / _badteeth (GOG).

---

## 6. Multiplayer: several player factions

Ground rule from ARCHITECTURE: wherever the exe tests "the player faction", test **the flag**. Concretely:

- `FactionState::playerFaction` (bool) + the Session's control group = the characters of that faction
  (today `Fighter::playerControlled` is per fighter and "no behavioural effect"; it becomes derived from
  the faction flag + the session binding).
- **DC_IS_PLAYER(T)** = T's faction carries the flag (`faction+0x250` in the exe is "any player-controlled
  faction", so this one is already MP-clean).
- **Terminology.** A dialogue is started by an **event** (EV_*): that is the trigger, and nothing else is.
  Most dialogues are monologues, and the 884 lines with an empty `text0` just run their effects; nobody
  "initiates" them in the talk sense. What multiplayer needs is only a *resolution* of the exe's "the
  player": `Conversation::playerContext` = the player faction that `world+0x2a0` resolves to for this
  conversation, **0 for NPC-NPC**. It is not required for a dialogue to run; a 0 only makes the
  player-reading sites below answer false / no-op.
- **Fan-out (DECIDED (user, 2026-11-01, REPORT_2026-11-01 §4), decisions 7 / 12).** In Kenshi "the player" is always the player
  FACTION (money is faction wide). In co-op a dialogue that targets the player runs **once per player
  faction ("foreach player")**: one dialogue event / `Conversation` per player faction, each with its own
  `playerContext`, instead of one event with one resolved context. *was: one `playerContext` resolved
  per event.* The per-site rules below apply inside each copy. MULTIPLAYER.md §6 (shared worlds, 6.3
  relations) is the other side of this.
- **Every exe site that reads *the* player (`world+0x2a0`) resolves to `playerContext`** (multiplayer
  review, decided; per player faction, see the fan-out above). Set from day one when the conversation opens, from the event's parties: T's faction if
  T is a player character (the talker for EV_PLAYER_TALK_TO_ME, the seen character for the I_SEE family,
  the criminal for the witness family, the intruder for SHOO / INTRUDER, the healer / patient for the
  medical pair); else S's faction if S is a player character (a player character barking); else 0. The
  sites, with their `playerContext` reading:
  - **Null-target fallback** (a T_TARGET condition with no target): the nearest character of
    `playerContext` to S (deterministic: nearest, ties by id), written into the conversation's target as
    the exe does; context 0 -> the condition is false. `TARGET_CHARACTER_EXISTS` runs before the fallback,
    as in the exe.
  - **DC_PLAYERMONEY** (ignores `who`), **DA_TAKE_MONEY / DA_GIVE_MONEY**: `playerContext`'s wallet
    (`PlayerFaction::money`); context 0 -> condition false / effect no-op, counted.
  - **DC_IS_RECRUITABLE**'s roster cap: `playerContext`'s living roster against the global cap
    (`DAT_142133f50` = GLOBAL CONSTANTS `max faction size`, 30 in the merged data); the recruit joins
    `playerContext`'s first squad. The exe also refuses law / military / trader / diplomats (in); UNSURE:
    its stats test (three maximum stat pairs, averaged >= 25) is not modelled.
  - **DC_PLAYERS_BEST_TOWN_LEVEL** (74): the highest `buildings::townLevel` over the towns owned by `playerContext` (-1 with none; false
    with no context). **DC_TOWN_LEVEL_CURRENT_LOCATION** (73): the level of the player-owned town the *speaker* stands in (S, not the
    condition's `who`; false when it stands in none or the owner is not a player faction). Both strict 3-way. OURS: "the town the speaker
    stands in" is `townAt` (nearest centre of the circles containing it); a town's level counts its owner's finished PlayerBuildings inside
    its circle (`towncircle::levelOf`, the same helper as the raid gate: FACTIONS.md 4 "Player gates"), computed lazily once per step.
    Pinned in a running sim by `ksim_dialogue_events` `townLevelSim` (real record towns owned by the player faction, PlayerBuilding rows,
    level 0 -> 1 -> 2 between passes: the cache is per step, so a building finished between passes is seen).
  - **DC_PLAYER_TECH_LEVEL**: per-faction research on `FactionState` (a global object in the exe).
  - `change relations` (listed faction -> the player faction): toward `playerContext`.
  - RELATIONS / REPUTATION with no target: `playerContext`, else false.
  - HUD notices from effects ("Relations with X improved", "Paid c.N", "Received <item>", "War breaks
    out", "Alarm raised"): **addressed events** (§7, NOTICE with an audience: `playerContext`, a faction,
    or everyone in interest), never strings assembled in the sim.
- **T_TARGET_IF_PLAYER** is **not** a condition `who` (it collapses to T_TARGET; 58 IN_MY_BUILDING entries
  are plain target tests); it only matters as a *line speaker*: offered as a reply menu when T is
  player-controlled by a connected client, otherwise treated as `T_TARGET` (Q9 / Q11 for the exe's rule).
- **Relations are per player faction already** (`state::Relation` pairs). `change relations` targets T's
  player faction; with **no target** the exe defaults to "the player faction": in MP that is ambiguous, so
  it is a **no-op with a counter** (decision 7). Same for TAKE/GIVE_MONEY with no T (they `need T` anyway).
- **Two players and one NPC**: one `Conversation` per speaker. A second `EV_PLAYER_TALK_TO_ME` while a
  conversation with a reply menu is open is refused (`busy`); while a bark runs, the `interrupt` rule applies
  (§3.6). Speech bubbles go to every client with interest in the speaker; REPLY_MENU / CONVERSATION_END /
  OPEN_WINDOW go only to the client that controls T.
- **Repetition and lock memory** are per speaker. For roots gated on `DC_IS_PLAYER == 1` (261 roots) the
  memory key is `(dialogue, T's player faction)` so player B is not silenced by player A's visit
  (decision 4). Locks (`DA_LOCK_THIS_DIALOG`, staged recruit conversations) likewise keyed by player faction
  when the root is player-gated.
- **Co-op allies** (allied player factions): `DC_IS_ALLY` is the usual >= 50 relation; nothing special.
- **Crowd / one-at-a-time** tokens are per squad, independent of players.

---

## 7. SIMLINK additions (additive; EVENT kinds and orders, no protocol bump needed)

Server -> client, EVENT (type 50) bodies. Numbering checked against SIMLINK.md §6/§7 at HEAD (d42d45d): 16 and 17 are bleed pool / blood smear; 18+ is
reserved for dialogue. Orders in use are 1-4, 10, 20, 30-32, 40, 41, 50, 51, 60-63; interact action 6 is
reserved "talk"; 71 and 72 are free.

| Kind | Body |
|---|---|
| 18 SPEECH | speaker u64, target u64 (0 none), dialogue strid, line strid, text strid (resolved, tokens substituted; client draws the bubble / panel), duration f32, flags u8 (bit 0 conversation (a menu may follow) vs bark, bit 1 interjection, bit 2 spoken by the target (a reply echoed), bit 3 shout (crowd / alarm)) |
| 19 SPEECH_END | speaker u64 (the bubble closes early: interrupted) |
| 20 REPLY_MENU | speaker u64, player char u64, menuSeq u32, n u8, n x (line strid, text strid). Only to the controlling client |
| 21 CONVERSATION_END | speaker u64, player char u64, reason u8 (0 finished, 1 timed out, 2 interrupted, 3 speaker gone / down, 4 walked away) |
| 22 OPEN_WINDOW | kind u8 (1 trade, 2 character editor, 3 slaves selling, 4 slaves buying, 5 prisoner bail, 6 animals buying), speaker u64, player char u64, arg i32 (the exe's operand, passed through) |
| 23 NOTICE | kind u8 (1 relation changed, 2 paid, 3 received money, 4 received item, 5 war declared / 6 ended, 7 alarm raised, 8 squad full, 9 enslaved, 10 freed ...), subject u64, faction strid, item strid, amount i32. **Structured, no text**: the client composes the wording. **Addressed**: the sim tags each notice with an audience (the `playerContext` faction, a named faction, or everyone with interest in `subject`) and `Session` filters by it, like DetectRecord; a notice never reaches a client it is not about |

Client -> server orders (type 100):

| Kind | Body |
|---|---|
| 60 interact, action 6 talk (already reserved) | `n = 1, id, target u64` : walk to talking range, then EV_PLAYER_TALK_TO_ME(speaker = target, target = id). Answers as interact does (`bad_target` not a character, `not_allowed` KO'd / dead / carried, `busy`) |
| 71 reply | id u64 (player char), speaker u64, menuSeq u32, option u8 -> `Command::DialogueReply`; event 9 / 10 |
| 72 end_talk | id u64, speaker u64 -> closes an open menu (the "walk away" reason 4) |

Everything stays in the sim-never-calls-the-view rule: windows are events, text is data, replies are
commands. Section 10-style per-client filtering already exists in `Session` (DetectRecord is the model).

Text on the wire vs client-side data (decision 2, **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4), follows the dual system of
decision 15): **native-tree lines go as record ids**; the client reads the game packs (players own the
game). A line with word swaps (`/PLAYER/`, town names) needs sim state, so it sends **id + swap values**.
**Lua / our own mod lines send resolved text** (server-side swaps, mods stay server-only; ~100 bytes per
line, string-table interned). *was: I recommended the server sends resolved text for every line, so the
client needs no dialogue data.*

---

## 8. What to build first

**Slice 1: barks that bite (unblocks crime / trespass).** Target: Blister Hill's "shoo civ",
"Bandit thug shoo", "Law spotting a generic crime" (ARREST / CRIME_ALARM), starving mugging, and the
I_SEE_ENEMY_PLAYER family running end to end in `ksim_server`, visible as bubbles in kclient.

1. `DialogueBook` compile (§1) + `tools/dialogue.py`-style counts in a `ksim_dialogue_coverage` tool
   (unknown kinds, dropped records). Test: the census numbers reproduce (1,890 / 22,783 / 293; Manhunters
   effective 63).
2. `packageSet` per character + event index + `staticPass`. Test: `tools/dialogue.py package` agrees.
3. Events: SHOO / INTRUDER (from `stepBuildings` + the resident rule), I_SEE_* (from `Noticed`),
   THIEF_CAUGHT / WITNESS_LOOTING (from the event-14 theft hook, with a minimal `CrimeRecord`: crime kind,
   victim faction, timer; the witness filters of CRIME_STATIC).
4. Conditions, the subset that covers these roots (about 30 kinds: PERSONALITY_TAG, IN_COMBAT,
   IS_IMPRISONED, IS_PLAYER, IN_MY_BUILDING, IS_SLAVE, TARGET_IN_TALKING_RANGE, IS_FEMALE, SAME_RACE,
   HAS_TAG, IS_RUNNING, IN_A_NON_PLAYER_TOWN, RELATIONS, BROKEN_LEG/ARM, DAMAGED_HEAD, SQUAD_IS_DOWN,
   SQUAD_ONLY_ANIMALS, IS_KO, IS_ALLY / ENEMY, IS_LEADER, SQUAD_SIZE, IS_OUTNUMBERED, COPS_AROUND,
   I_LOVE / HATE_THIS_GUY, STRONGER / WEAKER_THAN_ME, the race / faction / world-state lists). The rest
   fail closed and are counted.
5. Conversation state machine without reply menus (monologues only), interjections, speech timer, SPEECH /
   SPEECH_END (18 / 19) on the wire, DialogueMemory (repetition, locks) in the save.
6. Effects: FOLLOW_WHILE_TALKING, ATTACK_CHASE_FOREVER / STAY_NEAR_HOME, RUN_AWAY, GO_HOME, FLAG_TEMP_*,
   LOCK_THIS_DIALOG + locks / unlocks, FORCE_SPEECH_TIMER, AFFECT_RELATIONS + `change relations`,
   SUMMON_MY_SQUAD, CRIME_ALARM, ARREST_TARGET (the melee branch; the cage branch once FIND_CAGE exists),
   ASSIGN_BOUNTY (flat add). This needs the `IndividualGoal` component and a real first cut of the
   Individual layer (DefeatSquad / Follow / Flee / GoHome) which the combat slice already half-has
   (`orderTarget`, `followTarget`).
7. Scenario tests: walk a player into a Holy Citizen home at night -> SHOO every 2 s, no crime (CRIME_LIVE d);
   thug house -> shout, follow, attack when still inside; witnessed theft -> "Stop!" + arrest + bounty 2500
   (CRIME_LIVE a); starving bandits -> mugging line then attack. Save / load mid-conversation keeps the hash.

**Slice 2: talking to NPCs, and the Lua compiler.** The generator + builtins + equivalence corpus of §12
(scripted becomes the default once the corpus passes). Interact action 6, EV_PLAYER_TALK_TO_ME, `CHARACTER.dialogue` roots, reply
menus (20 / 21 / 71 / 72), TRADE / CHARACTER_EDITOR windows (22), recruitment (TAKE_MONEY,
JOIN_SQUAD_*, `ChangeSquad`), GIVE_MONEY, `give item`, PAY_BOUNTY, CLEAR_BOUNTY, the MP keys (§6).

**Slice 2, the slave and bounty pieces (task AY, 2026-11-06).** Built on the sim's own state: conditions
`DC_IS_SLAVE` 59 (slave state 1 or 2), `DC_IS_ESCAPED_SLAVE` 62 (state > 1) and `DC_TARGET_IS_SLAVE_OF_MY_FACTION`
61 (`Fighter::captorSid` is S's faction), read off `state::Fighter::slaveState` / `captorSid` (TOWN_LIFE_SLICE 5.19);
the bounty conditions `DC_CARRYING_BOUNTY_ALIVE` 4 / `_DEAD` 5 (the person in the target's hand, `Fighter::carrying`,
its bounty under S's faction, or a diplomat of a hostile faction) and `DC_BOUNTY_AMOUNT_ACTUAL` 47, read off the
`CrimeRecord` table. `DA_PAY_BOUNTY` 10 pays the bounty of the person T carries to T's side once (`Bounty::claim`,
halved when the prisoner is dead) and `DA_CLEAR_BOUNTY` 49 erases it and clears the pending crime (timer -1);
`DA_ASSIGN_BOUNTY` 32 shares that table. HOOKs: PAY's lift and caging of the prisoner (tasks 213 -> 112) and the
PRISONER tags; CLEAR's tag forgiveness both ways (no long-term tags yet). Tests: `ksim_dialogue_events` §3d (Grey's
59270/59272 for a slave / ex-slave and none for a free character; Seta's hand-in pays 2500 alive and 1250 dead, once,
across a save/load; the police line's assign then pays and clears) and `ksim_dialogue_econ` 5 (Grey's 59278 through the
reply menu).

**Slice 2, the imprisonment, memory, town and wall conditions (task BB, 2026-11-06).** Conditions on state the sim
already had, plus one new memory. `DC_IS_IMPRISONED` 14 (892 refs) is the character's location state 2: a cage,
`Fighter::cageBuilding != 0` (`world_state.cpp` counts a held slave too, UNSURE). `DC_IMPRISONED_BY_TARGET` 7 /
`_BY_OTHER` 8 ask the holder: `Fighter::captorSid` (the faction at the cuff), else the town owning the cage building;
the compared faction is S's, or the dialogue target's when W is S (DIALOGUE_CONDITIONS_STATIC 7; these two are not among
the who-redirected ids). `DC_TARGET_LAST_SEEN_X_HOURS_AGO` 18 / `DC_MET_TARGET_BEFORE` 20 read a new dialogue memory,
`state::Fighter::met` (save v75): the 32 most recent sightings with the sim tick, the oldest evicted, written by the
perception scan and `DA_REMEMBER_CHARACTER` 19 (now applied). Per character, where the exe's hangs off the squad
(UNSURE); a missing or evicted entry reads as the exe's no-entry 9999.99 hours, so a "seen more than N hours ago" line
fires for a stranger. `DC_IN_A_PLAYER_TOWN` 75: the dialogue's town index now holds the player's own outposts
(`state::PlayerTown` of a player faction) as well as the record `TownState`s; `DC_WITHIN_TOWN_WALLS` 57 reads the
perception gate regions (`Environment::inWalls`, APPROXIMATION: the sim has no wall polygon, only the gate circles).
Still failing closed: `DC_REPUTATION` 3 (51 refs) — the sim has no reputation value; it needs the faction reputation
slice (`DA_AFFECT_REPUTATION` 5 writes the same accumulator). Tests: `ksim_dialogue_events` §3e (a caged Dust Bandit
draws 13577 and a free one nothing; the by-target / by-other pair by the holder; the Dust Bandit's 66253 for a target
seen 3 hours ago and not 1, and for one never seen (9999.99 passes its 2+); its 9310 MET_TARGET_BEFORE line; the
perception scan's own stamp; the gate guard's 9521 outside / inside the walls; the Paladin's 96051 in a player town; the
memory across a save/load).

**Slice 2, shackles, cages, backpacks, illegal goods and the carrying / item lists (task BS, 2026-11-07).** Seven conditions and
three gate lists the sim already had state for. `DC_WEARING_LOCKED_SHACKLES` 64: an ARMOUR piece marked `is locked` worn in the
boots (9) or body (5) slot; the data's three locked records are the prisoner shackles (`world_tasks.cpp` wears SHACKLES in slot 9).
`DC_IS_IN_LOCKED_CAGE` 63: `Fighter::cageBuilding != 0` and the warden squad whose `SquadInstance::homeBuilding` is that cage has
`SquadRoutine::cagesLocked` (task AE; OURS: one flag per squad, the cage's own lock level is not modelled). `DC_NEARLY_KO` 30 /
`DC_IS_NEARLY_KO` 42: the medical minimum of (health - stun) / max over the organic parts, clamped at 1, below 0.1
(DIALOGUE_CONDITIONS_LIVE (d); UNSURE: a severed limb reads as nearly KO by that formula). `DC_NUM_BACKPACKS` 37: the squad members
wearing a CONTAINER in the BACKPACK slot (the data's bags are CONTAINER records with `slot` 12). `DC_CARRYING_SOMEONE_TO_ENSLAVE`
46: the person in W's hand, alive and class civilian 4 / slave 6 / bandit 8 / adventurer 9 (UNSURE: the exe's ignore list is not
modelled). `DC_HAS_ILLEGAL_ITEM` 69: W or a squad member S's sight table knows holds an item on the law faction's list — the faction
owning the town S stands in, else S's; the list is the union of the FACTION `forbidden items` and the `illegal goods` of its trade
culture's ITEMS_CULTURE (UNSURE: the exe reads one list at faction+0x100, and the Holy Nation keeps Hashish only in its culture), and
a hit retargets the dialogue to that member. Gate lists: `target carrying character` (a member of the target's squad, the target
alone when it has none, carrying a person of the list; a non-target carrier must be in S's sight records; retargets), `target has
item` (through the inventories, the worn backpack's contents and containers recursively), `target has item type` (UNSURE: ours
compares the data record type, and every list entry is an ITEM record, so it reads "any ITEM-class item"; retargets). `my subrace`
246 still fails closed: the RACE data has no subrace field. `DA_TAG_ESCAPED_SLAVE` 25 sets the state to ESCAPING (2) and clears the
slave's orders (UNSURE: the exe's escape timer is not modelled); `DA_ENSLAVE_TARGETS_CARRIED_PERSON` 40 stays out (it needs the
dialogue-issued orders 213 / 112). Tests: `ksim_dialogue_events` §3f (the Hungry bandit's 5405 shackles line 5412 and 5406 cage
line 5415 and their failures; the Slaver's 5267 nearly-KO target and the bodyguard's 5101 nearly-KO speaker, 5096 healthy; the Dust
King's 7154 two backpacks and the item-type lines for Hashish; the gate guard's 1534096 carried King Gurgler and 1534099 Gurgler
Head; 9527 Hashish vs 9522 clean; the slave trader's 49715 carrying a bandit vs 49696; the slaver's 5252 tagging the spotted slave).

**Still open in slice 2.** The Lua generator, builtins and equivalence corpus (§12) are not started. Which actions,
conditions and lists are ported, partial or missing is kept in the coverage tables, not here: `kcov gaps --kind dlg_action`
/ `dlg_cond` / `dlg_list` (Dolt `kenshi_coverage`; `docs/reference/DIALOGUE_COVERAGE.md` is generated from them). The ids with no
handler in the exe (FACTION_VARIABLE, IMPRISONMENT_IS_DEATHROW, NULL-target SQUAD_SIZE, MY_MISSION_IS_FRIENDLY) pass, as
in the exe; PLAYER_SELL_PRISONERS (50) and SURRENDER_MEMBER (51) have no branch in the exe (dead).

**Slice 3: the world talks.** Medical / get-up / defeated events, slavery family, campaign phases and
`trigger campaign`, announcements, biome arrival dialogs, `crowd trigger`, `interrupt`, MASSIVE_ALARM,
DECLARE / END_WAR, the remaining conditions. (`change AI` / `AI contract` landed with task BY.)

**Slice 4: coarse levels and mods.** Compiled outcome tables for squad-level NPCs; Lua `DC_SCRIPT` /
`DA_SCRIPT`; hot reload of the book.

---

## 9. Open questions: which blocks what

| Q | Blocks | Slice 1 stance |
|---|---|---|
| 1 sibling / root selection formula | fidelity of *which* line, never correctness | **answered** (§2.3 note, DIALOGUE_SELECTION_LIVE): pool of the best-scored passing lines, uniform pick |
| 2 `lines` v0 (0/50/66/100) | possibly timing or weighting | **answered**: never read by the exe; stored, ignored |
| 3 root choice among many, `one at a time` key | same as 1; token per squad | per squad |
| 4 repetition units and scope | tunables only | **answered** (DIALOGUE_SELECTION_LIVE): game hours 2 / 6 / 48 / 168 / 5000 / 0.33; the last-shown stamp is **global per line record** (all speakers and targets share it; parents stamped too). Ours per speaker would diverge: decide (§11) |
| 5 package merge / priority | which roots a character has (visible in coverage) | **answered**: the first non-empty package level wins, union within a level's list; package `dialogs` triple = event ids, 0 and 100 skipped |
| 6 `once only` vs DR_ONCE_ONLY; lock persistence | nothing (1 record; locks saved) | same thing; saved |
| 7 operand units | **answered** by ACTIONS_STATIC (9 actions read it) | done |
| 8 condition semantics | **answered** by CONDITIONS_STATIC (§1.3 here) except: COPS_AROUND's threat-manager flags (what we substitute), STRONGER's stat, the `>=` nuance (live check 3), the null-target fallback with several player factions (live check 2) | implement as decoded; COPS_AROUND = law / in-town military in S's sight records (UNSURE) |
| 9 `who` per event, T_TARGET_WITH_RACE, T_WHOLE_SQUAD, T_TARGET_IF_PLAYER when not a player | **slice 2** (menus) and §6 | monologues only in slice 1 |
| 10 interjector choice | cosmetic | nearest idle squad mate |
| 11 player menu rules | **slice 2** | - |
| 12 unnamed events 5 / 7 | no producer | none |
| 13 stale fields | nothing | dropped |
| 14 trigger campaign timing, crowd % | slice 3 | PendingTrigger exists |
| 15 built-in tokens | bubbles show `/MYTOWN/` until listed | left verbatim, counted |
| 16 "1 h seen memory", per-NPC locks | memory keys | per speaker (executor: `dlg+0x30`) |
| ACTIONS_STATIC 4 (CRIME_ALARM -> trespass crime 11) and CRIME_LIVE d (the unidentified trespass gate) | **crime 11 path of slice 1** | shoo works without it; INTRUDER_FOUND waits for the gate |
| 17 which building the EV 9 predicate resolves (kinspect findings/dialogue_pacing) | only an explanation of a closing-time shoo in a **public** shop | **OPEN**: the live predicate needs vf+0x290 = 0 and the public bar never shooed, but a public shop's keeper fired the whole EV 9 chain at 24:00 and the hook on vf+0x290 saw no call on that building object; the gate stands as decoded (the trespass sensor), the closing line arrives through another dispatcher (unidentified) |

Also needed from the exe, not in the list: the re-fire interval of I_SEE events and the
`DC_TARGET_IN_TALKING_RANGE` radius. One live hook each. (The speech timer formula `dlg+0x230` is decoded:
`(2.0 + 0.12 x characters)` hundredths of a game hour, §3.4 / findings/dialogue_pacing.)

---

## 10. Risks

- **Home buildings.** The original's shoo / trespass checks compare *the observer's home building* with the
  building X is in. Our only ownership is `Door::ownerSid` (faction) vs `Allegiance`: every Holy Nation
  citizen would shoo you out of every HN shop. Slice 1 needs `SquadInstance::homeBuilding` from the spawner
  (TOWN residents are placed in buildings; whether the spawner records which is not something I verified).
  Decision 9.
- **Crime record does not exist yet.** Dialogue's most valuable events (witness family) and three of its
  effects need it. Build the minimal `CrimeRecord` + `Bounties` from CRIME_STATIC §0 inside slice 1, not a
  full crime milestone.
- **Individual layer is a stub.** The effects are orders; without goals that expire, ATTACK_CHASE_FOREVER
  is a terminal state. The goal component with end conditions is the real work of slice 1, more than the
  dialogue code.
- **Event storms.** The original fires EV 9 every 2 s per observer (79 events over two short runs with six
  observers). With a dozen residents and the 30 s I_SEE re-fire this is fine; with crowds at a gate it is
  the budget (§2.2) that keeps the tick flat. Measure in the soak.
- **Speech timing is a guess** until hooked; conversations will feel wrong before they feel right.
- **Trespass gate unknown** (CRIME_LIVE d): crime 11 may never fire from perception in slice 1. The
  `DA_CRIME_ALARM` route ("Assistance!" with the target inside the speaker's non-public building) is a
  second path and is implemented in the same slice.
- **Scope creep via windows.** TRADE and the CHOOSE_* windows pull the economy UI in; keep OPEN_WINDOW as
  an event and let the client stub it.

---

## 11. Decisions for the user

1. **Dialogue at full detail only in V1**; squad / strategic NPCs get outcomes from the group layer, later
   a compiled outcome table. (Alternative: run conditions + effects without text at squad level; cost ok,
   but every effect then needs a coarse-level executor.)
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): full detail only; the existing system has no coarse mode.
2. **Resolved text on the wire** (server-side word swaps; the client has no dialogue data). Alternative:
   send sids and let kclient read the records.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): follows the dual system (#15). Native-tree lines may go as record ids (the client reads the game packs); lines with word swaps send id + swap values; Lua / our own mod lines send resolved text. **Changes the recommendation** (was: resolved text for everything; see §7).
3. **Revised (§12): the trees are compiled to Lua on the player's machine; DC_/DA_ are native builtins;
   the hot gate stays native.** The earlier version of this decision ("conditions in C++, Lua only for mod
   kinds") contradicted ARCHITECTURE "Behaviour layer: Lua", which lists dialog conditions and choices as
   Lua. What remains to decide is the *order*: native reference evaluator first, generated Lua second
   (recommended, §12.8), or Lua from day one.
   **DECIDED** (user, 2026-11-03): order settled by #15 (native first, both runners permanently). The generator is
   the **extension path**, not a second runtime for vanilla: original trees run native; the generator emits a
   readable unit a modder copies and edits, and that package then runs on Lua. It lives in its own source
   subdirectory, linked both as a standalone tool and into the engine; **the engine converts on the player's
   machine** (converted game data is never redistributed). Caching the output: TBD, measure first (no
   multi-GB cache imposed on users; §12.6 estimates the dialogue units at a few MB). §12.1.
4. **Memory keys**: repetition and locks per speaker; player-gated roots additionally keyed by the target's
   player faction.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): per NPC + per player faction (player-gated lines remembered per player faction). Line repetition is settled by #16.
5. **Reply wait cap** 30 s, then CONVERSATION_END timed out, no line spoken; the NPC resumes ordinary life.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): the exe freezes time and removes the UI; you cannot walk off (a guard search can't be ignored). V1: **freeze the character in the conversation** (it cannot walk off), **no timeout**. Real-time questions (attacked mid-conversation, etc.) are a late refinement, not a blocker: "make it work". **Replaces the 30 s cap** (was: 30 s then CONVERSATION_END timed out).
   **UPDATED (user, 2026-11-02): V1 freezes the SIM** (the tick pauses, as the original pauses time) while a client-facing interaction is open (reply menu, or a window opened by dialogue); one switchable policy per interaction, default `Freeze`, degrading to the character-only `Hold` on disconnect / no controlling client / persistent servers; no auto-timeout while the client is connected, a server guard log instead. Temporary "get it done" choice; refinements (a) classify dialogues `Freeze` vs `Hold` as data, (b) real-time action sets later. Lifecycle, pause lifting rules and tests: **§13**. *was (2026-11-01): freeze only the character, the world runs on.*
6. **A second player talking to a busy NPC is refused** (`busy`), not queued; barks can be interrupted by
   the root's `interrupt` list only.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): as recommended (refused `busy`).
7. **`change relations` / money effects resolve to the conversation's `playerContext`**; with none
   (NPC-NPC) they are a no-op, counted (the exe defaults to "the" player faction). Decided by the
   multiplayer review; listed here so the counter's existence is agreed.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): "the player" is always the player FACTION; in co-op a dialogue that targets the player **fans out, one dialogue event per player faction ("foreach player")**, not one resolved `playerContext`. **Changes §6** (was: resolve to one `playerContext`).
8. **Unknown condition kinds fail closed** and are counted (the dialogue is withheld), never fail open.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): fail closed, counted.
9. **Per-squad home building** is added to the spawner / `SquadInstance` for the shoo and trespass rules.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): as recommended (engineering).
10. **TAKE_MONEY**: the exe burns the cats (the recipient is not credited). Keep the exe's behaviour, or
    credit the speaker's faction (the real economy goal)? I lean to crediting; it is a one-line divergence
    worth recording in FEATURES.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): credit the speaker's faction. Low stakes: most factions don't use money that way; may change late (our own mods / improvements). Recorded in FEATURES (divergence row). **Landed** (task AK): `FactionState::cash`, save v70, `ksim_dialogue_econ`.
11. Package precedence (Q5): **answered by the exe** (DIALOGUE_SELECTION_STATIC): the first non-empty
    package level wins, a union within that level's list. Follow it (no decision left unless you want to
    diverge).
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): follow the exe.
12. **`playerContext` = the player faction of the event's player party** (§6: T's if T is a player
    character, else S's, else 0; my reading of which party that is per event family is the part to
    confirm). It is a resolution of "the player", not a trigger; the event is the trigger. NPC-to-NPC barks that test PLAYERMONEY (81 T_ME entries) go
    quiet in ours; the exe would read the one player's cash. Accept.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): same answer as #7: "the player" is the player FACTION and the dialogue fans out per player faction (§6). `playerContext` survives only as the per-copy resolution. Changes §6.
15. **Lua conversion order** (§12.8): A (native reference in slice 1, generated Lua in slice 2, native
    `when` retired in slice 3) vs B (Lua from day one). Recommended A. Also: the generated unit format
    (§12.2) is the mod-facing contract; review it before slice 2.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): native first. If not too complex, **run dual systems permanently**: each dialogue package picks its runner; trees that work on the original data stay native (99% of existing dialogue untouched). **Slice 3 "retire native" is dropped. Changes §12.8** (was: recommended A including retiring native).
14. **Event gate policy** (§2.2b): ship `Exe` as the default with `FullDetail` / `Always` selectable
    globally and per package; or ship `FullDetail` as the default (it is what the sim's own detail levels
    already mean) and keep `Exe` only as the fidelity reference for scenario tests. I lean to
    `FullDetail` default: the 10 s streamed-interior rule is a streaming artefact, not a design.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): `FullDetail` default. "Get it working now; refinement is a late second pass": we have to be late in the project, writing our own mods, to find this system's constraints. Also settles STATUS "Indoor dialogue gate default" and TRESPASS_LIVE's gate question.
13. **Comparator divergences are kept, not fixed**: `DC_RELATIONS > 15` means `>= 15` in the exe, the data
    was authored against that, and so does our compiler. The coverage tool prints the normalised op so a
    modder sees it.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): follow the exe (keep the quirks).
16. **Repetition scope** (Q4, DIALOGUE_SELECTION_LIVE): the exe stamps a line's last-shown time **once per
    line record, globally**: when any NPC says "Get out of here!", every other NPC with that line is quiet
    on it for its delay (parents stamped too). Copy that (fewer duplicate barks in a crowd, and the data was
    tuned against it), or keep the per-speaker memory of §8 (each NPC has its own clock; more chatter).
    I lean to copying the exe, global per line, and adding a per-package option later if a mod wants it.
   **DECIDED** (user, 2026-11-01, REPORT_2026-11-01 §4): global per line, as the exe.

---

## 12. Converting the dialogue trees to Lua (user's proposal; ARCHITECTURE "Behaviour layer: Lua")

ARCHITECTURE already lists "dialog conditions and choices" among what Lua decides. The question is not
whether but how, under two hard constraints: **the vanilla text is the game's content and cannot ship in
our FOSS repo**, and **mods keep shipping FCS records**. So this is a *compiler that runs on the player's
machine*, not a one-time port, and §1's `DialogueBook` is its front end.

### 12.1 Pipeline

```
kdata records (base + mods, our overlay)  ->  DialogueBook (§1, the IR: merged, cleaned, normalised)
    ->  Lua generator (install / first run / book hash change)  ->  <data dir>/dialogue/<package>.lua, <dialogue>.lua
    ->  mod overrides by path (a mod ships hand-written units with the same names; manifest load order)
    ->  ScriptHost loads one unit per (package set) on demand; cached by book hash
```

The IR is what makes it a compiler: inheritance merge, orphan dropping, comparator normalisation, tag
masks and the `who` collapse (§1.3) are decided once in C++, and the Lua that comes out is already
"what the exe does", not "what the record says". Generated files are regenerated whenever the record stack
hash changes (a mod added) and are never committed; the repo holds the generator, the builtins and the
default hand-written units we choose to write (our own behaviours, replacing jank).

**Decided 2026-11-03 (§11 #3):** vanilla packages run on the native evaluator; the generator is the modder's
starting point (export a dialogue or package, edit, ship). Code layout: the generator is its own subdirectory
and library, linked into a standalone CLI and into the engine, which runs it on the player's machine. Whether the
generated units are cached on disk or produced in memory on load is open until measured (size and time).
"Linked into the engine" means the generator **code**, never generated output: no build step reads game data
(a CI build, e.g. a GitHub Windows runner, has none); the binary ships no converted content, and conversion
happens only at run time against the player's install.

### 12.2 One readable unit per dialogue, one index per package

A dialogue becomes a Lua module a modder can read and replace. Sketch (generated from "Bandit thug shoo",
census §8):

```lua
-- 13509-Escapes.mod  "Bandit thug shoo"   generated: do not edit; copy to mods/<name>/dialogue/ to override
return dialogue {
	sid = "13509-Escapes.mod", monologue = true, repeat_limit = "vshort_020", chance = {100, 100},
	root = { },                                   -- no root conditions
	lines = {
		L13510 = { say = {"Who the fuck are you?", "..."}, effects = {follow_while_talking()},
		           next = {"L13511", "L45583"} },
		L13511 = { say = {"/FUCKOFF/!"},
		           when = function(c) return c.target.in_my_building() and not c.target.is_running() end,
		           effects = {attack_chase_forever()} },
		L45583 = { say = {"/GETLOST/, move it!"}, when = function(c) return c.target.in_my_building() end,
		           next = {"L45587"} },
		L45587 = { say = {"Shoulda done as you were told!", "..."}, when = function(c) return c.target.in_my_building() end,
		           effects = {attack_chase_forever()} },
	},
}
```

- `when` is a plain Lua function over the native builtins (12.3); the generator emits the short-circuit AND
  in data order, `>=` where the exe means it, and nothing for masks (12.4).
- `say` keeps the text variants verbatim (they are the player's data on the player's disk); tokens stay as
  `/NAME/`; the engine resolves them.
- `effects` is a list of builtin constructors returning **commands** (12.3); the engine applies them in the
  executor's order (§3.4), not the script.
- Per package: `<package>.lua` = `return package { inherits = {...}, events = { shoo_from_my_building =
  {"13509-Escapes.mod", ...}, ... } }`; the engine builds the `(packageSet, event)` index from it exactly
  as §1.2 (the index is native: Lua is not consulted to know who may speak).
- Hand-written units use the same shape plus whatever Lua they like within the sandbox; they can also
  declare `subscriptions` and `fields` (LUA_API §4, §7) for behaviour beyond the tree (a jailer's routine).

### 12.3 DC_ / DA_ as native builtins

Each DC_ kind is a C function on the `CondContext` of §4 (`c.target.relations()`, `c.me.in_combat()`,
`c.target.squad_size()` ...): one memoised read, no allocation, no Lua table traffic, returning a boolean
or an integer. Each DA_ kind is a constructor that returns a small command record (`{"attack_chase_forever"}`,
`{"take_money", 300}`) that the engine validates and applies through the effect sink of §5: a script never
writes the world (LUA_API's "read a view, return commands"). The retarget rule (HAS_ILLEGAL_ITEM,
`target race` ...) is a builtin that mutates the context's target and returns the result, so data order
still carries its side effects. Unknown builtin name in a hand-written unit = load error, logged, the unit
is skipped (fail closed, no terminal state: the NPC just has fewer lines).

### 12.4 What stays native: the hot gate as subscriptions

Lua is woken only for **candidates**. Native, before any Lua call (the gate of §1.3 steps 1-12):
personality / tag masks, `staticPass`, the event index, one-at-a-time, locks, repeat delay, chance
permanent / temporary, the list conditions (race, faction, town, world state, is character, has package)
and `target is type`, plus the context facts that every root needs (talking range 260, in combat,
imprisoned). These are the **subscription** of a dialogue unit, generated from the IR: a dialogue
subscribes to `{on = "dialogue_event", event = 9, needs = {"talking_range", "not_in_combat"}, ...}` and
the engine evaluates that natively like LUA_API §4's timers and relation crossings. Only roots that survive
the native gate get their `when` called, and only lines of a running conversation get theirs. The native
side also owns the state machine (§3) and the effect application; Lua decides, the engine runs.

### 12.5 Equivalence testing against the reference evaluator

The native evaluator of §1.3-§2.3 is kept as the **reference** (the pattern the faction layer already uses:
`UtilityFactionLayer::nativeDecide` vs `faction_intent.lua`, bit-identical state hash, `ksim_behaviour`).
Tests: (a) for every root and line in the book, a generated-Lua evaluation and a native evaluation over a
corpus of synthetic contexts (fuzzed facts: relations, squad sizes, flags, with and without target) must
agree, condition by condition, including the retarget side effects; (b) the CRIME_LIVE scenario tests run
twice (native / scripted) with equal state hashes; (c) the 1 = 4 workers test with scripts on. **Superseded by decision 15 ((user, 2026-11-01, REPORT_2026-11-01 §4)):** the native evaluator is **not retired**; native and Lua run side by side
permanently, each package picks its runner (§12.8). Tests (a)-(c) stay as the equivalence oracle for the
generated units. The "no silent duplicate fallback" direction (LUA_API §8) still holds: a broken unit
fails loudly for its dialogue, it does not fall back to native. *was: the native evaluator is retired to
`retired/` only after (a)-(c) hold, per "retire, not delete".*

### 12.6 Cost under the per-call instruction budget

- Volume: a `when` call runs once per surviving candidate root (§4: ~5-20 per event after the native
  gate) and once per child when a line advances. A root `when` is 12-17 ANDed builtin calls; a line's is
  1-4. At roughly 10 VM instructions per builtin call plus the call itself, a root is ~300 instructions,
  far under the 50,000 per-call budget (LUA_API: count hook, sticky). So one *call per candidate*, never
  one call per event covering every candidate (that would be the whole package's roots in one budget, and
  it would make the budget the thing that decides which NPC speaks).
- Latency: a `lua_pcall` of a tiny function is ~1 µs; 20 candidates = 20 µs per event versus 3 µs native.
  At 100 events/s that is 2 ms/s: acceptable, but it is the reason the native gate (12.4) must drop most
  roots first, and why `Always` mode (§2.2b) plus Lua is the combination to measure in the soak.
- Memory: one `lua_State` per region thread (ARCHITECTURE), 4 MiB budget each. The generated units for
  1,890 dialogues are ~25k lines of Lua; loaded **lazily per package set on first use** and evicted by
  LRU if the budget is tight; the text variants are the bulk (~1.5 MB as strings) and can stay in the
  native string pool with the Lua holding only ids (recommended: `say = {13510, 3}` = line, variant count).
- Determinism: candidates are called in the sorted order of §2.2; the only randomness is `ksim.random`
  seeded from `hash(seed, speaker, tick, line)` for that call; no `pairs` over object keys (ScriptValue
  rules); the same script text on every thread.

### 12.7 Saves

No Lua heap is saved (LUA_API §7). The conversation state machine, `DialogueMemory`, `SquadMemory` and
`Conversation` are native components (§3) and are what the save holds. A hand-written unit that needs
memory of its own declares `fields` on the **speaker** (a `DialogueScriptState` list on
`CharacterInstance`, the `FactionAI.script` pattern: typed, sorted, synchronised with the declaration on
load, at most 16). Generated units declare none. Unit text is not saved; a mod change between save and
load is safe by construction.

### 12.8 What it changes in the build order, and a recommendation

> **DECIDED (decision 15, (user, 2026-11-01, REPORT_2026-11-01 §4)): native first; then run both systems permanently.** If not too complex,
> the native evaluator and the generated Lua run **side by side for good**: each dialogue package picks its
> runner, and trees that work on the original data stay native (99% of existing dialogue untouched).
> **Slice 3 "retire native" below is dropped.** *was: A with slice 3 retiring the native `when` to
> `retired/`.* Consequences: the native evaluator keeps full maintenance (it is not "written knowing it
> will be retired"); the wire format follows the same split (decision 2, §7).

It does not change slice 1's content; it changes what slice 1's evaluator *is for*. Two orders:

- **A. Native reference first, Lua in slice 2.** Slice 1 ships the native evaluator (needed anyway as the
  reference of 12.5) and the native gate; slice 2 adds the generator, the builtins and the equivalence
  corpus, and switches the default to scripted; slice 3 retires the native `when` evaluation to
  `retired/` once (a)-(c) hold (**dropped**, see the note above). Cost: the native evaluator is written knowing it will be retired (small:
  ~70 kind handlers over `CondContext`, which the builtins reuse one for one).
- **B. Lua from day one.** Slice 1 writes the IR, the generator and the builtins, and no native `when`.
  Faster to the modding payoff, but nothing to test the generator against except the exe, and the
  conversion is "scary" precisely because the exe's semantics (who collapse, `>=`, retargets) are easy
  to lose in generation; a corpus test against a native oracle is how that fear is retired.

**Recommendation: A.** The builtins *are* the native evaluator's kind handlers exposed to Lua, so A costs
one thin layer, not a second implementation; it gives the equivalence oracle the faction layer already
proved the value of; and it keeps slice 1 (crime / trespass) on the critical path without the generator.
Fix now, so A does not paint us in: `CondContext` is designed as the builtin surface from the start (one
function per DC_ kind, memoised, with a mutable target); the effect sink takes command records, not direct
calls; the index and the state machine never depend on where `when` is evaluated. The generator's output
format (12.2) is the mod-facing contract and should be reviewed with the user before slice 2.

---

## 13. Client interaction lifecycle (addendum, Fable, 2026-11-02)

> Status: proposal, read-only review, with one user decision folded in (**V1 freezes the sim**, below;
> decision 5 in §11 updated, `was:` kept). Answers the user's challenge (2026-11-02): dialogue runs on the
> server for everyone (a starving bandit reacts whether or not it wants to *talk*), but a line that needs
> the player's screen (a reply menu, a trade / recruit / slaves / bail window) has to coordinate with a
> client that may be slow, gone, or looking at two menus at once, **without a terminal state** and without
> the sim ever waiting on the client's UI. Sourced facts are marked (SIMLINK, MINE_SELL_SLICE 3.3,
> MULTIPLAYER 2c/2d); the rest is proposal. Wire deltas are additive (§13.7). Slice 2 work (§8); slice 1
> (barks) needs none of it.

### 13.1 Two kinds of client-facing interaction, and only one of them is sim state

The dialogue engine produces exactly two things a client must act on, and they want different owners:

| | **Reply menu** (REPLY_MENU, §3.3) | **Window** (OPEN_WINDOW: trade, character editor, slaves, bail, animals) |
|---|---|---|
| What waits | the speaker's `Conversation` (NPC S holds a line open for T's answer) | **nothing in the sim**. The action fires at line start like every effect (§3.4 step 3), the conversation runs on to its end, and the window outlives it |
| Owner | `state::Conversation` on S (§3.1: `pendingReplies`, `menuSeq`); already **saved** | the **session** of the controlling client, like `Session::trades_` today (sourced: MINE_SELL_SLICE 3.3, `simlink.hpp OpenTrade`): not sim state, not saved |
| Audience | T's **player faction** (not a socket): "the controlling client" is whichever session controls T when the event is sent or resent | the session the OPEN_WINDOW went to; a window never changes hands |
| Transactions | order `reply` -> `Command::DialogueReply` (§3.5), logged, deterministic | ordinary orders (68 transfer_entry, a future `recruit` / `buy_slave` / `pay_bail`), each validated against sim state at apply |
| Ends | the reply, `end_talk`, or a forced end (§13.3) | the client (86 trade_close), a closing rule (§13.5), load / new_world (event 40 reason 2, sourced) |

So a **generic "pending client interaction" component is not needed** in the sim: the menu is already a
field of a saved component, and a window is a session record whose consistency comes from validating
every order against the sim. What is needed is one **server-side registry of open interactions**
(§13.2: it drives the pause) and the lifecycle of the menu wait (§13.3). (Fallback if kinspect shows the
exe *resuming* a tree after a CHOOSE_* window: that is a menu with `n = 0` and a `window` kind whose
answer is order `reply` with `option` = the window's result code; same lifecycle, no new state. UNSURE;
not needed for TRADE.)

### 13.2 The pause: one policy point, V1 = freeze the sim (user decision, 2026-11-02)

**DECIDED (user, 2026-11-12, kenshi-k16.9.10): Hold is the V1 default; Freeze is the server option `--dialogue-pause freeze`
(`ServerContext::dialogueFreeze`).** The user: "default kenshi pauses the world for a dialog, but our client/server model sucks for that. Maybe make
it all real time for now, even if it breaks inspections... but at least we can get the barman dialog working." One global policy; the classification
(a) below stays later data. Accepted: guard inspections and shakedowns can be escaped. Under Hold CLOCK's dialogue pause bit is never set and
REPLY_MENU's freezes flag (bit 2) is 0; no wire value is new. The paragraphs below describe Freeze, the option.

**DECIDED (user, 2026-11-02): V1 freezes the sim tick while a client-facing dialogue interaction is open**
(a reply menu, or a window opened by a dialogue action), as the original pauses time. Deliberately
temporary ("let's just get this done"), refined later by (a) / (b) below. *was (decision 5, 2026-11-01):
freeze only the character, the world runs on.* The design requirement: the pause must not be able to
become a terminal state, and refinement (a) must be a data change, not a rewrite. So:

**One policy point.** Every open interaction carries an `InteractionPolicy`, chosen when it opens:

```
enum class InteractionPolicy : u8 { Freeze,   // the sim does not tick while this is open (V1 default)
                                    Hold };   // real time; only the two parties are held (§13.4)
policy = book.interactionPolicy(dialogue | package | window kind)  // data: our overlay column, default Freeze
       ∧ server.allowsFreeze                                        // persistent model (pause_policy none) forces Hold
       ∧ controlling session present                                // no client to answer: Hold (§13.3.2-3)
```

`Freeze` degrades to `Hold` whenever a freeze cannot be held honestly (the three terms above); `Hold` never
promotes to `Freeze`. Nothing in the sim reads the policy: the pause is **server state**
(`ServerContext::interactions`, a list of `{kind, speaker, target, session, policy, openedWall}`), a
third OR term in CLOCK `paused` next to the idle pause and the admin pause (sourced: SIMLINK "Pause when
nobody is connected": `paused = idle | admin`; now `| dialogue`). Exactly like the idle pause, it is wall
clock, never saved, never hashed; the speed is untouched and comes back on resume.

**What lifts a `Freeze` (each one is a client order or a server event, so none needs the sim to tick):**

| Trigger | Effect |
|---|---|
| order `reply` / `end_talk` / window close (86) from the controlling session | the interaction leaves the registry; when it is empty the tick resumes. The order is stamped on the first tick after resume, like any order during the idle pause |
| controlling session disconnects or is dropped | its interactions **degrade to `Hold` at once** and the tick resumes (a world cannot wait for a socket); the menu stays open under §13.3 (grace, then a logged `EndConversation`). Reconnect inside the grace: §13.4 resend, and the policy goes back to `Freeze` |
| save (80) | allowed while frozen: the `Conversation` is saved mid-wait, the pause itself is not state |
| load (81) / new_world (84) / server restart | the registry is rebuilt from the loaded `Conversation`s whose target is controlled by a present session (menus: re-sent with the resend flag, frozen again); windows are gone (session state, event 40 reason 2, sourced) so they lift. A loaded menu with no present controller runs under `Hold` |
| server shutdown | nothing: the save holds the conversation; the next start is the load row |
| admin | `EndConversation` is a server-authored command (§13.3), so a server console / admin order can end a stuck interaction; the only escape hatch beyond the client's own `end_talk` |

The tick batch re-checks the pause before every tick it catches up (`simlink_server.cpp`), so a menu that opens inside a batch stops the
world on that tick, not at the batch's end.

**Guard and log (no auto-timeout of the dialogue itself while the client is connected):** `ServerContext`
logs `dialogue pause: N s, kind, speaker, target, session` once at 60 s of wall clock and then every
60 s, counts `dialoguePauseSeconds` / `dialoguePauseLongest` in the server stats, and shows the pause
reason in CLOCK's `paused` byte (new bit value, additive) so a co-op client can display "paused: X is
talking". A server option `dialogue_pause_warn` (default 60 s) sets the log interval; there is no
`dialogue_pause_max`: the freeze ends only by the rows above. The log is the guard; the rows are the proof
there is no fourth state.

**Multiplayer.** In a co-op lobby **one player's menu pauses everyone**; accepted for V1 (user,
2026-11-02). It composes with MULTIPLAYER 2d/6.2: `pause_policy` governs *who may pause by order*; the
dialogue pause is not an order and is not gated by it; the persistent model (`pause_policy = none`)
forces `Hold`, since a shared persistent world never pauses. Refinement (a) changes this exactly where it
hurts: a barkeeper chat classified `Hold` never pauses the lobby; only non-optional interactions (a gate
guard's inspection) do, and those are short.

**Planned refinements (future direction, recorded, not designed here):**

- **(a)** Classify every existing dialogue, and let our own extended data per dialogue / package carry
  `policy` (`Freeze` | `Hold`): e.g. talking to a barkeeper is `Hold`, a non-optional gate-guard
  inspection stays `Freeze` for now. With the policy point above this is a column in the book's overlay
  (the same place as the §2.2b `gate` column), a mod-visible field, and nothing else moves. Windows get
  the same column per kind; the trade window is the natural first `Hold` entry, because the user's
  "walk away with the window open" behaviour (§13.5) only exists in real time.
- **(b)** A later real-time game may refactor the whole thing: non-optional dialogues get new action
  sets instead of a freeze (e.g. "player is running away from me" -> a hostile loop). §13.4's `Hold`
  rules are the first cut of that world and are what `Freeze` degrades to today, so (b) extends them
  rather than replacing the lifecycle.

### 13.3 The menu wait: a hold, not a terminal state

Under `Freeze` the world does not move, so nothing in the sim can end a wait; under `Hold` (real time)
the parties are held and the world moves. Both need the same guarantee that the wait ends. ARCHITECTURE
already has one legal hold: the AI-off toggle ("a deliberate hold, not a terminal state", because an
agent exists who will end it). A menu wait is the same shape **as long as that agent exists**: the hold is
conditioned on T's faction having a connected, attached client (`PlayerFaction.present`, MULTIPLAYER 2c).

1. **While T's faction is present: no timeout** (decision 5, kept). The wait costs one branch per tick
   under `Hold` (§3.2) and nothing at all under `Freeze`; nothing polls the client.
2. **Controlling client disconnects.** The menu stays open for the session grace (MULTIPLAYER 2c "Common
   details": default 30 s; under the persistent model the linger timer). A reconnect inside the grace gets
   the menu again (§13.4). At expiry the **server** enqueues `Command::EndConversation{speaker, target,
   reason = no_client}` for every open menu of that faction, exactly as it enqueues `FighterDrive stop` on
   disconnect today: a **logged command**, so the replay reproduces it (the disconnect itself is wall
   clock and must never reach the sim directly). The NPC speaks no reply; it ends as "walked away" and
   resumes ordinary life. Consequence accepted: a guard search abandoned by a dropped client is forgotten,
   not enforced; the alternative (hold the guard until the player returns) is a terminal state for an NPC
   and is rejected.
3. **Faction with no connected client when the menu would open** (fan-out copy for an offline co-op
   partner, dormant faction): already §6: `T_TARGET_IF_PLAYER` is offered as a menu only when T is
   "player-controlled by a connected client"; otherwise the line is a plain `T_TARGET` line or skipped (Q9)
   and the NPC continues alone. No wait is ever created without an agent.
4. **Save / load / server restart.** `Conversation` is saved mid-wait (§3.1, incl. `pendingReplies` and
   `menuSeq`); after a load the menu is **re-sent** (§13.4), not ended. This is the opposite of a trade,
   which closes on load because it is session state (sourced, SIMLINK TRADE). Both are right for what they
   are: the menu is the NPC's decision pending, the window is a view.
5. **Sim-side ends** (§13.4) are the existing §3.6 path with new reason codes; they only occur under
   `Hold`, since under `Freeze` the sim does not tick.

The guarantee: every open menu has, at every tick, exactly one of (a) a present controlling faction (frozen
or held, its choice ends it), (b) a running grace timer that ends in a logged command, or (c) a sim
condition from §13.4 that ends it. There is no fourth state. A debug counter `menusOpen` and a
`--log-menus` print (speaker, target, age, policy) in the style of `--log-world-squads` make a forgotten
wait visible in the soak.

### 13.4 Under `Hold`: what is held, what ends it, and the resend

What is held (the real-time fallback today; the default once (a) classifies a dialogue `Hold`):

- **Move-order rule (DECIDED, ksim 2026-11-12, replacing the refusal below):** a non-queued move order of T (move_to, attack, follow, drive,
  interact on something else) ends the conversation with reason 4 (walked away) and is then applied; no `in_conversation` refusal and no new
  event-10 reason. Non-moving orders are accepted. Built: `Simulation::apply` calls `endMenusOf` before such an order; the speaker's `TalkHold` is
  kept ahead while its menu is open (`dialogue_sim.cpp`), the leash and the other ends are unchanged.
- **T (the player character):** a derived `Fighter::hold = InConversation{speaker}` set while a menu
  addressed to it is open (rebuilt from `Conversation` on load, not saved). Movement orders for T
  (move_to, attack, follow, drive, interact on something else; the trade list of event 40 reason 1) are
  refused with event 10 reason **`in_conversation`** (new). Non-moving orders (inventory, equip, job
  edits) are accepted. T's own individual AI picks no move goal while held. The rest of T's squad and
  faction is **not** held.
- **S (the speaker):** its current individual goal is `Converse{target = T, leash = 2 x TALKING_RANGE}`
  (§5 "Individual goal" row; FOLLOW_WHILE_TALKING already lives there). S's group layer may *want* S
  elsewhere; its intent waits until the goal ends, as for any goal. S's squad mates are not held.
- **Time is not held.** Hunger, bleeding, weather, other players run.

Ends during a held wait (all through `end(reason)`, §3.6; CONVERSATION_END to T's client; hold released):

| Condition | Reason | Note |
|---|---|---|
| S or T takes a hit, or S's arena state leaves idle | 2 interrupted | §3.6 already for S; extended to T. Then `EV_SPEECH_INTERRUPTED_*` fires as a normal event, so the hostile branch still runs |
| S KO'd, dead, carried, despawned, or demoted below full detail | 3 speaker gone | detail demotion is the sim moving S "by itself"; the menu cannot survive a speaker that no longer ticks a Conversation |
| T KO'd, dead, carried, enslaved, or leaves full detail (its own bubble, so only via demotion of the area) | 3 | |
| S and T further apart than the leash (physics push, a gate, a carry) | 4 walked away | the leash is the only distance rule; no speech is "shouted across the map" |
| order `end_talk` from the controlling client (also under `Freeze`) | 4 | **V1: always allowed** (default + deferred: per-root "not dismissible" for guard searches is a tuning flag next to the (a) policy column, not foundation) |
| grace expired after disconnect (§13.3.2) | 5 no_client (new) | logged command |
| `EndConversation` on dormancy extraction (MULTIPLAYER 2c "Dialogue in progress: ended as walked away") | 4 | already in the dormant design |
| the speaker's region unload / world reload | n/a | `Conversation` is saved and reloaded; nothing to end |

**Reconnect and load: resend, idempotent.** On `sendTick(initial)` (connect, reconnect, load: the
`resetKnown` path, SIMLINK "Load") the session scans the `Conversation`s whose target it controls and
re-emits REPLY_MENU with the **resend** flag and the same `menuSeq`; the server registry re-adds the
interaction with its policy (so a `Freeze` menu freezes again). OPEN_WINDOW is **not** resent: the window
went with the session; the client talks again, as for TRADE today. `menuSeq` = the low 32 bits of the tick
the menu opened (unique per speaker: one menu per tick; 2^32 ticks at 20 Hz is 6.8 years), so there is no
counter to save, a resend carries the same seq and the client replaces its panel, and a reply with a seq
the speaker no longer has answers `stale`. Nothing double-applies.

### 13.5 Windows are sessions with a validity rule, not modals

TRADE already is one (sourced, MINE_SELL_SLICE 3.3): `Session::trades_` keyed by actor; the sim keeps only
the stock and the wallets; order 68 carries the trader so the sim re-judges range and parties on every
transfer; distance never closes it, a transfer past 1000 u is refused `out_of_range` (MQ8); a non-queued
move order closes it (event 40 reason 1); load closes it (reason 2). Proposal: make that the pattern,
`Session::windows_` with a `kind` (1 trade, 2 character editor, 3/4 slaves, 5 bail, 6 animals) and, per
kind, a **validity rule evaluated per order, never per tick**: parties alive and conscious, counterparty
not asleep / KO'd / out of the detail bounds, actor conscious, range between the *inventories' owners*
(MQ8), allowed source inventories. OPEN_WINDOW (§7) is then a one-shot event that *creates* the session
record (as the talk arrival does for trade) and registers it in `ServerContext::interactions` with the
window kind's policy; a window has no sim identity and no save.

**Implemented 2026-11-12 (kenshi-k16.9.3, k16.9.11, k16.9.12; test `ksim_barman_trade`), the trade window only:** with the dialogue engine on, the player's talk to a merchant runs his dialogue (the Hub
barman, `Bar blackmarket`: the `shopkeep` package's root `trader1` 161-gamedata.quack, "Looking to trade?" -> the reply 182 "Let's trade." carrying `DA_TRADE`, or 184 "No, sorry.").
`DA_TRADE` first ends the conversation (CONVERSATION_END, event 44 reason 0 finished, then the conversation is removed: the exe's `140674830` before `1409524f0`), then calls
`Simulation::windowObserver_` (not OPEN_WINDOW 22: the wire has no new frame); the Session opens the same `trades_` record the direct open makes (TRADE 53 to the session, the stock feed). The
client sees REPLY_MENU, then CONVERSATION_END, then TRADE; no menu stays open behind the window. There is **no fallback**: a merchant whose talk says nothing (no root passes, e.g. the shop
is closed and secured: `houseLocked`) gets `nothing_to_say` and no trade, and a root without `DA_TRADE` just runs its dialogue; a trade opens only from a selected line with `DA_TRADE`
(DIALOGUE_TRADE_STATIC.md, the exe's answer). `--dialogue 0` (engine off only) keeps the direct open on arrival. `DC_BUILDING_IS_CLOSED_AND_SECURED` (11) reads the speaker's squad
package `+0xe8` "house locked" = `Fighter::houseLocked`. Client note: the exe's RMB menu offers Talk To only while a root passes (DIALOGUE_TRADE_STATIC.md); ours always offers it and the sim answers nothing_to_say.

Policy note: under the V1 default a window opened **by a dialogue action** is `Freeze` (user, 2026-11-02),
so the world stands still until it closes; today's stopgap talk -> trade (MINE_SELL_SLICE M4, not a
dialogue action yet) stays real time and is unaffected. The user's original behaviour (walk far with the
window open; sell from the chests of a player building straight to the shop) is real-time behaviour, so
it appears once the trade window is classified `Hold` under (a), and is then two **parameters**, not a
redesign:

| Parameter | Today (implemented) | User's report of the exe | Pending |
|---|---|---|---|
| `window.close_on_move` | yes (event 40 reason 1) | no: the window stays open while walking "absurdly far"; each transfer is range-checked instead | **MQ11** (kinspect). Flip the default to `no` when confirmed; the per-transfer range stays the guard either way |
| `window.sources` | the trading character's own inventory; range from that character | any container of the player's within range of the trader (chests in a player building) | **MQ12, implemented 2026-11-02:** order 68 carries an optional trailing `source` (an owned container); the range is container to trader (1000 u). SIMLINK.md order 68. The 60 u / AABB+40 same-group case is not modelled |

Neither parameter touches sim state, save or threading; both are server options with the kinspect answer
as the default.

### 13.6 Several interactions per client at once

- Sim side: one `Conversation` per speaker (§3.1). A player character T is the target of **at most one
  open menu**: a second NPC whose tree reaches a `T_TARGET_IF_PLAYER` line while T is held treats it as
  "not player-controlled" (§13.3.3) and continues alone, the same rule as an absent client; it does not
  queue, and `D8 busy` stays the rule for the speaker side. A faction with five characters may have five
  menus open, plus any number of windows, plus barks (SPEECH) all round. Under `Freeze` the registry
  holds them all and the tick resumes when the **last** one closes (a frozen world cannot open new ones,
  so the set only shrinks; under `Hold` it may grow).
- Client side: menus are keyed `(speaker, playerChar, menuSeq)`, windows `(kind, actor, counterparty)`;
  the UI stacks or tabs them (one panel per key, newest on top; SPEECH is a bubble, never a panel). The
  **sim never waits on the client's ordering**: each menu resolves independently by its key, a reply to
  menu B while menu A is on top is valid, and a reply carrying a seq the sim no longer has answers
  `stale`. A client that renders a menu as modal is making a UI choice the server cannot see.

### 13.7 Wire deltas (additive to §7) and save

Numbering first: **§7's numbers are stale** (checked against SIMLINK.md today): event 18 is `dismissed`,
19 `discovered`, 20 `lock_roll`, 40 `trade_closed`; orders 71 / 72 are `squad_delete` / `squad_rename`.
Proposed block, for b0 to confirm: events **41 SPEECH, 42 SPEECH_END, 43 REPLY_MENU, 44 CONVERSATION_END,
45 OPEN_WINDOW, 46 NOTICE**; orders **87 reply, 88 end_talk** (87-89 are free). Bodies as §7, plus:

| Where | Delta |
|---|---|
| REPLY_MENU | trailing `flags u8`: bit 0 **resend** (after reconnect or load; the client replaces, no new sound), bit 1 **dismissible** (`end_talk` accepted; always set in V1), bit 2 **freezes** (the client may show "paused" and skip its own idle animations) |
| OPEN_WINDOW | trailing `flags u8`: bit 2 **freezes**, as above |
| CONVERSATION_END `reason` | 5 `no_client` (grace expired), 6 reserved |
| CLOCK `paused` | a new value / bit for "dialogue pause" (additive; a client that only tests non-zero keeps working) |
| event 10 reasons | `in_conversation` (a move order for a held character); `stale` reused for a wrong `menuSeq` |
| Command | `EndConversation{speaker, target, reason}` (server-authored, logged; grace expiry, admin) |
| Server | `ServerContext::interactions` registry (wall clock, not saved), `dialogue_pause_warn` option, stats counters |
| Save | **no new fields**: `Conversation` (§3.1, saved) already holds `pendingReplies` / `menuSeq`; `Fighter::hold`, `Converse` and the pause registry are derived from it on load. The §3.1 version bump stands. The book's overlay gains the `policy` column when (a) starts (data, not save) |

### 13.8 Tests that pin it (slice 2; `ksim_dialogue_menu` or rows in `ksim_simlink`)

Rows 1-3 are pinned at the server / wire level by `ksim_dialogue_menu` (an in-process server and a loopback client): the freeze and
CLOCK's dialogue bit, the pause invisible to the sim (a twin answered at the same tick hashes the same), end_talk, the disconnect's Hold,
the 30 s grace's logged `EndConversation` reason 5, and the reconnect's resend flag and `menuSeq`. The sim half of the same rows stays in
`ksim_dialogue_events` "reply menus".

1. **Freeze:** menu opens -> CLOCK says paused (dialogue bit), the tick count does not advance for 2 s of
   wall clock, the sim hash is unchanged; `reply` -> resumes on the next tick with the reply applied; the
   hash equals a run where the reply arrived instantly (the pause is invisible to the sim).
2. **Disconnect mid-menu (frozen):** drop the session -> the tick resumes at once under `Hold`; for the
   grace the sim hash equals a `Hold` run with the client present; at expiry the log holds
   `EndConversation` (reason 5), the NPC resumes (picks a goal within N ticks), T's hold is gone (a move
   order applies). Replay of the log reproduces it.
3. **Reconnect resend:** drop and reattach inside the grace: REPLY_MENU arrives once with the same
   `menuSeq` and the resend bit, the freeze re-engages, the reply applies; the hash equals the
   uninterrupted run.
4. **Save / load with a menu open:** save while frozen, load, REPLY_MENU re-sent (resend bit), frozen
   again, reply applies, the continued hash equals the uninterrupted run (the pattern of `ksim_trade` /
   `ksim_saves`). Load with **no** controlling session present: the menu runs under `Hold`, the world
   ticks.
5. **Attack during a held wait:** a hit on T (and, separately, on S) -> CONVERSATION_END 2, hold released,
   the next move order applies, `EV_SPEECH_INTERRUPTED_*` queued for S.
6. **Hold:** move_to for T while a held menu is open -> `in_conversation`; inventory move accepted;
   `end_talk` then move_to accepted. Squad mate of T moves freely throughout.
7. **Two menus, one client:** two NPCs, two player characters, both menus open (one pause); replies in
   either order both apply; the tick resumes only after the second; a reply with the other menu's seq ->
   `stale`.
8. **Absent partner (fan-out):** two player factions, one connected; the copy for the absent faction
   never opens a menu (no hold, no pause, the NPC finishes alone).
9. **Policy column:** the same dialogue with `policy = Hold` in the overlay opens the menu without a
   pause; `pause_policy = none` (persistent) forces `Hold` for a `Freeze` dialogue.
10. **Thread count:** 1 = 4 workers with menus open (the §2.2 test pattern).
11. **Windows:** walk the actor 900 u from the trader, transfer ok; 1001 u, `out_of_range`; load closes
    with 40 / 2 (exists in `ksim_trade`); once MQ11/12 land, the two parameters each get a row.

### 13.9 User decisions (foundational only) and defaults

**Decided (user, 2026-11-02):** V1 freezes the sim per open client-facing interaction, as one switchable
policy point defaulting to `Freeze`; refinements (a) / (b) recorded in §13.2. Decision 5 in §11 updated.

**Foundational, still open (shifts the command log / determinism model), one:**

- **D13.1 Forced end after the disconnect grace, as a server-authored logged command** (recommended),
  versus holding the NPC indefinitely while the faction is absent. A disconnect already lifts the freeze
  (a world cannot wait for a socket); the question is what happens to the menu afterwards. Holding it is
  a terminal state for the NPC (a guard waiting for a player who never returns) and violates the core
  rule; the forced end costs one new command type and makes the disconnect visible in the replay, which
  is what `FighterDrive stop` already does. Recommend: forced end; grace = the session grace (30 s),
  linger under the persistent model.

**Defaults with a deferred tag (playtest-able, do not block):** `end_talk` always allowed in V1 (per-root
"not dismissible" later, next to the (a) policy column); under `Hold` a hit on either party interrupts;
leash 2 x TALKING_RANGE; `dialogue_pause_warn` 60 s; `window.close_on_move` and `window.sources` follow
MQ11 / MQ12 when answered, today's implemented values until then; the event / order renumbering of §13.7
is b0's to confirm, not the user's.

**Talk during an ambient greeting (k16.6.169, 2026-11-14; test `ksim_barman_trade` "talk during greeting").** Cause of the first-talk `nothing_to_say` at the Hub bar: the barman greets the arriving player himself (a conversation with the player as target, event 3, dialogue 3197, ~100 ticks, no `interrupt`, no menu); a walk-up talk lands 20-30 ticks into it, the pick is `Busy` and there is no interrupt root. A second talk after the greeting ended opened 161 "Looking to buy?". Fix: a player talk to a busy speaker whose running conversation has no interrupt root, no open menu and was not itself a talk ends that conversation (SPEECH_END, reason 2) and the talk event is evaluated again next step. **UNSURE** (no data settles it): the original may instead refuse the talk during the greeting; the default is the lead's ("the player's talk ends an ambient conversation without a menu and opens the talk root"). Verify: playtest.
