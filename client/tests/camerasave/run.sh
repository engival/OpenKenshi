#!/bin/bash
# camerasave (kenshi-k16.2.134.1): the camera sidecar of a save. live: one OpenKenshi-server, quicksave, move / rotate / tilt / zoom the camera away,
# quickload; the camera must come back. paused: the same with the sim paused before the quickload (no snapshots while paused). restart: a second
# client with its own local server (the title), LOAD GAME -> the same save; the sidecar survives the restart. delete: the same title panel, the
# selected save, Delete -> Yes; the sidecar must go with the save. newgame: quickload that save, then NEW GAME from the game menu; the new game's
# placement must win over the sidecar (the lead's rework check). All take the same OUT: XDG_DATA_HOME is OUT/xdg there, so the server's saves
# (ksim/saves) and the client's sidecars (kclient/cameras) stay inside the run and out of the user's own files. Each mode writes OUT/client-MODE.log
# (live / paused / newgame: also server-MODE.log) and OUT/script-MODE.txt.
# Usage: run.sh live OUT, then run.sh restart OUT / run.sh delete OUT / run.sh newgame OUT; or run.sh paused OUT on its own. Port: PORT (47835).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
mode=${1:?live|paused|restart|delete|newgame}
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
SETSRV=()	# --local-server's own server: pin it to the chosen binary when that is not the one beside kclient (a shared one)
if [ "$SHARED" = 1 ]
then
	SETSRV=(--set "session.server=$server")
	[ ${#SRV_SIMDATA[@]} -eq 0 ] || SETSRV+=(--set "session.server_args=--scenario newgame ${SRV_SIMDATA[*]}")	# the deploy has no simdata beside it
fi
export XDG_DATA_HOME="$out/xdg"
mkdir -p "$XDG_DATA_HOME/ksim/saves"
case "$mode" in restart) name=title_load ;; delete) name=delete_sidecar ;; paused) name=paused_load ;; newgame) name=newgame_after_load ;; *) name=save_load ;; esac
script="$here/$name.txt"
[ -f "$script" ] || { echo "run.sh: no script $script" >&2; exit 2; }
sed "s#OUTDIR#$out#g" "$script" > "$out/script-$mode.txt"
rc=0
if [ "$mode" = live ] || [ "$mode" = paused ] || [ "$mode" = newgame ]
then
	"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/camerasave.kscen" --save-dir "$XDG_DATA_HOME/ksim/saves" > "$out/server-$mode.log" 2>&1 &
	pid=$!
	for i in $(seq 240); do grep -q "^listening" "$out/server-$mode.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
	grep -q "^listening" "$out/server-$mode.log" || { echo "run.sh: server did not listen" >&2; kill "$pid" 2>/dev/null; exit 3; }
	cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script-$mode.txt" > "$out/client-$mode.log" 2>&1
	rc=$?
	ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
else
	cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --local-server ${SETSRV[@]+"${SETSRV[@]}"} --debug --script "$out/script-$mode.txt" > "$out/client-$mode.log" 2>&1
	rc=$?
fi
echo "client rc $rc; camera sidecar: $(grep -c 'camera sidecar' "$out/client-$mode.log") lines; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client-$mode.log")"
exit $rc
