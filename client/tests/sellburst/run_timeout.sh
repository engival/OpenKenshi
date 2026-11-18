#!/bin/bash
# menu_query_timeout (kenshi-k16.2.121): run menu_query_timeout.txt against a frozen server, so order 99's MENU_ENTRIES never comes and
# the client's 0.5 s wait opens the menu with Talk offered (the older-server path). The freeze follows the client's frame log (a `hud:`
# line every 300 frames) so it lands before the script's right-click (frame 401), not at a wall-time guess.
# Usage: run_timeout.sh OUT  (OUT is created; logs OUT/server.log, OUT/client.log). Port: PORT (default 47834).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
port=${PORT:-47834}
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
sed "s#OUTDIR#$out#g" "$here/menu_query_timeout.txt" > "$out/script.txt"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/sellburst.kscen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
grep -q "^listening" "$out/server.log" || { echo "run_timeout.sh: server did not listen" >&2; kill "$pid" 2>/dev/null; exit 3; }
cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1 &
cpid=$!
for i in $(seq 1800); do grep -q "^hud: " "$out/client.log" 2>/dev/null && break; ps -p $cpid > /dev/null || break; sleep 0.1; done    # frame 300
sleep 0.8    # frame ~350: the world is up, the right-click (401) not yet
kill -STOP "$pid"
wait "$cpid"; rc=$?
kill -CONT "$pid" 2>/dev/null; kill "$pid" 2>/dev/null
echo "client rc $rc; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
exit $rc
