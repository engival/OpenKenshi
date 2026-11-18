#!/bin/bash
# pausetoggle (kenshi-k16.2.180.1; the player verdict kenshi-k16.2.192): run.sh OUT. bar_lure, the Wanderer followed; pause toggled every
# 0.5 s for 10 toggles standing, then 20 mid-walk (two `move` world-target orders south and west of the Hub), then 10 standing again
# (toggle.txt). `log pos all` writes every drawn character's sim position, drawn position, render clock tick and newest snapshot tick per
# frame to OUT/client.log, so a pause that moves or snaps a character shows on its drawn step (that character's move since its previous
# logged frame). The run ends with, per character, the largest step while paused and while running, then the rock count: the frames where
# the drawn stepped against the sim's own move that frame (a back-and-forth). The followed player (taken from `dump chars`, the character
# the script names) must also have walked the route the two move orders ask for: without a walk the paused counts only cover standing
# characters, so the summary ends with its walked distance and a PASS/FAIL (the exit code), and the server's order log (`--order-log`,
# OUT/orders.log) must hold the two `move_to` orders for that id.
# A walk is ~0.25-1 u a rendered frame here (the headless run's frame is longer than the script's 1/60 s), so a snap reads well over it.
# Shots: stand_start.png / walk_end.png (OUT). Port: PORT (47835).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
sed "s#OUTDIR#$out#g" "$here/toggle.txt" > "$out/script.txt"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/bar_lure.kscen" \
	--navworld "${KENSHI_NAVWORLD:?set KENSHI_NAVWORLD to a navworld bake dir}" --order-log "$out/orders.log" > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
cd "$tree" && nice -n 10 timeout 400 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug \
	--script "$out/script.txt" > "$out/client.log" 2>&1
# the order log is written when the server stops (a SIGTERM stop, the server tool cpp/tools/ksim_server.cpp): wait for it to exit, not just for the signal
if ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port"; then kill "$pid"; for i in $(seq 100); do ps -p $pid > /dev/null || break; sleep 0.1; done; fi
echo "sg errors: $(grep -c '\[sg\]\[error\]' "$out/client.log")"
echo "pos lines: $(grep -c '^pos ' "$out/client.log")"
# fields: 1 pos, 2 t, 3 dt, 4 dtValue, 5 id, 6 idValue, 7 tick, 8 tickValue, 9 newest, 10 newestValue, 11 paused, 12 pausedValue,
# 13 sim, 14..16 sim x/y/z, 17 drawn, 18..20 drawn x/y/z, 21 step, 22 stepValue (pausedValue 1 = paused; 0 = running)
# `step` is a character's drawn move since its own previous logged frame. While paused the clock and the sim freeze, so every paused
# frame after the first must be 0.000; the first paused frame still carries the last running frame's move (the pause lands after that
# interval), so it is reported apart. A rock is a frame where the drawn stepped against the sim's own move that frame.
# The followed character: `dump chars` prints one `char ID "NAME" ...` line per drawn character; the script follows the Wanderer.
player=$(awk '/^char / && /"Wanderer"/ { print $2; exit }' "$out/client.log")
echo "player: ${player:-none} (the Wanderer, the followed character)"
awk -v player="${player:-0}" -f "$here/summary.awk" "$out/client.log"
rc=$?
# the two walk orders, from the server's own log (only orders that passed its checks are logged): each must name the player
moves=$(grep -c " sim move_to ids=${player:-0} " "$out/orders.log" 2>/dev/null)
echo "walk orders to ${player:-none}: ${moves:-0} in orders.log (two are asked for, at frames 520 and 840)"
[ "${moves:-0}" -ge 2 ] || rc=1
echo "first frame: $(grep -m1 '^first_world_frame' "$out/client.log")"
exit $rc
