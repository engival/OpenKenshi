# Client audio engine (kaudio)

Merged into master (the `audio` branch and worktree are retired). Code: `client/src/audio/`, test CLI `client/src/kaudio_dump.cpp`, CMake target `kaudio` (static lib).
kclient: `client/src/audio/audiohook.cpp`, four calls in `main.cpp` (init in the "sky and weather" step, update after `sky.update`, shutdown first in `cleanup()`). `main.cpp` belongs to the client session; message it before touching it.
Format research: `docs/reference/AUDIO.md`, `docs/reference/audio/MUSIC.md` (bank trees), `audio/AUDIO_EXE.md` (game-side calls).

## Pieces

| file | what |
|---|---|
| `wwbank.*` | `.bnk` chunks (BKHD, DIDX, DATA, HIRC); embedded media lookup. Whole file read into memory. |
| `wwvorbis.*` | Wwise Vorbis `.wem` → PCM. Rebuilds the Vorbis id/comment/setup headers and each audio packet's first bits **in memory on load**, then decodes with system libvorbis. Codebooks: `third_party/wwise_codebooks/` (aoTuV 6.03 from ww2ogg, BSD-3, embedded at build time by `tools/bin2inc.cmake`). |
| `mixer.*` | stereo float mix, Hermite resampling, smpl loops, gain ramps with Wwise curve shapes, everything schedulable on the mixer clock (frames rendered); buses music/ambience/sfx/voice; sokol_audio device (PulseAudio). |
| `wwhirc.*` | HIRC music objects (v112): Event, Action, Music Segment/Track/RanSeq/Switch. C++ port of `tools/wwise_bnk.py`. Banks merge into one `Hierarchy`. |
| `music.*` | `MusicPlayer` (events, playlists, switch transitions), `BankSet` (banks by name), `GameMusic` (biome → `Music_<Biome>_Play`, re-trigger timer, Day/Night). |
| `sound.*` | `SoundPlayer`: sound structure (Sound, Random/Sequence, Switch, Layer containers) as run trees; Play/Stop/Set-Reset Volume·Pitch·LPF/SetState/SetSwitch actions; RTPCs re-evaluated per voice each update. `GameAmbience`: zone → `Enter_<zone>_Day/_Night` / `Leave_<zone>`, Camera_Zoom, weather events and Intensity, indoors. |
| `../audiohook.*` | kclient side. A control thread (~20 Hz) owns the mixer, device, banks, `GameMusic` and `GameAmbience`, so bank loads never stall a frame; `audiohook_update` only stores the camera (position, forward, right), ground height, weather and clock hours. Area at the camera = areasmap.tga cell → BIOME_GROUP `music` / `ambience`. Logs each music/ambience/day change on stderr. Env: `KCLIENT_NOAUDIO=1` off; `KAUDIO_WAV=FILE` renders in real time to a float WAV instead of the device (headless tests, `client/tools/headless.sh`). |

UI events (bbcbc2a7; kenshi-inspect `findings/audio_load_pause.md`): the audio thread also loads SFX_General_Bank, MT_Music_Bank and VO_Bank (v112, 443 media; the game's boot list, kenshi-h2v.29.24) and posts
`Boot_Up_Game` once (the menu music). `audiohook_event(name)` queues a Wwise event name; the thread runs it on a UI `SoundPlayer` (kSfx) and
the `MusicPlayer`, each ignoring nodes it has no part for, and logs `audio: event NAME (id): N actions`, then each action, sound voice and
music segment. Posted from main.cpp: `Create_or_Load_Game` at a load / new game / quickload request; `Pause_Game` / `Resume_Game` on a
flip of the clock's pause bit 0; `Game_Speed_Normal` / `_x2` / `_x4` on a speed key or HUD button.

Pause duck (kenshi-k16.2.13): `Pause_Game` sets SFX Bus (1502772432) to -96 dB and bus 2508418636 to -9 dB over 1 s; `Resume_Game` resets
both over 3 s (SetBusVolume / ResetBusVolume, action classes 0x0C / 0x0D). Init.bnk is loaded for its bus tree (CAkBus: only the parent is
read). Under Master Audio Bus: SFX Bus (child 1919282061), Music Bus 3127962312, VO Bus 2018813558, and unnamed 2508418636, 16064149 and
3217363488. Names are FNV hashes matched against guesses; the other three are not in names.txt. A voice's output bus is the nearest node up
its chain that sets one:
- The zone ambiences sit under actor-mixer 245475126, which routes to 2508418636. So paused ambience plays at -9 dB, not silenced, even though
  that mixer's parent is on SFX Bus.
- The UI clicks route to 3217363488 through mixer 1026189871 and are not ducked.
- Music voices carry no Wwise bus and are never ducked (the game ducks neither music bus).

The mixer keeps one dB ramp per Wwise bus, interpolated in dB (UNSURE), applied once per render block and summed up the bus tree.
Check: agent `scripts/audio/pause_duck.sh`. It renders `kaudio_dump amb ... 10:pause 25:resume` against the same seed without them: -9.0 dB
from 11 s to 25 s, back to 0 by 28 s.

Volumes: options `audio.master`, `audio.music`, `audio.environment` (ambience + weather), `audio.effects` (SFX and voice buses) in kclient.cfg (config.hpp), percent 0..100, gain = (p/100)². The audio thread re-applies them when `cfg::generation()` changes; `audio.music=0` also stops new music triggers (the game's rule). The old `audio.cfg` is no longer read (a notice is logged when it exists). No UI yet.

`kaudio_dump tree NAMES BANK...` prints event → action → object trees (ids named from `docs/reference/audio/names.txt`); `kaudio_dump amb DIR ZONE OUT|- SECONDS T:key=value...` renders the ambience offline with a timeline (zone, day/night, height, rain, wind, inside/outside, pause/resume).

## World sounds (kenshi-h2v.29.4)

The original posts every game sound on a world object with a position (`audio_sfx_triggers.md` "How the posts are made"). kclient API, `audiohook.hpp`:

| call | what |
|---|---|
| `audiohook_object(id, pos)` | register or move an object (the host's id, 0 invalid); every frame it exists. Playing sounds follow it. |
| `audiohook_object_remove(id)` | forget it; sounds still playing stay where it was |
| `audiohook_object_switch(id, group, value)` | set a switch on the object by name (`Armor_Worn`, `Cloth`); the sounds posted on it read it before the global one |
| `audiohook_object_post(id, event, gate)` | post an event by name on the object; `gate` > 0 drops the post (not quiet playback) when the object is farther than that from the listener (the original's per-post gate: footsteps 200, swings and hits 1000) |
| `audiohook_update(camPos, camFwd, camRight, ...)` | the listener is the camera: position and orientation |

Everything is queued to the audio thread in order (positions first, then switches, posts, removals); nothing runs before the first `audiohook_update`. Engine side: `SoundPlayer::setObject / removeObject / setListener / setSwitch(.., object) / postEvent(event, bus, object, gate)`; the kclient audio thread owns one extra `SoundPlayer` (`world`, kSfx) for them, so object switches never touch the UI player. Each post logs `audio: object N event NAME (id): D u, K voices` (or `gated, D u > G u`) and one `world sound` line per voice with its dB, pan and distance.

Attenuation (HIRC 0x0E, `Hierarchy::attenuations`, `kaudio_dump hirc`, `KAUDIO_ATT=1` lists them): the 23 v112 banks hold 10 objects, all consumed exactly, no unresolved reference. Layout (v112): u8 cone flag; with a cone, 5 floats (inside degrees, outside degrees, outside volume, LPF, high-pass; 2 objects have one); 7 bytes `curveToUse` (volume dry, aux game, aux user, LPF, 3 more; 0xFF = none); u8 curve count; per curve u8 scaling, u16 points, points (x, y, u32 interpolation); u16 RTPC count (always 0). Volume curves use scaling 2 (stored [-1, 0] = 20 log10(y + 1) dB, -1 silence), LPF curves scaling 0 (the 0..100 amount). x is the distance in world units; curves end at 400, 500 or 1000. The footstep's (att 376250768, on the Surface_Material switch): 0 dB at 20 u, -8.6 at 150, -14.6 at 250, silent at 500.
A node plays 3D when the nearest node up its chain with a positioning override says so (`Hierarchy::positioning`); its attenuation is that node's. A voice on an object gets volume dB and LPF of the curves at the distance and a pan of the object's side, re-evaluated every update, so a moving object or listener changes a playing sound. Sounds on no object (UI, ambience) are unchanged.

Decoded but UNSURE: the cone (read, not applied: needs an object facing); aux-send and spread / focus curves (read, not applied); pan law (sine of the azimuth against the camera's right vector, full side at 90 degrees; Wwise pans from speaker angles); a sound on an object stays at full stereo when the object is under 1 u.
Footsteps (event 2385628198) resolve through the `Surface_Material` switch (default Sand): the client sets no surface yet, so every step plays the default. Its unset RTPC 1519944498 (not named in names.txt) reads 0: -8.6 dB, -423 cents, LPF 32 on top of the attenuation.

Debug op (`--debug`): `sound at R F U [ARMOR [GATE]]` posts Footsteps (Armor_Worn set, default Cloth; gate default 200) on a fresh object R right, F forward and U up (world units) of the camera; `sound char ID [ARMOR [GATE]]` on the character's own object. Check (headless, `KAUDIO_WAV`, `--set audio.music=0 --set audio.environment=0`): 6 posts each at 20, 150 and 250 u ahead: the 250 u ones are gated (no voice); the voices' logged level is -30.6 dB at 20 u and -39.2 dB at 150 u (the curve's 8.6 dB); WAV peaks (varying by sample): mean per-voice peak 0.0015 at 20 u and 0.0005 at 150 u. Posts at 40 u left and right give pan -0.71 / +0.71 and the WAV's louder channel follows. Captures: `~/git/kenshi/findings/kenshi-h2v.29.4/`.

## Animation notifies (kenshi-h2v.29.6)

Footsteps, swings, swim splashes and work impacts are type-105 ANIMATION_EVENT records attached to animations (findings `anim_sfx_notifies.md`, oracle kenshi-rwh.68).
`Characters::init` reads the `events` refs of ANIMATION and COMBAT_TECHNIQUE records (`notesOf`; the log prints `anim notifies: 65 ... on 17 ANIMATION records and 34 COMBAT_TECHNIQUE`):
the ref's int is a percent of the clip, or on a technique a frame over `num frames` (1 = percent). `Characters::fireNotes` runs after `locomote` each update and compares each
playing layer's clip progress with its notes, ascending, again on every loop (`AnimationEvents::update` 140064690). A locomotion layer counts while its weight is above 0.5
(UNSURE: a crossfade would otherwise post both clips' steps; the game's state-weight rule is not traced); the action layer always, and an action that arrives more than 0.25 s
into its clip does not post its early notes late. Backward play (a body backing away) posts nothing. The fired notes (`takeSounds`) are posted by main.cpp on the character's
world object (its sim id), as `AnimationSFXEvent::vf1` 1400649c0 does: Footsteps get `Armor_Worn` (the heaviest worn ARMOUR `class`, Cloth / Leather / Chain / Plate; UNSURE, the
game reads `char+0x68c`) and the 200 u gate; every other event the record's own switch override (`Weapons` Work_Pick / Gorillo_*, `Impacts`), else for `Attack` the held weapon's
`Weapons` value (Katanas, Sabres, Blunt, Heavy, Hackers, Bow, Turret; unarmed sets none), and the 1000 u gate. `--debug` logs `notify: char ID EVENT [GROUP=VALUE] [from ACTION]`.
Not posted: `AnimationGroundEvent` (the foot's dust / water effect at the bone) and `AnimationReloadEvent` (the crossbow reload is not a sound); no shipped record sets `stop`.
Not covered: ANIMAL_ANIMATION notifies (the bull's footsteps; the client plays animals outside `Characters::locomote`) and BUILDING_FUNCTIONALITY `animation events` (mine, smith,
farm and dummy impacts: the wire does not say which machine a worker uses). The `stonemine` ANIMATION's own Pickaxe notify fires when that clip plays as an action.
Check (`findings/kenshi-h2v.29.6/`): the Wanderer on bar_lure_stealth_arrows runs then walks, `KAUDIO_WAV` render: one Footsteps voice per notify, ~0.30 s apart running and ~0.46 s walking.

## Combat sounds (kenshi-h2v.29.7)

`SimView::cueSounds` turns SIMLINK events into world posts (`SoundCue`, drained in `main.cpp` after `Characters::update`; `--debug` logs `combat sound: tick T object ID EVENT group=value`). Source: `audio_sfx_triggers.md` sections 7 and 8, all with the 1000 u gate.

| event | post | switches |
|---|---|---|
| 1 hit, result 0 (melee) | `Impact` on the victim | `Impacts` = `HtH_Impacts`; `HtH_Impacts_Robot` when the attacker holds no weapon in the hand (the findings' `attacker+0x1f0 == 5`; UNSURE: an animal's natural weapon counts as held) |
| 1 hit, result 1 (blocked) | `Impact` on the victim | `Impacts` = `Parry` |
| 1 hit, result 2 (dodged) | nothing | |
| 1 hit that a landed event 49 of the same batch made (a bolt, hit type 6) | `Deflection` when a worn piece covers the part, else `Impact` | `Armor_Worn` = the covering piece's MaterialType (Cloth / Leather / Chain / Plate; UNSURE: class vs material), else `Impacts` = `HtH_Impacts` |
| 1 hit, result 0, a draw of (tick, victim) | `VO_Get_Hit` (human) / `VO_Creature_Take_Damage` (animal, race `sounds` >= 4) | `VO_Races` (`sounds` 0-3 + gender) or `Creature_VO` and `Weapons` (`sounds` - 4, min Dog). The draw stands for `rand() & 8 == 0`: half the hits |
| 5 death | `VO_Creature_Die` on an animal; `VO_Creature_Victory` on the animal that landed its last hit | as above |
| 49 shot | `Attack` on the shooter's object (the gunner for a turret) | `Weapons` = CROSSBOW `shoot sound` (0 Crossbow, 1 Harpoon, 2 Crossbow_Turret, 3 Harpoon_Turret) |
| 49 shot, result 2 (missed) | `Impact` on a temporary object at `to` | `Impacts` = `Stone` (the ground material is not on the wire); posted at the launch, the bolt's flight time is not modelled |

Not done: `Light_Hit` / `Heavy_Hit` (the findings give "from behind and damage above a threshold" without the threshold), `VO_Creature_Alert` / `VO_Creature_Passive` (need perception and an idle timer), the ground material of a bolt's impact, the turret record on the wire (event 49 carries the gun's CROSSBOW record, not the turret's).

## Door, building and task sounds (kenshi-h2v.29.8)

`SimView::cueWorld` reads the wire's state each sync and posts the same `SoundCue`s as the combat sounds (gate 0: the findings list none; the attenuation curves do the distance). A first sight posts nothing, and a door's first second after its first sight is arrival, not a change. `audiohook_indoors` carries the interior state to the audio thread (`GameAmbience::setIndoors`).

| post | from | object / switches |
|---|---|---|
| `Door_Open` then `Door_Shut` | INTERACTABLE door state 1/2 -> 0; `Door_Shut` after 1 / `door move speed` s (the client does not animate the leaf: UNSURE unit, 2 s without the record) | door object `0x40000000 + id` at the door's pos; `Door_Type` = `door type` (0 Wood, 1 Mechanical_Metal, 2 Gate) |
| `Door_Close` then `Door_Shut` | state 0 -> 1 | same |
| `Break` | state -> 3 (broken) | same |
| `Character_Lock` | state -> 2 | the door |
| `Character_Unlock_Success` | `lock_roll` success (event 20) on the picker; a door going out of 2 with no roll success that batch posts it on the door | |
| `Character_Unlock_Fail` | `lock_roll` failure on the picker | |
| `Character_Pick_Up_Person` / `Character_Drop_Person` | a body's `carriedBy` set / cleared, on the carrier | `Armor_Worn` = the body's highest worn MaterialType |
| `Character_Pick_Up_Ground` | event 7 (picked up) on the character | `Inventory_Items` = the item's sound switch (none for weapons) |
| `Character_Inspect` | event 13 action 5 (loot) result 0 / 6 on the actor: the loot order's arrival; the original posts at the task's start | |
| `Enter_Building` / `Enter_Building_Big` / `Leave_Building` | the lead character's indoors bit (CHAR_DYNAMIC bit 19) changes; log `audio: Enter_Building Indoors=... Building_Integrity=...` | `Indoors` = BUILDING `interior ambience` (1 Metal, 2 Stone, 3 Wicker: UNSURE order), RTPC `Building_Integrity` = `interior integrity`, Big = `cavernous`; the record is found by `WorldBuildings::recordOf(building key)` |

Inventory sounds (`Item_Lift`, `Item_Drop`, `Sell_Item`, `Character_Drop_Ground`) are the UI's (`ui/gear.cpp`, `ui/itemwindows.cpp`). Not done, wire gaps: `Character_Unlock` at the task's start and `Character_Lock`'s own end (the wire has TASK_PROGRESS rows, no task-start event), `Character_Bed`, `Character_Medic_Start/Stop`, `Character_Build_Loop_*`, `Character_Consume`, `Turret_Mvt_*`, `Building_Place` and the other build-panel posts (the UI session's).

## Verified

- All 2686 media in the 24 banks decode to their header sample count. PCM equals ww2ogg+oggdec within 1 LSB (49 files, 8 banks). `kaudio_dump check`.
- Mixer at step 1 reproduces the direct decode exactly (MT theme).
- HIRC counts equal MUSIC.md: 402 segments (163 silent), 239 tracks, 19 ranseq, 10 switches; every object consumes its size; all media resolve. `kaudio_dump hirc`.
- `kaudio_dump music DIR Savannah - 30 night=900`: three layers per trigger, re-triggers at 4-7 min, ~9 % of the time audible; Day→Night mid-piece fades the piece out with the switch's rule (R8, 10.4 s).
- Sound structure: every object of the 23 v112 banks decodes to its exact size (1629 sounds, 240 random/sequence, 44 switch, 40 layer containers, 34 actor-mixers); all children and media resolve (the 41 media-less sounds are the Wwise Silence plugin). `kaudio_dump hirc`.
- Ambience (`kaudio_dump amb`): Plains bed + one-shots, day→night crossfade (10 s, the Night event's Stop actions), zone change (Leave_ stops the zone's actor-mixer), Camera_Zoom moves the bed levels, rain/wind layers follow Intensity, indoors muffles the outdoor mixer. 150 s render in 1.1 s CPU.
- Curve shapes are Wwise's (`AkInterpolation::InterpolateNoCheck` as documented in wwiser `generator/render/bnode_rtpc.py`); RTPC graph lookup and dB scaling (`20 log10(y + 1)`, -1 = silence) likewise.
- Ambience is authored quiet: media ~-42..-33 dBFS RMS; Init.bnk bus volumes are only +6/-4/-9 dB, HDR threshold 0 (no make-up). Our ambience sits ~20 dB under the music, as in the data.
- Exe facts (decompile): zone events are **`Enter_<zone>_Day` / `Enter_<zone>_Night`** (bank hashes; the decompile drops the suffix argument). The zone key carries night in bit 8 (`FUN_140258940`), so a day/night flip re-posts Enter_ without Leave_. Day = time in (dayStart, dayEnd) (`FUN_14066cb20`). `Camera_Zoom` = (camera y - ground y) / 2000 * 100 (`FUN_1406b0140`, constants 141700640 = 2000, 141683250 = 100). The game posts `Weather_Wind_Stop`, but the bank's event is `Weather_Wind_End`: wind is never stopped, its Intensity curves only turn it down (one layer stays at ~-7 dB at Intensity 0). We post the same names.
- BIOME_GROUP `ambience` (0..9) is the alphabetical zone list Ashlands, Boneyard, Canyons, Coast, Desert, Forest, Jungle, Plains, Swamplands, Tarsands (checked against the 77 groups' names: Bonefields 1, The Crags 2, Shem 4, Vain 6, Skinner's Roam 7, The Swamp 8, Deadlands 9); `music` = 0 Dunes, 1 Savannah, 2 Swamplands, 3 Tarsands, 4 None.
- Weather (`FUN_1409e8f70`): `Rain_Intensity` = WEATHER `rain intensity` x strength; `Intensity` = `wind intensity` x wind speed (effect+0x18 = lerp(wind speed min, max, strength), `FUN_1409dcf60`) x 0.01 (double at 14167c310). kclient: the live weather's strength and wind speed (CLIENT_GOALS.md "Live weather"); without a sim, strength 1 and wind speed max.
- kclient run (headless, `KAUDIO_WAV`): Skinner's Roam → Dunes music + Plains ambience; a jump to The Swamp → Swamplands for both.
- Names (FNV-1 of the lower-cased name): state group `1572762421` = `Time_in_Game`, states `Day` 311764537 / `Night` 1011622525; switch group `975021402` = `Biomes`, values = biome names; `Music_<Biome>_Play/_Stop` = the region events.

## Interpretations (UNSURE, from MUSIC.md section 9)

- Playlist state persists per container across plays (Step semantics); avoid-repeat clamps to children-1.
- A Play of a node that is still playing is ignored.
- Among several matching transition rules the last wins. Fade **offsets are ignored**: both fades start at the sync point. The Wwise code is in the exe decompile (private, kenshi-inspect; statically linked): a targeted lookup could settle the offset sign and rule priority.
- Biome change posts the new Play immediately without stopping the old region (AUDIO_EXE: that path has no Stop).
- Sound player:
  - 3D sounds: world objects have positions now ("World sounds"); the 3D path/automation data (random emitter positions around the listener) is still skipped. One-shots picked by a container on no object get a random pan in [-0.7, 0.7]; beds are centred.
  - LPF amount → cutoff: exponential 20 kHz (0) .. 50 Hz (100), a one-pole filter. Wwise's table is unknown.
  - Layer crossfade curves are read as linear gain. Every curve seen is flat 1, so it has made no difference so far.
  - Ranged props (random volume/pitch/LPF) are rolled for each voice, along the whole node chain.
  - Set-prop actions are treated as offsets. Every one seen targets a node without its own value, so absolute and offset are the same here.
  - Unset RTPCs read 0 (the Init.bnk game-parameter defaults are not decoded). Bus volume actions (0x0C/0x0D) and the bus tree are applied (the pause duck). Bus props, ducking between buses, aux sends (reverb) and
    state props are not.
  - Loop count n > 1 plays the whole file n times (loop points ignored for the count).

## Next

1. kclient: volume sliders and a mute key, once the client has its UI widget system and unified options handler (volumes are `audio.*` options now); the listener at the selected character instead of the camera (the game's +0x58 node is not identified); indoors from the client's building interiors.
2. Who posts `Enter_Building` / `_Big` / `Leave_Building` and sets the `Indoors` switch (Metal/Stone/Wicker, C53/C54) is not traced; `setIndoors` is a stand-in.
3. 3D positioning: the path vertices / automation ranges (random emitter positions around the listener), the cone, aux and spread curves; callers for the world sounds (footsteps, swings, hits: h2v.29 workers).
4. VO events: VO_Bank is loaded and plays (combatsound `shot`: `VO_Get_Hit (1028316347): 58.3 u, 1 voices` for a Human_Male victim; Human_Female sets are chosen too, the 1000 u gate drops the far ones). Animal voices (`VO_Creature_*`, Creature_VO) play too (combatsound `goat`, kenshi-h2v.29.28); an animal's landed hit posts `Impact` with `Impacts` = `HtH_Impacts` on its victim, its natural weapon counting as held (combatsound `beak`, kenshi-k16.2.315). The 8 unnamed VO_Bank events (findings audio_sfx_triggers.md) are not posted. SFX_Bank.bnk is bank v88 (`kaudio_dump tree` refuses: HIRC objects only for v112): needs its own HIRC layouts, not written.
5. Bank unloading (game: 60 s after leaving); bank loading on a worker.
