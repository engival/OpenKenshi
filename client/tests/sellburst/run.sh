#!/bin/bash
# Run one sellburst script against a fresh OpenKenshi-server (the sellburst.kscen fixture) and a headless kclient.
# Usage: run.sh SCRIPT OUT  (SCRIPT: a .txt name here without .txt, e.g. burst20hz). Port: PORT (default: a free port). Fixture: KSCEN (default sellburst; windows.txt wants sellbox).
# OUTDIR in the script is replaced by OUT; @Name@ by the character's wire id (charids.sh, a probe run; exit 4 when missing) and @box@ by the sellbox fixture's own container. Binaries: client/build of this tree (BUILD overrides) (target ksim_server, kclient).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../runcheck.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
script="$here/${1:?script name, e.g. burst20hz}.txt"
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}	# BUILD=<abs dir>: another build (the Debug build: client/build-debug)
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
[ -f "$script" ] || { echo "run.sh: no script $script" >&2; exit 2; }
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/${KSCEN:-sellburst}.kscen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
grep -q "^listening" "$out/server.log" || { echo "run.sh: server did not listen" >&2; kill "$pid" 2>/dev/null; exit 3; }
# the Wanderer (and any @Name@) comes from a probe run; @box@ is the sellbox fixture's own container (the Wanderer's faction owns it; k16.2.313)
if ! probe_char_ids "$build" "$tree" "$port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
own=$(sed -n 's/^things: own faction "\(.*\)"$/\1/p' "$out/idprobe.log" | head -1)
box=$(awk -v own="$own" '$1 == "thing" && $3 == "kind" && $4 == 3 && index($0, " owner \"" own "\"") { print $2; exit }' "$out/idprobe.log")
[ -z "$box" ] || printf '%s\tbox\n' "$box" >> "$out/ids.txt"
charids_fill "$script" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
echo "client rc $rc; stale: $(grep -c stale "$out/client.log"); [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
exit $rc
