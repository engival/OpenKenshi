#!/bin/bash
# The view never jumps when the followed / centred body's drawn position steps (kenshi-k16.2.474.1; user 2026-11-21, Guy with a Dog: "they run
# to their target spot, then the entire screen jumps as they nudge each other": a sim shove of 9.8 u in 56 ms on the followed character,
# kenshi-k16.6.219). order_sounds' first player character is walked to a spot and stands; then, in each camera mode (the follow camera, the RTS
# auto-centred camera), `debug nudge 10 0 <name>` steps its drawn position 10 u in one frame (the wire correction's shape: the same step
# SimView::nudge makes, then the drawn offset eases back). KCLIENT_CAMTRACE prints the eye per frame (camtrace: lines).
# PASS: in each mode's window (the nudge frame .. +90 frames) no per-frame eye step above kMax = 3.0 u (the body stands, so any step is the
# correction's; a 70 u/s sprint is 1.2 u a frame; on the base the eye steps the whole 10 u in the nudge frame). The nudge must have hit.
# Settle off (look.settle_tau 0 = the base behaviour) must FAIL: pass SETTLE_TAU=0 to see it (the check then exits 1).
# Exit 0 PASS, 1 FAIL.  usage: camera_settle_check.sh OUT_DIR [PORT]   (PORT in 47830-47839)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: camera_settle_check.sh OUT_DIR [PORT]}
. "$HERE/ports.sh"
PORT=$(pick_port "${2:-}") || exit 3
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
MAX=3.0
LAGMAX=${LAGMAX:-3.0}
cat > "$OUT/script.txt" <<EOS
10 set animate 1
200 key 1 down
201 key 1 up
260 move -50560 -1360
900 follow on
1000 debug nudge 10 0 Wanderer
1200 follow off
1202 key Z down
1204 key Z up
1220 follow state
1300 debug nudge 10 0 Wanderer
1500 follow on
1510 set simspeed 5
1520 move -50560 -300
2200 set simspeed 1
2060 debug nudge 10 0 Wanderer
2300 quit
EOS
SETTLE=${SETTLE_TAU:+--set look.settle_tau=$SETTLE_TAU}
HEADLESS_TIMEOUT=300 KCLIENT_CAMTRACE=1 nice -n 10 "$HERE/headless.sh" -W 1280 -H 720 -- "$HERE/demo.sh" --port "$PORT" \
	--scenario-file "$TREE/scenarios/order_sounds.kscen" -- --size 1280x720 --script "$OUT/script.txt" ${SETTLE:-} > "$OUT/run.log" 2>&1 || true
grep -q 'simlink: server: HELLO timeout' "$OUT/run.log" && { echo "camera_settle_check: FAIL (HELLO timeout: rerun when the machine is quiet)"; exit 1; }
grep -E '^(camtrace|nudge|follow state)' "$OUT/run.log" > "$OUT/trace.txt" || true
fail() { echo "camera_settle_check: FAIL ($*)"; exit 1; }
[ "$(grep -c '^nudge: [1-9]' "$OUT/trace.txt")" -ge 2 ] || fail "the nudge hit no character (twice wanted)"
# a window starts at each nudge line: the frames printed after it
awk -v max="$MAX" '
	/^nudge:/ { w++; n = 0; next }
	/^camtrace:/ {
		if (w == 0 || n++ > 90) { px = $9; py = $10; pz = $11; have = 1; next }
		if (have) { d = sqrt(($9 - px) ^ 2 + ($11 - pz) ^ 2 + ($10 - py) ^ 2); if (d > mx[w]) { mx[w] = d; at[w] = $3 } }
		px = $9; py = $10; pz = $11; have = 1; mode[w] = $5
	}
	END { bad = 0; for (i = 1; i <= w; i++) { printf "window %d (%s): max eye step %.3f u at frame %s\n", i, mode[i], mx[i], at[i]; if (mx[i] > max) bad = 1 } exit bad }' "$OUT/trace.txt" > "$OUT/steps.txt" || { cat "$OUT/steps.txt"; fail "an eye step above $MAX u"; }
cat "$OUT/steps.txt"
# the sprint at x5 (no nudge): the view's slack (the part of the body's drawn step the view has not followed yet: camtrace's slack field) stays small
# in the walk. A constant per-frame allowance (100 u per REAL second) lets a x5 sprint (5.8 u a frame) pile up ~60 u of slack for the whole walk: the view
# trails the body. Wanted: the MEDIAN slack over the frames the body moves (> 1 u a frame) in 1530..2040 <= LAGMAX u (a real catch-up after a wire gap
# is eased on purpose and may peak higher for a few frames); the body must have walked 400 u.
awk -v lm="$LAGMAX" '/^camtrace:/ && $3 >= 1530 && $3 <= 2040 && $5 == "follow" { if (!f) { f = 1; x0 = $13; z0 = $14 } else if (sqrt(($13 - px) ^ 2 + ($14 - pz) ^ 2) > 1) { v[++n] = $19 + 0 } px = $13; pz = $14; x1 = $13; z1 = $14 }
	END { m = asort(v); w = sqrt((x1 - x0) ^ 2 + (z1 - z0) ^ 2); med = n ? v[int((n + 1) / 2)] : 0; printf "sprint x5: walked %.0f u, view slack median %.2f u, max %.2f u over %d moving frames (median wanted <= %.1f)\n", w, med, v[n], n, lm; exit !(n > 40 && w > 400 && med <= lm) }' "$OUT/trace.txt" > "$OUT/lag.txt" || { cat "$OUT/lag.txt"; fail "the view trails the sprint at x5 (or it never walked)"; }
cat "$OUT/lag.txt"
grep -q 'mode rts center [0-9]' "$OUT/trace.txt" || fail "Z did not centre the RTS camera"
echo "camera_settle_check: PASS (no per-frame eye step above $MAX u after a 10 u drawn step, follow and RTS-centred)"
