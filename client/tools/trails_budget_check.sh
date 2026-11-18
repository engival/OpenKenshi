#!/bin/bash
# Trails budget check (kenshi-k16.19.6.1): the k16.19.6 bug was the debug trails' unbounded draw filling the default sgl context, after which
# sokol-gl dropped every later command of the frame without a word: the HUD, the minimap, the name labels and the windows (the trade grid) vanished.
# This runs a 64-character fight (scenarios/trails_mob.kscen) with the trails on at simspeed 5 until every track is full, and checks the fix:
# the trails record into their own sgl context (923,392 vertices at the 30 s default), only the newest debug.trails_seconds are drawn, the
# default context keeps its 262,144 vertices for the HUD/minimap/windows, and an overflow (if one happened) logs `sgl: vertices full in the
# <context> context` once per change of state instead of losing the frame silently.
# usage: trails_budget_check.sh OUT_DIR [SCENARIO] [PORT]
#   OUT_DIR gets server.log, client.log, check.txt (the generated client script), fight_early/fight_mid/fight_full (.png + a `dump trails` line
#   before each) and data/kclient/trails/ (debug.trails_dump's segment files: the points that dropped out of memory, ~28 MB a minute at 64
#   characters). SCENARIO defaults to scenarios/trails_mob.kscen (64 fighters); spectate_fight.kscen works too (8). PORT must be 47830-47839.
#   The server needs no navworld bake (it builds its runtime navmesh from the shipped navtiles); KENSHI_NAVWORLD=DIR, if set, is passed as --navworld.
#   Exit: 0 the trails stayed inside their context and nothing dropped out of the default one; 1 an sgl context filled (see the log line);
#   2/3/4 setup, server or shot failure.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CLIENT=$(cd "$HERE/.." && pwd)	# the client tree (client/)
TREE=$(cd "$CLIENT/.." && pwd)	# the kenshi tree (its cpp/ builds the server)
OUT=${1:?usage: trails_budget_check.sh OUT_DIR [SCENARIO] [PORT]}
SCENARIO=${2:-scenarios/trails_mob.kscen}
. "$(dirname "${BASH_SOURCE[0]}")/ports.sh"
PORT=$(pick_port "${3:-}") || exit 3
case "$SCENARIO" in /*) ;; *) SCENARIO=$TREE/$SCENARIO ;; esac
. "$HERE/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$TREE" "$CLIENT/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
KCLIENT=$CLIENT/build/kclient
[ -x "$KCLIENT" ] || { echo "trails_budget_check: no $KCLIENT (build target kclient)" >&2; exit 2; }
[ -x "$SERVER" ] || { echo "trails_budget_check: no OpenKenshi-server (build target ksim_server)" >&2; exit 2; }
[ -f "$SCENARIO" ] || { echo "trails_budget_check: no scenario $SCENARIO" >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
export XDG_DATA_HOME=$OUT/data	# the dump segments land under OUT/data/kclient/trails (never in the repo)
SCRIPT=$OUT/check.txt
# The client script: the trails on over the whole field, the camera over the fight (the paused_motion_check view), 5x until the tracks fill
# (a track holds 2048 points: ~102 game seconds at the decimation's 20 Hz top rate), a shot early, mid and full; `dump trails` before each
# prints the kept/drawn counts and the last draw's vertex count.
cat > "$SCRIPT" <<EOF
5 set animate 1
6 trails all
7 set debug.trails_dump 1
8 dump trails
12 trails window 0
20 set pos -50000,-760
24 set alt 170
28 set yaw 0
32 set pitch -38
40 set simspeed 5
360 set simspeed 1
600 dump trails
604 screenshot $OUT/fight_early.png
2400 dump trails
2404 screenshot $OUT/fight_mid.png
5200 trails window 60
5210 dump trails
5260 dump trails
5264 screenshot $OUT/fight_full.png
5340 quit
EOF
(cd "$TREE" && exec "$SERVER" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen "127.0.0.1:$PORT" --scenario-file "$SCENARIO" \
	${KENSHI_NAVWORLD:+--navworld "$KENSHI_NAVWORLD"}) > "$OUT/server.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT
for i in $(seq 240)
do
	grep -q '^listening ' "$OUT/server.log" && break
	kill -0 $pid 2>/dev/null || { echo "trails_budget_check: server died:" >&2; tail -5 "$OUT/server.log" >&2; exit 3; }
	sleep 0.5
done
grep -q '^listening ' "$OUT/server.log" || { echo "trails_budget_check: server not listening on $PORT" >&2; exit 3; }
nice -n 10 "$HERE/headless.sh" -- "$KCLIENT" --debug --simlink "127.0.0.1:$PORT" --script "$SCRIPT" > "$OUT/client.log" 2>&1 || true
kill $pid 2>/dev/null || true
trap - EXIT
grep -c '\[sg\]\[error\]' "$OUT/client.log" > "$OUT/sg-errors.txt" || true
echo "trails_budget_check: [sg][error] lines: $(cat "$OUT/sg-errors.txt")"
echo "trails_budget_check: sgl lines:"
grep '^sgl: ' "$OUT/client.log" || echo "  (none)"
echo "trails_budget_check: trails status lines:"
grep '^trails: ' "$OUT/client.log" || true
DUMP=$OUT/data/kclient/trails
if [ -d "$DUMP" ] && compgen -G "$DUMP/*.tsv" > /dev/null
then
	first=$(compgen -G "$DUMP/*.tsv" | sort | head -1)
	{ ls -l "$DUMP"; echo; head -5 "$first"; } > "$OUT/dump.txt"
	echo "trails_budget_check: dump segments under $DUMP:"
	ls -l "$DUMP"
	echo "trails_budget_check: head of $(basename "$first"):"
	head -5 "$first"
else
	echo "trails_budget_check: no dump segment files under $DUMP (the run ended before a track filled?)" >&2
fi
for f in fight_early fight_mid fight_full
do
	[ -f "$OUT/$f.png" ] || { echo "trails_budget_check: no $f.png (the run ended early: see client.log)" >&2; exit 4; }
done
if grep -q '^sgl: .* in the .* context' "$OUT/client.log"
then
	echo "trails_budget_check: FAIL: an sgl context filled (the fix must keep the trails inside their own budget)" >&2
	exit 1
fi
echo "trails_budget_check: ok: no sgl context filled"
