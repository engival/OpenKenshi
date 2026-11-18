#!/bin/bash
# creatorload (kenshi-k16.2.133.1): New game -> Wanderer -> BEGIN from the title with the client's own local server; newgame.txt takes a shot every
# 0.5 s (session.loading_screen=2: scripted runs have no loading screen by default). Usage: run.sh OUT. OUT/s00..s23.png, OUT/client.log (the `creator:` / `session:` / `first_world_frame` lines say when the scene was drawn).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"
server=$SERVER
SETSRV=()	# --local-server's own server: pin it to the chosen binary when that is not the one beside kclient (a shared one)
if [ "$SHARED" = 1 ]
then
	SETSRV=(--set "session.server=$server")
	[ ${#SRV_SIMDATA[@]} -eq 0 ] || SETSRV+=(--set "session.server_args=--scenario newgame ${SRV_SIMDATA[*]}")	# the deploy has no simdata beside it
fi
export XDG_DATA_HOME="$out/xdg"
mkdir -p "$XDG_DATA_HOME"
sed "s#OUTDIR#$out#g" "$here/newgame.txt" > "$out/script.txt"
cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --local-server ${SETSRV[@]+"${SETSRV[@]}"} --debug --set session.loading_screen=2 --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
echo "client rc $rc; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
exit $rc
