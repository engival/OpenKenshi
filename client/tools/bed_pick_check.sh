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
. "$(dirname "${BASH_SOURCE[0]}")/ports.sh"
PORT=$(pick_port "${2:-}") || exit 3
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
BED=6285449610646571553	# a Storm House floor-1 bed in scenarios/bar_lure.kscen
cat > "$OUT/script.txt" <<EOS
400 select id=41191
401 move -50628.5 1081.9 -1185.4
1500 camera thing id=$BED
1502 set yaw 31.1
1503 set pitch -35
1504 set dist 234.4
1505 key R down
1506 key R up
1507 key R down
1508 key R up
1509 key R down
1510 key R up
1511 mouse pos 640 360
1512 dump pick
1513 screenshot $OUT/bed_floor3.png
1520 key F down
1521 key F up
1522 key F down
1523 key F up
1524 key F down
1525 key F up
1530 mouse pos 790 280
1532 dump pick
1533 screenshot $OUT/stairs_floor0.png
1540 mouse pos 700 420
1542 dump pick
1560 quit
EOS
HEADLESS_TIMEOUT=300 nice -n 10 "$HERE/headless.sh" -W 1280 -H 720 -- "$HERE/demo.sh" --port "$PORT" --scenario-file "$TREE/scenarios/bar_lure.kscen" -- --play bar_lure --script "$OUT/script.txt" > "$OUT/run.log" 2>&1 || true
grep '^pick (' "$OUT/run.log" > "$OUT/picks.txt" || true
cat "$OUT/picks.txt"
n=$(wc -l < "$OUT/picks.txt")
if [ "$n" = 3 ] && sed -n 1p "$OUT/picks.txt" | grep -q "kind 7 id $BED" && sed -n 2p "$OUT/picks.txt" | grep -q "kind 1 " && sed -n 3p "$OUT/picks.txt" | grep -q "kind 1 "
then echo "bed_pick_check: PASS (bed = use at the full level; stairs = walk at floor 0)"; exit 0; fi
echo "bed_pick_check: FAIL (expected bed, ground, ground)"; exit 1
