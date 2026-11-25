# Sim scheduling: staggered decisions, cheap execution (design, kenshi-k16.6.302)

Status: **proposed** (ksim, 2026-11-16). Reviewed once by a Fable sub-agent (section 9); the design below already
takes its findings. Work items: section 8.

Contents: 1 Why · 2 What runs per tick today · 3 What the original does · 4 Rules · 5 Design (O output cadence,
A one think slot, B wake-ups, C execution and its detail level, D the 1 Hz passes, E route queries) · 6 Threading ·
7 Tests, saves and determinism · 8 Order of work · 9 Review

## 1. Why

User 2026-11-15: "Not every squad needs to reevaluate their goals multiple times a second. One npc might decide
attacking is a great idea, and then that decision could stick for (?? 100ms? 250ms? dynamic ms?)... so not every NPC
needs to make a decision in the same tick, it can be staggered. As long as the 'carry out the current orders' work is
cheap enough." And on threading: could squads too far apart to interact form "NPC islands", each a sim thread? "is that
even worth it?"

The numbers (PROFILING.md "Heft under live load", "Verdict"): 1x holds. 5x does not: the steady tick plus the encode is
about 11.2 ms against a 10 ms budget, and the 1 Hz pass and the routine scorer land 25-100 ms spikes on the same ticks.
The combat arena (about 305 Full-detail characters, every tick) is about half of the tick; one `routeFrom` of 37+ ms
inside one member's update makes the isolated 43-73 ms combat ticks. All of these were measured on a loaded box; a
quiet-box baseline is still missing (later pairs, k16.6.279, show a 5-7 ms tick mean: the p99 may be the real miss).

## 2. What runs per tick today

| work | cadence | cost at Heft (5x, PROFILING) | kind |
|---|---|---|---|
| `CombatArena::step`: every member's `update` (state machine, `pickTarget` from the state handlers, `incoming` defence check, steering, swings), `correct`, `separate` | every tick, all ~305 | step 2.7-4 ms; `cs_update` ~1.0-1.6 ms after k16.6.263 | decide + act mixed in one call |
| `senseAll`: the sensor scan | **already budgeted**: each member once per `scanInterval(n)` = n / 480 s, clamped 0.5-1.5 s, staggered by dice key (`perception.hpp` `kScanBudgetPerSecond`) | inside step | decide (perception) |
| mirror of every `Fighter` into the ECS | every tick | 0.85 ms | copy |
| fight-goals pass, world-task prelude | every tick | 0.07 + 0.05 ms (after k16.6.233) | decide |
| routine scorer (`stepRoutines`) | every 20 ticks per member (`kScoreIntervalTicks`), **in lockstep** at two of three sites (`routine_scorer.cpp` 2706, 3597 lack the `id % 20` offset 2570 has) | 0.85 ms mean, 40-78 ms spikes | decide |
| `AiSystem::squadStep`, world squads, carry prep, the world-task goal selector | 1 Hz, **all squads in tick % 20 == 0** | +17 ms on that tick | decide |
| dialogue evaluations | budget: 64 per tick (`kEvalsPerTick`, DIALOGUE.md 2.2) | small | decide, already budgeted |
| faction / strategic | about hourly | small | decide |
| encode (`Session::sendTick`) | **after every sim tick, catch-up ticks included** (`simlink_server.cpp` run loop) | 2.8 ms (p99 14) | output |

So: the spikes are the lockstep timers and the long routes; the steady cost is the combat `update` and, at 5x, the
encode, which runs 100 times a second for a client that draws at most its frame rate.

## 3. What the original does (PERCEPTION_STATIC.md section 6, SQUAD_FOLLOW_LIVE.md "Update tier")

- **A count budget, round robin:** per frame at most **8 characters get a full tick** (`AI::update` with the sensor),
  rotating by a cursor over the active list (`FUN_140786e30`); each gets the **accumulated dt**, so nothing is lost.
  With N loaded characters each thinks about every ceil(N / 8) frames (300 characters at 60 fps: about 0.6 s).
- **Execution every frame for the important ones:** a character flagged `+0xe4` (always for the player's faction and
  platoon leaders; otherwise in view, within 3,250 u of the camera node) gets its per-frame update (vf `+0xe0`) every
  frame; the others get a few prep slots per frame, so far NPCs **move in jumps** (every 45-100 frames, clog_follow1/2).
  What vf `+0xe0` runs (the combat class AI's swing / block reactions, or only movement) is **not yet known**: oracle
  question, section 8.
- The sensor runs inside the same full tick as `AI::update` (PERCEPTION_STATIC TL;DR 1 and 8): seeing and re-deciding
  share one slot, so the lag from "enters sight" to "reacts" is one rotation, not two.
- Reactions: one seen character per post-update call; a per-pair "sighted" throttle of about 2 s.

The user's idea is the original's own structure: decisions are budgeted by count and stick between thinks; execution
is cheap and runs every frame where someone can see it. Our `senseAll` already follows the first half for the sensor.

## 4. Rules for the design

1. **Budgets count work, never time.** A tick's work must not depend on machine speed or thread count
   (ARCHITECTURE "Threading"; replays and debugging). So: N decisions per tick, a node count per route query, never
   "until 2 ms are spent". Game speed is a sim input, so a count scaled by speed is still deterministic.
2. **Decide and act are separate.** A decision (target choice from the sensor, goal, routine pick) persists until the
   next think or a wake; acting on it (steer, state transitions, swing timers, blocks, animation state) runs every
   tick for the characters that are seen.
3. **A decision carries the elapsed time.** A think gets the ticks since its last think (the original's accumulated
   dt), so timers that a decision reads (alert decay, "waited longest") stay right at any cadence.
4. **Events wake, the budget orders.** A hit, the target lost or down, an order: the character thinks on the next tick
   ahead of the round robin.
5. **No terminal state** (core AI rule): a character never waits on a think or a route that cannot come; every wait is
   bounded (sections 5A, 5E).
6. **One scheduler.** The sensor, the target re-evaluation and the routine scorer share one cursor, not three staggers.

## 5. Design

### O. Output cadence (cheapest first)

At speed > 1 the run loop encodes a snapshot after every catch-up tick: at 5x, 100 snapshots a second, 2.8 ms each.
**One snapshot per catch-up batch** (one per wall-clock pass of the run loop) carrying the batch's events removes about
2.2 ms per tick at 5x, changes no sim result and needs no thread. It is a wire-cadence change (SIMLINK): the client lead
approves the layout first (events spanning several ticks, interpolation over a bigger step). **Done (kenshi-k16.6.304):** client
lead's variant: at most floor(speed) ticks per snapshot (the view runs 2 x speed ticks behind), events each stamped with their
own tick; wire in formats/SIMLINK.md "Cadence" under SNAPSHOT_BEGIN / SNAPSHOT_END.

### A. One think slot (sensor + re-evaluation + scorer)

- **The think slot is the scan.** `senseAll` already gives each member one scan per `scanInterval(n)` (n / 480 s,
  0.5-1.5 s), the original's 8-per-frame budget. A think is that scan **plus** the sensor-driven re-evaluation that
  follows it in the same slot: the target / threat choice from the new sight list (the part of `pickTarget` that
  weighs candidates), the fight-goal pick and the routine scorer's pass for that member.
- **What stays in act, every tick:** the state machine's transitions, including the picks the handlers make at once
  per the exe (a fighting member's hub re-pick, combat.cpp around 2338, "dt4"; an idle member is gated, see "Idle re-pick gate" before section 9), the `incoming` check that enters defence, steering and
  swing timers. A swing aimed at a member is answered on the tick, never on the think: blocks must not lag 0.64 s. If
  the oracle answer (section 8) shows the exe's combat class AI runs in the 8-per-frame tick, act shrinks to match.
- **Budget:** keep the scan's own count budget (480 per game second, K = 24 per tick at 1x) as the think budget; the
  scan's `senseClock` becomes the one cursor (round robin by dice-key order, oldest-due first), so seeing and
  re-deciding are one rotation, as in the exe. **Speed:** K is per tick by default (decisions per game second constant;
  5x pays per tick what 1x pays). An option scales K by 1 / speed (the exe's budget is per frame, so its thinks per game
  second drop at 5x); a setting, measured both ways.
- **The routine scorer** joins the slot but keeps a **minimum period** of 20 ticks per member (today's
  `kScoreIntervalTicks`): under the scan's 13-tick rotation alone it would score 1.5x as often as now. A member whose
  think comes early skips the scorer pass until its period is up. The lockstep spikes are fixed first by the `id % 20`
  offset at the two sites that lack it (k16.6.232's sibling, section 8).
- **Bound:** every member thinks at least once per `kScanMax` (1.5 s) and the rotation (ceil(N / K) ticks), plus one tick
  of wake spill. A floor for engaged fighters is an option only if fight-feel tests read sluggish.

### B. Wake-ups

| event | who wakes |
|---|---|
| hit taken, KO / down, a limb severed | the victim; its squad's leader |
| its target dies, is downed, leaves sight or goes out of reach | the attacker |
| an order (player, squad leader, dialogue action) | the ordered member |
| a route blocked / a door shut on its route; a route result arriving (5E) | the walker |

New hostiles in sight are not a separate wake: the scan finds them, and the scan is the think. A wake marks the member
for the next tick; at most once per tick; wakes count against K and spill to the next tick oldest first (a big brawl
slows the thinks, never the tick).

### C. Execution and its detail level

- **Every tick:** the act half for members that matter to a player: the player's faction, anyone within the
  interest radius of a player's view (the original's 3,250 u), and anyone fighting one of those.
- **The others act every 2-4 ticks** with the accumulated dt (the original's far NPCs moving in jumps). This changes
  results (the state hash; arrival ticks) but follows the original, so it is ours to decide (k16.6.236); it lands
  **after** A, and only if 5x still misses once O, A and D are measured.
- **The mirror** (0.85 ms) shrinks with C: a member that did not act writes nothing; then a dirty bit from act is the
  natural place for the in-place write the k16.6.233 trial lacked.

### D. The 1 Hz passes

`squadStep`, world squads, carry prep and the world-task selector run one slice per tick (squad id % 20), each squad
keeping its 1 s period (kenshi-k16.6.320, from k16.6.232). Not replaced by A (squads are not arena members). The rule
that makes a load equal a continuous run:

- **A slice only decides.** Per-squad work reads shared state and writes the squad's own (its board, its members); it
  never builds state that other squads' slices read.
- **Shared derived state is built once a second, whole, before any slice reads it** (and once after a load or a start):
  the occupancy map `occupiers_` is rebuilt from every board at the second's tick by `stepWorldTasks`, not by the squads'
  slices (`occupiersBuilt_`). It reads each board as its squad's last slice left it, so a newly started occupation shows
  up at most one second later than when one pass did both.
- **A save holds the arena's truth.** The ECS `Fighter` is mirrored by `stepCombat`, mid-tick; the writers after it
  (`stepWorldTasks`: the follow orders; the routines) change the arena only. A save or a hash taken between ticks used to
  hold the Fighter without them and a load lost them. That hole is not new, but with one pass per second it needed a save
  at tick % 20 == 0 to show; with slices every tick has a writer. `save` / `saveGame` / `stateHash` now encode the arena's
  Fighters (`Simulation::ArenaMirror`: copied in, old values put back; the next tick reads what it read before, and
  nothing is paid per tick). It is its own commit on `ds/stagger-mirror` (from master) so it can land alone.

What a slice does NOT make equal (kenshi-k16.6.320 rework 1, measured with a same-binary switch that put every slice
back on tick % 20 == 0): a squad's second moves by up to 19 ticks, and everything that follows keys on the tick, so a
fight-dependent scenario goes down another branch. Named for the two tests that still fail sliced and pass unsliced:

- `ksim_raid_takeover_scenario` (Heft variant): the assault package (59293, PassingByTownAssault) finishes, and the
  board switches to the occupy package (96451), only when at least two members have a target at a once-a-second sample
  (`Situation::engagedMembers`, sticky once seen). The members all take RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE (216) at
  their first score, whose search point is a die keyed by (seed, member, tick). A tick earlier or later sends them to
  other points: sliced, the five members ran 300-1,700 u away from the leader while it fought the downed player's
  defender for 15 s, so one member was engaged at every sample and the package stayed on 59293 (status Staying) to the
  end of the run; unsliced, a member passed within 128 u of the fight and two were engaged at the 76,720th tick. No
  state is read from another squad's slice; the leader, the route (`along` 120-135 of 146 u) and the board are the same
  until the fight. The scenario depends on the members' random walk meeting the fight, in any scheduling.
- `ksim_raid_goals` 270 ("no member took ASSAULT_FORTIFICATIONS_PREFER_GATES"): NOT chaos (a scripted fixture: squad placed on the gate, package applied by `changeAI`, gate locked by command). A scratch print in the 270 pick showed the squad's selector ran ONCE (tick 26810, eight members, `fortifications` 112, `shut` 0: `doors->bashFor(faction, gate)` found no refusing gate) and never again in the 4,000 ticks. Two things to check, not yet done: why `bashFor` sees the gate open at that tick (the door system's `sync` runs at the second's tick only, the lock command was enqueued 2 ticks before the squad's slice), and why the squad's selector does not run again (its slice tick, or a held Routine). Verify: print `bashFor` and the squad's `stepSquad` ticks with slices on / off.

Two more, fixed in the tests (kenshi-k16.6.374), same cause, a fight-dependent branch moved by the phase shift:

- `ksim_token_fights`: the "same pair fights at most once" and "one faction does not fight" checks counted every `offscreen fight` line; sliced, a third
  hostile token (10332 vs 11539) wanders into the winner's reach in the window and fights as another pair. The pair clock and `stepTokenFights` (still at
  tick % 20 == 0) are unchanged; the checks now count the pair's own lines.
- `ksim_slave_obedience`: the walk of the slavers' caravan (about 12,000 ticks) outlasts the 8,000-tick window when the captive is still down at the cuff,
  and sliced that is seeds 1, 706960716, 1151889715 and most of 2..9. Seeds 4 and 5 are freed in the window and graft with and without the row; the test
  now runs those (`KSIM_SEEDS=1,2,3` surveys others).

So these two pass or fail by where the dice land after a phase shift, not by a stale read; the sim lead decides whether
the test is made robust (a scripted member in reach, or the assault package's engaged rule sampled every tick) or pinned.

### E. Route queries

`routeFrom` is not one Detour query: a probe `findPath`, our clearance A* and a second `findPath` (`navmesh.cpp`
around 843-919, 1453, 1504), and `routeForBody` retries at radius 0 (combat.cpp around 603). Detour's sliced API covers
none of the clearance search. So, in order:

1. **Measure first:** per `routeFrom`, nodes expanded per stage and found / not found. A 37 ms route is likely a
   failed or near-exhaustive search; a reachability pre-check (the island ids the k16.7.39 test computes) or a negative
   cache (from-poly, to-poly, mesh revision) may remove it with no slicing.
2. **Repeat queries:** PROFILING k16.6.235 saw a member re-querying 4-7 ms routes for tens of ticks. That costs more
   than the rare spike: a re-query only on a wake (route blocked, target moved past a threshold), never every tick.
3. **Only then, slicing:** each request gets an inline node allowance (most finish in it, as today); only the overflow
   is sliced, a node budget per tick over all overflow requests, no head-of-line blocking (round robin among them). The
   requester keeps its old route or a straight-line carrot toward the goal, and the wait is bounded (after B ticks the
   best partial route is taken). A query restarts if the mesh revision changes mid-query. Save: the request and the
   nodes consumed are saved, and a load replays it to the same point (in-flight Detour state cannot be serialised).

## 6. Threading

- **First, no threads:** O, A, D and E cut the spikes and the steady cost without changing the thread model.
- **The encode** (2.8 ms) on its own thread is the next real parallelism: it needs the tick to hand over an immutable
  view (the ECS is written by the next tick). After O, and only if 5x still misses; it changes no sim result.
- **Perception in parallel:** `senseAll` runs before anyone moves and is read-only over that state; a parallel scan into
  per-member buffers merged in id order gives identical results at any thread count. **This does not extend to
  `update`**, which runs in place in member order and reads other members' current-tick positions: never parallelise
  update on this reasoning.
- **NPC islands:** not worth it for one player. Heft's 306 characters are inside one interaction range of each other,
  so islands do not split the case that misses 5x; the in-place `update` would need components anyway. One one-off
  measurement (union characters within the longest interaction range, log the component sizes at Heft and on a real
  save) settles it; then islands wait for multiplayer, where separate players' areas are the natural units
  (ARCHITECTURE "Per level": one area per player; the regions design is the general form).

## 7. Tests, saves and determinism

- The state hash and tick-pinned tests move once per step (O none; A, C, D, E each); each repin says why the new value
  is right.
- **Save state:** the think cursor, the wake queue with its ages, each member's last-think and last-score ticks, and the
  route requests in flight (5E) are saved: think order changes results (update runs in place in member order), so the
  cursor is state. A save bump per step that adds any of these, recorded in `formats/RUNTIME_SAVE.md`.
- `think_budget = 0` means unlimited (every member thinks every tick): a configuration for A/B comparison and for a
  test that isolates a behaviour from the cadence, not a dev-only path.
- A test of the bound: N members, budget K, no wakes: every member thinks within the bound; with a storm of wakes the
  tick's think count never exceeds K.
- Per-member RNG streams (`f.rng`) keep draws independent of think order across members; keep it so.
- Fight-feel tests (bar brawl, arrest, capture chains) must keep their outcomes: a decision lag of up to 0.6 s is the
  original's, so an outcome that needs a faster reaction is checked against the exe first.

## 8. Order of work

1. k16.6.320 lands (the 1 Hz spread; a load must equal a continuous run). The scorer timer stagger (k16.6.232) is parked behind it:
   alone it did not cut the spikes (ticks over 20 ms 0.58% -> 0.98% on a loaded box; the 1 Hz pass was 281 of 349), and it resumes only
   if 320's quiet-box numbers still show the scorer tick.
2. O: one snapshot per catch-up batch at speed > 1 (wire proposal to the client lead first).
3. Measure, on a quiet box: tick mean and p99 at 1x and 5x after 1-2; think vs act share inside `cs_update`; the route
   node counts and found / not found (5E.1); the one-off islands count (section 6). **Gate:** if the think share is
   under about 0.5 ms, A is not worth its refactor of `combat.cpp`; then E and C are the work. Measured (k16.6.305, loaded box,
   PROFILING.md "Scheduling measurements"): think 1.0-1.35 ms at Heft (above the gate), 0.12 ms on the long x5 walk (below).
4. E.1-E.2 (route pre-check / negative cache, no per-tick re-queries); E.3 only if the spikes remain.
5. A and B (one think slot, the scorer's minimum period, wakes), after the oracle's answer on where the exe's combat
   class AI runs.
6. Re-measure 5x; then C (k16.6.236) and the encode thread on those numbers; parallel perception last.

### Idle re-pick gate (k16.6.632)

An idle member (state Done) sleeps instead of flipping Done -> Hub -> pickTarget every tick. It re-picks (Hub, same step) when: its own
sensor scan ran this step (`senseAt == time`), it holds an order, a wake set it to Hub (hit, `markHostile`, being targeted, a squad-mate
engaged; the wake is the persisted state), or it has spent 3-5 steps in Done (0.15-0.25 s, the count by dice key, times the cadence
factor of the scan interval at game speed above 1: not for a player character or a fighter). Sight-to-react is at most one scan interval
+ 0.25 s. No faction or scenario special case.

## 9. Review

One Fable sub-agent review, authorised by the user on kenshi-k16.6.302, of the first draft (88857828). Findings and what
was taken:

| # | finding | taken |
|---|---|---|
| 1 | The draft targeted the smaller cost: the think half is at most part of `cs_update` (~1-1.6 ms) while the encode is 2.8 ms per tick and runs after every catch-up tick (100 a second at 5x) | yes: 5O, step 2, and the measurement gate in step 3 |
| 2 | Perception is already budgeted the same way (`scanInterval`, 480/s, staggered); the draft would add a second rotation and double the sight-to-react lag | yes: 5A makes the scan the think slot; the "new hostile" wake goes (verified in `combat.cpp` `senseAll`, `perception.hpp`) |
| 3 | `pickTarget` is called from the state handlers and the hub of a fighting member re-picks at once per the exe (an idle member in Done re-picks only after its own scan, on a wake, or on a 3-5 step slot: k16.6.632); the `incoming` defence check is per tick; where the exe runs its combat class AI is unknown | yes: think is narrowed to the sensor-driven re-evaluation, transitions and defence stay in act; oracle question in step 5 |
| 4 | Joining the scorer to K = 24 scores 1.5x as often; the lockstep spikes are fixed by the `id % 20` offset alone | yes: the scorer keeps a 20-tick minimum period; the offset goes in step 1 |
| 5 | `routeFrom` is three searches, not one Detour query; sliced state cannot be saved; mesh changes; head-of-line blocking; "stands until it arrives" is near-terminal; measure first; per-tick re-queries cost more | yes: 5E rewritten as measure, pre-check / negative cache, no re-queries, then overflow slicing with a bounded wait |
| 6 | Scheduler state must be saved; think order affects results | yes: section 7 |
| 7 | K per tick at 5x is five times the exe's thinks per wall second | partly: per tick stays the default (constant decisions per game second); K / speed is a measured option |
| 8 | Parallel perception does not extend to `update` | yes: section 6 |
| - | Agreements: count budgets; `think_budget = 0`; islands not worth it for one player (trim to a one-off count); wakes against K; per-member RNG; 232 first, C last; measurements on a loaded box | kept; the quiet-box baseline is in step 3 |
