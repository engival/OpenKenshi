# An attack order on a hostile while other attackers engage (kenshi-4bps.1, kenshi-karw)

Scripts against `scenarios/bar_lure_hostile.kscen` (a fresh OpenKenshi-server each run): `PORT=47836 client/tests/far_attack/run.sh SCRIPT OUT`.
The scenario puts the player at the Hub's Bar and a Blood raider patrol of eleven on the open ground past it; they are always hostile, so the
fight starts on sight. Ids move with the data, so a script names characters as `@Wanderer@`, `@Blood raider#11@` (the 11th of that name, in id
order) and the runner fills them in from a probe run's `dump chars` (`client/tests/charids.sh`; `OUT/ids.txt`, `OUT/idprobe.log`).

- `far_attack.txt`: a real right click (`mouse char=N` puts the pointer on the body centre, and refuses when the pick there is not that
  character) on a standing raider while all eleven have the player engaged. The facts are the run log's `sim click (684, 352): character
  40982 attk (-1): sent` line and the shot's route sidecar's `order 40971 seq 6 ... what attack target 40982 ... outcome applied tick 83257`
  row; the `dump chars` after the click shows the player's AI goal switching from `SELF_PRESERVATION/32` to `FOCUSED_MELEE_ATTACK/5` on
  40982, the clicked raider.

Why it exists: user 2026-11-16 (kenshi-karw), "I ran away, then ordered an attack on someone close to the bar door. I expected my character to
run past all the other attackers and go after them. Not sure what interrupted the run, but it looked awkward." The sim lead pinned the sim
side (ksim 248ce605, `ksim_order_attack_far`: the sim keeps the order). This test asks whether the client sends it at all: before kenshi-4bps.1
the route sidecar kept move_to rows only, so a sent attack left no trace in a capture, and the capture's sidecar reads as "never sent".

What it does not show: the run to a far target. The sim's world starts before the client's first frame, so the patrol is always within ~20 u of
the player when a scripted click can happen, and the ordered raider is inside the fight's reach from the first tick; the far-target run is the
sim's own test (`cpp/tests/ksim_order_attack_far`).
