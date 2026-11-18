#!/bin/bash
# loadpose (kenshi-k16.2.144): a client with its own local server, the title's LOAD GAME -> SAVE (a copy of KENSHI_TEST_SAVE, a standing player
# indoors); the log and the shots show the player's pose in the frames after the load. Usage: run.sh OUT [SAVE]. XDG_DATA_HOME is OUT/xdg.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
save=${2:-${KENSHI_TEST_SAVE:?SAVE (2nd arg) or KENSHI_TEST_SAVE: a .save file, a standing player indoors}}
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
mkdir -p "$XDG_DATA_HOME/ksim/saves"
cp "$save" "$XDG_DATA_HOME/ksim/saves/"
cp "$save" "$XDG_DATA_HOME/ksim/saves/quicksave"
sed "s#OUTDIR#$out#g" "$here/load.txt" > "$out/script.txt"
cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --local-server ${SETSRV[@]+"${SETSRV[@]}"} --debug --script "$out/script.txt" > "$out/client.log" 2>&1
echo "client rc $?; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
