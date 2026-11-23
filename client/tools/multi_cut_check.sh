#!/bin/bash
# Every building holding a player character is cut at the selected floor, whichever squad is selected (kenshi-k16.2.332.1; user 2026-11-17, the
# original's GOG shot: "Player characters are in TWO buildings (they're not even in the same squad). With floor selected at 0, both buildings are cut
# open. This is the correct behavior"). scenarios/multi_cut.kscen: the Wanderer (squad 1) and the Drifter (squad 2), each in one of the two Heft Storm
# Houses (two-storey), floor selector 0. Three headless runs with --debug: nothing selected by the script (the default selection), the Wanderer
# selected, the Drifter selected. Each: `dump cuts` lists BOTH buildings at floor 0 (shown 0, top 1) with 2 occupants, a shot from above shows both
# open, and `dump pick` of a click at the centre of the NON-selected character's house, camera over its upper floor, says the pick skipped what is
# above the floor cut of a building ours are in (the cut must not follow the selection; peek / glass rules are not touched).
# Exit 0 PASS, 1 FAIL.  usage: multi_cut_check.sh OUT_DIR [PORT]   (PORT in this worker's 47830-47839 range; without PORT demo.sh picks a free port per run)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: multi_cut_check.sh OUT_DIR [PORT]}
PORTARG=(); [ -n "${2:-}" ] && PORTARG=(--port "$2")	# no PORT: demo.sh picks a free one and retries on a bind failure (kenshi-k16.2.417.3)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
fail() { echo "multi_cut_check: FAIL ($*)"; exit 1; }

# The Wanderer's house (A) and the Drifter's (B) in the scenario; the camera looks down at the house of the one NOT selected.
A_POS=68240,-70797
B_POS=68445,-70717
run() {	# run NAME SELECT-LINE PICK-HOUSE-POS
	local name=$1 sel=$2 at=$3
	cat > "$OUT/script-$name.txt" <<EOS
400 dump chars
$sel
500 key F down
501 key F up
502 key F down
503 key F up
510 dump cuts
511 dump floor
520 set pos 68343,-70757
521 set alt 330
522 set yaw 0
523 set pitch -80
540 screenshot $OUT/$name-both.png
550 set pos $at
551 set alt 120
552 set pitch -60
560 mouse pos 640 360
561 dump pick
562 dump cuts
570 screenshot $OUT/$name-pick.png
600 quit
EOS
	HEADLESS_TIMEOUT=600 nice -n 10 "$HERE/headless.sh" -W 1280 -H 720 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} \
		--scenario-file "$TREE/scenarios/multi_cut.kscen" -- --size 1280x720 --script "$OUT/script-$name.txt" > "$OUT/run-$name.log" 2>&1 || true
	grep -q 'simlink: server: HELLO timeout' "$OUT/run-$name.log" && fail "$name: the server never said HELLO (kenshi-k16.2.268); rerun when the machine is quiet"
	grep -E '^(cuts|cut |floor: .* shown|pick)' "$OUT/run-$name.log" > "$OUT/$name.txt" || true
}

run default "" "$B_POS"
WAN=$(sed -n 's/^char \([0-9]*\) "Wanderer".*/\1/p' "$OUT/run-default.log" | head -1)
DRI=$(sed -n 's/^char \([0-9]*\) "Drifter".*/\1/p' "$OUT/run-default.log" | head -1)
[ -n "$WAN" ] && [ -n "$DRI" ] || fail "the Wanderer / Drifter are not on the wire (dump chars)"
run wanderer "450 select id=$WAN" "$B_POS"
run drifter "450 select id=$DRI" "$A_POS"

for n in default wanderer drifter; do
	echo "== $n"; cat "$OUT/$n.txt"
	f="$OUT/$n.txt"
	grep -qE '^cuts: 2 level 0 occupants 2$' "$f" || fail "$n: dump cuts does not say 2 cuts at level 0 with 2 occupants"
	[ "$(grep -cE '^cut [0-9]+ "Storm House" shown 0 top 1$' "$f")" -ge 2 ] || fail "$n: both Storm Houses are not listed cut at floor 0"
	[ "$(grep -E '^cut [0-9]+ ' "$f" | awk '{print $2}' | sort -u | wc -l)" = 2 ] || fail "$n: the two cut buildings are not distinct instances"
	grep -qE '^pick \(640, 360\): ' "$f" || fail "$n: the click picked nothing"
	grep -qE '^pick skipped ".*" at .*: above the floor cut of Storm House' "$f" || fail "$n: the upper floor of the unselected house is not named as above the floor cut"
	[ -f "$OUT/$n-both.png" ] || fail "$n: no screenshot"
done
echo "multi_cut_check: PASS (both Storm Houses cut at floor 0 with the default, the Wanderer's and the Drifter's squad selected; the unselected one's upper floor is above the cut)"
