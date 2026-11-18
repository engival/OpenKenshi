#!/bin/bash
# kenshi-k16.2.237.4: scripted direct drive STRAFE: V, mouse look held (right button), then A, D, W+A, W+D, S+A, S+D, a shot every STEP frames, `log pos` per frame (strafe bits).
# Usage: strafe.sh OUT. Derived from run.sh (k16.2.237.1: scripted direct drive (V follow) with held keys, a screenshot every STEP frames from the key press, camera behind the character.
# Usage: run.sh OUT KEYS [RUN]   KEYS e.g. "W", "S D"; RUN=1 toggles run (the speed diamond) via shift not needed: default speed.  PORT (47830), BUILD.
# The driven character is CHAR (default Wanderer: the id the script hard-coded, 41253, was this scenario's lead before the wire ids moved;
# today 41253 is Green, CHAR=Green runs it that way). Its id comes from a probe run of the fresh scenario (charids.sh, kenshi-k16.2.308);
# a scenario without that name exits 4.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
keys=strafe
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
step=${STEP:-30}
PHASE=${PHASE:-90}; GAP=${GAP:-30}; ADV=${ADV:-8}
d=$out/strafe
mkdir -p "$d"
rm -f "$d"/*.png
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/order_sounds.kscen" --debug-view > "$d/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$d/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$d/server.log" $pid $port
if ! probe_char_ids "$build" "$tree" "$port" "$d/ids.txt"; then echo "run.sh: id probe found no characters (see $d/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
id=$(char_id "$d/ids.txt" "${CHAR:-Wanderer}") || { kill "$pid" 2>/dev/null; exit 4; }
{
	echo "10 set animate 1"
	echo "100 select id=$id"
	echo "110 key V down"
	echo "112 key V up"
	echo "120 set dist 40"
	echo "125 log pos $id"
	echo "140 mouse down right"
	f=150
	for ph in "A" "D" "W A" "W D" "S A" "S D"; do
		for k in $ph; do echo "$f key $k down"; done
		for i in $(seq 0 $step $((PHASE - step))); do echo "$((f+i+ADV)) screenshot $d/$(echo "$ph" | tr -d ' ')_$(printf %03d $i).png"; done
		f=$((f+PHASE))
		for k in $ph; do echo "$f key $k up"; done
		f=$((f+GAP))
	done
	echo "$f mouse up right"
	echo "$((f+5)) quit"
} > "$d/script.txt"
(cd "$tree" && timeout 420 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/script.txt" > "$d/client.log" 2>&1)
rc=$?
kill $pid 2>/dev/null; wait $pid 2>/dev/null
# the exit status comes from what the run produced (kenshi-k16.2.301): every scripted shot is a file and the client quit clean (rc 0)
want=$(grep -c " screenshot " "$d/script.txt")
have=$(ls "$d"/*_[0-9][0-9][0-9].png 2>/dev/null | wc -l)
echo "driven $id (${CHAR:-Wanderer}); shots $have of $want; client rc $rc"
[ "$rc" -eq 0 ] && [ "$have" -eq "$want" ] && [ "$want" -gt 0 ]
