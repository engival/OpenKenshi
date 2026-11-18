#!/bin/bash
# pointer (kenshi-k16.2.205.12.97): run.sh OUT [extra client args, e.g. --set render.thread=lockstep]. The boxselect scenario live (the cursor needs a sim), the
# pointer moved by `mouse pos` between two shots, a 500 ms logic stall after the second move (ptr.txt). ptr_a.png has the cursor at (300,200), ptr_b.png at (900,500).
# Until overlap's redraw (T8 / T21) lands the stall runs in lockstep, so this shows the newest position is the one drawn, not the overlap case itself.
# Exit 0 when the client ran to its quit with no sg error and both shots exist. Port: PORT (47853).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"
server=$SERVER
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd); shift
. "$here/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/../boxselect/boxselect.kscen" \
	--navworld "${KENSHI_NAVWORLD:-$HOME/git/kenshi/navworld}" > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
sed "s#OUTDIR#$out#g" "$here/ptr.txt" > "$out/script.txt"
cd "$tree" && nice -n 10 timeout 300 client/tools/headless.sh -- stdbuf -oL "${KCLIENT:-$tree/client/build/kclient}" --simlink 127.0.0.1:$port --debug "$@" --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
kill "$pid" 2>/dev/null
for i in $(seq 100); do ps -p "$pid" > /dev/null || break; sleep 0.1; done
sg=$(grep -c '\[sg\]\[error\]' "$out/client.log")
echo "sg errors: $sg, rc $rc"
[ "$rc" -eq 0 ] && [ "$sg" -eq 0 ] && [ -f "$out/ptr_a.png" ] && [ -f "$out/ptr_b.png" ] && echo "pointer: PASS" || { echo "pointer: FAIL"; exit 1; }
