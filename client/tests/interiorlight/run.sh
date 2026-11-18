#!/bin/bash
# interiorlight (kenshi-jj3.2.1): run.sh OUT [EXPOSURE [SAVE]] (SCRIPT=inside2: the acceptance views, kenshi-jj3.2.2). A client with its own local server loads SAVE (default: KENSHI_TEST_SAVE, a Wanderer in the Storm House) from the
# title and shoots the same views with render.interior_light 1 and 0 (OUT/off_*.png = the original lighting, the effect off; OUT/on_*.png = the effect full) at a fixed exposure (default 1). The bake takes a few seconds on a worker thread
# the first time (the cache after). XDG_DATA_HOME is OUT/xdg (the saves); the lightbake cache stays the user's.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"
server=$SERVER
SETSRV=()	# --local-server's own server: pin it to the chosen binary when that is not the one beside kclient (a shared one)
if [ "$SHARED" = 1 ]
then
	SETSRV=(--set "session.server=$server")
	[ ${#SRV_SIMDATA[@]} -eq 0 ] || SETSRV+=(--set "session.server_args=--scenario newgame ${SRV_SIMDATA[*]}")	# the deploy has no simdata beside it
fi
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
exposure=${2:-1}
save=${3:-${KENSHI_TEST_SAVE:?SAVE (3rd arg) or KENSHI_TEST_SAVE: a .save file, a Wanderer in the Storm House}}
export XDG_DATA_HOME="$out/xdg"
mkdir -p "$XDG_DATA_HOME/ksim/saves"
cp "$save" "$XDG_DATA_HOME/ksim/saves/"
sed "s#OUTDIR#$out#g" "$here/${SCRIPT:-inside}.txt" > "$out/script.txt"
cd "$tree" && timeout 360 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --local-server ${SETSRV[@]+"${SETSRV[@]}"} --debug --exposure "$exposure" --script "$out/script.txt" > "$out/client.log" 2>&1
echo "rc $?; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
