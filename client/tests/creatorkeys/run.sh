#!/bin/bash
# creatorkeys (kenshi-k16.2.472.1): New game -> Guy with a Dog -> BEGIN with the client's own local server, types names into the creator (member 1 and
# the dog) and bound letters; check.sh judges client.log. Usage: run.sh OUT. OUT/client.log, OUT/check.txt (the verdict); exit 0 = PASS.
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
sed "s#OUTDIR#$out#g" "$here/keys.txt" > "$out/script.txt"
cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --local-server ${SETSRV[@]+"${SETSRV[@]}"} --debug --set session.loading_screen=2 --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
echo "client rc $rc; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
"$here/check.sh" "$out/client.log" | tee "$out/check.txt"
exit "${PIPESTATUS[0]}"
