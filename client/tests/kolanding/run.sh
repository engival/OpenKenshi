#!/bin/bash
# kenshi-ettl: where do KO / death ragdolls land against the sim's rest point? Runs scenarios/kolanding.kscen (two hostile squads of 8, no player)
# on a fresh OpenKenshi-server and a headless kclient (free camera over the fight, sim at SPEED x), then tabulates the ragdoll log:
# `ragdolls: <id> pull starts X u ...` and `ragdolls: <id> settled Y u from the rest point, Z across, at T s` -> OUT/table.txt.
# Usage: run.sh OUT   (PORT, BUILD, SPEED default 3, SHOTS = script lines to add, e.g. "300 screenshot OUT/a.png"; OUTDIR in them is OUT)
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../runcheck.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$here/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"
server=$SERVER
scen=${SCEN:-$tree/scenarios/kolanding.kscen}
shots=${SHOTS:-}
shots=${shots//OUTDIR/$out}
sed -e "s|@SPEED@|${SPEED:-3}|" "$here/kolanding.txt" | awk -v s="$shots" '{ if ($0 == "@SHOTS@") print s; else print }' > "$out/script.txt"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$scen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
grep -q "^listening" "$out/server.log" || { echo "run.sh: server did not listen" >&2; kill "$pid" 2>/dev/null; exit 3; }
cd "$tree" && timeout 300 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
awk '/ragdolls: [0-9]+ pull starts/ { st[$2] = $5 } /ragdolls: [0-9]+ settled/ { printf "%s pull_start %s settled %s across %s time %s\n", $2, st[$2], $4, $10, $13 }' "$out/client.log" | sed 's/,//g' > "$out/table.txt"
echo "client rc $rc; bodies: $(wc -l < "$out/table.txt"); >=10 u starts: $(awk '$3>=10' "$out/table.txt" | wc -l); across>1: $(awk '$7>1' "$out/table.txt" | wc -l)"
cat "$out/table.txt"
exit 0
