#!/bin/bash
# Run one interiormask script (kenshi-h2v.11.4.2) against a fresh OpenKenshi-server (scenarios/cage_prisoner.kscen) and a headless kclient.
# Usage: run.sh SCRIPT OUT  (SCRIPT: a .txt name here without .txt, e.g. mask). Port: PORT (default: a free port). Scenario: KSCEN (a .kscen path; default scenarios/cage_prisoner.kscen).
# OUTDIR in the script is replaced by OUT. Binaries: client/build of this tree (BUILD overrides) (target ksim_server, kclient).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
script="$here/${1:?script name, e.g. mask}.txt"
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}	# BUILD=<abs dir>: another build (the Debug build: client/build-debug)
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
[ -f "$script" ] || { echo "run.sh: no script $script" >&2; exit 2; }
sed "s#OUTDIR#$out#g" "$script" > "$out/script.txt"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "${KSCEN:-$tree/scenarios/cage_prisoner.kscen}" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
grep -q "^listening" "$out/server.log" || { echo "run.sh: server did not listen" >&2; kill "$pid" 2>/dev/null; exit 3; }
cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
echo "client rc $rc; stale: $(grep -c stale "$out/client.log"); [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
exit $rc
