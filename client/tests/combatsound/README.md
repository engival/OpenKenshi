# combatsound (kenshi-h2v.29.7)

`run.sh fight|beak|shot|female|goat OUT` runs a scenario headless with `KAUDIO_WAV=OUT/mix.wav`; `OUT/client.log` has the `combat sound:` lines (tick, object,
event, switches) and the audio thread's `world sound` voices (run.sh sets `debug.audio_trace=1`, which gates those lines). Scripts: `fight.txt` (dismember_fight,
the Wanderer attacks the Dust bandit), `beak.txt` (beak_open_ground; the Beak Thing downs the Wanderer with its first hit, so no animal is hit:
animal voices unverified there; the bird's hit on the Wanderer posts `Impact Impacts=HtH_Impacts` on the victim, kenshi-k16.2.315), `shot.txt`
(cage_prisoner: world guards shoot with hand crossbows: `Attack Weapons=Crossbow`, impacts, parries, hit voices), `female.txt`
(female_fight, kenshi-h2v.29.28: the Dust bandit is `female=1`, so a landed hit posts `VO_Get_Hit VO_Races=Human_Female`), `goat.txt` (goat_fight,
kenshi-h2v.29.28: a six-character katana squad attacks one wounded wild Goat: `VO_Creature_Take_Damage` and `VO_Creature_Die`, both voiced). The scripts
name characters (`@Dust bandit@`, `@Slaver boss@`, `@Goat@`, ...) and run.sh fills their ids from a probe of the fresh scenario (`client/tests/charids.sh`,
`OUT/ids.txt`; kenshi-k16.2.308), so the numbers in the lines quoted below are one run's, not constants.

The female and animal voices (kenshi-h2v.29.28), both verified with the run.sh modes:

- `beak` (kenshi-k16.2.315): the beak world fights on its own - the first hit lands inside the world's first second and downs the Wanderer, who
  is then eaten (eat damage grows his wounds with no wire event). The id probe is a client session too, and the sim ticks while it is connected,
  so the beak mode probes the ids on a throwaway server and runs the measured client against a fresh one; the measured run then opens with
  `combat sound: tick <T> object <Wanderer id> Impact Impacts=HtH_Impacts at 0.02 s`, `audio: object <Wanderer id> switch Impacts = HtH_Impacts`,
  `event Impact (3257506471): 397.3 u, 1 voices` and `world sound 52946379: voice 6 ...`. The bird's natural weapon counts as held (the
  `HtH_Impacts` branch, not `HtH_Impacts_Robot`, as an unarmed attacker gets). One hit is all this world lands, so the drawn `VO_Get_Hit`
  (1 in 8 hits) usually does not post here: the Impact line is the mode's cue.

- `female`: five `combat sound: object <Female id> VO_Get_Hit VO_Races=Human_Female` posts, each followed by `audio: object <Female id> switch VO_Races = Human_Female`,
  `event VO_Get_Hit (1028316347): 62.1 u, 1 voices` and a `world sound ... voice 67 ...` playback line.
- `goat`: `combat sound: object <Goat id> VO_Creature_Take_Damage Creature_VO=Goat Weapons=Goat` on the landed hits (5.82 s, 6.18 s in the last run), each with
  `audio: object <Goat id> switch Creature_VO = Goat`, `switch Weapons = Goat`, the event's `1 voices` and a `world sound` voice; then the death at 17.20 s,
  `combat sound: object <Goat id> VO_Creature_Die Creature_VO=Goat Weapons=Goat`, `audio: object <Goat id> event VO_Creature_Die (995114143): 60.9 u, 1 voices`,
  `world sound 565770973: voice 22 at 20.02 s, -19.4 dB, pan +0.22, rate 1.634`.

The goat starts with a bleeding wound (`bleeding=2`) because in this sim an animal cannot be beaten to death: a KO body is not a target (combat.cpp, task BK),
death needs `blood < -maxBlood`, and fists deal blunt, which costs no blood. `scenarios/goat_fight.kscen`'s header has the runs and the numbers.
