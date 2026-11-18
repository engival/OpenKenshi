#!/bin/bash
# kenshi-k16.2.183: live checks of the sim's k16.6.171 (drawn flag + sheathed weapon, formation steer, target lock).
# Run one script against a fresh OpenKenshi-server and a headless kclient.
# Usage: run.sh SCRIPT OUT   (SCRIPT: a .txt name here without .txt: sheathe | sheathe_dense | formation | lock; OUT: output dir)
# KSCEN: the .kscen (default per script: see the SCEN table); PORT (default: a free port). OUTDIR in the script is replaced by OUT, and
# @Name@ by that character's id (charids.sh: a probe run's `dump chars` of the fresh scenario; a name it lacks exits 4).
# Output: OUT/{server,client}.log, OUT/script.txt, the shots. Exit: the client's status, non-zero also when a scripted shot is missing
# (runcheck.sh). Binaries: client/build of this tree (BUILD overrides).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../runcheck.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
name=${1:?script name: sheathe | sheathe_dense | formation | lock}
script="$here/$name.txt"
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
case $name in
	sheathe|sheathe_dense) scen=$tree/scenarios/cage_prisoner.kscen ;;
	formation) scen=$here/formation.kscen ;;
	lock) scen=$tree/scenarios/bar_lure_hostile.kscen ;;
	*) scen= ;;
esac
scen=${KSCEN:-$scen}
[ -f "$script" ] && [ -f "$scen" ] || { echo "run.sh: missing $script or scenario '$scen'" >&2; exit 2; }
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$scen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
grep -q "^listening" "$out/server.log" || { echo "run.sh: server did not listen" >&2; kill "$pid" 2>/dev/null; exit 3; }
if ! probe_char_ids "$build" "$tree" "$port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$script" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
cd "$tree" && timeout 300 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
echo "client rc $rc"
min=1
[ "$name" = sheathe_dense ] && min=0	# that script takes no shots: its product is the per-frame wire dump
run_status "$rc" "$out/script.txt" "$min" || exit 1
if [ "$name" = sheathe_dense ] && [ "$(grep -c '^wire ' "$out/client.log")" -eq 0 ]
then
	echo "run.sh: no wire dump lines in client.log" >&2
	exit 1
fi
exit $rc
