#!/bin/bash
# carrylabel (kenshi-pgvp, k16.2.517): run.sh OUT. slavers_downed: the Wanderer is put down and carried by the caravan. `dump namelabel id=` lines in OUT/client.log pair the sim's carrier with
# whether the name tag was drawn: carried -> no tag, not carried -> tag, both states must occur. Shots OUT/s_*.png. Port: PORT. Exit 4: a character is missing from the probe.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/slavers_downed.kscen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
if ! probe_char_ids "$tree/client/build" "$tree" "$port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$here/script.txt" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
cd "$tree" && timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
grep -a '^namelabel id' "$out/client.log" > "$out/labels.txt"
[ "$rc" -eq 0 ] || { echo "carrylabel: FAIL (client rc $rc)"; exit 1; }
# the tag is the player's: the Wanderer's lines must show carried-without-tag and not-carried-with-tag
wid=$(char_id "$out/ids.txt" Wanderer) || exit 4
w=$(grep -a "^namelabel id $wid " "$out/labels.txt")
echo "$w" > "$out/wanderer.txt"
c0=$(echo "$w" | awk '$5 != 0 && $7 == 0' | wc -l); cbad=$(echo "$w" | awk '$5 != 0 && $7 == 1' | wc -l)
f1=$(echo "$w" | awk '$5 == 0 && $7 == 1' | wc -l); 
echo "Wanderer: carried without tag $c0, carried WITH tag $cbad, free with tag $f1"
[ "$c0" -gt 0 ] && [ "$cbad" -eq 0 ] && [ "$f1" -gt 0 ] || { echo "carrylabel: FAIL"; exit 1; }
echo "carrylabel: PASS"
