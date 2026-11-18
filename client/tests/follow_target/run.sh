#!/bin/bash
# kenshi-k16.2.293.1: one follow target. While the camera follows one of your characters (Z: the RTS camera centred on it):
#   - a plain portrait click makes that character the selection and the follow target;
#   - a Shift+click adds, and the target is the selection's first in squad order (tried with both click orders, then after the squads window's
#     drag puts another character first);
#   - wheel zoom, Q / E rotation and a middle-button orbit drag keep the follow; a pan key (W) ends it.
# The squad is the progress_bars scenario's squad 1: Wanderer, Lockpicker, Sawyer (their tasks keep them where they stand).
# Usage: run.sh OUT   (OUT: output dir). PORT (default: a free port), BUILD (default client/build of this tree).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
d=$out
{
	echo "100 key 1 down"
	echo "101 key 1 up"
	echo "110 key Z down"
	echo "112 key Z up"
	echo "130 follow state"			# S0: centred on the first (Wanderer)
	echo "135 dump chars"
	echo "140 portrait Lockpicker"
	echo "150 follow state"			# S1: Lockpicker
	echo "160 portrait add Sawyer"
	echo "170 follow state"			# S2: {Lockpicker, Sawyer}: Lockpicker
	echo "180 portrait Sawyer"
	echo "190 follow state"			# S3: Sawyer
	echo "200 portrait add Lockpicker"
	echo "210 follow state"			# S4: {Sawyer, Lockpicker} (click order reversed): still Lockpicker
	echo "220 squadfirst Sawyer"
	echo "300 portrait Lockpicker"
	echo "310 portrait add Sawyer"
	echo "320 follow state"			# S5: Sawyer now leads the squad
	echo "330 portrait Wanderer"
	echo "340 portrait add Sawyer"
	echo "350 follow state"			# S6: {Wanderer, Sawyer}: Sawyer (first place)
	echo "360 scroll 3"
	echo "365 key Q down"
	echo "385 key Q up"
	echo "390 mouse down middle"
	echo "391 mouse 40 5"
	echo "392 mouse up middle"
	echo "400 follow state"			# S7: still centred on Sawyer, rtsDist / yaw changed
	echo "410 key W down"
	echo "430 key W up"
	echo "440 follow state"			# S8: centre 0
	echo "450 quit"
} | awk '{ $1 += 400; print }' > "$d/script.txt"	# the sim may take a while to stream the squad in
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/progress_bars.kscen" --debug-view > "$d/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$d/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$d/server.log" $pid $port
(cd "$tree" && timeout 580 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/script.txt" > "$d/client.log" 2>&1)
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
mapfile -t st < <(grep -o "follow state: .*" "$d/client.log")
fail=0
ids=$(grep -o "[0-9]* \"\(Wanderer\|Lockpicker\|Sawyer\)\"" "$d/client.log" | head -0)
cen() { echo "${st[$1]}" | sed -n 's/.* center \([0-9]*\) .*/\1/p'; }
sel() { echo "${st[$1]}" | sed -n 's/.*selected \(.*\)/\1/p'; }
# the ids: the S1 / S3 / S6 centres name Lockpicker / Sawyer; S0 names Wanderer
W=$(cen 0); L=$(cen 1); S=$(cen 3)
chk() { if [ "$2" = "$3" ] && [ -n "$3" ]; then echo "PASS $1"; else echo "FAIL $1: got '$2' want '$3'"; fail=1; fi; }
[ ${#st[@]} = 9 ] || { echo "FAIL expected 9 state lines, got ${#st[@]}"; exit 1; }
echo "ids: Wanderer $W Lockpicker $L Sawyer $S"
chk "click B: follow B" "$(cen 1)" "$L"
chk "click B, shift C: first in squad order (B)" "$(cen 2)" "$L"
chk "click C: follow C" "$(cen 3)" "$S"
chk "click C, shift B (reversed): still B" "$(cen 4)" "$L"
chk "Sawyer first in the squad: click B, shift C -> C" "$(cen 5)" "$S"
chk "click A, shift C -> C (first place)" "$(cen 6)" "$S"
chk "wheel, rotate, orbit drag: still C" "$(cen 7)" "$S"
chk "pan key: follow off" "$(cen 8)" "0"
y6=$(echo "${st[6]}" | sed -n 's/.* yaw \(-\?[0-9.]*\) .*/\1/p'); y7=$(echo "${st[7]}" | sed -n 's/.* yaw \(-\?[0-9.]*\) .*/\1/p')
d6=$(echo "${st[6]}" | sed -n 's/.* rtsDist \([0-9.]*\) .*/\1/p'); d7=$(echo "${st[7]}" | sed -n 's/.* rtsDist \([0-9.]*\) .*/\1/p')
if [ "$y6" != "$y7" ] && [ "$d6" != "$d7" ]; then echo "PASS the zoom and rotation did act (yaw $y6 -> $y7, dist $d6 -> $d7)"; else echo "FAIL zoom / rotation did nothing (yaw $y6 -> $y7, dist $d6 -> $d7)"; fail=1; fi
exit $fail
