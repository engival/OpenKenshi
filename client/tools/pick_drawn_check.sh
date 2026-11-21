#!/bin/bash
# The click picks the first DRAWN surface (kenshi-k16.2.259; user 2026-11-15: "I clicked at the far end of the room on a valid navmesh
# tile ... the click landed on something in the foreground that we can't see"). Two shots' own cameras and a live scene, each a headless
# run with --debug; `dump pick` must reach what is drawn behind the geometry it passed and name that geometry in its `pick skipped` lines:
#   A  the camera inside a collision shape, and geometry above the floor cut in an EMPTY building: the Heft shop's shot
#      (kclient-20261114-235024, eye 68399.4 1226.0 -71318.7), floor selector 0, the click at 876,617 picks the floor
#      (walkable yes), not the TwoStorey shape the camera sits inside (above the floor cut, not drawn at floor 0);
#   B  a CONTAINER above the floor cut, in the Storm House the lead is in: the water-tank shot (kclient-20261115-001410, eye
#      68288.1 1519.2 -70752.2), floor 0, the click at 658,475 over a floor-0 lamp must not pick the Water Tank on floor 1, and
#      the report names the container among the skipped. The scenario streams the tank itself (as a prop box above the cut), so
#      the case needs a live sim: a HELLO timeout (kenshi-k16.2.268) or things 0 fails the run here instead of passing silently.
#   C  the occluder ghost (camera.occluder_ghost, kenshi-k16.2.259.2.1): our character OUTSIDE behind the Hub's Storm House (order_sounds),
#      walked round it to its far side, the camera on the near side in the RTS auto-centred mode (Z). The segment camera->chest crosses
#      the house, so `dump occluders` names it an occluder and it fades to glass; a click on the ground beside the character, seen through
#      the glass, picks that walkable ground (not the wall) and names the house's shapes skipped "an occluder faded to glass" - in the log
#      and the F12 sidecar. The click through glass itself predates 259 (see_through, 47e2c75f), so a build without 259.1's named skip
#      passes the ground pick but fails here on the skip lines and the sidecar.
# The free camera reproduces a shot's rts eye exactly (mouse_ray is the same for both modes). Runs with --debug (dump ops need it).
#   D  (kenshi-k16.2.259.3) the click's own record (camera, pointer, ray, pick, skipped, ghosts) in the log and the sidecar's `clicks:` block, and `click replay`
#      of case A's record in a second run reaching the same pick.
# Exit 0 PASS, 1 FAIL.  usage: pick_drawn_check.sh OUT_DIR [PORT]   (PORT in this worker's 47830-47839 range; without PORT demo.sh picks a free port per run)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: pick_drawn_check.sh OUT_DIR [PORT]}
PORTARG=(); [ -n "${2:-}" ] && PORTARG=(--port "$2")	# no PORT: demo.sh picks a free one and retries on a bind failure (kenshi-k16.2.417.3)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
# The shot's eye over the ground at its xz (the ground is 1384.3 there: measured with an alt-130 run, the shot's camera line 1514.3).
TANK_ALT=134.9

cat > "$OUT/script-shot.txt" <<EOS
500 dump floor
501 dump building
502 dump occluders
510 set pos 68399.4,-71318.7
511 set alt 105
512 set yaw -150.4
513 set pitch -15.8
520 key F down
521 key F up
522 key F down
523 key F up
524 key F down
525 key F up
530 dump floor
540 mouse pos 876 617
541 dump pick
545 mouse down right
546 mouse up right
560 screenshot $OUT/shot.png
600 quit
EOS
HEADLESS_TIMEOUT=600 nice -n 10 "$HERE/headless.sh" -W 2560 -H 1385 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} \
	--scenario-file "$TREE/scenarios/cage_prisoner.kscen" -- --size 2560x1385 --script "$OUT/script-shot.txt" > "$OUT/run-shot.log" 2>&1 || true
grep -E '^(pick|move marker|sim click)' "$OUT/run-shot.log" > "$OUT/shot.picks.txt" || true
grep -q 'simlink: server: HELLO timeout' "$OUT/run-shot.log" && { echo "pick_drawn_check: FAIL (the shot run's server never said HELLO: kenshi-k16.2.268; rerun when the machine is quiet)"; exit 1; }

cat > "$OUT/script-tank.txt" <<EOS
500 dump floor
505 dump things
510 set pos 68288.1,-70752.2
511 set alt $TANK_ALT
512 set yaw -33.2
513 set pitch -51
520 key F down
521 key F up
522 key F down
523 key F up
524 key F down
525 key F up
530 dump floor
540 mouse pos 658 475
541 dump pick
560 screenshot $OUT/tank.png
600 quit
EOS
HEADLESS_TIMEOUT=600 nice -n 10 "$HERE/headless.sh" -W 2560 -H 1385 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} \
	--scenario-file "$TREE/scenarios/cage_prisoner.kscen" -- --size 2560x1385 --script "$OUT/script-tank.txt" > "$OUT/run-tank.log" 2>&1 || true
grep -E '^pick' "$OUT/run-tank.log" > "$OUT/tank.picks.txt" || true

# C: the occluder ghost. order_sounds' three player characters stand on open ground north-west of the Hub bar (the Storm House at
# -50589,1073,-1225, its parts' box reaching x -50683..-50501, z -1317..-1145); the first is walked round to the south-east side
# (-50560,-1360, walkable ground), and the camera is put in the RTS mode the game uses on it (Z, focus_char): the free camera's
# alt 40 over a -6 deg pitch is what set_rts turns into rtsDist (40 / sin 6 = 382.7), and the centre then orbits the camera to the
# character's chest minus the view: here 382 u north of it, ACROSS the house. 640,480 now lands on the ghost's solid ground floor (the walkable parts at or under its foundY stay drawn
# and pickable, kenshi-k16.2.412.3); the faded upper parts and the containers above it are passed through (the skipped ... glass lines).
cat > "$OUT/script-ghost.txt" <<EOS
10 set animate 1
200 key 1 down
201 key 1 up
260 move -50560 -1360
880 dump chars
900 set pos -50560,-1360
901 set alt 40
902 set yaw 0
903 set pitch -6
910 key Z down
912 key Z up
930 follow state
940 dump occluders
960 mouse pos 640 480
961 dump pick
970 mouse down right
971 mouse up right
990 mouse pos 640 480
1000 screenshot $OUT/ghost.png
1100 quit
EOS
HEADLESS_TIMEOUT=600 nice -n 10 "$HERE/headless.sh" -W 1280 -H 720 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} \
	--scenario-file "$TREE/scenarios/order_sounds.kscen" -- --size 1280x720 --script "$OUT/script-ghost.txt" > "$OUT/run-ghost.log" 2>&1 || true
grep -E '^(char |follow state|occluders|Storm House|pick|move marker|sim click)' "$OUT/run-ghost.log" > "$OUT/ghost.picks.txt" || true

# D (kenshi-k16.2.259.3): the click's own record, and its replay. The shot run's click printed a `click record:` line (the camera, pointer, ray and pick
# at its frame); a second run prepared the same way replays that record's words and must reach the same pick (MATCH).
grep -E '^(click record:|  )' "$OUT/run-shot.log" > "$OUT/shot.record.txt" || true
REC=$(grep -m1 '^click record: ' "$OUT/run-shot.log" | sed 's/^click record: //' || true)
if [ -n "$REC" ]; then
	sed -n '1,/^530 /p' "$OUT/script-shot.txt" > "$OUT/script-replay.txt"
	printf '%s\n' "540 click replay $REC" "600 quit" >> "$OUT/script-replay.txt"
	HEADLESS_TIMEOUT=600 nice -n 10 "$HERE/headless.sh" -W 2560 -H 1385 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} \
		--scenario-file "$TREE/scenarios/cage_prisoner.kscen" -- --size 2560x1385 --script "$OUT/script-replay.txt" > "$OUT/run-replay.log" 2>&1 || true
	grep -E '^click replay' "$OUT/run-replay.log" > "$OUT/replay.picks.txt" || true
fi

cat "$OUT/shot.picks.txt" "$OUT/tank.picks.txt" "$OUT/ghost.picks.txt"
fail() { echo "pick_drawn_check: FAIL ($*)"; exit 1; }
grep -q 'simlink: server: HELLO timeout' "$OUT/run-tank.log" && fail "the tank run's server never said HELLO (kenshi-k16.2.268); rerun when the machine is quiet"
grep -q 'simlink: server: HELLO timeout' "$OUT/run-ghost.log" && fail "the ghost run's server never said HELLO (kenshi-k16.2.268); rerun when the machine is quiet"

# A: the far floor, walkable, and the shape the camera is inside named as skipped for the floor cut, in the log and the F12 sidecar.
grep -qE '^pick \(876, 617\): kind 1 id 0 .*walkable 1' "$OUT/shot.picks.txt" || fail "the shot's click did not pick the walkable floor"
grep -qE '^pick skipped ".*4028-TwoStorey\.mod shape [0-9]+" at .*: the camera is inside it, above the floor cut of ' "$OUT/shot.picks.txt" \
	|| fail "no shape named as skipped for the camera inside it, above the floor cut"
grep -qE 'move_to 6833[78](\.[0-9])? ([0-9.]+ )?-71098(\.[0-9])?( \(navmesh point\))?: sent' "$OUT/shot.picks.txt" || fail "the click did not send the move_to"
grep -qE '^move marker: seq [0-9]+ click .* -> route end ' "$OUT/shot.picks.txt" || fail "the move marker did not take the sim's route end"
[ -f "$OUT/shot.png" ] && [ -f "$OUT/shot.txt" ] || fail "no screenshot or F12 sidecar written (the sidecar is <shot stem>.txt)"
grep -q '^pick ray: near plane ' "$OUT/shot.txt" || fail "the F12 sidecar has no pick-ray (near plane) line"
grep -q '^skipped shape: ".*the camera is inside it, above the floor cut of ' "$OUT/shot.txt" || fail "the F12 sidecar does not name the skipped shape"

# B: the tank on floor 1 over the floor-0 click is not picked, and the report names the container as skipped.
grep -qE '^pick \(658, 475\): kind 5 id 41365' "$OUT/tank.picks.txt" && fail "the tank above the cut still picks (Water Tank 41365)"
grep -qE '^pick \(658, 475\): kind [0-9]' "$OUT/tank.picks.txt" || fail "the tank shot's click picked nothing"
grep -qE '^pick skipped "container .*" at .*: above the (floor|level) cut of ' "$OUT/tank.picks.txt" || fail "the container above the cut is not named as skipped"

# C: the RTS camera across the house, the house named an occluder, and the click through the glass picking the ground beside the character.
grep -qE '^follow state: mode rts center [0-9]+ drive 0 subject [0-9]+ ' "$OUT/ghost.picks.txt" || fail "Z did not put the camera in the RTS auto-centred mode"
grep -qE '^Storm House at -50589,1073,-1225 .*: occluder$' "$OUT/ghost.picks.txt" \
	|| fail "dump occluders does not name the Storm House an occluder (the ghost never fired across it)"
grep -qE '^pick \(640, 480\): kind 1 id 0 .*walkable 1' "$OUT/ghost.picks.txt" || fail "the click through the glass did not pick the walkable ground"
grep -qE '^pick skipped ".*" at .*: an occluder faded to glass' "$OUT/ghost.picks.txt" || fail "no shape is named as skipped for the occluder glass"
grep -q 'move_to .*: sent' "$OUT/ghost.picks.txt" || fail "the right click through the glass did not send the move_to"
[ -f "$OUT/ghost.png" ] && [ -f "$OUT/ghost.txt" ] || fail "no screenshot or F12 sidecar written (the sidecar is <shot stem>.txt)"
grep -q '^camera: rts eye ' "$OUT/ghost.txt" || fail "the F12 sidecar is not the RTS camera"
grep -qE '^ *(skipped shape: |skipped )".*" at .*: (an occluder faded to glass|inside the occluder fade.s cone)' "$OUT/ghost.txt" || fail "the F12 sidecar does not name the shape skipped for the occluder glass"
# the click lands on the ghosted Storm House's solid ground floor (kenshi-k16.2.412.3): 'pick world ... building 1', the Storm_House_01_Interior_Floor
# walk part, and the hit inside the house's footprint (x -50683..-50501, z -1317..-1145), y within 10 u of the house's 1073
grep -qE '^pick world at t [0-9.]+ \(.*\) building 1$' "$OUT/ghost.picks.txt" || fail "the click through the glass is not in the Storm House (pick world ... building 1)"
grep -qE '^pick walk part ".*Storm_House_01_Interior_Floor' "$OUT/ghost.picks.txt" || fail "the click through the glass did not pick the Storm House's ground floor part"
hit=$(sed -n 's/^pick (640, 480): kind 1 id 0 hit \(-\{0,1\}[0-9.]*\) \(-\{0,1\}[0-9.]*\) \(-\{0,1\}[0-9.]*\).*/\1 \2 \3/p' "$OUT/ghost.picks.txt" | head -1)
[ -n "$hit" ] || fail "no ground hit for the click through the glass"
set -- $hit
awk -v x="$1" -v y="$2" -v z="$3" 'BEGIN { exit !(x >= -50683 && x <= -50501 && z >= -1317 && z <= -1145 && y >= 1063 && y <= 1083) }' \
	|| fail "the picked floor $hit is not inside the Storm House's footprint at y 1073 +-10"
# D: the record, the sidecar's clicks block, and the replay's agreement
[ -n "$REC" ] || fail "the shot's click printed no 'click record:' line"
grep -q '^clicks: [1-9][0-9]* recorded' "$OUT/shot.txt" || fail "the F12 sidecar has no 'clicks:' block"
grep -q '^click record: .*at=876.0,617.0 .* eye=.* yaw=.* pitch=' "$OUT/shot.txt" || fail "the sidecar's click record lacks the camera / pointer words"
grep -q '^  skipped ".*4028-TwoStorey' "$OUT/shot.record.txt" || fail "the click record does not list the skipped shape"
grep -q '^  ghosts: ' "$OUT/shot.record.txt" || fail "the click record has no ghosts line"
grep -qE '^click replay: .*: MATCH$' "$OUT/replay.picks.txt" || fail "the replay of the click record did not MATCH its recorded pick"
echo "pick_drawn_check: PASS (camera inside a shape + above-cut floor in an empty building, container above the cut, the occluder glass across the Storm House, the click record and its replay)"
