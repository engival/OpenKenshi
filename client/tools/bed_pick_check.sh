#!/bin/bash
# Bed pick vs the Storm House stairs (kenshi-k16.2.233.1; user 2026-11-14: "regression, 'use' hitbox is too high, I can't walk on the stairs").
# bar_lure: the lead walks into the Storm House, the camera frames a floor-1 bed. At the full level (R x3: the auto floor took the view to
# floor 0 when the centred lead walked in, kenshi-k16.2.236.1) the cursor on the bed is a bed pick
# (kind 7, the `use` cursor); stepped down to floor 0 (the floor-1 beds are cut away) the cursor on the upper stair treads and on the lower
# stairs is the ground (kind 1, walk): the hidden bed must not pick. Exit 0 PASS, 1 FAIL.
# usage: bed_pick_check.sh OUT_DIR [PORT]   (PORT one of this worker's 47830-47839)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: bed_pick_check.sh OUT_DIR [PORT]}
PORTARG=(); [ -n "${2:-}" ] && PORTARG=(--port "$2")	# no PORT: demo.sh picks a free one and retries on a bind failure (kenshi-k16.2.417.3)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
# The click (400,480) is on the bed's drawn mattress (bed1). It was (530,150), a pole of the shade tent over the beds (tent3_poles), when bedPiece took the
# tent for the bed; the bed is its own bed1 piece now (kenshi-89l0: the tent's box holds every bed under it, the smallest piece box is the bed).
BED=6285449610646571553	# a Storm House floor-1 bed in scenarios/bar_lure.kscen
# no select: the lead is the selection at the start of bar_lure (a fixed character id went stale, kenshi-k16.2.417.4)
cat > "$OUT/script.txt" <<EOS
401 move -50628.5 1081.9 -1185.4
1500 camera thing id=$BED
1502 set yaw 31.1
1503 set pitch -35
1504 set dist 234.4
1505 key PERIOD down
1506 key PERIOD up
1507 key PERIOD down
1508 key PERIOD up
1509 key PERIOD down
1510 key PERIOD up
1511 mouse pos 400 480
1512 dump pick
1513 screenshot $OUT/bed_floor3.png
1520 key COMMA down
1521 key COMMA up
1522 key COMMA down
1523 key COMMA up
1524 key COMMA down
1525 key COMMA up
1530 mouse pos 790 280
1532 dump pick
1533 screenshot $OUT/stairs_floor0.png
1540 mouse pos 700 420
1542 dump pick
1560 quit
EOS
HEADLESS_TIMEOUT=300 nice -n 10 "$HERE/headless.sh" -W 1280 -H 720 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} --scenario-file "$TREE/scenarios/bar_lure.kscen" -- --set key.floor_up=Period --set key.floor_down=Comma --play bar_lure --script "$OUT/script.txt" > "$OUT/run.log" 2>&1 || true
grep '^pick (' "$OUT/run.log" > "$OUT/picks.txt" || true
cat "$OUT/picks.txt"
n=$(wc -l < "$OUT/picks.txt")
if [ "$n" = 3 ] && sed -n 1p "$OUT/picks.txt" | grep -q "kind 7 id $BED" && sed -n 2p "$OUT/picks.txt" | grep -q "kind 1 " && sed -n 3p "$OUT/picks.txt" | grep -q "kind 1 "
then echo "bed_pick_check: case A PASS (bed = use at the full level; stairs = walk at floor 0)"
else echo "bed_pick_check: FAIL (case A: expected bed, ground, ground)"; exit 1; fi

# Case B (kenshi-89l0; user 2026-11-22, F12 kclient-20261122-230043, k16.2.518 / k16.2.519): the three beds under the shade on the Hub Storm House roof (floor 1,
# scenarios/bed_putin.kscen). The middle bed never picked: its sleeping spot lies in the shade tent's (tent3) 38 x 78 u box, the tent's origin was the nearer, so
# bedPiece chose the tent and a click on the bed's own drawn mesh (bed1) was ground. Each bed is aimed at with the free camera over its layout piece (the
# piece origin in bedPiece's dump, which is not the sleeping spot); every click must pick that bed (kind 7), and debug.pick_mask=on must mask it (cells masked > 0).
# The capture's click records n=4..9 are in bed_pick_roof.txt beside this script (text; `click replay` of each reproduces its recorded ground hit, MATCH, at
# x -51160..-51187, z 2600..2685: floorboards beside and below the beds, not on the middle bed's mesh), so the assert aims at each bed itself.
BEDS=(5115565543370603965:-51174.1:2602.7 5115562244835719332:-51196.2:2626.2 5115563344347347543:-51210.2:2638.9)	# id:x:z of the camera (the piece origin, +20 on z)
{
	f=1500
	for b in "${BEDS[@]}"; do
		IFS=: read -r id x z <<< "$b"
		echo "$f set pos $x,$z"; echo "$((f+1)) set alt 130"; echo "$((f+2)) set yaw 0"; echo "$((f+3)) set pitch -60"
		f=$((f+30)); echo "$f mouse pos 640 360"; echo "$((f+1)) dump pick"; echo "$((f+700)) screenshot $OUT/mask_$id.png"; f=$((f+720))
	done
	echo "$f quit"
} > "$OUT/script-roof.txt"
HEADLESS_TIMEOUT=300 nice -n 10 "$HERE/headless.sh" -W 1280 -H 720 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} --scenario-file "$TREE/scenarios/bed_putin.kscen" -- --debug --set debug.pick_mask=on --size 1280x720 --script "$OUT/script-roof.txt" > "$OUT/run-roof.log" 2>&1 || true
grep '^pick (' "$OUT/run-roof.log" > "$OUT/picks-roof.txt" || true
cat "$OUT/picks-roof.txt"
ok=1
[ "$(wc -l < "$OUT/picks-roof.txt")" = 3 ] || ok=0
i=0
for b in "${BEDS[@]}"; do
	i=$((i+1)); id=${b%%:*}
	sed -n ${i}p "$OUT/picks-roof.txt" | grep -q "kind 7 id $id" || { echo "bed_pick_check: case B: bed $id does not pick as a bed"; ok=0; }
	m=$(grep "^pickmask thing $id: .* cells masked" "$OUT/run-roof.log" | sed -E 's/.* ([0-9]+) cells masked.*/\1/' | tail -1)
	[ "${m:-0}" -gt 0 ] || { echo "bed_pick_check: case B: bed $id draws no pick mask"; ok=0; }
done
[ $ok = 1 ] && { echo "bed_pick_check: PASS (case B: all three roof beds pick as a bed and draw their mask)"; exit 0; }
echo "bed_pick_check: FAIL (case B)"; exit 1
