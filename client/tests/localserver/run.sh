#!/bin/bash
# localserver smoke (kenshi-379.6.1): the title's single player on a client binary: New Game -> Wanderer -> BEGIN -> CONFIRM, a few seconds in the
# Hub, quit. Usage: run.sh BINARY OUT [smoke|menu] (BINARY = client/build/OpenKenshi, the full target with the server on a std::thread, or client/build/kclient).
# OUT/client.log has the `local server:` lines (in process (thread) / pid N, "stopped" at the end), OUT/creator.png, game1.png, game2.png.
# smoke (default): ends with a scripted `quit`. menu (kenshi-379.12): ends with the title's EXIT button after Escape -> EXIT GAME -> YES; the script has
# no quit command, so the run must end by itself within 90 s and the exit 0 must follow the click within 2 s (the `kclient: quit:` line).
# Exit 0 = the run ended by itself (no hang on exit); the last line counts [sg][error] lines.
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
bin=${1:?binary}
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
mode=${3:-smoke}
case "$mode" in smoke) limit=400 ;; menu) limit=90 ;; *) echo "mode: smoke or menu"; exit 2 ;; esac
export XDG_DATA_HOME="$out/xdg"
mkdir -p "$XDG_DATA_HOME"
sed "s#OUTDIR#$out#g" "$here/$mode.txt" > "$out/script.txt"
start=$(date +%s)
cd "$tree" && timeout $limit client/tools/headless.sh -- stdbuf -oL "$bin" --local-server ${SETSRV[@]+"${SETSRV[@]}"} --debug --set session.loading_screen=2 --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
echo "rc $rc in $(( $(date +%s) - start )) s; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
if [ "$mode" = menu ] && [ $rc -eq 0 ]
then
	ms=$(sed -n 's/.*quit requested \([0-9]*\) ms ago.*/\1/p' "$out/client.log" | tail -1)
	echo "menu quit: server stopped ${ms:-?} ms after the request"
	if [ -z "$ms" ] || [ "$ms" -gt 2000 ]; then echo "FAIL: the quit took longer than 2 s (or never came from the button)"; rc=1; fi
fi
exit $rc
