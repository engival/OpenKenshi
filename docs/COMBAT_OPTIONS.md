# Combat options: where we may differ from the original

Combat matches the original near 100% (ARCHITECTURE.md). This file collects the few places where
the original has a bug or quirk that we may fix. Each entry is a named setting with a default, the
evidence, and a short write-up for the final config screen describing original Kenshi behaviour.
Status: proposal; the user decides the defaults before release.

## 1. Extra attack slots never free

- **Setting:** `combat.fix_extra_attack_slots`. **Proposed default: on (fixed)**: the user leans
  towards opting out of the bug for slightly more fluid combat (2026-10-31). Off reproduces the
  original exactly. `CombatSettings::fixExtraAttackSlots`; `ksim_server --fix-extra-attack-slots on|off`, named in the startup log
  (`ksim_server: options ...`). Not saved: a setting of the process.
- **Original behaviour:** a defender has `max num attack slots` (global constant, 1) + race
  `extra attack slots` (0-3) slots, clamped 1..5. The code that frees slots (`release`, the 5 s /
  20 s timer in `tick`, `clearAll`, `onCombatEvent`) loops only to the global constant, so only
  slot 0 is ever freed. Attackers who take slots 1-4 hold them until combat ends, even after
  being knocked out or leaving (a dead slot).
- **Evidence:** decoded in `docs/reference/combat/ENGAGEMENT.md`; confirmed live in fight c
  (`docs/reference/combat/MEASURED.md` item 8): with 3 slots, slot 0 rotated among 3-4 bandits,
  slots 1 and 2 were held by one bandit each for the whole 60 s.
- **Who it affects:** humans have no extra slots, so nothing changes in human fights. It shows in
  squad fights against races with extra slots (Big-Bones and Leviathan +3; 9 races +1, 8 races +2)
  and for everyone under mods that raise the global constant.
- **Fixed behaviour:** every usable slot is released, timed and cleared like slot 0.
- **Why it may not matter in practice (user):** the fights where it shows are rarely close. A group
  of low-level characters against a Beak Thing gets wiped out and eaten anyway (the Beak Thing
  attacks area-wide); a high-level character kicks it dead. Few players will notice either way.
- **Config screen text (draft):** "Fix extra attack slots. Large creatures can be attacked by
  more than one character at a time. In original Kenshi, the extra attackers never rotate out:
  whoever gets in first keeps attacking until the fight ends, and a slot can stay blocked by a
  knocked-out character. On: extra attackers take turns like the first one. Off: original
  behaviour."

## 2. Skills above 100 run away

- **Setting:** `xp.no_runaway_skills`. **Proposed default: on (fixed)**; off reproduces the original. `CombatSettings::noRunawaySkills`;
  `ksim_server --no-runaway-skills on|off`, named in the startup log (`ksim_server: options ...`). Not saved: a setting of the process.
- **Original behaviour:** every XP gain goes through `CharStats::addXP` (`1408c5df0`):
  `q = ((101 - skill)/101)^2`, `skill += q*amount`, applied only when `0 < amount <= 20` and
  `q <= 20`. Below 101, `q` shrinks to 0 as the skill nears 101, so XP alone can never pass it
  (an overshoot would need `amount > 10201/(101-skill)`; the cap is 20). But `q` is squared, so
  a skill that gets **above 101** by some other route has `q > 0` again, and grows. The further
  it goes, the faster it grows, until `q > 20` at about skill 553, where gains stop. NaN resets
  the skill to 20.
- **Authored skills above 100 exist; keep them.** Game data (scan of the 4 always-loaded files):
  `rebirth.mod` STATS `tinfist` (used by CHARACTER Tinfist) has unarmed **110**, every other
  combat skill 100. `Newwworld.mod` STATS `10000 experience` (CHARACTER "Experienced Man",
  probably a test character; placement not checked) has 100..3000, which `rebirth.mod` overrides
  to 300 for the weapon skills. `gamedata.base` STATS `maxxed` is all 100. So in the original, Tinfist's
  unarmed gains `q = (9/101)^2`, as fast as at skill 92, and climbs further every time he fights.
- **How a player's skill gets above 100:** not through addXP. The user remembers an exploit that
  pushes one skill (not every skill; maybe not toughness) over 100, details forgotten. Open: list
  the writers of the skill floats that bypass addXP.
- **Fixed behaviour:** addXP gives nothing at or above the cap (`skill >= 101`: no gain), and skills
  above the cap no longer grow on their own. Authored values are **not** clamped: Tinfist keeps his 110, and
  every formula reads the stored value as the original does. Open: whether to also clamp skills
  raised above 100 by the exploit (needs the exploit found first).
- **Config screen text (draft):** "Stop runaway skills. In original Kenshi, a skill above 100
  (Tinfist's unarmed, or one pushed there by an exploit) keeps rising with use, faster the higher it
  gets. On: skills above 100 stay where they are. Off: original behaviour."

## 3. The last levels take forever

- **Setting:** `xp.endgame_curve`. **Proposed default: on (softened)**; off is the original
  curve. Shape to be decided by the user.
- **Original behaviour:** with `q = ((101-skill)/101)^2`, the time to go from skill `a` to `b` at
  a steady XP rate is proportional to `1/(101-b) - 1/(101-a)`. In those units:

  | Range | Time | vs 0->95 |
  |---|---|---|
  | 0 -> 95 | 0.157 | 1x |
  | 95 -> 99 | 0.333 | 2.1x |
  | 99 -> 100 | 0.5 | 3.2x |
  | 95 -> 100 | 0.833 | 5.3x |

  The last 5 points take 5.3 times as long as the first 95; the last point alone takes 3.2 times
  as long. This is the original design, not a code bug, so it is a balance option.
- **Open: needs play testing (user, 2026-10-31).** Candidate: keep the original curve up to a knee (e.g. 90) and make the rest linear, at the
  knee's rate or a fixed fraction of it. Any change must leave 0..knee identical, so early and
  mid-game progression matches the original exactly.
- **Config screen text (draft):** "Softer endgame training. In original Kenshi, raising a skill
  from 95 to 100 takes about five times as long as raising it from 0 to 95. On: the last levels
  take a reasonable time. Off: original behaviour."

## 4. The medical clock (synced with the original, 2026-11-07; live-tunable)

- **Original behaviour:** `Character+0xc8`, the time `MedicalSystem::update` is given, accumulates a global
  with two writers: GameWorld (dt x game speed) and MainListener (a clamped wall-clock delta). So bleed-out,
  KO and recovery timers partly follow wall clock and frame rate (measured 1.17 - 1.97 x the summed frame dt,
  MEASURED.md item 16).
- **Ours:** medical time is the fixed sim tick x game speed and nothing else. No medical, bleed or KO timer
  reads wall time (`CombatArena::update` passes its own `dt` to `updateMedical`). Required for deterministic
  replay and save/load.
- **Setting `CombatSettings::medicalClock` (default 1 / 1.04 = 0.9615 medical units per sim second, user 2026-11-07: "sync it with original kenshi").** Scales the whole medical step except hunger (bleed,
  clotting, blood loss, stun decay, KO timer, healing). The rate is stated two ways in the live work and they agree:
  the KO timer's live T values run at **1 unit ~= 1.04 real s at speed 1** (`KO_DEATH_LIVE.md`, task BA), and
  medicalClock 2.0 measured **2.08x the original's medical units** (task BA's report of the same comparison).
  It is the nominal rate: the original's frame-rate/wall-clock tail (below) is not modelled, because it would make
  replay and save/load depend on the frame rate. UNSURE: the frame-rate term itself.
- **The watched duels read faster than the nominal rate**, and that is the tail: the Escaped Servant's medical clock
  was 2.00 x sim time in the katana duels (blood loss per sim second = 2.00 x the rate in every quiet window; wounds
  clot 1.98 - 2.0 x, also while KO'd) and Engival's about 1.5 x (a 54.7 KO timer ran 0.3346 game h = 36.5 s;
  2.04 HP per game h), while a body far from the camera reads 1.0 (one update per 50 frames; MEDICAL_HUNGER_LIVE (b2),
  kinspect 32) and the dt2 collapse traces ~1.4. So no single constant is "the" original; the nominal one is our match.
  `healFactor` (1.17 / 1.53, stun decay) is the same quantity folded into one term, so at a clock above 1 stun decay
  counts it twice.
- **History:** the default was 2.0 from 2026-11-01 (`docs/history/REPORT_2026-11-01.md` section 4, the watched-duel
  logs) to 2026-11-07; that read 2.08x the original's medical units, so KOs and healing ran about twice as fast in game
  time. The reference rows that pin 2.0 stay, labelled as the watched-duel clock: a duel loser's rate at the KO reads
  0.12 - 0.39 (median 0.21) at 1.0 where the logs show 0.068 - 0.141 (mean 0.108), 0.129 at 2.0 (test
  `ksim_combat_armed`); a far-camera KO'd body (rate 0.22) halves in 27.0 s at 1.0 against the live 28.7 s
  (test `ksim_bleed_decay`, MEDICAL_HUNGER_LIVE (b2)).
- **Live tuning:** DEBUG_SET_OPTION key `medicalClock` (SIMLINK "DEBUG VIEW", frame 215) sets it for the session;
  it is never saved, and a fresh load returns to the default. The world's new-game options (cod / gdm / ht) are
  separate and saved: `reference/ADVANCED_OPTIONS_STATIC.md` "Ours".

## Kept on purpose: the toughness-training tricks

These are original behaviours players rely on. They are not bugs we fix, and they need no setting:
matching the original's damage, knockout and XP code reproduces them. Listed here so nobody
"fixes" them by accident (user, 2026-10-31).

- **Starving-bandit / play-dead training:** get knocked down near a crowd (or play dead and stand
  up in range of many hostiles), get beaten while down, repeat. Toughness rises absurdly fast.
- **Fogman limb eating:** fogmen eat a downed character's limbs; toughness rises. Rescue before
  the limb is lost, heal, repeat.

**Mechanism (decoded, to confirm live):** `applyDamageToPart` (`14064f300`) on the hit path does
not check whether the character is already unconscious. Every hit that leaves a vital part with
`health - stun + bonus < 0` restarts the KO timer and calls `FUN_1408c6720`, which awards a flat
`0.075 * rate[toughness]` toughness XP (`addXP`, so it shrinks as `((101-skill)/101)^2`). Each hit
also awards the normal defender XP (kind 4, scaled by damage vs toughness; RESOLUTION.md 7).
So every blow on a downed body pays twice, and a crowd lands many blows. Being eaten is presumably
the same path, through damage to a downed body (to decode: kinspect dt39 Q5, FEATURES "Fog eat and worship"; the peeler seems to award none).

**Rule for our sim:** these follow from the code above, so the AI must not "politely" stop
hitting downed characters where the original keeps hitting. Raid goals and the Lua behaviour
layer may decide who attacks; once a character is attacking, the combat layer does what the
original does.

**Measured (kinspect 9731dcd, `docs/reference/combat/TOUGHNESS_XP_LIVE.md`).** Toughness has
four XP sources:
1. Kind-4 "was hit" XP: 0.33-0.49 per damaging hit, scaled by the hit.
2. The KO award: 0.075 x race rate on every hit leaving a vital part < 0. Logged live on the KO hit.
3. Fist self-injury when punching: 0.075 x k x 1.33 x race rate (k 0.05 landed, 0.5 blocked).
4. **The play-dead stand-up award** (`Task_GetUp::start` 14033ca10 -> `FUN_1408c6780`):
   `0.075 x race rate x (nearby enemies + 10) x m`. `m` starts at 1 and gains 2 for each of: an arm
   unusable (2 flags), blood coma, `med+0xa4 < 15`. It pays only when getting up with enemies
   around, nothing if alone. With 50 enemies nearby it is (50+10)/(1+10) = 5.5x the one-enemy value, before `m`: this
   is the "stand up among 50+ NPCs" trick. Decoded; not yet seen live.

**Downed bodies are not beaten (measured).** A lone attacker never struck a KO'd Engival
(`clog_tough3`). In fight g, all 10 bandits dropped combat within 2 frames of his KO, the leader
looted him, and nobody hit the body (MEASURED.md 15). So the trick is **repeated cycles**: knocked
down (KO award once, plus the hit XP), get up among the crowd (the stand-up award, scaled by
enemies), knocked down again. Our AI must reproduce exactly that: drop a downed target, and
re-engage when it stands. The fogman eating path is still open.

## 5. Follow heading at a 180-degree turn (squad.follow_heading_fix)

- **Setting:** `squad.follow_heading_fix`. **Default: off (original)**; the user wants to A/B it by playing (2026-10-31).
- **Original behaviour:** a follower smooths the leader's direction into a stored heading, `heading += (h - heading) x 0.5 dt`,
  renormalised, and puts its slot on that heading. When the leader reverses exactly, `h = -heading` is a fixed point of the
  update, so the heading stays on the old direction for many seconds: the slot sits ~108 u on the stale side and the follower
  walks past the leader the wrong way before trailing 150-250 u (`SQUAD_FOLLOW.md` s9).
- **Fixed behaviour:** if the heading and the leader's direction differ by more than 150 degrees the heading snaps to the leader's.
- Wiring: a `CombatSettings` field (`squadFollowHeadingFix`) like the other options; `ksim_server --follow-heading-fix [on|off]` (bare = on),
  named in the startup log (`ksim_server: options ...`).
- **Config screen text (draft):** "Squad turn-around fix. In original Kenshi, when the squad leader turns straight back, followers
  keep heading for where the leader was going and overshoot before coming back. On: followers turn with the leader. Off: original behaviour."

## 6. Target re-pick hold (combat.retarget_hold_s)

- **Setting:** `combat.retarget_hold_s`, seconds. **Default: 1.0 (original, rwh113)**; the user wants to judge combat feel in playtest
  scenarios with a switch to compare (2026-11-10, gate kenshi-hfd). `CombatSettings::retargetHold`; also live through
  `Simulation::setRetargetHold`.
- **Original behaviour:** `pickTarget` keeps the current target while `+0x2f0 < hold`; the hold is 1.0 s for a normal NPC (0.3 s with
  CharStats+0x12b, the non-combat mode, which we do not model). The timer counts up and only a re-pick resets it, so a fighter keeps
  its target unscored for 1.0 s, then re-scores. (Task BE's 0.0 was an f32 read of the doubles; corrected 2026-11-13.)
- **Evidence:** `docs/reference/combat/ENGAGEMENT.md` "Task BE" (corrected), rwh113 `findings/rwh113_target_lock.md`; pin `ksim_target_lock`.
- **Who it affects:** every fighter that is not player-controlled and not on an attack order. It only shows when the current target
  leaves the candidate scan (the +5, and +10 while fewer than 2 attack it, otherwise keep it): sight lost behind a wall, a refusal, a
  hostility change. `ksim_retarget_hold` (a 3v3 with a forced drop-out every 0.4 s) counts 329 target changes at 0 against 175 at 1.0.
- **Set per run, no rebuild:** `ksim_server --combat-retarget-hold S` (0..10; over everything); or `retarget_hold=S` on the
  scenario header record (`PLAYTEST.md` 1) to pin a playtest version. Neither: 1.0. The value is in the startup log and in the
  `scenario_load` log line (`combat.retarget_hold_s S`); a scenario load sets it afresh, so a pin never leaks into the next scenario.
- **Config screen text (draft):** "Target switching delay. In original Kenshi a fighter re-evaluates whom to attack every moment, so
  in a crowd it can swap targets very often. A delay makes fighters stick with their target for that many seconds before they may
  swap. 1.0: original behaviour."

## 7. Off-screen token speed (ai.token_speeds)

- **Setting:** `ai.token_speeds`, on / off. **Default: off (today's flat 15 u/s)** until the user's detail-settings ask (kenshi-niy) decides;
  the original itself moves no unloaded platoon across a leg at all, so neither value is "the original". `AiSystem::Config::tokenSpeeds`;
  `ksim_server --token-speeds on|off`; the value is in the startup log (`ai.token_speeds`).
- **Original behaviour:** an unloaded platoon's token has the speeds 25 (move mode 0), 55 (mode 1) and 60 / 90 (state 6), capped at 70, but
  the gates that would use them almost never open: 999 unloaded platoons over 11.2 game-hours had no state change and drifted at most
  373 u (`rwh.38`, kinspect `rwh37_unloaded_platoon_static.md`).
- **On:** a group-layer token (a squad not built as Fighters) moves at 55 u/s, or 25 u/s while it stands inside a town's circle (the
  exe's Town radius, `towncircle.hpp`, re-read every squad step), both capped at 70. A built squad's carrot keeps 15 (the Fighters'
  pace), and a wandering trader keeps `trader::kWanderSpeed`. The constants are OURS (exe values the original never applies to a whole leg).
  Hub to Squin, 17.7 km: about 5.4 game-minutes at 55 u/s, plus the in-town part at 25.
- **Open (deferred: playtesting):** the default; whether a trader caravan should take the token speeds; the state-6 speeds (60 / 90) once
  the group layer has that state; ROAD routing (kenshi-k16.6.48).
- **Config screen text (draft):** "Fast off-screen travel. In original Kenshi squads that nobody is looking at stand still. On: they cross
  the world at about 55 units per second (25 inside a town), so a caravan or raid arrives in minutes instead of hours. Off: slow walk."

## 8. Off-screen medical step and persistent corpses (world.offscreen_medical)

- **Setting:** `world.offscreen_medical`. **Default: off (original)**; the user picks the shipped default in kenshi-niy's ask (2026-11-10 ksim:
  the epic's default = the original, against the user's "we must not lose per-member health"). `Simulation::setOffscreenMedical`; per run
  `ksim_server --offscreen-medical on|off`. Not saved: a setting of the process.
- **Original behaviour:** a platoon that leaves the loaded zones is frozen in its record: its characters do not bleed, heal or die while away.
  A dead NPC is evaluated for decay only on screen and is destroyed when its zone unloads ("corpse unloaded", ARCHITECTURE.md "Corpses").
- **On:** a demoted world squad's members with a body record (`state::MemberDelta`) step on a game-minute clock with the in-detail functions
  (`CombatRules::updateMedical`: bleed, clot, KO, the blood coma, death; `Simulation::treatBodies`: first aid). Each tick of the step a
  conscious squadmate with a first-aid kit treats the squad's worst bleeder at the in-detail rate for its medic skill (a lone member only
  itself, at the self skill). A member back at its template loses its record. A member whose blood runs out dies at the token's position:
  the entity stays as an ordinary dead body (a `Fighter` plus `CorpseDecay` with the death tick and cause, the gear in its `Inventory`),
  lootable at once when the player comes near. A corpse is evaluated everywhere, ages 12 game hours from its death wherever it lies, and is
  never destroyed for lying outside every player area.
- **Not stepped:** hunger (not stored), the walk of a medic to its patient, the interact time. Parts do not regenerate (as in detail: health
  rises only through a bandage pool); a part over 20 % damaged keeps degenerating, so an untreated badly hurt member can die over hours.
- **Config screen text (draft):** "Living off-screen. In original Kenshi, characters that are not near you are frozen: wounds do not bleed,
  nobody dies, and bodies vanish when you walk away. On: wounded characters keep bleeding, heal or die where they are, squadmates with a first-aid
  kit patch each other up, and the dead stay lootable until they rot (12 game hours). Off: original behaviour."


## 9. Off-screen fights between hostile tokens (world.offscreen_fights)

- **Setting:** `world.offscreen_fights`, on / off. **Default: off (original)**; the user picks the shipped default in kenshi-niy's ask.
  `Simulation::setOffscreenFights`; `ksim_server --offscreen-fights on|off` (and `ksim_soak`). Not saved: a setting of the process (so is the
  pair clock below).
- **Original behaviour:** none. Two hostile squads staged on one spot for 2.1 game-hours were byte-identical afterwards (`rwh.38`): nothing
  off-screen fights. The fight is OURS (user 2026-11-10: "off-screen fights is a good thing"; ARCHITECTURE.md "Strategic-level encounters").
- **On:** once per group-layer step (every 20 ticks), two tokens (`token_fights.cpp`) whose squads are hostile (`Simulation::squadsHostile`: a
  factionless squad is hostile to every faction, two of one faction or two wild squads are not, else either faction's relation to the other is
  hostile), neither at Full detail (not built, no alive `Fighter` member; the player's own faction is never fought; the test needs no player,
  so the soak fights too),
  within `tokenfight::kEncounterRadius` (120 u) fight once, a pair at most once a game hour. A squad fights at most once per step.
  - **Strength** = the sum over the living, conscious stored members of the member's score: the **mean of attack, defence, toughness,
    strength and the skill of the wielded weapon** (`tokenfight::memberScore`), read from the `Fighter` as `templateFighter` plus its
    `MemberDelta` give it.
  - **Winner:** P(A wins) = SA^2 / (SA^2 + SB^2), A the lower squad Id. The draw is the first of the stream
    `streamSeed(streamSeed(streamSeed(streamSeed(seed, kTokenFightStream), min id), max id), tick)`; the second decides whether the winner's weakest
    member falls (probability 1 - P(winner wins)). A pure function of the state: nothing is saved.
  - **Losses:** the loser's weakest `ceil(n/2)` living members fall, the winner's weakest with probability 1 - P(winner). With
    `world.offscreen_medical` **off** a fallen member dies now (`Simulation::leaveCorpse`, `CorpseDecay::cause` 2, gear in its `Inventory`);
    **on**, it is KO'd (`koTimer` = the in-detail knockout time) with one bleeding wound (`kFallWound` = 600 x the race's bleed rate; measured with no aid, a 500 wound clots and the member lives, 600 bleeds out in
    about 4.5 game hours) written into its `MemberDelta` body block, and the off-screen medical step decides whether it lives (a later death is cause 1).
  - **Rout:** the loser's `GroupAI` is reset (goal none, route cleared, `homeTick` 0, no cooldown on GoHome), so its next squad step picks the
    way home at the top of the home urgency; a loser with no home town keeps its goal and stays. No terminal state.
  - **Log** (the task log): `offscreen fight <A> vs <B> at x,z: <winner> wins p=<P(A)> losses A=<n> B=<m>`; A, B and the winner are squad Ids,
    x,z the midpoint of the two tokens.
- **Not done:** a fight that involves a Full-detail squad (the in-detail combat takes it when the squad is built), healing on promotion
  (kenshi-k16.6.47), town raids while unobserved (kenshi-k16.6.49).
- **Open (deferred: playtesting):** the default; the encounter radius (120 u against the 55 u/s token speed of §7); the score; the wound a
  fallen member bleeds with; whether the rout should cancel a faction raid order (today the order stays and the raid goal can outscore home);
  the pair clock is lost by a load (a pair can fight again within the hour of a reload); a straggler token (kenshi-k16.6.53) is a token like
  any other here.
- **Config screen text (draft):** "Fights off-screen. In original Kenshi, hostile squads that nobody is looking at pass through each other.
  On: when two hostile squads meet off-screen they fight a quick, strength-weighted battle: the loser loses half its people and runs home,
  the winner may lose one. Off: original behaviour."

## 10. Campaign victory AI (world.campaign_victory_ai)

- **Setting:** `world.campaign_victory_ai`, on / off. **Default: off (original).** `AiSystem::Config::victoryAi` (`Simulation`'s AI takes it at
  `enableAi`); `ksim_server --campaign-victory-ai on|off` (and `ksim_soak`); the server's startup log names it. Not saved: a setting of the
  process, as sections 7-9 (a saved per-world switch comes with the options wire, if ever).
- **Original behaviour (rwh.61, H static):** a raid campaign record carries `pt1 AI`, `pt2 AI`, `retreat AI` and, in the data, a `victory AI`
  package. The exe loads the first three and never the fourth: the string `victory AI` is not in the executable, no state 3 exists. So the
  30 campaigns whose data says "victory: occupy the town" (23 Holy Nation assaults, 2 Band of Bones, 2 Kidnap - Southern Hive, 1 Empire Assault
  Big, 1 Krals Chosen, and the base record they inherit from) win, run `Retreat and refresh` and go home. Win events only chain further campaigns
  (`victory trigger`). The raiders that DO occupy in the original are squads whose template carries `passing town assault then occupy` (36
  templates: bandits, Band of Bones, Southern Hive, Crab Raiders, Black Dragon Ninjas, Rebel Farmers and others): that path is unchanged
  by this setting.
- **Off:** the original: those 30 campaigns retreat and go home after the fight.
- **On:** a force that WON its town holds it (kenshi-h2v.22.10; `raid_phases.cpp` `watchRaidVictory`): when its stay is over (Retreating) its leader is alive, conscious and
  standing in the target town and no fighter able to fight that is hostile to it stands there (UNSURE, deferred: playtesting: the original has no
  win test; ours reads the built Fighters). The force then has no retreat, no deadline expiry and no walk home (`GroupAI::until` and the order's
  deadline pinned at the maximum; the campaign's `victory AI` package, or the template's own occupy package for a scenario squad with no
  campaign, runs) until it is killed, or its occupation ends by the existing rules (FACTIONS.md 2.1b) and no member has occupied for 60 s: then the walk home.
  A force that only outlasted the stay keeps the retreat. The town's owner never changes. Not saved (the hold is two existing fields; the watch
  is derived).
- **Affects:** mostly Holy Nation raids on player and rival towns; Band of Bones and Southern Hive kidnap raids; the Empire's big assault; Kral's
  Chosen. Bandit-style raids occupy in either setting.
- **Config screen text (draft):** "Raiders stay after winning. In original Kenshi, a bug stops big faction raids (mostly Holy Nation assaults,
  but also Band of Bones, Southern Hive kidnappers, the Empire and Kral's Chosen) from ever using the 'victory: occupy the town' orders their
  data gives them: they win the fight, then always march home, however it went. Only bandit-style raiders (Dust Bandits, Crab Raiders, Rebel
  Farmers, Black Dragon Ninjas and similar) ever moved in. On: the winners of those raids stay and occupy the town until you kill them all;
  the town stays yours. Off: original behaviour."
- **Open (deferred: playtesting):** the default (the user said "enable", so shipped off); whether the 30 campaigns' occupation should also be
  limited in time.

## 11. Dialogue pause (`--dialogue-pause hold|freeze`, kenshi-k16.9.10)

- **Setting:** `ServerContext::dialogueFreeze`; `ksim_server --dialogue-pause hold|freeze`; the startup log names it. **Default: hold** (real time).
  Not saved: a setting of the process, as sections 7-10. One global policy; the per-dialogue data column (DIALOGUE.md 13.2 (a)) is not built.
- **Original behaviour:** the game pauses the world while a dialogue menu is open.
- **Ours:** hold (the default) keeps time running, holds only the two parties (the speaker stands still, the player character's own move,
  attack, follow, drive or interact order ends the talk as "walked away", reason 4, and is then carried out), and CLOCK's dialogue pause bit
  stays clear. Freeze is the old behaviour: the tick stops while a menu is open. Accepted under hold: a guard's inspection or a shakedown can be
  walked away from.
- **Config screen text (draft):** "Dialogue pause. In original Kenshi the whole world pauses while you talk. Hold (default): the world keeps
  running, because a shared server cannot stop for one player's conversation; walking away ends the talk. Freeze: the world stops while a
  dialogue menu is open, as in the original."
