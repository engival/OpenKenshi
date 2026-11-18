#!/bin/bash
# carryanimal (kenshi-k16.2.168): run.sh OUT. goat_fight: the squad downs the goat, the Wanderer lifts and carries it (goat.txt). The carried animal is a live
# ordinary-race ragdoll with its anchor body pinned to the carrier (Ragdolls::hang). OUT/client.log: count `off the shoulder` lines per carry; shots stand_/walk_/turn_/back_.
# SCRIPT=lying.txt: the KO'd goat lying (no carry). Port: PORT (47834). KSCEN overrides the scenario. The script names characters as @Name@
# (charids.sh: a probe run's `dump chars`; a name it lacks exits 4); exit is non-zero also when a scripted shot is missing (runcheck.sh).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../runcheck.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
script="$here/${SCRIPT:-goat.txt}"
[ -f "$script" ] || { echo "run.sh: no script $script" >&2; exit 2; }
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "${KSCEN:-$tree/scenarios/goat_fight.kscen}" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
if ! probe_char_ids "$tree/client/build" "$tree" "$port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$script" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
cd "$tree" && timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
echo "client rc $rc; off the shoulder lines: $(grep -c 'off the shoulder' "$out/client.log")"
run_status "$rc" "$out/script.txt" 1 || exit 1
exit $rc
