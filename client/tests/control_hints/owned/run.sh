#!/bin/bash
# kenshi-db9v: control hints for an automatic producer (Ore Drill III) and a container of ours, placed by owned.kscen (the Heft slave shop world). `hints thing ID` lists, `thingclick [right] [shift] [label] ID` clicks; every click prints `click hint: <kind>` then its `sim click` line, checked against expected.txt. Usage: run.sh OUT (PORT, BUILD as in ../things/run.sh).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../../charids.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../../../tools/ports.sh"	# server_listening, pick_port
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$build/cpp/OpenKenshi-server"
server=$SERVER
d=$out
awk '!/^#/ && NF { $1 = $1 * 4 + 400 } { print }' "$here/script.txt" > "$d/script.in"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/owned.kscen" --navworld "${KENSHI_NAVWORLD:-$HOME/git/kenshi/navworld}" > "$d/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$d/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$d/server.log" "$pid" "$port"
if ! probe_char_ids "$build" "$tree" "$port" "$d/ids.txt"; then echo "run.sh: id probe found no characters (see $d/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$d/script.in" "$d/ids.txt" "$d" "$d/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
charids_fill "$here/expected.txt" "$d/ids.txt" "$d" "$d/expected.txt" || { kill "$pid" 2>/dev/null; exit 4; }
(cd "$tree" && timeout 280 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/script.txt" > "$d/client.log" 2>&1)
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
# the click coordinates are the pointer's, not a result
grep -E "^(hints: |  [A-Z][A-Za-z+]*( [a-z]+)*  |click hint: |sim click )" "$d/client.log" | sed -E 's/^sim click \([^)]*\)/sim click/' > "$d/actual.txt"
if diff -u "$d/expected.txt" "$d/actual.txt" > "$d/diff.txt"; then echo "PASS control_hints/owned"; exit 0; fi
cat "$d/diff.txt"; echo "FAIL control_hints/owned"; exit 1
