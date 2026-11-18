#!/bin/bash
# cutshadows (kenshi-jj3.3.1): run.sh OUT. One headless run of bar.txt on the bar_lure scenario (OUT/on): the Wanderer inside the Hub bar with the
# roof cut; the shots must show the room in the roof's shadow (render.cut_shadows is gone, always on: kenshi-jj3.30). scenes.txt: the same switch in --scene-file scenes
# (the camera alone cuts nothing there: only a character inside does). Port: PORT (47833).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
base=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
for mode in on
do
	out="$base/$mode"; mkdir -p "$out"
	sed "s#OUTDIR#$out#g" "$here/bar.txt" > "$out/script.txt"
	start_server_on_free_port "$out/server.log" "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:@PORT@ --scenario-file "$tree/scenarios/bar_lure.kscen" --debug-view	# a fresh port per start (kenshi-k16.2.358)
	pid=$SERVER_PID; port=$SERVER_PORT
	(cd "$tree" && timeout 300 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1)
	ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
	echo "$mode: [sg][error] $(grep -c '\[sg\]\[error' "$out/client.log")"
done
