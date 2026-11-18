#!/bin/bash
# kenshi-k16.2.294.4: control hints step 1, portraits. For each state the portrait's hint list (`hints portrait NAME`) is checked against
# expected.txt, and each gesture is then played (`portrait [add|dbl] NAME`) and the logged action kind must match the hint's kind:
#   alone: nothing selected or centred; others: Wanderer selected (Shift lists add, a selected one lists remove); following: the RTS camera
#   centred on Wanderer (labels name the follow target, first in squad order); driving: direct drive on Wanderer (`follow 1`... via key V).
# expected.txt names the characters @Wanderer@ / @Lockpicker@ / @Sawyer@: charids.sh fills their wire ids in from a probe run before the diff (exit 4 when one is missing).
# Usage: run.sh OUT   (OUT: output dir). PORT (default 47831), BUILD (default client/build of this tree).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../../charids.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../../../tools/ports.sh"	# server_listening
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
port=${PORT:-47831}
build=${BUILD:-$tree/client/build}
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
d=$out
{
	echo "100 key W down"				# a pan key frees the camera: no follow at the start
	echo "110 key W up"
	echo "120 portrait Wanderer"			# click: kind select
	echo "130 hints portrait Wanderer"		# alone: only Wanderer selected, camera free
	echo "140 hints portrait Lockpicker"		# others: Lockpicker is not in the selection
	echo "150 portrait add Lockpicker"		# shift: select_add
	echo "160 hints portrait Lockpicker"		# in the selection now: remove
	echo "170 portrait Wanderer"			# a plain click: back to Wanderer alone (Shift never removes)
	echo "180 key Z down"
	echo "182 key Z up"
	echo "190 hints portrait Lockpicker"		# following Wanderer: the labels name the follow target
	echo "200 portrait add Lockpicker"
	echo "210 portrait dbl Sawyer"			# centre
	echo "220 key V down"
	echo "222 key V up"
	echo "230 hints portrait Lockpicker"		# driving Sawyer
	echo "240 portrait Lockpicker"
	echo "250 quit"
} | awk '{ $1 = $1 * 4 + 400; print }' > "$d/script.txt"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/progress_bars.kscen" --debug-view > "$d/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$d/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$d/server.log" "$pid" "$port"
if ! probe_char_ids "$build" "$tree" "$port" "$d/ids.txt"; then echo "run.sh: id probe found no characters (see $d/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$here/expected.txt" "$d/ids.txt" "$d" "$d/expected.txt" || { kill "$pid" 2>/dev/null; exit 4; }
(cd "$tree" && timeout 580 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/script.txt" > "$d/client.log" 2>&1)
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
grep -E "^(hints: |  (Click|Shift\+Click|Double click) |portrait: )" "$d/client.log" > "$d/actual.txt"
if diff -u "$d/expected.txt" "$d/actual.txt" > "$d/diff.txt"; then echo "PASS control_hints/portrait"; exit 0; fi
cat "$d/diff.txt"; echo "FAIL control_hints/portrait"; exit 1
