#!/bin/bash
# sunshafts (kenshi-jj3.7.1): run.sh OUT [SHAFTS]. Two headless runs of bar.txt on the bar_lure scenario, render.sun_shafts 0 (OUT/off) and SHAFTS (default 1; OUT/on): the Wanderer inside the Hub bar with the roof cut, at several sun heights
# (the shots t08..t1730 and t01 at night); `dump gpu` at the end for the cost. Port: PORT (47834).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
base=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
for mode in off on
do
	out="$base/$mode"; mkdir -p "$out"
	sed "s#OUTDIR#$out#g" "$here/${SCRIPT:-bar}.txt" > "$out/script.txt"
	"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/bar_lure.kscen" --debug-view > "$out/server.log" 2>&1 &
	pid=$!
	for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
	server_listening "$out/server.log" $pid $port
	extra=(--set render.sun_shafts=0); [ "$mode" = on ] && extra=(--set render.sun_shafts=${2:-1})
	(cd "$tree" && timeout 300 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug "${extra[@]}" --script "$out/script.txt" > "$out/client.log" 2>&1)
	ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
	echo "$mode: [sg][error] $(grep -c '\[sg\]\[error' "$out/client.log")"
done
