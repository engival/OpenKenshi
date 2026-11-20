#!/bin/bash
# Paused motion check (kenshi-h2v.23.1; user 2026-11-11, bar_lure v2 playtest: "The game is paused. Those bandits are shimmering in
# place. The client keeps correcting their location. Does the trails log that?"): run a fight, pause it with SPACE, and show whether
# the drawn characters move. The render clock must hold while the sim is paused, and the debug trails must record the drawn centres so
# a regression shows up in their .tsv.
# usage: paused_motion_check.sh OUT_DIR [SCENARIO] [PORT]
#   OUT_DIR gets server.log, client.log, check.txt (the generated client script) and fight_before_pause, paused_p1s, paused_p10s,
#   paused_p30s, resumed (.png + .trails.tsv each). client.log holds one `dump wire r` line per shot (each fighter's drawn position to
#   0.01 u), and the run ends with per-file and per-character lines: the drawn centre's move between the three paused shots and how many
#   drawn points each paused trail kept at its paused t beyond the last live frame's (a handful: the hold's one settle; under the old
#   clock the shimmer's, and the move between +1 s and +30 s grows).
#   SCENARIO defaults to scenarios/spectate_fight.kscen (two NPC patrols, nothing ordered); bar_lure_hostile.kscen works too.
#   PORT must be one of this worker's (47830-47839). +1 s / +10 s / +30 s are 60 / 600 / 1800 frames at 60 fps (headless runs 50-60).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CLIENT=$(cd "$HERE/.." && pwd)	# the client tree (client/)
TREE=$(cd "$CLIENT/.." && pwd)	# the kenshi tree (its cpp/ builds the server)
OUT=${1:?usage: paused_motion_check.sh OUT_DIR [SCENARIO] [PORT]}
SCENARIO=${2:-scenarios/spectate_fight.kscen}
. "$(dirname "${BASH_SOURCE[0]}")/ports.sh"
WANT=${3:-}
case "$SCENARIO" in /*) ;; *) SCENARIO=$TREE/$SCENARIO ;; esac
. "$HERE/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$TREE" "$CLIENT/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
[ -x "$SERVER" ] || [ "$SHARED" = 1 ] || SERVER=$TREE/cpp/build/OpenKenshi-server	# a cpp-only build tree (no client/build wrapper)
KCLIENT=$CLIENT/build/kclient
[ -x "$KCLIENT" ] || { echo "paused_motion_check: no $KCLIENT (build target kclient)" >&2; exit 2; }
[ -x "$SERVER" ] || { echo "paused_motion_check: no OpenKenshi-server (build target ksim_server)" >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
SCRIPT=$OUT/check.txt
# The client script. The fight is brought on at 2x (the patrols are 224 u apart in spectate_fight), then 1x, a shot of it moving, SPACE,
# the three paused shots, SPACE again, one more. `set animate 1`: poses animate too (a scripted run freezes them otherwise).
cat > "$SCRIPT" <<EOF
5 set animate 1
10 trails all
12 trails window 0
20 set pos -50730,-1040
24 set alt 85
28 set yaw 90
32 set pitch -9
60 set simspeed 2
600 set simspeed 1
605 dump wire r
610 screenshot $OUT/fight_before_pause.png
640 key SPACE down
644 key SPACE up
695 dump wire r
700 screenshot $OUT/paused_p1s.png
1235 dump wire r
1240 screenshot $OUT/paused_p10s.png
2435 dump wire r
2440 screenshot $OUT/paused_p30s.png
2480 key SPACE down
2484 key SPACE up
2595 dump wire r
2600 screenshot $OUT/resumed.png
2660 quit
EOF
START_PORT=$WANT start_server_on_free_port "$OUT/server.log" env -C "$TREE" "$SERVER" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen "127.0.0.1:@PORT@" --scenario-file "$SCENARIO" \
	--navworld "${KENSHI_NAVWORLD:?set KENSHI_NAVWORLD to a navworld bake dir}" --bake "$KENSHI_NAVWORLD"
pid=$SERVER_PID; PORT=$SERVER_PORT
trap 'kill $pid 2>/dev/null || true' EXIT
nice -n 10 "$HERE/headless.sh" -- "$KCLIENT" --debug --simlink "127.0.0.1:$PORT" --script "$SCRIPT" > "$OUT/client.log" 2>&1 || true
kill $pid 2>/dev/null || true
trap - EXIT
echo "paused_motion_check: [sg][error] lines: $(grep -c '\[sg\]\[error\]' "$OUT/client.log" || true)"
for f in paused_p1s paused_p10s paused_p30s; do
	[ -f "$OUT/$f.trails.tsv" ] || { echo "paused_motion_check: no $f.trails.tsv (the run ended early: see client.log)" >&2; exit 4; }
done
# The drawn centre (the tsv's "drawn" track). While paused the trail clock t_ holds, so every point recorded during the pause shares the
# file's largest t: their count and biggest step are the movement the pause showed. Then each track's last point across the three paused
# shots (0.000 when the clock held). The pre-pause shot is the reference the paused ones extend.
awk -F'\t' '
	FNR == 1 { next }
	$5 != "drawn" { next }
	{
		f = FILENAME; i = ++rows[f]
		rid[f, i] = $1; rt[f, i] = $3 + 0; rx[f, i] = $6 + 0; ry[f, i] = $7 + 0; rz[f, i] = $8 + 0
		last[f, $1] = $6 " " $7 " " $8
		ids[$1] = 1
		files[f] = 1
		if (!(f in maxt) || $3 + 0 > maxt[f]) { maxt[f] = $3 + 0 }
	}
	END {
		printf "drawn points at the largest t of each file (those of the last live frame are among them; the excess is motion during the pause), and their biggest step:\n"
		for (f in files) {
			if (f ~ /fight_before_pause/) { continue }
			added = 0; big = 0
			for (i = 1; i <= rows[f]; i++) {
				id = rid[f, i]
				if (rt[f, i] >= maxt[f] - 1e-9) {
					added++
					if ((f, id) in lt) {
						d = sqrt((rx[f, i] - lx[f, id])^2 + (ry[f, i] - ly[f, id])^2 + (rz[f, i] - lz[f, id])^2)
						if (d > big) { big = d }
					}
				}
				lx[f, id] = rx[f, i]; ly[f, id] = ry[f, i]; lz[f, id] = rz[f, i]; lt[f, id] = 1
			}
			printf "  %s: %d points at t %.4f, biggest step %.3f u\n", f, added, maxt[f], big
		}
		printf "drawn centre, the last point per character, across the three paused shots:\n"
		for (id in ids) {
			line = sprintf("  char %s", id)
			p = ""
			worst = 0
			for (f in files) {
				if (f ~ /fight_before_pause/) { continue }
				if (!((f, id) in last)) { line = line "  (none)"; continue }
				split(last[f, id], v, " ")
				line = line sprintf("  (%.3f, %.3f, %.3f)", v[1], v[2], v[3])
				if (p == "") { p = last[f, id]; continue }
				split(p, a, " ")
				d = sqrt((a[1] - v[1])^2 + (a[2] - v[2])^2 + (a[3] - v[3])^2)
				if (d > worst) { worst = d }
			}
			printf "%s  max move %.3f\n", line, worst
		}
	}' "$OUT"/fight_before_pause.trails.tsv "$OUT"/paused_p1s.trails.tsv "$OUT"/paused_p10s.trails.tsv "$OUT"/paused_p30s.trails.tsv
