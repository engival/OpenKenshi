#!/bin/bash
# kenshi-eq2z: control hints step 8b, the key rows. With ui.control_hints on, the pointer is put on a HUD widget (`mouse hud ...`) and `hints shown`
# prints what the panel lists there (the widget's own lines, then the key rows from the bindings as they are now): a portrait, the pause and speed
# buttons, the floor and move speed diamonds and BUILD; then `set key.speed_2 F9` / `key.focus_char X` rebinds and the lists are read again (the new key
# shows, the old one is gone), and `set key.speed_3 ""` unbinds (no row). expected.txt is the whole `hints shown` output.
# Usage: run.sh OUT   (OUT: output dir). PORT (default 47832), BUILD (default client/build of this tree).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../../../tools/ports.sh"	# server_listening
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
port=${PORT:-47832}
build=${BUILD:-$tree/client/build}
. "$tree/client/tools/server_bin.sh"	# pick_server
pick_server "$tree" "$build/cpp/OpenKenshi-server"
server=$SERVER
d=$out
{
	echo "60 set ui.control_hints true"
	n=100
	step() { echo "$n $*"; n=$((n + 6)); echo "$n hints shown"; n=$((n + 6)); }
	step mouse hud portrait 0
	step mouse hud pause
	step mouse hud speed 2
	step mouse hud floor
	step mouse hud movespeed
	step mouse hud build
	echo "$n set key.speed_2 F9"; n=$((n + 6))
	echo "$n set key.focus_char X"; n=$((n + 6))
	echo "$n set key.speed_3 none"; n=$((n + 6))
	step mouse hud speed 2
	step mouse hud speed 3
	step mouse hud portrait 0
	echo "$n quit"
} | awk '{ $1 = $1 * 4 + 400; print }' > "$d/script.txt"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/progress_bars.kscen" --debug-view > "$d/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$d/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$d/server.log" "$pid" "$port"
(cd "$tree" && timeout 300 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/script.txt" > "$d/client.log" 2>&1)
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
grep -E "^(hints shown: |  [^\"]+  \")" "$d/client.log" > "$d/actual.txt"
if diff -u "$here/expected.txt" "$d/actual.txt" > "$d/diff.txt"; then echo "PASS control_hints/keys"; exit 0; fi
cat "$d/diff.txt"; echo "FAIL control_hints/keys"; exit 1
