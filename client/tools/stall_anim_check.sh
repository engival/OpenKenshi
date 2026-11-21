#!/bin/bash
# Stall animation check (kenshi-k16.2.479.1; user 2026-11-20: "they freeze frames right along with the frozen ticks"): a fight's two patrols walk
# at each other; the script holds the snapshots (`set stall 1`, dev-only) for ~1 s mid-walk at x1 and again at x5, and `log gait all` writes one
# line per character per frame (the drawn x z, the locomotion phase, the render tick). The animation must hold through a stall (phase and drawn
# position unchanged) and carry on from there. gait.txt keeps the "gait" lines; the summary gives, per stall window, the biggest phase and position
# change between consecutive frames inside it, and the biggest jump in the 5 frames after it.
# usage: stall_anim_check.sh OUT_DIR [PORT]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CLIENT=$(cd "$HERE/.." && pwd)
TREE=$(cd "$CLIENT/.." && pwd)
OUT=${1:?usage: stall_anim_check.sh OUT_DIR [PORT]}
SCENARIO=$TREE/scenarios/spectate_fight.kscen
. "$HERE/ports.sh"
WANT=${2:-}
. "$HERE/server_bin.sh"
pick_server "$TREE" "$CLIENT/build/cpp/OpenKenshi-server"
KCLIENT=$CLIENT/build/kclient
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SCRIPT=$OUT/check.txt
cat > "$SCRIPT" <<EOS
5 set animate 1
20 set pos -50730,-1040
24 set alt 85
28 set yaw 90
32 set pitch -9
40 log gait all
300 set stall 1
600 set stall 0
750 set simspeed 5
1000 set stall 1
1200 set stall 0
1400 set simspeed 1
1500 quit
EOS
START_PORT=$WANT start_server_on_free_port "$OUT/server.log" env -C "$TREE" "$SERVER" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen "127.0.0.1:@PORT@" --scenario-file "$SCENARIO" \
	--navworld "${KENSHI_NAVWORLD:?set KENSHI_NAVWORLD to a navworld bake dir}" --bake "$KENSHI_NAVWORLD"
pid=$SERVER_PID; PORT=$SERVER_PORT
trap 'kill $pid 2>/dev/null || true' EXIT
timeout 300 nice -n 10 "$HERE/headless.sh" -- "$KCLIENT" --debug --simlink "127.0.0.1:$PORT" --script "$SCRIPT" > "$OUT/client.log" 2>&1 || true
kill $pid 2>/dev/null || true; trap - EXIT
grep '^gait ' "$OUT/client.log" > "$OUT/gait.txt" || true
echo "gait lines: $(wc -l < "$OUT/gait.txt"); [sg][error]: $(grep -c '\[sg\]\[error\]' "$OUT/client.log" || true)"
