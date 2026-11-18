# Kenshi save format (sho_battai test series)

Source: ds job `save_format` (Sonnet, 2026-10-30) on six vanilla saves taken about a minute apart at Sho-Battai (2026-10-30), checked against in-game screenshots. Tool: `tools/ksave.py dump|diff`. Saves live in the Proton prefix: `compatdata/233860/pfx/drive_c/users/steamuser/AppData/Local/kenshi/save/`. The game prefixes names when saving, e.g. `#140805datatest_2`.

Saves examined: `datatest_1`, `#140805datatest_2..6` in `~/git/kenshi/kenshi-saves/sho_battai/` (game 1.0.68, mods
`rebirth`, `Newwworld`, `Dialogue` active). Tool: `ksave.py` (`dump <dir>`, `diff <dirA> <dirB> [--all]`);
outputs `dump_1.txt`, `dump_6.txt`.

A later, larger save (SkinHouse, day 140, 1,165 platoons, 22 record types) is inventoried against our state in `reference/SAVE_IMPORT.md`.

Used as a test fixture source (characters within a radius, clock, weather, camera) by `tools/ksave_scenario.py`: `reference/SCENARIO_IMPORT.md` (field mapping and what is UNSURE). Not save compatibility.

Confidence tags: **[GT]** matches the screenshot ground truth, **[diff]** verified by diffing the six saves or
by a cross-file consistency check, **[exe]** read from the decompile, **[guess]** plausible but unproven.
"Save k" = k-th save of the series.

## 0. Summary

* Saves use the same record container as the mods: **version 15, no header**. kmod parses all 302 files
  (6 × (quick.save + 46–50 platoons + 1 zone)) without error. Platoon files are consumed to the last byte. `quick.save`
  and `.zone` carry a **trailing block of ID pools** (`i32 n; i32[n]` repeated) that kmod reports as `leftover`; once
  that block is parsed, `kmod.write(f) + tail == original bytes` for all 302 files.
* Game state lives in a few singleton records of `quick.save`:
  clock + money in `CAMERA` (`time day/hour/minute`, `player money`), weather per biome slot in `BIOMES`,
  faction relations in `GAMESTATE_FACTION`, squads in `PLATOON`, towns in `GAMESTATE_TOWN`.
  Per-character state is in one `platoon/<name>.platoon` file per squad; building state and items of the
  loaded zone are in `zone/zone.X.Y.zone`.
* **The current AI task is not saved.** A character has only `GAMESTATE_AI.jobs=1` plus (7 of 242) a permanent-job handle.
  The squad-level order is `PLATOON.currentPackage` (an `AI_PACKAGE` sid) and `mission *` fields.
* Ground truth: clock, money, name/squad, hunger and the "Dust Storm" status are all reproduced (§3).
* Stable ids do not exist: every save renumbers all `N--INGAME` ids from a running counter (`lastID`). Characters must be
  matched across saves by content (see §4).

## 1. Container

`kmod.parse(path)` returns `(f, bytes)`, not `(header, records)`: `f['ver']`, `f['header']` (None for v15),
`f['lastID']`, `f['records']`, `f['leftover']`.

| file | ver | header | lastID (saves 1..6) | records (saves 1..6) | size | leftover |
|---|---|---|---|---|---|---|
| `quick.save` | 15 | none (kmod only reads one for v≥16) | 642, 1242, 1839, 2438, 3041, 3645 | 578, 580, 581, 581, 583, 583 | 2.23–2.27 MB | 27212 B in save 1 = 4 pools |
| `platoon/*.platoon` | 15 | none | 12, 24, 36, 48, 60, 72 for `Nameless_0` (+12 per save) | 11–126 (Nameless_0: 11) | 3–45 KB | 0 |
| `zone/zone.43.11.zone` | 15 | none | 79862 (save 1) | 3449, 3449, 3449, 3446, 3446, 3438 | 2.64 MB | 5712 B = 1 pool |
| `portraits_texture.png` | – | – | – | – | – | not a record file |

* **Counts of files:** 46, 47, 49, 48, 50, 50 platoon files in saves 1–6 (squads spawn/despawn; `quick.save` always holds the same number of
  `PLATOON` records) **[diff]**.
* **Record fields in saves:** `flags` is 0 everywhere. `id` is a non-zero running counter (in mods it is 0) and equals
  the number in `sid`. `sid` for save-created records is `"<id>--INGAME"` (empty origin file between the dashes), or
  `"<id>-<zone>-INGAME"` in zones (`79820-zone43.11-INGAME`). The `size` field is nonzero only on a few records:
  = number of instances for `INSTANCE_COLLECTION` and `INVENTORY_STATE`, 1 on some `INVENTORY_ITEM_STATE`, 0 elsewhere. Reader ignores it **[diff]**.
* **Tail block (non-record data), `quick.save`:** four pools `[46, 320, 3349, 3084]` entries in save 1 (`[50, 320, 3443, 3084]` in save 6):
  each is `i32 n` + ascending unique `i32`. It is a set of allocated handle indices **[diff]**:
  * pool 0 = the platoon pool: equals the `handleC` of all 46 `PLATOON` records (range 1..48, with gaps);
  * pool 1 (320) covers every `GAMESTATE_TOWN.handC` (type 13 towns and type 85 nests; 304+15 values) plus one more;
  * pool 2 (3349, max 3573) and pool 3 (1..3084, contiguous in all six saves) – **unknown** (not towns, platoons or characters).
* **Tail block, zone:** one pool of 1427 values. It contains every `GAMESTATE_BUILDING.handleI` (1397/1397) and 1279/1280 of the
  other `*I` handle fields in the zone; the 30 extra values are even numbers 2..60 **[diff]**. So the pool is the set of used per-zone item indices.
* `platoon` files have no tail. `portraits_texture.png`: not examined.

## 2. Inventory of record types

Counts in save 1 (platoon = sum of 46 files; zone = `zone.43.11`). "Game data" columns: count in `gamedata.base`
/ in the three mods / in shipped `*.level` / in shipped `*.zone`.

| type | quick | platoon | zone | gamedata.base | mods | *.level | *.zone |
|---|---:|---:|---:|---:|---:|---:|---:|
| GAMESTATE_FACTION | 103 | | | 0 | 0 | 0 | 0 |
| WAR_SAVESTATE | 103 | | | 0 | 0 | 0 | 0 |
| PLATOON | 46 | | | 0 | 0 | 0 | 0 |
| GAMESTATE_TOWN | 319 | | | 0 | 0 | 1678 | 0 |
| GAMESTATE_TOWN_INSTANCE_LIST | 1 | | | 0 | 0 | 6 | 0 |
| CAMERA | 1 | | | 1 | 0 | 0 | 0 |
| BIOMES | 1 | | | 15 | 122 | 0 | 0 |
| RESEARCH | 1 | | | 93 | 182 | 0 | 0 |
| TERRAIN_DECALS | 1 | | | 0 | 0 | 0 | 0 |
| INSTANCE_COLLECTION | 2 | 46 | 1 | 0 | 0 | 528 | 650 |
| GAMESTATE_CHARACTER | | 242 | | 0 | 0 | 0 | 0 |
| GAMESTATE_AI | | 242 | | 0 | 0 | 0 | 0 |
| MEDICAL_STATE | | 242 | | 0 | 0 | 0 | 0 |
| CHARACTER_APPEARANCE | | 242 | | 0 | 0 | 0 | 0 |
| STATS | | 242 | | 24 | 38 | 0 | 0 |
| INVENTORY_STATE | | 245 | 231 | 0 | 0 | 378 | 1361 |
| INVENTORY_ITEM_STATE | | 1247 | 1282 | 0 | 0 | 5912 | 26 |
| GAMESTATE_BUILDING | | | 1397 | 0 | 0 | 31277 | 14637 |
| ITEM_PLACEMENT_GROUP | | | 538 | 12 | 18 | 9276 | 0 |

**Save-only types** (in no shipped file): `GAMESTATE_FACTION`, `WAR_SAVESTATE`, `PLATOON`, `TERRAIN_DECALS`, `GAMESTATE_CHARACTER`,
`GAMESTATE_AI`, `MEDICAL_STATE`, `CHARACTER_APPEARANCE`. Types that exist in shipped `leveldata.level`/`.zone` files
but not in `gamedata.base`/mods: `GAMESTATE_TOWN`, `GAMESTATE_TOWN_INSTANCE_LIST`, `GAMESTATE_BUILDING`, `INVENTORY_STATE`,
`INVENTORY_ITEM_STATE`. Types reused with a different meaning in saves (state instead of definition): `CAMERA`, `BIOMES`,
`RESEARCH`, `STATS`, `ITEM_PLACEMENT_GROUP`, `INSTANCE_COLLECTION`. No other save types occur (`STATE`, `SAVED_STATE`, `MEDICAL_PART_STATE`,
`GAMESTATE_CRAFTING`, … never appear) **[diff]**.

**References.**

* *Game data -> by stringID* `"<n>-<originFile>"`, with the real origin file: `204-gamedata.base`, `1533661-rebirth.mod`,
  `2309-clothes_v1.mod`, `5341-chareditor.mod`. Origin names are the *names the records were created under*, not necessarily files in the install: `changes_otto.mod`,
  `furniture_otto.mod`, `Ark Haircuts.mod`, `weather_otto.mod`, and `gamedata.quack` (the original name of `gamedata.base`: `31-gamedata.quack` = LOCATIONAL_DAMAGE "Right Leg",
  `17-gamedata.quack` = RACE "Human") all occur as the suffix of sids that live inside `gamedata.base`/`rebirth`/`Newwworld`/`Dialogue`.
  Match on the full sid string, never on the file name. **All 46 195 sid-shaped strings** (fields 20 956, refs 23 047, instance refs 2 192) in the 46+1+1 files of save 1 resolve against those four data files **[diff]**.
* *Saved record -> saved record, same file:* `"<id>--INGAME"`. The platoon file's `INSTANCE_COLLECTION` holds one instance per
  character: `inst.id="2--INGAME"`, `inst.ref` = CHARACTER template sid (`1533661-rebirth.mod` "UC start"), `xform` = position + rotation,
  `inst.states` = the 6 state records (GAMESTATE_CHARACTER, GAMESTATE_AI, INVENTORY_STATE, MEDICAL_STATE, STATS, CHARACTER_APPEARANCE).
  `INVENTORY_STATE.inst[k].ref` = an `INVENTORY_ITEM_STATE` sid.
* *Zone:* `INSTANCE_COLLECTION "0-buildinglist"`: inst `id` (original `10-Newwworld-INGAME` or new `74671-zone43.11-INGAME`), `ref` = BUILDING
  template, `states` = `[INVENTORY_STATE?, <id>-S23 GAMESTATE_BUILDING]` or `[ITEM_PLACEMENT_GROUP]`.
* *Handles (cross-file object pointers)* are 5 ints with a common prefix (`handle`, `hometown`, `currenttown`, `mounted`,
  `owner...`, `carrying`, `in what`, `slaver`, `residents`, `TI town`): suffixes **C, I, CS, S, TYPE**. **[diff]** (consistency checks):
  * `TYPE` = itemType of the target (13 TOWN, 34 PLATOON, 1 CHARACTER, 0 BUILDING, 85 NEST); **11 (NULL_ITEM) = null handle**
    (other fields 0 or stale garbage, e.g. `currenttownCS=1074175124`).
  * `C` = container index, `I` = index inside the container, `CS` = container serial, `S` = item serial (meaning of the serials: guess; they look like
    random or hashed 32-bit values).
  * Town: `C` = town pool index (Sho-Battai = 74), `CS` = town serial (1488037120), `I=S=0`. Platoon `Nameless_0`: `C=1`, `CS=1934326144`.
    Character `Player`: `C=1` (its platoon), `CS=1934326144` (= its platoon's CS), `I=1` (1-based index in the platoon), `S=-1481857024`, `TYPE=1`;
    `CAMERA.selected_character*` holds exactly that handle.
  * Building: `C=748` = zone cell index `X + 64*Y + 1` (43 + 704 + 1), `CS=11111` (constant for the zone), `I` = per-zone index (e.g. 2325), `S` serial.
  * Handle serials `S`/`CS` are stable across saves for the same object (Player `S=-1481857024` in all six saves) except `TI town*`, which drift **[diff]**.
* *Faction ids:* `faction stringID` / `owner faction ID` / `relationSID<i>` hold FACTION sids (`204-gamedata.base` = Nameless,
  `defaultEmpireFactionSID` = United Cities, `nofac`, `GENERATION_TEMPLATE_FACTION` are special strings).
* Town states: name `"Town state <Name>"`; the text field `instance` (`155-rebirth-INGAME`) is the id of the town's instance in the
  shipped townlist; `zzX0/zzY0` = zone cell (Sho-Battai: 43, 11).

## 3. Game state: where things are

### Clock, weather, money, version (`CAMERA`, one record; BIOMES, one record)

| what | record.field | observed | conf. |
|---|---|---|---|
| day | `CAMERA.int "time day"` | 1,1,1,1,1,2 (saves 1–6); 1-based | [GT] Day 1 |
| hour | `CAMERA.int "time hour"` | 10,12,14,16,21,0 | [GT] |
| minute | `CAMERA.int "time minute"` | 26,47,9,32,48,6 | [GT] |
| money | `CAMERA.int "player money"` | 750 in all six | [GT] "c.750" |
| game version | `CAMERA.string version` | `1.0.68` | |
| player faction | `CAMERA.string "pfaction name"` | `Nameless` | [GT] |
| area name / biome group | `CAMERA.string area`, `biome_text_groud` | `The Great Desert`, `55621-rebirth.mod` | |
| camera | `CAMERA.vec3 pos`, `vec4 rot`, `float zoom` | pos (53926,1177,-93603)→(53883,1149,-93640); zoom -546.5 | [diff] |
| tracked / selected | `CAMERA.selected_character*`, `tracking*` | Player handle; `tracking*` was the Player in save 1 and null afterwards | [diff] |
| used uniques | `CAMERA.usedUniques<k>` (+handle, state) | 5 unique characters already spawned | [guess] |

* **Clock unit:** three ints, hour/minute with no seconds. Elapsed time in game minutes = `1440*day + 60*hour + minute`. Game minute 0 is
  day 0 midnight, so day 1 starts at 1440. Save 5 21:48 + 50 min = 22:38 **[GT]** and the real time interval matches the screenshot
  (+20 s real at the observed ~2.6 game-min/s ≈ +52 min; 22:38 → 22:58 in 7 s = 2.9 min/s).
  Cross-check **[diff]**: `BIOMES.<n>_w_wi_start <= now <= <n>_w_wi_end` and `<n>_w_wi_udp >= now` in all 71 slots (nonzero starts)
  of all six saves with `now = 1440*day+60*hour+minute`; and `PLATOON.towntime` (hours = `now/60`, see below).
* **Weather:** stored per biome slot in the one `BIOMES` record (71 slots, key prefix `<n>_`): `w_w_id` (string: current WEATHER sid),
  `w_s_id` (SEASON sid), `w_s_end` (int, percent?), `w_wi_*` wind state (`start`, `end`, `udp` in game minutes as above;
  `time`, `str`, `speed` floats; `dir` vec3; `bu_*` gust state), `<n>_nestcount` (nests alive in the slot), `<n>_<sid>_count/start/respawn`
  (**effect-handler** state of the slot's weather/effects, not squad spawn timers: the exe code that writes them, `FUN_140103700`/`FUN_140104290`, builds `EffectHandler*` objects; corrected 2026-10-31, see `docs/reference/WILDERNESS_SPAWNS.md`; incl. `weather_otto1.mod` ids). The slot→biome-group mapping is not stored. Matching the season sid
  against `BIOME_GROUP.seasons` identifies slot 42 as "The Great Desert" (`56647-rebirth.mod` "great desert" belongs only to
  that group; `CAMERA.biome_text_groud` = that group) **[guess]** but consistent with the camera string `area`.
  Slot 42 weather: saves 1–4 `great desert streamers` (63474-rebirth.mod, `dust`=1.2, wind 100), saves 5–6 `GD ground sand orange`
  (63153-rebirth.mod, `dust`=1.0, wind 60) **[diff]**. The screenshot "Dust Storm" is **not a weather name**: it is the character status text
  from `medical.cpp` ("Dust Storm ({1} protection)" in `FUN_140648ef0`, driven by a per-character float derived from the weather's `dust`) **[exe]**.
  Both weathers are heavy dust, consistent with the ground truth **[guess]**; no field stores the status text itself.
* **Sky:** `CAMERA` float `sky update current time` 30.012→30.036, `sky update total time` 30.0, `sky clouds density speed`
  −0.0083→0; read by `FUN_14066d7d0` (the time/sky saver, writes `time day/hour/minute` and the five `sky *` keys) **[exe]**. `biome_text_timer`
  decreases by roughly 0.13–0.45 per game minute: −41.7, −99.0, −132.3, −189.0, −308.6, −363.6.

### Factions (`GAMESTATE_FACTION`, 103; `WAR_SAVESTATE`, 103)

* `GAMESTATE_FACTION`: `name` (record name), string `gamedata stringID`, int `platoonIDs` (= number of live platoons), `rank`, float `prosperity`
  (1000 at start, falling slowly for factions with wandering squads, e.g. Manhunters 1000→990.0→961.3 [diff]), `global trust`, and 102 triplets
  `relationSID<i>` (faction sid) + floats `relation<i>`, `trust<i>`, `trustNeg<i>`.
  Row-faction → column-faction relation: values -100..100. The player's own faction row (`Nameless`) has no relations; the
  **relations toward the player** are the entries in other factions' rows with `relationSID = 204-gamedata.base`: 82 factions 0,
  16 at −100 (Bloodraiders, Cannibals, Fogmen, Grass Pirates, …), Dust Bandits/Starving Bandits/Vagrants −10, Southern Hive −50.
  Identical in all six saves: no `relation*`/`trust*` field changed **[diff]**.
* `WAR_SAVESTATE` (one per faction): string `faction`, ints `num plats/poss/requests/actives`, `fwc id`, float `updatetime`
  (40.0 in saves 1–3, 46.06 in 4–5, 52.10 in 6; meaning unknown, **[guess]** a war-AI timer), `poss sid<k>`, `poss time<k>` (possible targets).

### Squads (`PLATOON` in quick.save; content in `platoon/<platoon stringID>.platoon`)

Fields (value in `Nameless_0` / `United Cities_7`, save 1):

| field | meaning | conf. |
|---|---|---|
| `platoon stringID` `Nameless_0`, `faction name` `Nameless`, `faction stringID` `204-gamedata.base` | identity, faction (FACTION sid) | [diff] |
| `content file` `platoon/Nameless_0.platoon` | file holding the members | [diff] |
| `squad template` `1533660-rebirth.mod` ("startoff- Wanderer UC"), `sqt` 1 | SQUAD_TEMPLATE the squad was made from | [diff] |
| vec3 `position` (53926.75, 1166.18, −93603.45) | **always equals the leader's position** (46/46 squads) | [diff] |
| `char count` | members (equals the number of instances in the file for 46/46) | [diff] |
| `money` (UC_7: 8406; Nameless_0: 0 – the player's 750 is in `CAMERA`) | squad purse; decreases over time for shops (UC_11 3000→2844) | [diff] |
| handles `hometown`, `homebuilding`, `currenttown`, `target town`, `occupied`, `separated`, `owned0..`, `mission employer/town/target` | town/building/object links; `hometown` of UC_7 → C 74 (Sho-Battai), `currenttown` of the player squad → town 74 | [diff] |
| strings `basetown` (UC_7 `55651-rebirth.mod` = TOWN "Sho-Battai"), `currentPackage` (UC_7 `3108-gamedata.base` = AI_PACKAGE "Shop-24hr"), `replacementAI`, `contractjob`, `mission data`, `map area name/sid` | AI order/package of the squad | [diff] (strings resolve; semantics of replacement/contract fields guess) |
| float `towntime` | absolute game time **in hours** (`24*day + hour + min/60`) at which the squad entered its current town; 34.0 = Day 1 10:00 for squads already in town at world start; 7/7 newly arriving squads in saves 2–6 have a value inside the interval between the two saves (e.g. Herbivore_0: 0 → 35.629 between 34.43 and 36.78) | [diff] |
| `jobhr`, `inventory refresh time` (46.0 → 58.0 for shops: shop restock timer, +12 h) | | [diff] |
| bools `persistent`, `dead`, `intact`, `is resident`, `homelok` (0→1 for many UC squads between save 1 and 6), `never been activated`, `runningAwayMode`, `imprisoned`, `special`, `canref` | flags; `Manhunters_1` had `dead` 0→1 and `char count` 14→1 | [diff] |
| `handle*` (CS/S/C/I/TYPE=34) | the squad's own handle; `squad index` (13 for UC_7, 0 for Nameless) | partly [guess] |

The player's squad is identified by faction `Nameless`/`CAMERA.pfaction name`, the selected character handle, or `squad mem type`;
nothing else flags it **[guess]**.

### Characters (one platoon file per squad; Nameless_0 file: 11 records)

Each character = 6 state records (+ items) referenced from the platoon's `INSTANCE_COLLECTION`:

* **Position/rotation:** the instance `xform` = `x y z qw qx qy qz`. Player save 1: (53926.754, 1166.177, −93603.453), q=(0.995, 0, 0.102, 0) – y-axis rotation (11.7°); y is absolute height (ground 1166) here **[diff]**.
  Zone cell for this position = `floor((x+147456)/4608), floor((z+147456)/4608)` = (43, 11) **[diff]**.
* **`GAMESTATE_CHARACTER`:** string `name` (`Player`), `owner faction ID` (`204-gamedata.base`), bools `is leader`, `ranged mode` 1, `chase mode` 1, `passive mode`,
  `defensive mode`, `taunt mode`, `non combat mode`, `stealth`, `carrying`, `kidn`, `escap`, `shaved`, ints `speed mode` (2), `personality` (0–10), `squad mem type`
  (0/1/2), `slavestate` (1 on 5 chars), `sentence`, `portrait_index/serial`, handles `carrying*`, `in what*`, `isindoors*` (building the character is inside, set only when indoors, with `floor`), `slaver*`, floats `age` (1.0 adult;
  animals 0.66→0.71 grow with time [diff]), `decay`, `psts`, `ssct`, `disguiseblown`, `TI townCS/S/C/I/TYPE/TI dat<k>/TI day` (town-instance info; CS and S drift every save – meaning unknown).
  Also bounty/crime lists `bountyfac<k>`, `bountyexp<k>`, `crimes<k>`, `amount<k>`, `claim<k>` (6 chars).
* **`STATS`** (record name = character name): floats for 44 skills/attributes (`strength`, `dexterity`, `toughness2`, `endurance`, `perception`, `melee: attack/defence/dodge`, `katana`…, `athletics`, `stealth`, `medic`…), `xp`, `free attribute points`. Player: mostly 1.0; `cooking`/`farming`/`science` 1.2, `unarmed` 0.1, `dodge` 0.1, `assassin` 0.5, `tracking`/`climbing`/`doctor` 0, `athletics` 1.1496 (save 1) → 1.2138 (save 2) → trained by walking [diff]. Everything else constant for the Player in 6 saves. For named NPCs STATS is constant; the STATS values are random per NPC and unique (242 distinct) so they serve as identity key in the diff tool.
* **`MEDICAL_STATE`:** bools `dead`, `coma`, `unconcious`, `incapacitated`; floats `blood` (Player 75.75 in all saves; NPCs mostly 78 or 50, animals ~148), `bleeding`, `KO`, `fed`,
  **`hung`** (hunger/100: Player 2.9936, 2.9621, 2.9438, 2.9119, 2.8417, 2.8109 in saves 1–6; interpolated to 22:38 → 283.05 = ground truth "Hunger 283" **[GT]**; max 3.0 → UI 300; rate ≈ 1.3 UI-points per game hour),
  per-limb `flesh<i>`, `hit<i>`, `hitmult<i>`, `wear<i>`, `bandage<i>`, `rig<i>`, `stun<i>`, strings `sid<i>` = LOCATIONAL_DAMAGE sids for i=0..6:
  Head, Chest, Stomach, Left Arm, Right Arm, Left Leg, Right Leg (resolved names **[diff]**). Player: `flesh<i>`=100 on all limbs in all six saves,
  `hit<i>` = 80, 140, 140, 80, 40, 80, 80. `flesh` is the current limb health value (guess; baseline 100 for an unhurt player; NPCs 80, 125 etc.); `hit`: guess = per-limb hit-point scale; `wear`, `rig`, `bandage`, `stun`: all 0 in the observed data, semantics guessed from names.
* **Inventory:** `INVENTORY_STATE.inst[]` -> `INVENTORY_ITEM_STATE`: strings `base data sid` (ITEM/ARMOUR/WEAPON template), `material sid`, `company sid`, `color sid`, `section` (body slot or `main`/`back`/`backpack_content`/`hip`/`head`/`shirt`/`armour`/`legs`/`boots`/`backpack_attach`),
  ints `quantity`, `level` (quality tier 5…60), `item function` (5 = back/weapon slot, 6 = worn clothing; 4/0 in zone containers), `inventory x/y` (grid cell in a container), `ownedby*` handle, `insideBuilding*` handle, floats `quality` (100), `charges`, bools `in inventory`, `death`. Player (save 1): `Iron Club` (`52295-rebirth.mod`, section `back`), `Wooden Sandals` (boots), `Rag Shirt` (armour), `Halfpants (ragged)` (legs). Stays identical in 6 saves **[diff]**.
* **`GAMESTATE_AI`:** only bool `jobs` (=1 for 242/242) and, for 7 characters, `pjob0` (an AI_TASK-like id, 57 or 187) with handle `pjobT0*`. **No** current task, goal, target, path or timers are saved; the squad's `PLATOON.currentPackage`/`mission *` is the only order information, plus the character's `speed mode` and stance bools [diff].
* **`CHARACTER_APPEARANCE`:** `sex female`, `Age`, `head`, `hair style`, `beard`, `idle stance`, `body version`, ~68 float morphs, vec3 `Skin Tone`, ref `race` (`17-gamedata.quack` Human).
* **Squad membership:** a character belongs to the squad in whose file it is; file order within a platoon is *not* stable across saves (e.g. Drifters_2: Jaffar, Sergal → Sergal, Jaffar), the leader is the one with `is leader`.
  Squad membership across files changes as characters move between squads or squads despawn [diff].

### Towns (`GAMESTATE_TOWN`, 319 = 304 towns + 15 nests)

Fields: bools `public`, `started`, `explored`, `discovered`, `recently discovered`, `ded`, `is nest`; ints `plevel`, `zzX0/zzY0` (zone cell), `handC/CS/S/I/TYPE`, `artifacts count`;
strings `faction` (owner FACTION sid), `factions here<k>`, `building material`, `replacementTown`, `instance`; floats `pop`, `pop2`, `popd`, `popd2`, `tod`;
`ref "trade goods"` (item sid + stock count in v0, e.g. `1913-gamedata.base`, 10931). Only `pop2` of the 15 nests changed over the series (e.g. Raider Camp 4: 21.097→21.61 in 141 game minutes) [diff].
`GAMESTATE_TOWN_INSTANCE_LIST` (15 entries here): town/nest instances with world position.

## 4. Diff of the six saves

| save | real time | game clock | Δ game min | Δ real s | ratio | Player position | Player hunger (UI) | platoon files / chars |
|---|---|---|---:|---:|---:|---|---:|---|
| 1 | 15:29:52 | D1 10:26 | – | – | – | (53926.8, 1166.2, −93603.5) | 299.36 | 46 / 242 |
| 2 | 15:30:50 | D1 12:47 | 141 | 58 | 2.43 | (53934.7, −93583.2) | 296.21 | 47 / 238 |
| 3 | 15:31:24 | D1 14:09 | 82 | 34 | 2.41 | (53941.1, −93582.7) | 294.38 | 49 / 245 |
| 4 | 15:32:21 | D1 16:32 | 143 | 57 | 2.51 | (53943.5, −93583.8) | 291.19 | 48 / 219 |
| 5 | 15:34:21 | D1 21:48 | 316 | 120 | 2.63 | (53949.0, −93581.7) | 284.17 | 50 / 232 |
| 6 | 15:35:16 | D2 00:06 | 138 | 55 | 2.51 | (53949.5, −93581.6) | 281.09 | 50 / 237 |

Total 820 game minutes in 324 real seconds. The Player's net displacement is ≈32 units over 13.7 game hours (idle, "hands-off"; y stays 1166.18), staying in the Sho-Battai area (town 74).

**Always changes:** `CAMERA.time *` (and `sky update current time`, `biome_text_timer`, camera pos/rot), `lastID`, every `N--INGAME` id, `MEDICAL_STATE.hung`
(all characters; Player −0.1827 over 820 game minutes = −0.0134 per game hour), `BIOMES.<n>_w_wi_time/start/end/udp/dir/...`
(wind/weather re-rolls: weather id changed in 9 of 71 slots between saves 1 and 2, in 35 between 1 and 6, in 43 in at least one of the six), `WAR_SAVESTATE.updatetime`, `GAMESTATE_CHARACTER.TI townCS/TI townS` (for every character, all saves), character positions (of 197 characters matched between saves 1 and 2, 115 moved (median 124 units) and 82 did not; saves 5→6: 196 matched, 124 moved, 72 not),
and `PLATOON.position` (follows the leader).

**Changes for some:** `PLATOON.currenttown*`/`towntime` (entering a town), `PLATOON.money` and `inventory refresh time` (shop activity), `homelok` 0→1 (UC squads, between save 1 and 6),
`char count`/`dead`/`intact` (casualties: Manhunters_1 14→1), `GAMESTATE_FACTION.platoonIDs`, `prosperity`, `GAMESTATE_TOWN.pop2` (nests),
`MEDICAL_STATE.blood`/`flesh<i>` for a minority of NPCs (wounds, 210 field changes 1→2), animal `age`, `STATS.athletics` of the Player,
zone building states (§5), items of NPCs (`level`, `base data sid` change when an NPC swaps gear).

**Constant:** `CAMERA.player money` (750), `pfaction name`, `area`, `version`, `time`-independent `usedUniques*`, `RESEARCH` (`_Default Start` only), all `GAMESTATE_FACTION.relation*/trust*`, `PLATOON` squad template, faction, home, basetown and packages,
all of the Player's STATS (except athletics), CHARACTER_APPEARANCE, inventory, `blood` (75.75), limb values, `handle*` serials, `mod list` (`CAMERA ref mods`: base, Newwworld, Dialogue, rebirth), `zones` ref (`0: 43,11,1`).

**Identifying characters across saves.** Platoon file names are reused by different squads (3 files vanished/4 new in save 2), and the order of characters inside a platoon changes. The tool matches a character by `(name, md5 of STATS rounded to 2 decimals excluding athletics/xp)`: unique among all 242 characters and equal for 197 of 242 characters in saves 1/2 (212/245 in 2/3, 197/219, 189/232, 196/237)
– the rest despawned/spawned.

## 5. `zone.43.11.zone` (Sho-Battai) in a save versus the shipped zone

* Records (save 1): 1397 `GAMESTATE_BUILDING`, 1282 `INVENTORY_ITEM_STATE`, 538 `ITEM_PLACEMENT_GROUP`, 231 `INVENTORY_STATE`, 1 `INSTANCE_COLLECTION "0-buildinglist"` (1935 placements). The shipped layers hold 164 (Newwworld) + 1 (Dialogue) + 184 (rebirth; 16 tombstones) placements for this cell. **All live placements of the three shipped layers (164 + 1 + 168) are in the save** (same targets; transforms equal except 2 Newwworld ones), tombstoned ids are absent, i.e. the save holds the *merged result*, not the diffs. **[diff]**
* The other **1766 placements have ids `<n>-zone43.11-INGAME`** (and 3 `Sho-Battai walls-INGAME`, 4 `rebirth-INGAME`): furniture/props spawned into the interiors at runtime from the interior layouts. They are renumbered per save, so the diff tool keys them by target+position.
* **Building state (`GAMESTATE_BUILDING`)** (a superset of the shipped fields): bools `broken`, `destroyed`, `power on`, `gets battery`, `is complete`, `is public day`, `pause`, `foliage`, `is inside building`; floats `batterycharge`, `world Y pos`, `construction progress` (3.0), `<k>construction mats`, `production amount`; ints `townC/CS/S/I/TYPE` (town 74 → ownership by town), `residents*` handle (20 → characters TYPE 1, 3 → platoons TYPE 34),
  `mounted*` handle, `handle*` (zone-local, see §2), `isInsideBuilding*` (parent building for interior furniture), `floornum`, `flags`, `version` (270), `serialised interior` (1); strings `owner faction ID` (986/1397 `defaultEmpireFactionSID`, 145 Tech Hunters `1233-gamedata.base`, 96 Traders Guild, 75 Shinobi Thieves `14491-rebirth.mod`, 50 Slave Traders `1084`, 45 Machinists `50358-rebirth.mod`), `name`, `squad template`, `residents template`. Door/gate state: `state0`, `doa0`, `locked0` (per door), turret `current shots`.
* **All 1397 buildings were intact** (`destroyed` 0, `is complete` 1; 6 had `power on` 0) in saves 1–6 **[diff]**. Ownership did not change in 13.7 game hours.
* **Items:** `INVENTORY_STATE` instances = a container's contents (shop shelves, chests), `INVENTORY_ITEM_STATE` = one stack (see §3 fields), mostly `section=backpack_content` with grid `inventory x/y`. 1282 item states in save 1 / 1273 in save 6; shop stocks change (e.g. building `1579-Newwworld-INGAME` `production amount` 0.1157→0.0014→0, quantity 1→7→10).
* **What changes in 13.7 h [diff]:** gates/doors of Sho-Battai close at night (`10-,14-,16-,29-,31-Newwworld-INGAME`: `state0` 1→0, `doa0` 1→0, `locked0` 0→1 between save 1, 10:26 and save 6, 00:06; `27-Newwworld-INGAME` opened), the walls' `current shots` counter, shop restocking (item `base data sid`, `level`, `quantity`, `ownedby*` of containers' items), production amounts.
  Only 6 fields changed between saves 1 and 2 (the zone is mostly static in 141 game minutes).

## 6. Open points / what is *not* known

* Meaning of the 3rd and 4th trailing pools of `quick.save` (3349 and 3084 entries) and why pool 3 is exactly `1..3084`.
* Meaning of `TI town*` (S and CS change every save), handle serial semantics, `PLATOON.squad index`/`sqt`, `WAR_SAVESTATE.updatetime`, `CAMERA ... ref zones (0, 43, 11, 1)`, per-limb `hit`/`hitmult`.
* Weather slot→biome mapping (slot 42 is a guess), exact rule that yields the "Dust Storm" status; `w_s_end`, `w_wi_bu_*`.
* Whether `portraits_texture.png` is referenced by `portrait_index/serial` (not examined).
* Only one zone (43.11) was saved: zone files of other cells are not covered; behaviour of the pools for several zones is unknown.
