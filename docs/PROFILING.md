# Per-system tick profiling (KSIM_PROFILE, kenshi-k16.6.79, part of kenshi-k16.6.62)

Which part of the tick costs what, measured in the sim, for headless runs and (later) a hosted server. Compiled out unless asked for.

## Build

`cmake -DKSIM_PROFILE=ON` (default OFF; a dev box uses a second build dir, `cpp/build-prof`). OFF: `ksim/core/profile.hpp` defines the macros as nothing, so `Simulation::tick`, `Session::sendTick` and `Simulation::save` hold no profile code, `Simulation` has no `profiler()` and `profile.cpp` is an empty object. Check an OFF build: `nm -C cpp/build/CMakeFiles/ksim.dir/src/ksim/{simulation,simlink,soak,profile}.cpp.o | rg -i profil` prints nothing, and `strings -a .../simulation.cpp.o | rg 'Profiler|section_mean_us'` prints nothing.

## What is timed

`Profiler` (one per `Simulation`, used on the sim's thread only) keeps a fixed ring of the last `kWindow` = 4,096 ticks (about 800 KB, allocated once, never grows) and running totals since it was made. `KSIM_PROFILE_MARK(Section)` in `Simulation::tick` charges the time since the previous mark to that section, so the sections of one tick sum to the tick (less the closing counts, a few clock reads). A `KSIM_PROFILE_SCOPE` times stretches outside the tick (`Session::sendTick`, `Simulation::save` / `saveGame`); that time joins the next tick's sample.

| section | covers (stages of `Simulation::tick`) | the issue's group |
|---|---|---|
| setup | commands, unlocks, discovery, known factions, door paint | |
| systems | the interval-scheduled systems (`systems_`) | |
| weather | weather regions and beams | |
| wild | wilderness respawns | |
| ai_strategic | faction strategy | AI / goal runner |
| ai_squad | squad goals, carry, bars, the world squads' steps | AI / goal runner |
| tokens | off-view stragglers, token fights, stored medical, corpses | token movement off-view |
| combat | the arena step | combat |
| interact | player squads, buildings, doors, self aid, interactions | |
| producers | producers, item nodes, carrying | producers / economy |
| tasks | intruders, crime goals, slave trade, world tasks, torture, gate crews | |
| residents | player areas, residents, routines, rearm, shopping, stealth KO | |
| crime | crime and dialogue | |
| goals | individual goals, prospecting, order queues, jobs | AI / goal runner |
| economy | traders, economy, construction, meals | producers / economy |
| nav | door paint, region tick, line of sight | navigation (world state) |
| tail | scenario hold, scheduler mirror | |
| simlink | `Session::sendTick`: frame encode | simlink encode and send |
| save | `Simulation::save` / `saveGame` | save |

Navigation and pathing are not a stage: path queries are made from inside the AI, combat and residents sections. They are counted apart from the navigator's own counters (`NavStats`: coarse, local, cross-border and snap calls and wall time; `Navigator::profileStats`), as queries and microseconds per tick, and their time is *inside* the sections above. The send itself (the socket write) is the server loop's, not timed here (only the encode is).

Counts per tick: characters (`Fighter` entities), squads (`SquadInstance`), path queries. Both are cached flecs queries (`count()`), not per-tick scans.

## Where the numbers go

The sim never prints or writes them on its own.
1. **Soak report** (`ksim_soak`, a KSIM_PROFILE build): a "profile" block, per section total seconds, share of the tick, mean us per tick and the max us of the window, then the window's tick mean / p50 / p99 / max and the per-tick counts.
2. **JSON line on stderr**, off by default: `KSIM_PROFILE_JSON=<seconds>` in the environment, `ksim_soak --profile-json SECONDS`, or `Profiler::setJsonSeconds`. One line, at most once per interval, from the tick: `{"profile":1,"tick":N,"window":W,"tick_us":{mean,p50,p99,max},"section_mean_us":{<section>:us,...},"counts":{characters,squads,path_queries,path_us}}`.
2b. **Slow-tick log** (kenshi-k16.6.699), on in a KSIM_PROFILE build: a tick over `KSIM_PROFILE_SLOW_MS` (default 50; 0 = off) prints one stderr line `SLOWTICK tick=N total_ms=T <section>=ms ... nav_convert=msxN nav_stale=msxN nav_bake=msxN nav_components=msxN path_q=Q path_ms=M chars=C` (sections under 1 ms are left out; the `nav_*` stretches ran inside the sections that called them, so they overlap: `nav_stale` includes `nav_bake`; N is the number of calls in that tick). `KSIM_SLOWQ=1` adds the `COMPONENTS` and `SLOWQ` lines. Run: `KSIM_PROFILE_SLOW_MS=50 KSIM_SLOWQ=1 cpp/build-prof/OpenKenshi-server ... 2> server.log` (the client's `KSIM_SERVER=` takes it); `tools/walk_probe.py` walks the gate_heft Wanderer headless over SIMLINK and writes the wire arrival times.
3. **SIMLINK, admin only**: order 120 `profile_query` and frame 70 `PROFILE` (the window summary and the per-section rows), kenshi-k16.6.90; the wire is SIMLINK.md PROFILE.
4. **Per-tick CSV, dev probe** (kenshi-k16.6.230): `KSIM_PROFILE_TICKS=<path>` writes one row per tick: the tick and every section in us, 31 finer stages inside `ai_squad`, `tokens`, `combat`, `tasks` and `residents` (`sub_*`, `kSubNames` in profile.hpp, marked with `KSIM_PROFILE_SUB`; each charges the time since the previous sub mark, and nothing is timed without the variable), and the counts. Summarise with `tools/heft_ticks.py FILE BUDGET_US`; `tools/heft_run.sh` + `tools/heft_probe.py` drive a server at Heft like the client. Compiled out with the rest.

## Cost of ON

Measured 2026-11-12 on the dev VM (shared with other workers, so wall times move by about 10% between identical runs).
- **The profiler itself:** `begin()`, 17 marks and `end()` cost **6.7 us per tick** (2 M ticks, `-O2`, a scratch benchmark of `profile.cpp` alone), almost all of it the 18 `steady_clock::now()` reads (about 0.35 us each on this Xen VM; a bare-metal clock is several times cheaper). Add two cached-query `count()` calls and the navigator counter reads per tick. The soak's mean tick is 2.0 ms (wall / ticks), so the cost is about **0.3%** of a tick.
- **Four-day soak, `--bake $HOME/navworld --regions 0 0 15 15 --days 4 --workers 1 --offscreen-fights on --offscreen-medical on`, 209,472 ticks, builds alternating:** OFF 448.5 s and 473.7 s, ON 422.9 s and 525.7 s. The spread between two runs of one build (5% and 24%) is larger than the cost, so the soak cannot resolve it; the state hash is the same in all four (367cc931387821fe): profiling does not touch the sim.
- **Memory:** the ring is 4,096 samples (about 0.8 MB) per `Simulation`; nothing else grows.

Reference breakdown, ON build, the first ON run of that soak:

```
profile (KSIM_PROFILE, 209472 ticks, 422.6 s inside Simulation::tick; the Simlink and Save lines are outside it):
  section        total s   of tick   mean us/tick   window max us
  setup             1.17      0.3%            5.6          1406.1
  systems           0.08      0.0%            0.4            18.0
  weather           2.10      0.5%           10.0            52.6
  wild              0.91      0.2%            4.3          8511.1
  ai_strategic      0.97      0.2%            4.6          2734.0
  ai_squad         73.57     17.4%          351.2        128438.4
  tokens          194.95     46.1%          930.7         31022.6
  combat           32.32      7.6%          154.3          4807.2
  interact         13.54      3.2%           64.6           435.4
  producers         0.98      0.2%            4.7            78.5
  tasks            88.23     20.9%          421.2          1127.0
  residents         8.02      1.9%           38.3          1666.3
  crime             1.14      0.3%            5.4           123.8
  goals             0.38      0.1%            1.8            24.1
  economy           0.47      0.1%            2.3          3145.9
  nav               1.66      0.4%            7.9            55.5
  tail              0.24      0.1%            1.1            21.0
  simlink           0.00      0.0%            0.0             0.0
  save              0.00      0.0%            0.0             0.0
  last 4096 ticks: tick us mean 1840.1, p50 576.1, p99 26936.8, max 135546.8; per tick 188.6 characters, 4733.9 squads, 1.2 path queries (207.1 us, inside the sections above)
  path queries over the run 271521, 42.3 s
```

Reading it: `tokens` (off-view stragglers, token fights, stored medical, corpses) is 46% of the tick, `tasks` (intruders, crime goals, slave trade, world tasks, torture, gate crews) 21% and `ai_squad` 17%: the places to look first. `tasks` has a small window max (1.1 ms) against a 421 us mean, so it is a fixed per-tick cost, not spikes. The window's p99 tick (27 ms) is 15x its mean (1.8 ms): the spikes are `ai_squad` (max 128 ms) and `tokens` (31 ms).

## SIMLINK pair (implemented, kenshi-k16.6.90)

Order 120 `profile_query` (c->s, no body, admin session only, session control, not in the order log) is answered with frame 70 `PROFILE` (`Session::writeProfile`): the window summary (tick mean / p50 / p99 / max, the tick budget, per-tick counts) and one row per section (name, mean us, max us, total ns). One-shot, to the asker; an OFF server answers `unsupported`, a non-admin session `not_allowed`. The layout and rules are in SIMLINK.md PROFILE (approved by the client lead, kenshi-k16.6.88). Nothing in it assumes loopback or a file: the numbers come from the ring in memory, and a hosted runner can instead take the JSON line (item 2 above). Test: `ksim_profile_wire`.

## Decisions taken (defaults)

- Marks, not nested scopes, inside the tick: one `steady_clock::now()` per stage (about 25 ns), no stack of timers, and the stages partition the tick.
- A process-wide singleton was rejected: the profiler is a member of the `Simulation`, so a test with several sims does not mix rings. It is not thread-safe; the server loop runs ticks, frames and saves on one thread.
- The ring keeps the last 4,096 ticks (0.05 s ticks: 205 s of sim time, about 1.9 game hours), enough for a p99; the soak's run totals are the cumulative sums, the max and percentiles are the window's.
- `ksim_soak` without `--offscreen-fights` / `--offscreen-medical` has no combat arena: the `combat` and `tokens` rows are then near zero.

## 5x tick tail (kenshi-k16.6.178, from the client's 5x stutter, kenshi-k16.6.177)

bar_lure at 5x (`set speed 5`, `set_interest` 2000 and `set_view` at the Wanderer, as the client sends them), a KSIM_PROFILE `OpenKenshi-server`, a raw-socket probe client counting SNAPSHOT_END arrivals (the box was at load 30-35 from other jobs, so wall times move about 15%; compare the CPU seconds too). Order 120 `profile_query` for the sections.

| 60 s at 5x | before | after |
|---|---|---|
| server CPU (of 60 s wall) | 56-60 s: saturated, 92 snapshots/s of the 100 due | 34-43 s, 100/s |
| `simlink` section (the encode) mean / max | 4.9-5.2 ms / 20-22 ms | 1.4-1.7 ms / 14-18 ms |
| snapshot arrival gap p50 / p99 | 0.03 ms / 102 ms (bursts) | 9.6-9.8 ms / 29-36 ms |
| sim tick p99 / window max | 16.6-17.1 ms / 279 ms | 14-18 ms / 215-280 ms (the max is the first ticks) |

**Cause 1, fixed:** at 5x the budget is 10 ms and the tick was about 5.6 ms of sim plus 4.3-5 ms of encode, so the server ran at 100% and any slow tick became a backlog the loop paid back in a burst (`budget` 8 ticks, each sending). Most of `Session::sendTick` was per-tick world scans that built records for things that had not changed: the navmesh tile stream walked every tile (1.0 ms; now skipped while `NavMesh::revision()` and the bounds stand), `bedViews()` rebuilt 744 beds with strings (1.2 ms; `bedViewsRef()` caches the list per `SiteIndex::revision()` and refreshes occupants), every door / item / container / producer / bed was built into an INTERACTABLE record, sorted and set-inserted (0.8 ms; `visit` checks bounds and version first and builds only a changed record), `playerSquads()` scanned all squads once per squad and re-read the constants (0.55 ms). Wire bytes are identical: `--dump-bin` of bar_lure over 600 ticks, master vs this branch, `cmp` equal.

**Cause 2, not fixed (design change):** the p99 tick is the 1 Hz pass. Every 20th tick runs `ai_squad` (`AiSystem::squadStep` over the ~4,800 squads, 3-7 ms, plus `prepareWorldCarry` / `stepWorldSquads`), `stepWorldTasks` (3-7 ms) and `stepResidents` (2 ms) together: 15-20 ms against a 10 ms interval, then 1-2 ticks of catch-up. `stepRoutines` adds 12-30 ms about every 400 ticks, and `ai_squad` 45-60 ms now and then. To bring every tick under 10 ms they must be spread (a squad slice of id % 20 per tick for `squadStep`, the same for the world-task executor); each squad then steps at a different tick of its second, which moves the world state and the pins of the ticks-sensitive tests, so it is a decision for ksim. The 50 ms stalls are the same passes under the box's load.

## Heft under live load (k16.6.92, k16.6.91; kenshi-k16.6.230)

Server: KSIM_PROFILE `OpenKenshi-server` (master d8eb4282 plus the probe above), `--scenario-file scenarios/cage_prisoner.kscen --navworld ~/git/kenshi/navworld` (the user's Heft run), driven by `tools/heft_probe.py` as the client does: `set_interest` 2000 and `set_view` at the Wanderer (68331, -71190), `set_speed`, PING, order 120 every 10 s. Recipe: `tools/heft_run.sh NAME PORT WALLSECS SPEED`, then `tools/heft_ticks.py NAME.ticks.csv BUDGET_US`. The first 100 ticks are dropped (cold caches). **The dev box was shared and busy: load average 22-48 on 16 cores during every run (the other worker's tests), so the times are inflated by an unknown 10-40%; server CPU seconds are given. Re-measure on a quiet box before taking a gain as final.** The world at Heft: 306 live characters (max 323), 4,827 squads, no path-query count (the AI navigator reports no `NavStats` in this configuration, so `path_queries` reads 0; the stack samples below put routing at 6-7% of the wall).

| | 1x, 10 min | 5x, 10 min wall | 5x, 12 min wall (1.15 game days) |
|---|---|---|---|
| ticks / sim time | 11,901 / 595 s | 48,655 / 2,433 s (**4.05x**, not 5x) | 60,333 / 3,017 s (4.19x) |
| server CPU of wall | 147 s of 600 | 562 s of 600 | 676 s of 720 |
| tick mean / p50 / p99 / max (us) | 9,967 / 8,334 / 32,942 / 84,041 | 9,307 / 7,626 / 32,566 / 97,942 | 9,134 / 7,617 / 30,986 / 103,504 |
| budget (tick only) | 50 ms: **0.36%** over (43 ticks) | 10 ms: **27.7%** over | 10 ms: **24.1%** over |
| tick + encode (`simlink`) mean / p99 | 12.8 / 35.9 ms; 0.39% over | 12.1 / 35.9 ms; **53.1%** over | 11.7 / 33.7 ms; **50.2%** over |
| snapshots delivered (probe) | 20.0/s of 20 | 81.3/s of 100 (arrival gap p99 122 ms) | 83.9/s of 100 (p99 116 ms) |
| tick % 20 == 0 (the 1 Hz pass): mean / p99 | 29.3 / 71.3 ms | 26.6 / 64.5 ms | 25.8 / 64.4 ms |
| the other 19 ticks: mean / p99 | 8.95 / 18.3 ms | 8.4 / 20.3 ms | 8.3 / 19.1 ms |

Per section (us per tick; the 5x 10 min run; `simlink` is the encode, outside the tick and not in its mean):

| section | 1x mean | 5x mean | 5x p99 | 5x max | share of tick |
|---|---|---|---|---|---|
| combat | 5,424 | 5,202 | 13,520 | 54,744 | 55.9% |
| tasks | 2,246 | 1,994 | 8,122 | 18,279 | 21.4% |
| residents | 1,119 | 1,017 | 7,163 | 75,192 | 10.9% |
| interact | 506 | 519 | 2,398 | 5,231 | 5.6% |
| ai_squad | 325 | 312 | 6,984 | 27,705 | 3.3% |
| setup / weather / crime | 92 / 46 / 97 | 39 / 46 / 76 | 230 / 114 / 1,118 | 8,600 / 2,996 / 6,164 | 0.4 / 0.5 / 0.8% |
| producers, tokens, goals, economy, nav, tail, systems, wild, ai_strategic | each under 35 | each under 30 | | `wild` 20,187 | under 0.4% each |
| simlink (encode) | 2,845 | 2,799 | 14,089 | 29,536 | (+23% on top) |

Finer stages (`sub_*`, a 5x run of 30,790 ticks; mean us per tick, p99):
- `combat`: **step 3,959 (p99 9,881)**, **mirror 849 (3,311)** (the copy of every member's `Fighter` back into the ECS after the step), pre 212.
- `tasks` (answers k16.6.91: **yes, a fixed per-tick part**; 1.6 of its 1.9 ms runs every tick, not once a second): the fight goals' pass **1,095 (p99 2,661)**, the prelude (town circles and fortifications rebuilt from `ecs.each` and `gateSites` every tick) **278**, the tasks-in-hand member loop 201, lock / build orders 8, and the 1 Hz native goal selector with the rest of the function 225 (p99 5,187); `stepSlaveTrade` 78 on top.
- `residents`: **`stepRoutines` 845 (p99 6,127, max 71,676)**; `stepResidents` 69.
- `ai_squad` (1 Hz only): `squadStep` 149 mean (about 3 ms per pass), world squads 56, carry prep 50, bar / depot attach 35.

**The worst ticks** (all three runs show the same ticks: the sim is deterministic, so a spike repeats at the same tick). 1x, over 40 ms: 58 ticks, 54 of them `stepRoutines` (83% on a tick % 20 = 0), 4 `stepWorldTasks`. Worst 10 at 1x (tick, total ms; what ran): 88020 84.0 `routines` 51.9 + `ai_squad` 9.6 + `combat` 9.3 + `tasks` 8.2; 81580 83.5 `routines` 59.3 + `world_tasks` 8.4; 87380 78.8 `routines` 57.5; 82780 78.3 `routines` 51.7 + `world_tasks` 10.3; 81980 76.2 `routines` 49.2; 78320 71.6; 87580 71.3; 80380 67.7; 79980 67.2; 83980 66.9 (the last six the same shape: `routines` 40-51 ms + `world_tasks` 6-10 ms + `ai_squad` 4-8 ms). At 5x the worst 10 are the same (81580, 83980, 87380, 94780 ... 98-104 ms) plus a `wild` respawn at tick 130920 (18.7 ms) and a `squadStep` / world-squads pass at 79580 (17 + 11 ms). Every one of the ten is a 1 Hz tick (tick % 20 = 0) that also runs the routine scorer (every 5 ticks) and the world-task selector. In the 5x run with the finer marks, ticks over 30 ms are 260: 165 `routines`, 48 the combat step, 29 `world_tasks`, 13 `squadStep`; 70% on the 1 Hz phase. The 48 combat ones are the exception: single ticks of **43-73 ms in the combat step off the 1 Hz phase** (ticks 106211 and 105851 at 72.8 ms, 107141 62 ms, 76940 54 ms), 3-5 per 10 min; cause NOT found (a path-query burst from `routeFrom` replans, or a fight, is a guess; verify with a `sub` mark inside `CombatArena::step`).

**Stack samples** (gdb, 300 stacks of the sim thread at 5x, so a saturated loop; the raw stacks are not kept): sim tick 75%, `Session::sendTick` 23%. `stepCombat` 40% (`CombatArena::step` 31%: `update` 15%, `hub` / `pickTarget` / `scanCandidates` 11%, `senseAll` 6%, `followStep` 4%; `Fighter::operator=` 8.7% is the mirror), `stepWorldTasks` 14%, `stepRoutines` 5.7% (`scoreMember` 5.7%, `routeRetry` 3.3%), `MeshNavigator::routeFrom` 6% (Detour `findNearestPoly` / `queryPolygons` 7%). **29% of the samples are inside flecs query iteration** (`ecs_query_next`, `flecs_query_*`): by caller `stepDoorSteps` 4.0%, `stepWorldTasks` 3.3%, `Session::writeSquads` 2.7%, `sensorContext` (from `scoreMember`) 2.0%, `sendTick` 2.0%, `characterSquads` 1.7%, `playerSquads` + `playerBuildings` 2%, `liveRole` 1%: uncached `ecs.each` over the world every tick, against the ARCHITECTURE rule.

**Fixed-cost tuning (kenshi-k16.6.233; branch ds/tick-fixed-costs; same recipe, 5x, 600 s wall, `heft_run.sh`; the box stayed shared, load averages below, so only the sub rows, not the tick means, are comparable):**

| | before (master) | after (fight goals + prelude) |
|---|---|---|
| load before / after the run | 23.4 / 13.7 | 32.1 / 18.1 |
| server CPU of 600 s | 575 s | 545 s |
| ticks, tick mean / p50 / p99 (us) | 48,748; 9,306 / 7,732 / 31,978 | 55,161; 7,805 / 6,252 / 29,113 |
| `tasks` mean (us) | 2,049 | 693 |
| `sub_wt_fight_goals` mean / p99 (us) | 1,169 / 2,493 | **69 / 230** |
| `sub_wt_prelude` mean / p99 (us) | 274 / 577 | **51 / 143** |
| `sub_combat_mirror` mean / p99 (us) | 823 / 2,531 | 827 / 3,351 (unchanged) |

- **Fight goals:** the pass walked `ecs.each` over every entity holding a `state::Id` (the 4,827 squads, towns and factions as well as the 306 bodies) and looked each one up in the arena. It now walks the arena's members and takes the entity by index (`combatEntities_`): 1.17 ms to 0.07 ms.
- **Prelude:** town circles, town ids and fortifications are cached (`PreludeCache` in `stepWorldTasks`), rebuilt when a town's entity / centre / record sids, the gate-site count or the door system's binding differ. What is left (51 us) is the per-tick key walk over the towns' cached query.
- **Mirror: NOT improved, change dropped.** Tried a defaulted `Fighter::operator==` with an in-place write of only the changed bodies: 1,014 us (load 27), no better than the `set` copy. Most members change every tick (position, timers), and a full compare of the idle ones costs about what the copy does. A real gain needs the arena to flag the bodies it wrote (a dirty bit set at each `update`), which touches every writer in combat: left for a decision.
- **Mirror, what changes per tick (kenshi-k16.6.257; measured, instrument removed).** Heft cage_prisoner, 5x, 200 s, 309.7 members per tick, each member's arena `Fighter` compared with its ECS copy before the `set` (a temporary defaulted `operator==`): **0.5% of member-ticks unchanged, 0.0% change only in pose (x, y, z, yaw, aim, speeds, headings, velocity), 78.9% change in pose plus timers only, 20.6% change in something more**. The fields that change on nearly every member every tick are not position: `hunger` 98.5%, `senseClock` 95.8% (both advance per tick for everyone, walking or not), then `targetAge` 32%, `stateTime` 32%, `fightGoalClock` 24%, `followAge` 18%; `sight` 7%, `parts` 6%, `blood` 6%. **Result: a per-body dirty bit buys nothing (99.5% would be dirty), and a position/full split buys nothing either (pose-only is 0%).** The copy is already the cheapest form of a whole-struct sync; a gain needs a field-group split where the per-tick group (`hunger`, `senseClock`, the pose, the few timers) is copied every tick and the cold group (strings, vectors, sub-structs) only when it changed. That needs a dirty bit for the cold group set in every writer (`find()` is mutable everywhere), or ECS readers of `hunger` / `senseClock` made lazy: a design choice, not done. Branch ds/mirror-dirty holds only this note.

**Combat step cost and the isolated spikes (kenshi-k16.6.235).** `CombatArena::step` has six sub marks now (`cs_index`, `cs_sense`, `cs_env` = weather / beam / `correct`, `cs_separate`, `cs_update`, `cs_tail`; the old `combat_step` mark keeps only the severs and deaths after it, so the step is the sum of the `cs_*` and it).
- **Spike cause (named): one `MeshNavigator::routeFrom` (Detour pathfind) inside a single member's `update`**, not a replan burst. A per-update timer (dev only, removed: a clock read costs several us on this Xen guest and distorted the run) caught tick 76519 (t = 3825.9 s): one member (state Attack, fight goal 32, target 28668), one route call, 37.6 ms of a 37.7 ms update; the steady cases are 4-7 ms single routes (a member approaching target 26998 re-queried for tens of ticks; `circle` / `pathLength`). The spike ticks are the same in both runs below (47 steps over 20 ms each), so they are the deterministic long-route queries. Fixing them changes behaviour (a node limit or a spread of the query), so it is NOT done: asked in the report.
- **Hot spots** (gdb samples of the sim thread, 30 / 55 samples inside the step, before and after): `scanCandidates` (it walked every member and called `perception::aware` per pair), `senseAll` (`perception::scan` took a `hypot` per pair; line of sight `HullTree::traceNode` / `LineOfSight::visible` stays 10-15%), `CombatData::race` (a linear string-compare scan, called by `footprintOf` per pair in `avoid`), `formationOffset` (per follower, a scan of all members with a binary-search lookup each), `correct` (navmesh height probes).
- **Done, no hash change** (same iteration order and results; the quick suite passes 199/199 and every tick-pinned test): `scanCandidates` walks the observer's sight records (sorted by id like the members) instead of all members; `perception::scan` rejects by squared range before the `hypot`; `CombatData::race` through a hash index (first match as before); `avoid` tests the height first; `formationOffset` skips the lookup of a follower of a leader that follows nobody; `protectOwnSquad143` returns at once for squad 0. **A spatial grid and a per-pair line-of-sight cache were NOT built**: the scan is bounded by the sight list, not by the member count, and no pair is asked twice in a tick (the cache would hit nothing); the line of sight is now the largest single piece of `cs_sense`.
- **Measured** (cage_prisoner at Heft, 5x, 10 min wall, `heft_run.sh`; the before and after servers ran AT THE SAME TIME on the same box, load 17.6 at the start, 10.2 at the end of other workers' jobs, so each pair shares its noise; the before binary is master plus the marks only):

| 600 s wall, 5x | before | after |
|---|---|---|
| server CPU seconds | 506.2 | 497.6 |
| ticks / sim time | 56,859 / 2,843 s | 57,525 / 2,876 s |
| combat step (sum of `cs_*`) mean / p50 / p99 / max (us) | 3,046 / 2,611 / 7,871 / 61,672 | **2,691 / 2,310 / 6,824** / 65,216 |
| `cs_sense` / `cs_env` / `cs_update` mean (us) | 531 / 468 / 1,927 | 505 / 479 / 1,574 |
| whole tick mean / p50 / p99 (us) | 6,928 / 5,382 / 26,016 | 6,613 / 5,113 / 25,747 |
| ticks over 10 ms | 12.2% | 11.1% |
| steps over 20 ms | 47 | 47 (the route spikes) |

  The step is 12% cheaper on the mean and p50 (-355 us), 13% on p99; the tick 4.5%. Earlier concurrent 130 s pairs gave -20% and -0% (a loaded box: noise is +-10%), so treat the gain as 10-20%, not more. The step was 4.0 ms in the k16.6.230 runs on a busier box; this box state gives 3.0 ms for the unchanged code. **The grid and the cache are not where the cost is**; the next pieces are `cs_update` 1.6 ms (the 305 members' own logic) and the LOS in `cs_sense`; then the LOD (kenshi-k16.6.236).

**`cs_update` and the line of sight (kenshi-k16.6.263).** `cs_update` cannot take sub marks per phase (a member's update interleaves them all, and a clock read per member distorts on this Xen guest), so the split is by gdb samples of the sim thread (150 samples, 56 inside the step; box load 35-45). Inside the step: `cs_update` 33 of 56 (59%), `cs_sense` 9 (LOS 7 of them), `correct` / `cs_env` about 12, `cs_separate` 2. Inside `cs_update` the top three:
1. **`scanCandidates` / `pickTarget` -> the faction relation** (`sensor::enemy` / `sensor::flags` -> `ctx.relation` -> `factionRelation` -> `GameData::find(std::string)`, one string copy and one hash lookup per call, a flecs `get<Relation>` before it): about 12 of the 25 `update` samples (about half of `cs_update`). Asked per candidate by `enemy`, by every `worldres` goal resolver and by `flags`.
2. **`formationOffset`** (per follower per step: an O(members) squad scan, then the random layout's Gauss-Seidel relaxation, 16 x n^2 pair tests with an RNG stream): 8 of the 56 step samples (14% of the step).
3. the rest of `update` (state machine, `aware` / sight-list walks, `routeFrom`): spread thin; no single frame over 3 samples.

- **Done, no hash change** (same iteration order and results; quick suite 199/199): the relation is memoised per `(from, to)` for the length of one `step()` (`CombatArena::setSensorContext` wraps the Simulation's function; reads through outside a step; nothing in the step writes the factions' relations, the observer only forwards events); `formationOffset` keeps the random layout per `(seed, leader dice key, n)` (a pure function of those) and reads the follower's slot from it.
- **LOS in `cs_sense`: 7 of 56 step samples (12.5% of the step, about 0.19 ms of the 1.5 ms `cs_sense`)**: `LineOfSight::visible` -> `traceImpl` = a hull-tree trace per loaded region the segment crosses, door leaves, then a terrain march (a `terrainHeight` per `kTerrainStep` plus bisections). Already gated by range (squared cull), the vision cone and the same-building test before it is asked, and `visible` has the exact-ends pair cache. A cheaper identical test was NOT found: (a) `perception::scan` computes `los` even where the result is unused (a sneakish, unengaged target with attention 0), but skipping the call would change what the pair cache holds (a later cast at the same ends within `kPairTicks` could answer fresh instead of cached across a door change), so it is not hash-identical in principle; the case is rare at Heft (no gain to pay for the risk). (b) A height / range pre-reject would answer `blocked = false` where the exact trace says blocked, so it changes results. Asked, not built.
- **Measured** (cage_prisoner at Heft, 5x, 600 s wall, `heft_run.sh` with the binary chosen, before and after running AT THE SAME TIME; the box was badly overloaded, load 40.6 at the start and 62.6 at the end, so the tick is 2x the k16.6.235 numbers and the pair is only comparable to itself; the before binary is ksim 1c69f8a1):

| 600 s wall, 5x | before | after |
|---|---|---|
| server CPU seconds | 257.7 | 257.2 |
| ticks run | 20,612 | 22,492 |
| `cs_update` mean / p99 (us) | 6,253 / 20,438 | **3,998 / 13,881** |
| `cs_sense` / `cs_env` / `cs_separate` mean (us) | 1,534 / 1,678 / 347 | 1,528 / 1,634 / 335 (untouched: the control) |
| combat section mean (us) | 12,773 | 10,511 |

  `cs_update` is 36% cheaper (-2.25 ms on this loaded box; the unchanged marks agree within 3%, so the gain is not the noise). On a quiet box scale it by the k16.6.235 ratio: roughly 1.57 ms -> 1.0 ms. A first 600 s pair was lost (the first "after" crashed at tick 0: object files built across a header edit, a stale-build artefact, not a code fault; rebuilt after `touch` and the pair rerun).

### Cached queries for the per-tick `ecs.each` callers (kenshi-k16.6.234)

`World::each(fn)` (`world.hpp`) is `ecs.each` over a cached query: the query is built once from the lambda's own parameters and kept for the life of the world; `World::eachMember(squad, fn)` walks the `(MemberOf, squad)` tables by id. Converted: `stepDoorSteps`, `stepWorldTasks` (the faction, town, door and squad lookups; not the prelude, which k16.6.233 caches), `sensorContext`, `playerFactionSid`, `worldStateFacts`, `playerSquads`, `completedResearch`, `discoveredPlaces`, `playerBuildings`, the encode's `writeSquads` and the world scans in `sendTick`, `liveRole` and `squadMembers`. NOT converted: `characterSquads` (already cached queries; its cost is the map it builds), `cagesNear` (its town scans use the cached query now; the per-building loop is unchanged), the prelude. A query with the squad as a variable (`.second("$squad")`) was tried first and was SLOWER (it scans every squad's table per call; `sub_world_tasks` 181 -> 283 us): the by-id walk is the fast one. The profiler's `path_queries` / `path_us` now count `MeshNavigator::routeFrom` (3.3 per tick, 380-510 us per tick incl. the 1 Hz bursts; the Heft run reads it nonzero).

State: `--dump-snapshot` / `--dump-bin` of cage_prisoner over 600 ticks, master ab2692f1 vs this branch, `cmp` equal on both (5,450,935 bytes). Server CPU seconds during each 180 s run (saturated, so they match; the ticks delivered differ) and the load average at the start of the before / after run sit next to the numbers; the box was shared (load 2-23), so only the pairs run back to back are comparable. Three before / after pairs, 5x, 180 s each, cage_prisoner at Heft (sections in us per tick, mean; `simlink` is the encode):

| pair | load (start) | tick mean before -> after | `simlink` before -> after | residents | `sub_world_tasks` | CPU s |
|---|---|---|---|---|---|---|
| 1 | 23.4 / 21.9 | 9,608 -> 9,589 | 2,661 -> 2,861 | 1,072 -> 965 | 232 -> 155 | 169.8 -> 170.1 (14,273 / 14,073 ticks) |
| 2 | 21.1 / 21.1 | 10,407 -> 9,549 | 3,145 -> 2,543 | 1,164 -> 892 | 243 -> 142 | 170.8 -> 171.8 (12,954 / 14,493 ticks) |
| 3 | 16.6 / 4.1 (the box emptied: not comparable) | 6,902 -> 6,288 | 1,820 -> 1,711 | 848 -> 685 | 193 -> 117 | 154.4 -> 145.5 (17,478 / 17,885 ticks) |

The gain is real but small and the noise is as big as it: 0.0-0.9 ms of the tick (0-9%), 0.1-0.6 ms of the encode, mostly `residents` (-0.1 to -0.3 ms: `sensorContext` per `scoreMember`) and the world-task selector (-0.08 to -0.1 ms). `combat` (55%, untouched) moved +-1 ms between runs on load alone. Re-measure on a quiet box. The ~29% of samples in flecs iteration was not re-sampled with gdb here; the remaining per-tick uncached `each` sites (the prelude, `characterSquads`, per-building loops) are the next place to look.

### The 1 Hz squad passes on id % 20 slices (kenshi-k16.6.320, from k16.6.232)

Change (OURS; the state hash and tick-pinned timings move): `squadStep`, `prepareWorldCarry`, `stepWorldSquads`, the world-task selector and the executors' per-member `walkTo` retries run every tick on the squads / members with `id % 20 == tick % 20` (`Simulation::inSlice`); each keeps its 1 s period. The squad list is the AI's cached one, bucketed by `id % 20` (`AiSystem::squadSlice`), so a tick walks 1/20 of it. The scorer timers are NOT staggered here (parked: k16.6.232). Rules and the load fix: SIM_SCHEDULING.md 5D.

**Round 1 as first reported (master vs branch, started together; kept for comparison; its routing reading is withdrawn below).** Measured with `tools/heft_run.sh`, cage_prisoner at Heft, 5x, **300 s wall** each (not 600), `KSIM_PROFILE` builds, before (origin/master b455874d) and after (ds/stagger-slices) **running at the same time** on dev. What else ran: the box was otherwise idle for pair 2 (load average 9.5 / 17.4 / 19.9 at the start, 6.1 at the end: the other worker's tests had just finished); pair 1 ran under load 29 / 27 / 21 (the other worker's ctest), so it is noise-heavy and only shows the direction. The two runs do not simulate the same thing after the first slice (the hash moved).

Pair 2 (idle box; after has the per-slice bucket, pair 1 did not):

| | before | after |
|---|---|---|
| ticks (first 100 dropped) | 29,881 | 29,174 |
| tick mean / p50 / p90 (ms) | 4.94 / 4.10 / 6.53 | 7.18 / 6.90 / 10.62 |
| tick p99 / p99.9 (ms) | 21.2 / 38.5 | 14.3 / 33.5 |
| ticks over 20 ms | 455 (1.52%) | 76 (0.26%) |
| tick % 20 == 0: mean / p99 (ms) | 18.3 / 44.7 | 11.8 / 35.5 |
| the other 19 ticks: mean / p99 (ms) | 4.2 / 8.1 | 6.9 / 12.9 |
| ticks over 20 ms, tick % 20 == 0 / other | 436 / 19 | 41 / 35 |
| `residents` per tick, mean / p99 (us) | 348 / 3,048 | 301 / 2,402 |
| `sub_squad_step` / `sub_world_tasks` / `sub_carry_prep` mean (us) | 133 / 96 / 53 | 314 / 200 / 114 |
| `path_queries` / `path_us` per tick, mean | 4.9 / 485 | 8.0 / 2,914 |
| `sub_cs_update` mean (us) | 1,201 | 3,226 |

Reading: the tail is what the slicing buys (p99 -7 ms, ticks over 20 ms -83%, the tick % 20 == 0 spike halved) and it is not free: the other 19 ticks cost 2.7 ms more on the mean. The slice loops' own fixed cost went up (squad step 133 -> 314 us) but is not the main part; **routing is** (`path_us` 485 -> 2,914 us per tick, 4.9 -> 8.0 queries): the same routes asked in one tick shared the pair / local caches (one batch of leader carrots and follow routes planned together), asked a tick apart they do not (UNSURE: not verified against the cache's code; the check is a route-query count per second with the slices on and off). Pair 1 (loaded box, no bucket): p99 27.7 -> 30.8 ms, mean 7.3 -> 11.9 ms, tick % 20 == 0 mean 23.9 -> 19.0 ms, squad step 196 -> 552 us per tick (the loop over all 4,800 squads each tick, which the bucket removed), the same routing rise. So **the gain is the tail, the price is routing; a cheaper route per slice (a cache that spans the second, or the leaders' carrot walks left on the second) is the next step.**

**Rework 1 (the routing price was a measurement artefact; three servers at once, same box).** `tools/heft_run.sh`, cage_prisoner at Heft, 5x, 300 s wall, `KSIM_PROFILE` builds, three servers started together so they share the box's load: (a) origin/master, (b) this branch with the slices switched off (a scratch env switch that made every slice test `tick % 20 == 0`; not committed), (c) this branch. Load average 16.8 at the start, 10.0 at the end (other workers' builds and tests ran; the box was NOT idle); server CPU 258 / 255 / 257 s of 300 (all three saturated, so they delivered different tick counts). The first 100 ticks are dropped.

| | (a) master | (b) slices off | (c) slices on |
|---|---|---|---|
| ticks | 27,025 | 26,711 | 28,143 |
| tick mean / p50 / p90 (ms) | 7.71 / 6.35 / 13.05 | 7.72 / 6.34 / 12.93 | 7.24 / 6.55 / 11.09 |
| tick p99 / p99.9 (ms) | 23.8 / 50.6 | 24.7 / 49.5 | 18.1 / 47.8 |
| ticks over 10 ms | 21.0% | 21.1% | 14.1% |
| `path_queries` per tick, mean | 4.78 | 4.79 | 5.58 (+17%) |
| `path_us` per tick, mean (us) | 1,918 | 1,899 | 985 (-48%) |

Split by phase (same three runs; ms): tick % 20 == 0: mean 20.4 / 20.8 / 13.7, p99 59.9 / 54.1 / 53.5; the other 19 ticks: mean 7.0 / 7.0 / 6.9, p99 18.1 / 17.7 / 16.3; ticks over 20 ms 2.31% / 2.52% / 0.59%; `residents` per tick mean / p99 (us) 451 / 3,348, 434 / 3,424, 388 / 3,120. The second's first tick keeps a p99 of 53 ms with the slices: what is left on it is the scorer timers (`stepRoutines` batches, the parked k16.6.232 stagger), the passes over every member (`secondTick` in `stepWorldTasks`) and the strategic ticks, not the squads.

A second pair (a different run, load 7-13, no master): slices off / on: tick mean 6.31 / 5.83 ms, p99 22.0 / 13.2 ms, over 10 ms 11.1% / 3.7%, queries 4.76 / 5.50 per tick, `path_us` 1,507 / 833. So: with the box shared the three behave alike except the slices, which cut the mean by 6-8%, the p99 by 25-40% and routing time by about half. **The earlier "routing rises 6x" was not the slices:** that pair compared a master build with the branch run under a different, heavier load (path_us 485 vs 2,914 us; master's own path_us reads 1,350-1,900 here), and the comparison of this branch with its own slices switched off shows the opposite. The same-binary switch is the control the first pair lacked. Where the calls fall (a per-stage counter, scratch, not committed; calls per run, slices off / on): world-task orders 63.0k / 68.7k, the combat update 31.8k / 65.1k, the selector's crime-goal pass 9.7k / 0.35k, residents 3.5k / 1.3k, delivery log 7.3k / 9.0k. The combat update's route requests doubled (the arena's own counters, `pathQueries` / reroutes / off-path repaths, moved by only +0.4k / +0.2k / -10 of the +33k: so they are not the combat's candidate path lengths or its re-plans; which combat call the rest are is NOT found). The +17% in route queries per tick is real in both pairs and is paid back by cheaper queries (the 1 Hz bursts no longer go cold through one query object). UNSURE: why the combat update asks for more routes when its members' orders are spread over the second; verify with a per-caller counter in `Combat::update`.

Tests repinned or fixed in this change (rework 1, after merging master): `ksim_slavers_capture` is repinned with the cause named: its seeds are fight-outcome pins, and a `KSIM_SEEDS` sweep (now in the test; 706960716, 1151889715, 1..7, 100..105, 2026-11-17) fails 5 of 15 seeds on master (1, 4, 7, 102, 104) and 5 of 15 on this build (2, 5, 100, 102, 104): the slices permute which seeds fail, not how many. The pinned seeds keep their runs and two flags follow the fight (1151889715 now wakes, 3 stays down). `ksim_trader_circuit` (the arrival is stamped at the caravan's own step tick, id % 20), `ksim_bed_choice` (watches from the first tick: the carrier is decided in its squad's slice and the put-in finishes inside the old 600-tick wait), `ksim_player_sleep` (the parser reads CHAR_DYNAMIC's 4-byte aim tail: a fighter is mid-swing in that world), `ksim_arrest` (the guard's squad-mates are held down: their selector lifts the downed target before the guard's order, by slice phase). The earlier `inFight` resting-block change is gone: master carries its own fix for that stall (`inFight` no longer counts a technique index alone). `ksim_bw_orders` passes sliced since master moved. Still failing sliced, passing unsliced (SIM_SCHEDULING 5D names the cause): `ksim_raid_takeover_scenario` (Heft variant) and `ksim_raid_goals` 270.

**Route requests traced (kenshi-k16.6.360, branch ds/k16.6-route-requests).** Method: a scratch `backtrace()` in `MeshNavigator::routeFrom` (route calls grouped by their callers, not committed) and a same-binary switch that puts every slice back on tick % 20 == 0 (not committed); `heft_run.sh`, cage_prisoner, 5x, 300 s, `KSIM_PROFILE`, sliced and unsliced started together. The sim is deterministic here: two sliced runs gave the same caller counts to 0.1%, so the differences below are real, not noise. Calls per run, unsliced / sliced, before the fix: world-task `walkTo` (world_tasks.cpp 857 via 1442) 74.0k / 71.6k, combat `chaseStep` replans (follow + integrate) 36.8k / 38.1k, `stepWorldSquads` carrot walks 20.9k / 24.6k, `scoreMember` 8.7k / 5.7k, **`stepIntruders` take-out walks 0.1k / 10.5k, `stepSlaveTrade` 0 / 2.9k**. The "combat update doubled" of the earlier per-stage counter is NOT in the arena's own calls (37k both ways): the extra is in the carry steps that run every tick after it.

Cause: `stepIntruders`' step 67 (and the slave chain's `runTo`) re-asked the route **every tick** while the walker was not walking and not within reach. A take-out walker that stops 7 u short of its body (the body's footprint, the 4 u `kBodyReach`) was given the same 2-point route to the same goal 10,000 times in a run (traced: one id, ticks 77895-77921+, position moving 0.1 u). Unsliced it did not happen in this run only because the fights went another way; the per-tick re-ask was already in master. Fix: `PathRetries::askedRecently(id, goal, tick, window)`: a walker that asked the same goal (1 u) less than a second ago does not search again (`stepIntruders::installRoute`, `stepSlaveTrade::runTo`).

| (same box, 300 s, pairs run together; first 100 ticks dropped) | unsliced | sliced | sliced, fixed |
|---|---|---|---|
| route queries per tick | 5.03 / 5.04 | 5.40 / 5.39 | 5.20 (unsliced, fixed binary: 4.94) |
| `path_us` per tick | 1,615 / 1,661 | 770 | 730 (unsliced 1,682) |
| tick mean (ms) | 6.46 / 6.75 | 6.08 / 6.29 | 6.27 (unsliced 6.95) |
| tick p99 / p99.9 (ms) | 22.9 / 47.1 | 13.3 / 41.1 | 13.6 / 39.0 (unsliced 24.8 / 50.2) |

(A fourth pair with a first version of the guard that required a non-empty route did nothing: the walker's route is cleared each tick, so the stamp alone gates.) `stepIntruders` walks 10.5k -> 0.24k; total route calls 160.1k -> 154.3k. **Remaining sliced extra, +5% queries per tick (5.20 vs 4.94), named:** combat `chaseStep` replans 42.2k vs 36.8k (the chase / follow replans of members whose target moved > `kChaseReplan`; the leaders' carrots are now set on 20 different ticks so a follower's target moves on every tick, not in one step per second: the replan count follows the target's motion, which the slices spread, UNSURE: not isolated; verify by counting replans per second with `kChaseReplan` at 2x), `stepWorldSquads` +3k (the squads' own carrot walks, one per squad per second in both; the sliced sim has more squads walking at that point), and `scoreMember` +4.4k (scorer ticks, state-dependent). Not changed: the tail is what the slices buy (p99 -45%) and `path_us` is -57%.

### Verdict

- **1x holds**: tick mean 10 ms of 50; 0.36% of ticks over (43 in 10 min), all of them the 1 Hz pass at 50-84 ms, none dropped. With the encode 0.39%. Server CPU 24%.
- **5x does not hold, and spreading the 1 Hz pass alone will not make it hold.** The steady tick (the 19 ticks that are not the 1 Hz pass) is 8.4 ms and the encode 2.8 ms: 11.2 ms against a 10 ms budget, on this loaded box. 53% of ticks are over; the server runs at 94% CPU and delivers 4.05x. Roughly 3.5 ms of the steady tick has to go before p50 is comfortably under 10 ms, and then the 1 Hz and routine-scorer spikes (25-100 ms) still land as catch-up bursts.
- Nothing found is a wall: every cost is a cache, a skipped copy, a spatial index or a phase stagger. The one lever that is a real design decision is the combat arena running all ~305 Full-detail members every tick (4 ms).

| cost | per tick | kind | proposed change | est. gain |
|---|---|---|---|---|
| `stepRoutines` batches: scorer timers in lockstep (`nextScore = now + kScoreIntervalTicks` at routine_scorer.cpp 2705 and 3595 has no per-member offset; only line 2569 adds `id % kScoreIntervalTicks`), so the same ticks spike 40-78 ms every run | spikes, 48% of the section | structural (moves the state hash and routine-timing tests) | stagger `nextScore` by `id % 20` at every site | spike 40-70 ms to about 3-8 ms; removes ~54 of 58 ticks over 40 ms at 1x and ~165 of 260 over 30 ms at 5x. UNSURE: the cause is read from the timing pattern, not traced; verify by logging how many members score per tick |
| the 1 Hz pass (`squadStep` ~3 ms, world squads, carry prep, the goal selector 5 ms p99) | 1 tick in 20: +17 ms | structural (id % 20 spread; moves hash, tick-pinned tests) | spread by id % 20 | 1 Hz tick 26 ms to about 9 ms; the cause 2 of "5x tick tail" |
| `combat_step`, ~305 members | 3.96 ms | tunable first (spatial grid for `scanCandidates`, cache `senseAll` and line-of-sight per pair) then structural (idle residents stepped every 2-4 ticks) | grid first | 30-50% of the step: 1.2-2 ms; LOD larger, moves the hash |
| `combat_mirror`: every member's `Fighter` (strings, vectors) copied into the ECS each tick | 0.85 ms | tunable | `set` only members that changed (a dirty bit from the arena), or write in place | 0.7-0.8 ms, no hash change |
| `tasks` fight-goals pass (every tick) | 1.1 ms | tunable | only members with a fight goal or a change; unverified what dominates inside | up to ~1 ms |
| `tasks` prelude (town circles, fortifications, rebuilt each tick) | 0.28 ms | tunable | cache on the town / gate-site revision | 0.28 ms |
| uncached `ecs.each` in per-tick code: `stepDoorSteps`, `sensorContext`, `cagesNear`, `liveRole`, the encode's `writeSquads` / `characterSquads` / `playerSquads` / `playerBuildings` / `completedResearch` / `discoveredPlaces` | ~29% of wall samples touch flecs; the named callers about 20% | tunable (cached query members) | one cached query each | 1.5-2.5 ms of tick and encode together (a sample share, not a timing) |
| the encode `simlink` | 2.8 ms (p99 14) | tunable (the list above) | as above | 1-1.5 ms |
| isolated 43-73 ms combat-step ticks | 3-5 per 10 min | one `routeFrom` of 37+ ms inside one member's `update` (k16.6.235) | cap the node count or spread the query: changes behaviour, asked | up to 40-70 ms on those ticks |

The tunables alone (mirror 0.8, prelude 0.3, fight goals up to 1, queries and encode 2-3, combat step 1.2-2) are worth about 5-7 ms of the 11.2 ms steady tick+encode, which would hold 5x on average; the p99 stays over 10 ms until the 1 Hz pass and the routine scorer are staggered. Not done here: any fix.

**Re-measure of 233 + 234 + 235 and the flecs share (kenshi-k16.6.264; branch ds/flecs-resample; 5x, 180 s wall, `heft_run.sh`, three before / after pairs back to back).** Before = ksim at 893bc582^ (no 233), after = ksim 1c69f8a1; the box was NOT quiet (load 29-47 from other jobs, never under 4), so read the pairs, not the absolutes. Server CPU was 161-170 s of 180 s in every run.

| pair | before mean / p99 (us), ticks | after mean / p99 (us), ticks |
|---|---|---|
| 1 | 9,423 / 33,176, 13,843 | 8,079 / 28,150, 15,506 |
| 2 | 8,598 / 30,098, 15,251 | 7,818 / 27,412, 15,892 |
| 3 | 9,163 / 32,422, 14,202 | 8,107 / 31,772, 15,303 |

All three pairs agree: mean -8 to -14% (about -1.0 ms), p99 -2 to -15%, 9% more ticks done in the same wall time. **Stack samples** (gdb, 300 stacks, after): flecs query iteration **26.7%** (was 29%); by caller `sensorContext` 6.3% (the faction map rebuilt per call by the routine scorer), `stepDoorSteps` 2.7%, `characterSquads` 2.7% (already a cached query; the cost is the 300-fighter walk and the map), `shopSquad` 2.0%, `playerSquads` 2.0%, `sendTick` 1.7%, `playerBuildings` 1.3%. **Cached:** `sensorContext` is kept per tick and faction count (one build instead of one per member); `stepDoorSteps` no longer copies every shut `state::Door` each tick. One run on that build, load 16-20: mean 7,214 / p99 25,471, 16,859 ticks (not paired). Not cached, named: `characterSquads` (2.7%, a content map callers may expect fresh), `shopSquad` (2.0%, a lambda over `TraderStock`), `playerSquads` (2.0%). The quick suite passes (199/199).

**Squad caches and the quiet-box re-measure (kenshi-k16.6.279; branch ds/squad-caches, d41cca73 on ksim e7964065).** `characterSquads`, `playerSquads` and `shopSquad` are now cached in `SquadCaches` (`include/ksim/world/squad_caches.hpp`); flecs observers registered in the `Simulation` constructor (`registerSquadCaches`, player_squads.cpp) set the dirty flags, so the cache is exact, not per tick. `characterSquads` returns a const reference (simlink holds a pointer, `squadIdOf`).

| cache | dirty after | writers (all of them) |
|---|---|---|
| `characterSquads` | `PlayerSquad` set / removed, `MemberOf` added / removed, `Fighter` added / removed, `Id` set / removed | every write of these goes through `entity.set / add / remove` or entity deletion (grepped: no `get_mut`, `ensure` or `modified` of `PlayerSquad`, `PlayerFaction`, `TraderStock`, `Id` or `Fighter` remains; the one in-place `PlayerFaction` write, `importPlayerFaction`, was changed to copy + `set` in the rework), which fire the observers; `restoreBoards` resets all three caches after a load |
| `playerSquads` (all but `factionCount`) | `PlayerSquad` / `PlayerFaction` / `Id` set or removed | player_squads.cpp orders and `adjustPlayerMoney`, import_squad.cpp `importPlayerFaction` (copy + set, reworked), trade.cpp 623, discovery.cpp 141, newgame.cpp, factions.cpp 250. `factionCount` and `limits` are recomputed on every call: they read the arena's `dead` flags and `squadLimits()`, which no ECS event reports |
| `shopSquad` | `TraderStock` / `Inventory` added / removed, `MemberOf` and `Fighter` as above, `Id` set, and `Simulation::touchHomes()` | the plain writes of `Fighter::homeBuilding`: routine_scorer.cpp 934 (resident adopt), simulation.cpp 1540 (`adoptFighter`) and the two `combat_->add` calls (1372, 1555), carry.cpp 405 (`placeInBuildings`), player_squads.cpp 581 (dismissal); each calls `touchHomes()` |

**Pairs** (`heft_run.sh`, 5x, 180 s wall, base = ksim e7964065, after = d41cca73, back to back, mean / p99 of the tick in us, CPU seconds of the server during the run, load average before -> after). The box was NOT quiet: the 40 min wait for load < 8 ended with the load at 18-38 from other workers' and CI test runs; only pair 1 started under 8 (6.1 / 3.9), and its after run drifted to 19 by the end. Sim state differs between runs (the probe is a live client at 5x), `combat` alone moved 3.3 -> 4.0 ms between pair 1's two runs.

| pair | before | after |
|---|---|---|
| 1 | 5,061 / 20,498, 17,886 ticks, 127 s CPU, load 6.09 -> 5.57 | 5,935 / 20,602, 17,784 ticks, 136 s CPU, load 3.91 -> 19.32 |
| 2 | 6,198 / 22,652, 17,473 ticks, 147 s CPU, load 19.61 -> 22.89 | 6,246 / 21,414, 17,655 ticks, 144 s CPU, load 23.23 -> 19.00 |
| 3 | 6,769 / 24,310, 17,282 ticks, 159 s CPU, load 18.53 -> 18.39 | 6,927 / 23,502, 16,907 ticks, 153 s CPU, load 18.25 -> 22.18 |
| 4 | 7,746 / 28,421, 15,506 ticks, 166 s CPU, load 21.39 -> 20.52 | 6,518 / 22,359, 17,480 ticks, 150 s CPU, load 19.15 -> 29.24 |
| 5 | 6,627 / 23,979, 17,263 ticks, 152 s CPU, load 29.63 -> 37.16 | 6,622 / 22,811, 17,102 ticks, 142 s CPU, load 38.23 -> 28.55 |

Read: the tick mean does not separate from the noise (+17%, +1%, +2%, -16%, 0%: the first is a combat-load difference, the fourth a loaded base run); the p99 is lower in 4 of 5 pairs (-3% to -21%) and +0.5% in pair 1. These three functions were about 7% of the sim thread's stacks, ~0.3 ms of a 5-7 ms tick, below what these runs resolve. **Stack samples** (gdb, 150 stacks of all threads, 5x, load 24-29): frames inside `characterSquads` / `shopSquad` / `playerSquads` base 5 / 3 / 2 of 150, after 0 / 0 / 0. **State hash:** `ksim_soak --bake navworld --days 10 --workers 1` (default window, 523,680 ticks), base and after both `74c087f96f6fd328` (127.8 s vs 105.9 s wall, both at load 30-40; the soak has no player faction, so `playerSquads` is covered by the quick suite only). Quick suite 200/200 (1 skipped, 1 disabled).

**Failed route searches at Heft (kenshi-k16.6.262; branch ds/route-spike; 5x, 600 s wall, `heft_run.sh`, box load 30+).** The isolated 40-110 ms combat ticks are not one long query but hopeless ones asked again and again: a temporary slow-query log in `routeFrom` (dev only, removed) found hundreds of failed `planned=0` searches of 3-50 ms, from fixed positions (a prisoner in the Heft cage, goal 60-70 u away behind a door the re-plan excludes). Base: 48 ticks with `combat` over 20 ms, 47 of them path-dominated (`path_us` over 80% of `combat`).
- **A closed plain probe ends the search.** `NavQuery::planPath` with a body radius runs the plain probe first; Detour returns a PARTIAL path as a success status, so a goal on another island passed the probe and the clearance A* then ran its budget (4 x the probe's nodes, 30-50 ms) to fail, and `routeForBody` ran the radius-0 search too. Now a probe that closed short of the goal with no node limit hit (the whole reachable set searched) ends the search (`PathResult::plainFailed`), and `routeForBody` skips the radius-0 retry when the probe closed from the unrefitted start. The original accepts only a goal-reached search and discards partials (rwh.144 section 2). The check must read the probe's corridor BEFORE `pushOffWalls` (it moves `endRef`; a first version tested after it and failed `ksim_move_no_route`).
- **A walk's failed re-request is blocked for 20 s.** The follow walk (off the route 5 s, or blocked) re-asked its own goal on every new plan, 3 times each. A failed request now blocks that goal for 20 s (`replanFail_`, `replanWalk`; the exe's `addBlockedTarget` on a failed request, rwh.144 section 1e) and a goal within 2 u counts as the same request (section 5); the old corridor is kept. The chase side already had its own throttle (re-plan only when the target moved 30 u, `kChaseCooldown`, the 20 s `giveUpUnreachable` block): OURS, looser than the original's 2 u goal-moved rule, left alone.

| 5x, 600 s wall | base (master a9c65b3c) | after |
|---|---|---|
| ticks with `combat` over 20 ms (path-dominated) | 48 (47) | 30 (25) with an 8192-node clearance cap, see below |
| max `combat` tick | 109.5 ms | 60.2 ms |
| single failed `planned=0` search | up to 50 ms | up to 36 ms |

(The table is the run that also had the cap below; the shipped code, without it, was not re-measured: the closed-probe fix alone measured 59 / 27 / 72.8 ms in an earlier run, noisy at load 35: the sim diverges per run, so tick counts are not exactly comparable.)

**NOT fixed: the acceptance (no combat tick over 20 ms from one query) is not met.** What remains: a start refitted onto ground the body fits (`planPath`'s refit) reaches the goal on the plain probe, but the clearance search through narrow gaps fails after its full budget (30-50 ms), and several members at one spot each pay it. A cap of 8192 nodes on the clearance search bounds it (about 12 ms) but dropped real routes (`ksim_route_soak`: 155 of 171 expected at radius 4), so it was NOT shipped. Candidates for ksim: memoise a failed clearance search per (start poly, goal poly) for some seconds, or move the query off the sim thread (the original's navmesh worker, rwh.144 section 0 row 3: out of scope, determinism). The original's 100,000-iteration cap is not reachable here (`kClickNodes` 32,768; Detour's pool is at most 65,535), so no cap was added. A `residents` tick (`stepRoutines`, `planned=1`) also pays up to 164 ms for one failed search: unchanged. **Determinism: the hash moves** (a failed walk re-request no longer repeats; a closed probe no longer reaches the clearance search). The 20 s hold is not saved (a load asks once more).

## Goal-fit exemption and route counters (kenshi-k16.7.62)

`clearanceCorridor` and `corridorFits` no longer ask the last hop into the goal's polygon to fit the body (the shrunk funnel keeps the corner off the wall): a goal in or beside a narrow passage the body cannot enter is reached by clamping instead of closing the search. Test `knav_clearance`: goals at z 96 / 97.5 / 99 in a 7 u gap at radius 4 were all `Closed` before and route now; a goal across the gap fails in a few nodes. New counters, kept in every build (`MeshNavigator::routeStats()`, printed by `ksim_soak` as "route failures"): `routeFailures[Failure]`, `routeClearanceNodes` (polygons the clearance searches closed), `radiusZeroRetries` (combat `routeForBody` retries; `Route::radiusFallback` marks the retried route). **Not measured:** the Heft before/after counts (needs a `build-prof` server run with `tools/heft_run.sh`); k16.6.319 re-measures with these counters. Component classes per radius (item 3) not done.

**One search per route (kenshi-k16.7.64; branch ds/route-one-search).** `planPath`: the probe corridor's pulled length is computed once (was twice when the clearance search ran), and its polys / clearance corridor / straight / cornerRefs / refs / corners buffers are `NavQuery::scratch_` members (were seven 16-48 KB allocations per route). `routeToStand` ranks the stand points by straight distance from the walker and stops at the first one longer than the best route found (a route is never shorter than the line): same winner, ties to the old order, far-side candidates never searched. NOT done: dropping the probe-then-clearance pair itself (the roundabout comparison keeps the shorter of the two, so removing it could move routes) and a before/after Heft run. `ksim_route_corners` (57 routes, quick, box shared so noisy): before 174 / 269 / 183 us per route, after 267 / 178 us (no measurable change at this size); p99 not measured. Quick suite 204/204 (2 skipped / disabled), no result moved.

**Measured later (kenshi-k16.7.92; branch ds/route-measure; `heft_run.sh` with the binary chosen, cage_prisoner at Heft, 5x, 300 s wall, `KSIM_PROFILE` builds; base 78ca5d04^1 and after 78ca5d04 started together, load 11 at the start, 8 at the end of other workers' jobs, so the pair shares its noise).** New KSIM_PROFILE counters (`knav/route_measure.hpp`: routeFrom time histogram, bucket upper bounds so p99 is good to about 9%; planPath clearance counts and time) print one cumulative line to stderr every 2000 routes; both builds carry the histogram, only the after build the planPath counters. Counts are since server start (the 25 s world load included), 142,000 routes at the last line, about 106,000 ticks in each run, about 1.3 routes per tick (a rate near 440 routes/s of wall). The three runs below all reached the same counts, so the sim is repeatable here.
| run | routeFrom mean | p50 | p99 |
|---|---|---|---|
| base (before 7.64) | 163.1 us | 46.3 us | 2097 us |
| after 78ca5d04 | 155.9 us | 42.5 us | 2097 us |
| after 78ca5d04, rerun | 148.7 us | 39.0 us | 1923 us |

Mean 4-9% lower after, p99 inside the bucket width: a small gain at most, the pair is within the box noise. Clearance search counts on 78ca5d04, over the 142,000 routes: 48,505 (34%) ran the clearance A* because the probe corridor did not fit; **7,225 (5.1%) ran it because the probe fit but was roundabout**; of those the clearance corridor won (shorter) **671 (9.3%)**. Time: all clearance searches 7.24 s of 21.1 s routeFrom (34%), but the roundabout ones only **80 ms (0.38% of routeFrom, 11 us each)**: they are short routes (`direct < kRefineRange`). **A single search would save under 1% of routeFrom time**, far under 10%; the clearance time is the probe-does-not-fit case, which a single search cannot drop. Keep the pair.

**Two more measurements (kenshi-k16.7.92 additions, 2026-11-19; branch ds/route-measure rebased on ksim c42c91ec).**

*Warm world load across k16.7.67 (4fe17d5b^1 vs 4fe17d5b).* `ksim_native_cache` (Release, Hub 3 x 3 zones, 2854 tiles), the warm open, 4 runs of each, alternating, box load 6-12 (the cold open is in the same range on both, 5.5-7.0 s). Before: 2122 / 1940 / 2584 / 2500 ms, median 2311. After: 2559 / 2556 / 2385 / 4999 ms (the last a one-off outlier), median 2556, mean without the outlier 2500. **About +9-10% (about +0.2 s) on the median, under the 20% threshold**; peak RSS +8 MB (518 -> 526 MB). The runs overlap in range, so the box noise is of the same size as the gain: no cheaper key is proposed. If it grows later: the digest cached beside the entry.

*walkOrder after k16.8.23 (c9a56232 vs 896e541d).* Same Heft run as above (`heft_run.sh`, 5x, 300 s, both servers together, load 7.5 -> 5.6, `KSIM_PROFILE` builds with a `WalkTimer` around combat's `walkOrder`, roomAhead probes included; the counters are on the ROUTEMEASURE line). walkOrder has no sub mark, so it is timed here directly. Ticks after the first 100: 29,871 / 29,875.
| | tick mean | p50 | p99 | walkOrder calls | per call | share of all tick time |
|---|---|---|---|---|---|---|
| c9a56232 | 5728 us | 5090 us | 15672 us | 939,598 | 3.4 us | 1.9% |
| 896e541d | 5936 us | 5215 us | 15814 us | 916,471 | 6.0 us | 3.1% |

walkOrder costs 76% more per call but is 3.1% of the tick (1.2 points more); the tick mean moved +3.6%, p99 +0.9% (inside the noise of a shared box). **Far under the 10%-of-tick threshold: no route-vertex cache needed.** The share counts the walkOrder time since server start (world load included) against all tick rows, so it is slightly low.

### Route spikes after k16.7.62 and k16.6.248 (kenshi-k16.6.319)

`KSIM_SLOWQ=1` (a KSIM_PROFILE build) makes `MeshNavigator::routeFrom` print one `SLOWQ` line per query over 5 ms (from, to, radius, planned, failure index, clearance nodes). Heft 5x, 600 s wall (`tools/heft_run.sh`, box load ~30): 57,236 ticks, mean 7.1 ms, p99 22.9 ms; 1,114 ticks over 20 ms, of which combat section 38, residents 110, tasks 6 (k16.6.262 left 30 combat ticks). 691 slow queries, 15.8 s in all: **Closed** (search ran out of reachable polygons, 29,135-29,213 nodes, plain search would succeed) 132 queries, 11.7 s, up to 281 ms; **OutOfNodes** at radius 22 (~2,400-4,400 nodes) 324, 2.4 s, 5-35 ms each; found routes 235, 1.6 s, max 17 ms. Closed planned=0 (combat): 24 queries, 4.9 s, the cage goals (67370,-70706 x14), 9 start spots. Closed planned=1 (residents, up to 281 ms): 108 queries, 6.8 s, 104 distinct goals: a poly-pair memo would not hit them; a clearance component id per radius class answers all 132 in O(1).

**Clearance components (kenshi-k16.6.382; branch ds/clearance-components).** `NavQuery::refusedByComponents`: per radius, every polygon of the mesh is labelled with a union-find over the portals the body fits (`fits`, the clearance search's own rule and cache; labels built whole-mesh on the first query at a radius, kept per geometry revision, so door repaints keep them and `replaceTiles` drops them; at most 32 radius classes; none on a mesh with off-mesh links). `planPathFrom` asks it where the clearance search would start (probe found a path that does not fit, no closed probe): when the start's and goal's components differ it answers `Closed` at once (found=false, 0 nodes). The search's exempt hops are modelled (the start's neighbours when `exemptFirst`, the goal's neighbours), doors and filters are ignored, so a refusal is only ever for goals the search could not reach either; found routes are untouched. A search that would have hit its node budget (OutOfNodes) now answers Closed for such goals. Test `knav_clearance`: the 7 u gap's far-side goals now cost 0 nodes (was 20-200), a same-side goal and radius 0 still route. A KSIM_PROFILE build with `KSIM_SLOWQ=1` prints `COMPONENTS r= polys= ms= bytes=` per label build (memory = 8 bytes per polygon: the parent array while building, 4 kept). The label build is sliced since k16.6.699 (see "Far walk stalls"); Heft far-walk numbers there.

## Far walk stalls, gate_heft at 1x (kenshi-k16.6.699, 2026-11-22)

Wanderer from Heft's gate to 66588,-50636, KSIM_PROFILE server (default native navtiles, warm disk cache), `tools/walk_probe.py` over SIMLINK (interest 2000 around the walker), 450 s, box load 2-7. Tick wall time over the last 4096 ticks (JSON line): p50 3.3 ms, p99 21.6 ms, max 172 ms (after the fix; mean 4.5 ms). Sim time per wall second stays at 20 ticks except where a stall hits; no catch-up burst (max 24 ticks in a wall second).

- **Cause 1, fixed here: the clearance component labels (k16.6.382).** Every region streamed in changes the mesh geometry, which dropped the labels, and the next refused-eligible query rebuilt them for the whole mesh on the tick thread: 155 ms mean at r 4 (40-65k polys), 283 ms at r 7, 461 ms at r 22 (max 603 ms), 90 builds in the 450 s walk, up to 3 in one tick (a 0.8 s tick). Now in `ksim_server` (`knav::setSlicedComponents(true)`; tests, ksim_run and replays keep the whole build, which is deterministic: with slices a query answers "no refusal" while pending, so routes depend on query history and ksim_world_follow traceReadOnly, which shares one navigator across two runs, diverged) the build runs in slices of 1024 polygons (~3 ms; a count, not wall time, so the sim stays deterministic) (`ClearanceCache::Build`), one slice per `refusedByComponents` call; until it is done the query answers "no refusal" and the search runs as before k16.6.382. Before / after (same walk, `KSIM_COMPONENTS_SLICE_POLYS=0` is the old whole build): slow ticks over 100 ms 104 -> 19, over 200 ms 34 -> 1, max tick 611 -> 285 ms; wire gaps of CHAR_DYNAMIC (20 Hz, 50 ms): p99 153 -> 84 ms, max 1160 -> 331 ms, gaps over 250 ms 35 -> 1, over 500 ms 9 -> 0. A wall second with fewer than 80 % of the ticks: 3 -> 0 (the one left is the run's end). Label memory is 4 bytes per polygon kept (8 while building): 0.2 MB per radius.
- **Cause 2, not fixed (k16.24): the stale-zone check on region entry.** `nav_stale` (hash recomputation of a zone against the shipped tile, bake 0 ms: patch cache hits) ran 40 times in the walk, 30-160 ms each (sum 3.7 s), always in the `residents` section, in bursts of 2-3 ticks (one zone per tick) at every region crossing; `nav_convert` (shipped tiles to the runtime mesh, 8-77 ms) 10 times. 18 stale checks at start (792 ms, tick 1). No `nav_bake`: the zone rebakes came from the cache. This is the client's 0.3-0.8 s zone-load stall less the client's own load; it moves to the k16.24 bake thread (the navigator is serial-step only, so a prefetch thread needs the stale check split from the tile swap).
- **Not ours at 1x:** the 0.37 s ticks the client saw at 71 s and 110 s are resident / task path searches (`residents` / `tasks` 100-280 ms with `path_q` 5-28, mostly Closed searches in Heft's 280-320 characters) and, before the fix, the label builds above (every one of them printed `nav_components=200x1`).
- Per-tick means, 450 s (JSON `section_mean_us`, first window): combat 7-8 ms and tasks 3-5 ms near Heft (300 characters), residents 3-4 ms, simlink (frame encode) 1.9 ms, ai_squad 1.7-2.3 ms; the window mean falls to 4.5 ms once the walker leaves Heft. The 5x split (item (3) of the bead) was not run.
- Run it: `KSIM_PROFILE_SLOW_MS=50 KSIM_SLOWQ=1 cpp/build-prof/OpenKenshi-server --listen 127.0.0.1:PORT --scenario-file scenarios/gate_heft.kscen 2> server.log`, then `python3 -I tools/walk_probe.py PORT 450 1 66588 -50636 wire.csv`.

## walkOrder re-plans by reason (kenshi-k16.8.39; branch ds/replan-measure2)

Dev counters (`dev::WalkStats`, `KSIM_DEV`): with `KSIM_WALK_STATS=1` a `WALKSTATS` line goes to stderr at the arena's end and every 600 sim s. A window re-plan (off the route window for 5 s) is split: **lost** = the walker was within `kOffPathReach` footprints of the WHOLE route (the window lost its place: routeNext ahead of or behind the walker), **real** = beyond that reach. Measured on ksim c42c91ec, Release, no behaviour change.

| run | walker-min | re-plans (/min) | window (lost / real) | stuck | blocked | abandoned | arrivals | arrival gap over 1.0 u |
|---|---|---|---|---|---|---|---|---|
| ksim_route_corners | 0 (plans only, never walks) | 0 | 0 | 0 | 0 | 0 | 0 | n/a |
| ksim_move_detour (16 short arenas) | ~0.4 | 0 | 0 | 0 | 0 | 0 | 16 | 0 (max 0.04) |
| ksim_path_offroute | 0.31 | 1 (3.2) | 1 (0 / 1) | 0 | 0 | 0 | 2 | 0 |
| ksim_town_soak | no walkOrder ticks, no output | | | | | | | |
| Heft, heft_run.sh 200 s wall at 5x | 256.5 | 36 (0.14) | 31 (0 / 31) | 0 | 5 | 0 | 1260 | 0 (max 1.00) |

- **Tracker drift is 0 % of re-plans** (0 of 32 window re-plans lost its place; 0 ahead, 0 behind). Every window re-plan had the walker more than 3 footprints from the whole route (pushed or displaced; path_offroute holds the walker off on purpose). Well under the 20 % bar: no corridor redesign or tracker fix is indicated by this data.
- Window re-plans are 86 % of the Heft total (31 of 36); the rest are 5 blocked (edge block aimed at the waypoint). No stuck re-plans, none failed, none abandoned.
- **Goal slack:** 1260 arrivals at Heft ended on average 0.13 u from the route's last point, max 1.00 (never over 1.0), so arrival fires. 3 walks ended by the stuck rule instead (held within 2 footprints, a crowd at the end): gap mean 2.75 u, max 4.75 u. The counters see the route's last point, not the order's goal, so the planner's up-to-1.5 u goal-to-last-point slack is not measured here.
- Caveat: Heft is one scenario and 200 s; the tests are tiny. Rerun with `KSIM_WALK_STATS=1` on a longer Heft or soak for more walker-minutes.

## Melee straight-walk gate (kenshi-k16.6.444)

Measured 2026-11-19/20 (dev VM, temporary timer around the walk, not kept): `ksim_fight` on the bar_crowd fixture, `--runs 2 --seed 4`, wall about 32 s on the merged ksim base (the first, pre-merge run: 23 s wall).
- Before (uncached): 147,499 `MeshNavigator::straightWalk` calls, 2.05 s = **6.4%** of the run (pre-merge run: 1.94 s of 23 s, 8%). Above the 2% line.
- After (`Ctx::walkMemo`, one walk per exact endpoint-coordinate pair per step): 103,553 calls (-30%), 1.69 s = **5.3%**. Behaviour unchanged by construction (a fighter that moved is a new key).
- Why only -30%: positions change every step, so only the repeats inside one step (inEngageRange, notDirect, approach) hit. Going lower needs a re-walk-only-when-moved-a-cell rule, which changes behaviour (the gate's rule is out of scope); left to ksim.

## Long travel at x5 (kenshi-k16.6.604, 2026-11-21)

From the user's report (kenshi-k16.2.477): x5 is unplayable on a long map move. Run: `tools/travel_run.sh` (KSIM_PROFILE build, `--scenario newgame --start 47924-rebirth.mod`, one move_to 26 ku east, set_speed 5, set_view / set_interest following the leader every 0.5 s, 300 s wall, under the worker lock, load 12.6-13.9). Raw data in the dev worker's scratch dir for k16.6.604 (`travel5x` = Guy with a Dog, which spawns at Bast, not the Hub; `hub5x` = Wanderer at the Hub).

- **Guy with a Dog, 27.4k ticks:** mean 5.6 ms, p50 3.3, p99 26.7, max 235; 15% of ticks over 10 ms.
  - The first 4.5k ticks (336 arena characters) average 14.1 ms, 72% over budget: 66 ticks/s delivered. That is the user's 30-65.
  - At 203 characters: 6.4 ms (98 t/s). At 88: 2.6-3.4 ms (100 t/s).
  - Phase A by section (µs/tick): combat 11156 (cs_update 8464, mirror 1056, separate 688, sense 476, env 384), ai_squad 901, tasks 610, interact 503, crime 376.
  - Whole run: combat 64.7%, ai_squad 14.1%, tasks 5.9%, interact 5.4%. Combat cost scales with arena count, about 33 µs per character per tick.
- **Region-crossing spikes** (about every 6 s of walking, always at tick % 20 == 0, ~16 ticks of catch-up each): stepPlayerAreas 28-123 ms, nav coverage 12-65 ms, residents 40-150 ms. 20 events on the Guy run, 12 on the Hub run.
- **Far-route stalls (Hub run):** single ticks of 320 ms (4 queries) and 739 ms (30 queries) in the goals section. ROUTEMEASURE: the clearance fit inside routeFrom is almost all of the router's 2.4 s over 16k routes. This matches the client's 250 ms stall plus catch-up (kenshi-k16.2.477.1).
- **Against Heft:** the same family (combat step, cs_update, the 1 Hz pass), but travel adds streaming-style spikes (areas / nav / residents) and far-route stalls. No zone bake ran in the tick (the navworld is prebaked).
- Fixes dispatched from this: the region promotion spread (kenshi-k16.6.614), the far-route budget and re-ask cap (kenshi-k16.6.307, once kenshi-k16.6.305's routeFrom node counts are in). The per-system rates for the arena cost go through the Fable review kenshi-k16.6.607 and design kenshi-k16.6.606. The Wanderer stopped after ~16 ku with 30 blocked routes: kenshi-k16.6.615.

### Region promotion spread (kenshi-k16.6.614, 2026-11-21)

Same recipe (Guy with a Dog, 26 ku east, x5, 300 s, KSIM_PROFILE build). The areas column is `stepPlayerAreas` + `syncNavCoverage` (stepPlayerAreas itself is ~0: the cost is the zone load); the nav column is `syncLineOfSight` (hull trees of a newly Full region), not the navmesh.
- **Before** (box load 13): areas max 123 ms (11 ticks over 10 ms), LOS 78 (9), residents 28 (15). **After** (load 17-21, so everything about 1.5x slower than the baseline box): areas max 22.5 ms (a zone load is spread to one a tick; 61 ticks over 10 ms, none over 23), LOS 37.5 (7 ticks over 10 ms), residents 19 (7).
- What changed: `syncNavCoverage(spread)` loads one zone per tick, nearest to the player first, drops first and alone, furnishes the doors and containers on the tick after the load, and prefetches the zones ahead from the heading; `syncLineOfSight(spread)` builds one region's hull trees a tick; `stepResidents` builds at most `kPromotesPerTick` squads / `kPromoteMembersPerTick` members a tick from a queue the 1 Hz pass fills, and a squad whose ground is not on the mesh yet waits (a member with no ground is skipped for good).
- **Left:** one zone load is ~10 ms (read 8, apply 2) and its furnish ~15 ms; one region's hull trees 15-45 ms; a promoted squad up to ~12 ms. The mean tick with an arena of 300+ characters is still 14-23 ms on this box, so the whole tick stays over 20 ms: that is the combat cost (k16.6.607), not the crossings. Next: build hull trees and read zones on a worker thread and hand the finished result to the tick (not built here).

## Scheduling measurements (kenshi-k16.6.305; SIM_SCHEDULING section 8 step 3)

Measurement only. Gate for SIM_SCHEDULING A: is the **think** share of `cs_update` above ~0.5 ms; are the long routes failed or exhaustive; is Heft one interaction component; how many components on the long travel, at what union-find cost.

**Tooling** (KSIM_PROFILE build only; nothing in a normal build):
- The `KSIM_PROFILE_TICKS` CSV gained probe columns (`kProbeNames` in `core/profile.hpp`): `think_us` (every `pickTarget` call the update loop makes; `scan_us` is its `scanCandidates` part; `*_calls`), `upd_us` (the whole update loop), `upd_route_us` (`routeFrom` time inside it) and the island columns. The combat arena steps **twice per tick** (0.025 s substeps): `isl_*`, `tgt_*`, `upd_members`, `isl_active` are summed over the tick's steps (column `steps`; divide by it); the `*_us` times are per tick. **Think** = `pickTarget`; **act** = the rest of the update loop minus all route time (state transitions, incoming check, steering, swings); routes are reported apart. The fight-goal pick runs outside the loop: it is the existing `sub_wt_fight_goals` column.
- `KSIM_ROUTELOG=<path>`: one CSV row per `routeFrom` call (`knav/route_measure.hpp`): tick, the profiler stretch it ran in, a caller tag (1 combat `pathLength`, 2 combat `replan`, 3 blocked-target test, 0 any other) with a member key (combat callers only), wall us, found, planned, radius, from, to, and the nodes of each stage: the plain probe `findPath` (`probe_nodes`; at radius 0 the whole search), the clearance A* first pass (`clear_nodes`), the goal-exempt second clearance pass (`again_nodes`), `searches` (refit and own-polygon plans), `find_paths`.
- Islands (`ksim/dev/island_probe.hpp`, header only): union-find on a 1500 u cell grid over the usable characters of the arena (range = the base sight, race mult 1, daylight 1; the indoor 500 and race multipliers are ignored), run after the update loop; a second union-find joins each member with its `target` ("fight" islands). An island's share of the loop = the sum of its members' own update times. The probe's own cost is `isl_uf_us`. Test: `ksim_profile` (the union-find cases).
- Summary: `[TICK_MIN= TICK_MAX=] tools/probe_summary.py NAME.ticks.csv [NAME.routes.csv]`. Recipes, each under the worker lock with `nice -n 10` and `KSIM_ROUTELOG` exported: `tools/travel_run.sh travel5 47321 300 5 26000` (kenshi-k16.6.604's recipe, reused as is: new game "Guy with a Dog", one move 26,000 u east, view and interest following the leader), `tools/heft_run.sh heft5 47322 300 5`, `tools/heft_run.sh heft1 47323 300 1`. 300 s wall each (not 10 min); the first 100 ticks are dropped.
- Raw files: `{travel5,heft5,heft1}.{ticks.csv,routes.csv,meta,probe.txt}` in the dev worker's scratch dir for k16.6.305.
- **Load**: the box was busy throughout (load average 16-19 on 16 cores at every start and end, see `.meta`): the times are inflated by an unknown 10-30%. The probe adds timers (about 40 ns per `pickTarget`, member update and route) and the union-find (0.05-0.2 ms a tick); Heft 1x read 12.3 ms mean against 10.0 ms in the k16.6.230 runs, part probe and part load.
- **The travel run has two phases**: the leader reaches the goal at 118 s wall (tick 82,381; 8,718 ticks, 436 sim s), then idles 180 s. The travel column below is the **walking phase** (`TICK_MAX=82400`); the idle phase (18,975 ticks) reads tick mean 3.3 ms, think 26 us, act 1.2 ms, and is not a travel number.

### (1) Tick

| | travel x5 (walking) | Heft x5 | Heft x1 |
|---|---|---|---|
| ticks delivered (target) | 8,718 in ~110 s = **80 /s of 100** | 28,515 in 300 s = 95 /s of 100 | 5,901 = 19.7 /s of 20 |
| tick mean / p50 / p99 / max (ms) | 10.5 / 9.2 / 42.3 / 134 | 8.6 / 7.8 / 23.1 / 274 | 12.3 / 10.6 / 38.2 / 221 |
| encode (`simlink`) mean / p99 (ms) | 0.69 / 10.3 | 0.96 / 13.8 | 2.67 / 14.6 |
| combat section mean (ms) | 8.2 | 5.5 | 7.8 |
| server CPU of the run's wall | whole run 170 s of 300 | 271 s of 300 | 85 s of 300 |

The x5 walk is the slower of the two x5 runs: 80 ticks/s against 95, tick mean 10.5 ms (the 10 ms budget), because zones stream in as the leader moves (the arena holds 250 characters on average, up to 450) and the crowd differs from Heft's steady 280-320.

### (2) `cs_update`: think vs act (per tick, us; two combat steps per tick)

| | travel x5 (walking) | Heft x5 | Heft x1 |
|---|---|---|---|
| `cs_update` loop mean / p99 | 6,222 / 33,835 | 3,022 / 7,420 | 4,651 / 10,482 |
| **think** (`pickTarget`) mean / p99 | **123 / 334** | **1,039 / 2,451** | **1,354 / 4,004** |
| of it `scanCandidates` | 89 / 256 | 834 / 2,121 | 1,061 / 3,674 |
| **act** (route time removed) mean / p99 | 5,264 / 32,291 | 1,872 / 5,357 | 3,085 / 7,701 |
| routes inside the loop mean / p99 | 835 / 6,237 | 111 / 1,169 | 213 / 2,318 |
| `pickTarget` calls per tick (scans) | 44 (23) | 234 (220) | 242 (219) |
| think share of the loop | 2.0% | 34% | 29% |

**Reading:** at Heft the think part is 1.0-1.35 ms per tick (p99 2.5-4.0 ms), **above the 0.5 ms gate** by 2-2.7x, and 80% of it is `scanCandidates` (about 4 us a scan, 220 scans per tick). On the travel it is 0.12 ms (p99 0.33), **under the gate** by 4x: the cost there is act (85% of the loop, p99 32 ms in the zone-stream ticks) and routes (13%). So A pays at Heft (a crowd scene) and does nothing for the x5 walk. `think_route_us` is 0: `pickTarget` only reads the path cache.

### (3) Routes

| | travel x5 (walking) | Heft x5 | Heft x1 |
|---|---|---|---|
| `routeFrom` calls / per sim second | 41,039 / 94 | 214,880 / 151 | 19,024 / 64 |
| wall ms mean / p99 / max; total | 0.20 / 1.5 / 27; 8.1 s of ~110 s | 0.09 / 0.6 / 257; 18.9 s of 300 s | 0.41 / 5.4 / 203; 7.7 s of 300 s |
| not found (n; wall) | 1,049 (1.1 s) | 74,173 (2.6 s) | 15 (0.7 s) |
| over 5 ms: n (share of route time) | 52 (5.9%) | 203 (18.6%) | 225 (30.7%) |
| the slow ones: found / not found | 17 / 35 | 143 / 60 | 211 / 14 |
| slow: probe nodes mean; clearance nodes mean | 1,646; 1,674 | 8,722; 7,564 | 4,959; 3,243 |
| radius-0 retries (calls at radius 0 after a miss; wall) | 946 (0.2 s) | 444 (0.1 s) | 40 |
| routes per tick (ticks with any) mean / p99 / max | 6.8 / 24 / 44 | 7.8 / 56 / 66 | 3.9 / 15 / 39 |

Nodes per call (all calls, mean): probe 33 / 22 / 152, first clearance pass 35 / 10 / 51, second (goal-exempt) pass 12 / 0.6 / 0.7; the largest probe closes the 32,768-node pool (65,772 over two searches at Heft). Calls by the stage they ran in (calls, wall): travel: `cs_update` 36,799 (7.3 s), `world_squads` 1,302 (0.5 s); Heft x5: `routines` 20,627 (8.3 s), `wt_in_hand` 161,412 (7.0 s), `cs_update` 27,483 (3.2 s); Heft x1: `wt_in_hand` 5,079 (4.0 s), `routines` 4,255 (2.3 s), `cs_update` 8,216 (1.3 s).

**Reading:**
- **On the travel the routes are combat re-plans**: 90% of the calls and 7.3 of 8.1 s are inside `cs_update`; 25,257 are `replan` (tag 2) from 433 members, one member 1,064 times, up to 2.4 per sim second per member, runs of 158 consecutive-tick queries; `pathLength` (tag 1, the cached pair length) is mild (175 members, 0.44 per member-second at most). The slow ones (52, 6% of route time) are **not found**: 34 of 52 are failed clearance A* runs (probe under the pool, `plainFailed` 0: the plain search found a way and the clearance search refused its portals), 8 ms each, 0.27 s together. About 3% of the calls miss (1,049).
- **At Heft the 30-40 ms routes are found, not failed**: 211 of 225 (1x), 143 of 203 (5x) slow routes are found, after a probe that closed 5-9 k nodes and a clearance A* of 3-8 k (up to 58 k). The failed slow ones are exhaustive: 37 of 60 at Heft x5 (2.1 s, 57 ms each) hit the 32,768-node pool in the probe then ran a clearance search, all from `routines` (the scorer's retry).
- **Re-queries, Heft**: combat `replan` at Heft x5 has one member re-planning 5,829 times in 1,426 sim s (4.1 per sim second at the worst; a run of 2,647 consecutive ticks). The volume is not combat: **161,412 of the 214,880 calls at Heft x5 come from `wt_in_hand`** (the tasks-in-hand member loop; `planned` routes, clearance 22), **73,877 of them not found**, on 3,826 ticks with up to 40 each, the same goals again (one goal cell asked 1,429 times). Each is cheap (0.035 ms; 2.6 s of 300 s) but it is 34% of the calls and all of the 74 k failures; at Heft x1 the same loop has 15 failures. So at 5x a task in hand re-asks an unreachable goal every tick: the "negative cache / no per-tick re-query" case of 5E.

### (4) Interaction islands (per combat step; sight range 1500 u over usable characters)

| | travel x5 (walking) | Heft x5 | Heft x1 |
|---|---|---|---|
| usable characters in the arena, mean / p99 | 255 / 452 | 276 / 320 | 292 / 324 |
| **components** mean / p99 / max | 14.9 / 20 / 20 | 7.7 / 9 / 9 | 7.8 / 9 / 9 |
| largest component mean / p99 / max | 52 / 72 / 81 | 231 / 242 / 244 | 234 / 244 / 244 |
| second largest mean | 43 | 15 | 21 |
| singletons mean | 2.2 | 0.2 | 0 |
| largest component's share of the update loop (own member times) mean / p99 | 0.28 / 0.86 | 0.65 / 0.81 | 0.61 / 0.84 |
| union-find cost per tick (both steps) mean / p99 / max (us) | 76 / 153 / 2,927 | 157 / 251 / 3,046 | 215 / 775 / 4,705 |
| fight islands (target links): non-singleton components mean; largest mean / p99 | 5.9; 6.0 / 26 | 1.8; 5.6 / 21 | 3.3; 8.8 / 23 |

**Reading:**
- **Heft is not one component, but one giant one**: 8 islands, the largest holding about 80% of the usable characters (231-234 of 276-292), the next 15-21. The sight graph at 1500 u is too connected to cut Heft's think into independent islands; the largest holds 61-65% of the loop's time.
- **The travel is about 15 islands**, the largest 52 (p99 72, max 81) of 255, 28% of the loop on average, up to 86-92% in the worst ticks.
- **The fights are small islands** (target links): 2-6 non-singleton groups of 6-9 members on average, at most 27. A "fighting members only" think (members in a fight or with an enemy in sight) is the one with room; the sight islands are not.
- **The union-find is cheap**: 76-215 us a tick (both steps), 0.7% of the travel tick and 1.8% of the Heft tick; the occasional ms-scale max is a cold grid. Cheap enough to run every tick if a design wants it.

### What this says about the design (for SIM_SCHEDULING 8, k16.6.607, k16.6.606)
- Gate (think > 0.5 ms): **yes at Heft (1.0-1.35 ms), no on the travel (0.12 ms)**. A is a crowd-scene optimisation and does not touch the x5 walk.
- The x5 walk (80 ticks/s of 100, tick mean 10.5 ms) is **act (5.3 ms) and combat re-plans (0.84 ms, p99 6.2 ms) inside `cs_update`**, with zones streaming in; the failed searches are 34 clearance floods of 8 ms.
- At Heft x5 the biggest single waste is `wt_in_hand`'s 74 k failed `planned` re-queries (E.1/E.2 target) and the 37 exhaustive `routines` retries (57 ms each).
- Not measured: no quiet box (load 16-19 throughout); the walkers, goal runner and scorer outside `cs_update` have no island timing; the fight-goal pick is the existing `sub_wt_fight_goals`.

## Arena member steps by update branch and distance band (kenshi-k16.6.631, k16.6.607 item 0)

KSIM_PROFILE-only probe columns in the `KSIM_PROFILE_TICKS` CSV (`b_<branch>_<band>`, `pick_<state>` in `kProbeNames`; a normal build has none of the code). Per combat step each member's `Ctx::update` is counted by the branch it took: `dead` (also the tick it dies), `down` (KO / crawler), `walk` (move order), `follow`, `drive` (direct drive and drive turn), `idle` (state Hub or Done on entry to the state machine), `fight` (Attack, Defend, Between, Circle, Wait, Approach on entry, and a ranged shot is not counted separately), `other` (stumble, recovery hold, stealth KO, mounted gunner, ranged task). Band = distance from the nearest living player-controlled character (the view focus is not used): `n` 0-2,000 u, `m` 2,000-3,250 u, `f` beyond. `pick_*` counts the outer `pickTarget` calls by the caller's state. All are summed over the tick's two combat steps (so ~2x the arena size); `tools/probe_summary.py` prints them.

Runs: `tools/travel_run.sh travel P 300 5 26000` (Guy with a Dog, 26 ku east, x5, 300 s) and `tools/heft_run.sh heft P 200 5`, one at a time under the worker lock, nice 10, at box load ~18-25 (counts do not depend on load; wall times do). Output kept in the dev worker's scratch dir for kenshi-k16.6.631. The travel summary covers the whole run (ticks 100+ after the first 100 are skipped), not only the walk.

| | travel (26,165 ticks) | Heft x5 (17,014 ticks) |
|---|---|---|
| member updates per tick (2 steps) | 296.8 (~148 arena members) | 619.4 (~310) |
| **near share (0-2,000 u)** | **16.9 %** | **75.5 %** |
| mid 2,000-3,250 u / far | 11.3 % / 71.8 % | 3.0 % / 21.5 % |
| follow | 216.1 (72.8 %; near 28.8, mid 26.0, far 161.3) | 74.2 (12.0 %) |
| idle Hub/Done | 21.2 (7.1 %) | 433.3 (69.9 %; near 408.7) |
| down | 22.7 | 51.4 |
| walk | 12.8 | 31.4 (near 22.5) |
| fight | 15.2 (near 7.8) | 17.2 (far 15.8) |
| dead / other / drive | 3.8 / 5.2 / 0 | 8.8 / 3.2 / 0 |

`pickTarget` calls per tick by state (mean; p99; max):

| state | travel | Heft |
|---|---|---|
| Hub | 11.4 (35; 166) | **218.8 (225; 234)** |
| Approach | 5.3 | 8.4 |
| Circle / Wait / Between | 1.9 / 1.5 / 1.2 | 2.7 / 1.6 / 2.4 |
| Attack, Defend, Stumble, Done | 0 | 0 |

Findings:
- **The walk is mostly far followers**: 72 % of the arena's updates are `follow` and 72 % are beyond 3,250 u of any player character; only 17 % are near. Heft is the opposite: 70 % of updates are idle Hub/Done members and 75 % are near (the whole bar crowd).
- **Heft's ~234 picks a tick are the idle Hub -> Done flip.** Idle updates are 433 per tick (two steps) and Hub picks are 219: one pick per idle member per two steps. `pick_done` is 0 and the Done entry runs `setState(Hub)` with no pick, so an idle member alternates Hub (pick, nothing found, Done) and Done (-> Hub) every other step. The p99 of 225 and max of 234 match the idle population, so it is a steady per-member cost, not a burst.
- The pick is not the Heft cost on the walk: Hub picks there are 11 a tick.
