#!/bin/bash
# Every kind 12 machine picks and fills the inspect panel (kenshi-v31x; user 2026-11-22: "Pretty much every machine in the game is at LEAST clickable to
# fill out the bottom left inspection window"). The South Stone Camp (scenarios/machine_camp.kscen: the Wanderer at the camp, faction Slave Traders).
# Run 1 lists the INTERACTABLE kind 12 machines the sim sent (`dump things`: id, machine kind, the drawn mesh's pick box). Then two checks:
#  A. the panel: a left click on each machine (`mouse force ID`: the click picks that thing, whatever the camera sees) followed by `dump machine`, which names
#     the machine the bottom-left panel follows and prints its lines. FAILS when a machine's panel is not that machine's or has no name / status line.
#  B. the pick, from the player's RTS view (kenshi-wsbz; a camera DIST back, pitch PITCH, HEADS headings, aimed at FRACS of the height of the machine's drawn
#     (pickable) parts, so a marker volume or a wall top over the lower half does not decide it): a real left click at the screen centre, `dump pick`
#     says what the click met. A view is VISIBLE when the machine's own drawn mesh is the nearest thing along the ray (its instance heads `dump pick`'s
#     drawn list, no nearer than the world hit). FAILS when a visible view does not pick that machine (the click went to the ground, a wall or another
#     thing), and when a machine has no visible view at all it is listed as "hidden" (a wall or the terrain covers it from every heading; shoot it).
# Also FAILS when the camp sent fewer than MIN machines (the sim or the wire lost them).
# usage: machine_pick_check.sh OUT_DIR [PORT]   (PORT one of this worker's 47830-47839). MIN (default 10) is env. Exit 0 PASS, 1 FAIL.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: machine_pick_check.sh OUT_DIR [PORT]}
PORTARG=(); [ -n "${2:-}" ] && PORTARG=(--port "$2")
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
MIN=${MIN:-10}
SCEN="$TREE/scenarios/machine_camp.kscen"
run()	# SCRIPT LOG
{
	HEADLESS_TIMEOUT=600 nice -n 10 "$HERE/headless.sh" -W 1280 -H 720 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} --scenario-file "$SCEN" -- --debug --size 1280x720 --script "$1" > "$2" 2>&1 || true
}
printf '%s\n' "500 dump things" "510 quit" > "$OUT/script-list.txt"
run "$OUT/script-list.txt" "$OUT/run-list.log"
# "thing ID kind 12 ..." then "  machine kind K ... pbox lx ly lz hx hy hz" -> ID K centre-x centre-z
awk '/^thing [0-9]+ kind 12 / { id = $2 } /^  machine kind / && id { for (i = 1; i <= NF; i++) { if ($i == "kind") k = $(i + 1); if ($i == "pbox") { cx = ($(i + 1) + $(i + 4)) / 2; cz = ($(i + 3) + $(i + 6)) / 2 } } printf "%s %s %.1f %.1f\n", id, k, cx, cz; id = "" }' "$OUT/run-list.log" | sort -u > "$OUT/ids.txt"
n=$(wc -l < "$OUT/ids.txt")
if [ "$n" -lt "$MIN" ]; then echo "machine_pick_check: FAIL: the camp sent $n kind 12 machines (want >= $MIN; run-list.log)"; exit 1; fi
fail=0

# --- A: the panel, by a forced click on each machine
{
	f=400
	while read -r id k x z; do
		echo "$f mouse force $id"; echo "$((f+2)) mouse down left"; echo "$((f+3)) mouse up left"; echo "$((f+12)) dump machine"; echo "$((f+14)) mouse force 0"
		f=$((f+20))
	done < "$OUT/ids.txt"
	echo "$f quit"
} > "$OUT/script-panel.txt"
run "$OUT/script-panel.txt" "$OUT/run-panel.log"
grep -E '^(machine |  panel |  state )' "$OUT/run-panel.log" > "$OUT/result-panel.txt" || true
awk -v want="$OUT/ids.txt" '
BEGIN { while ((getline l < want) > 0) { split(l, a, " "); ids[++n] = a[1] } }
/^machine [0-9]+ / { cur = $2; named[cur] = 1; next }
/^  panel "[^"]+"/ { if (cur != "") { pn[cur] = $0; cur = "" } next }
END {
	bad = 0
	for (k = 1; k <= n; k++) {
		id = ids[k]; good = (id in named) && (id in pn) && pn[id] !~ /status ""/
		printf "%s %s: panel %s\n", good ? "ok  " : "FAIL", id, (id in pn) ? pn[id] : "EMPTY"
		if (!good) bad++
	}
	exit bad ? 1 : 0
}' "$OUT/result-panel.txt" > "$OUT/summary-panel.txt" || { echo "machine_pick_check: FAIL: a machine's click did not fill the panel:"; grep '^FAIL' "$OUT/summary-panel.txt"; fail=1; }
echo "panel: $(grep -c '^ok' "$OUT/summary-panel.txt") of $n machines fill the inspect panel on a left click"

# --- B: the pick, by the drawn mesh, from the RTS camera
# ids.txt columns: id kind centre-x centre-z of the DRAWN (pickable) parts' box (dbox), low y, high y
awk '/^thing [0-9]+ kind 12 / { id = $2 } /^  machine kind / && id { for (i = 1; i <= NF; i++) { if ($i == "kind") k = $(i + 1); if ($i == "dbox") { cx = ($(i + 1) + $(i + 4)) / 2; cz = ($(i + 3) + $(i + 6)) / 2; ylo = $(i + 2); yhi = $(i + 5) } } printf "%s %s %.1f %.1f %.1f %.1f\n", id, k, cx, cz, ylo, yhi; id = "" }' "$OUT/run-list.log" | sort -u > "$OUT/boxes.txt"
HEADS=${HEADS:-"0 90 180 270"}; PITCH=${PITCH:--50}; DIST=${DIST:-80}; FRACS=${FRACS:-"0.6"}; PIXS=${PIXS:-"0,0 -30,0 30,0 0,-30 0,30 -30,-30 30,30 -30,30 30,-30"}	# the click grid round the screen centre (px)
{
	f=400
	while read -r id k x z ylo yhi; do
		first=1
		for fr in $FRACS; do
			y=$(awk -v a="$ylo" -v b="$yhi" -v c="$fr" 'BEGIN { printf "%.1f", a + (b - a) * c }')
			for h in $HEADS; do
				echo "$f camera view $x $y $z $h $PITCH $DIST"
				[ $first = 1 ] && { f=$((f+50)); first=0; } || f=$((f+8))
				for px in $PIXS; do
					echo "$f mouse pos $((640 + ${px%,*})) $((360 + ${px#*,}))"; echo "$((f+3)) mouse down left"; echo "$((f+4)) mouse up left"; echo "$((f+8)) dump machine"; echo "$((f+9)) dump pick"
					echo "$id" >> "$OUT/views.txt.new"
					f=$((f+12))
				done
			done
		done
	done < "$OUT/boxes.txt"
	echo "$f quit"
} > "$OUT/script-pick.txt"
mv "$OUT/views.txt.new" "$OUT/views.txt"
run "$OUT/script-pick.txt" "$OUT/run-pick.log"
# a block per view: it starts at the `dump machine` line; the view's click result and the nearest drawn instance's props are read from `dump pick`
awk -v views="$OUT/views.txt" -v boxes="$OUT/boxes.txt" '
BEGIN { while ((getline l < views) > 0) { vid[++nv] = l } while ((getline l < boxes) > 0) { split(l, a, " "); kind[a[1]] = a[2]; order[++no] = a[1] } }
/^machine( |:)/ { n++; next }
n && /^pick \([0-9]+, [0-9]+\): kind / { for (i = 1; i <= NF; i++) if ($i == "id") got[n] = $(i + 1); kd[n] = $5; next }
n && /^pick world at t / { w[n] = $5 + 0; hasw[n] = 1; wb[n] = $NF; next }
n && /^pick drawn t / && !(n in seen) {
	seen[n] = 1; dt[n] = $4 + 0
	if (match($0, / props [^:]*:/)) { props[n] = " " substr($0, RSTART + 7, RLENGTH - 8) " " } else { props[n] = "" }
	next
}
END {
	for (v = 1; v <= nv; v++) {
		id = vid[v]; views_[id]++
		own = (v in props) && index(props[v], " " id " ") > 0 && (!hasw[v] || dt[v] <= w[v] + 0.05)
		if (own) { vis[id]++ }
		if (got[v] == id) { hit[id]++ }
		else if (own && hasw[v] && wb[v] == 0 && dt[v] > w[v] - 0.25) { contact[id]++ }	# the mesh only grazes the terrain it is embedded in (a campfire in the sand): not clearly in front, so not a miss
		else if (own) { miss[id]++; instead[id] = got[v] "(kind " kd[v] ")" }
	}
	bad = 0; nhid = 0
	for (k = 1; k <= no; k++) {
		id = order[k]
		if (miss[id]) { printf "FAIL %s kind %s: %d of %d visible views missed (the click met %s instead), %d picked\n", id, kind[id], miss[id], vis[id], instead[id], hit[id]; bad++ }
		else if (!vis[id]) { printf "hide %s kind %s: no heading shows its drawn mesh as the nearest thing (%d views)\n", id, kind[id], views_[id]; nhid++ }
		else { printf "ok   %s kind %s: picked in all %d visible views (of %d)\n", id, kind[id], vis[id], views_[id] }
	}
	printf "visible machines missed: %d, hidden from every heading: %d, of %d\n", bad, nhid, no
	exit bad ? 1 : 0
}' "$OUT/run-pick.log" > "$OUT/summary-pick.txt" || { echo "machine_pick_check: FAIL: a machine's drawn mesh is visible from the RTS camera and the click does not pick it:"; grep '^FAIL' "$OUT/summary-pick.txt"; fail=1; }
grep -E '^(hide|visible machines)' "$OUT/summary-pick.txt" || true
[ $fail = 0 ] && echo "machine_pick_check: PASS ($n machines: every panel fills, every visible machine picks from the RTS camera)" || { echo "machine_pick_check: FAIL (logs under $OUT)"; exit 1; }
