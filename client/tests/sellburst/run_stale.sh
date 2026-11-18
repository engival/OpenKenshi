#!/bin/bash
# menu_query_stale (kenshi-k16.2.121): run menu_query_stale.txt against a frozen server. Two right-click holds go out (the Barman, then
# the Mercenary): the second supersedes the first, and neither is answered while the server sleeps. Both time out (the second opens its
# menu with Talk offered); the server is then resumed before the quit, so both MENU_ENTRIES arrive with nothing held any more and are
# dropped as stale ("(stale, dropped)" in the client log).
# Usage: run_stale.sh OUT  (OUT is created; logs OUT/server.log, OUT/client.log). Port: PORT (default 47834).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
port=${PORT:-47834}
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
sed "s#OUTDIR#$out#g" "$here/menu_query_stale.txt" > "$out/script.txt"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/sellburst.kscen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
grep -q "^listening" "$out/server.log" || { echo "run_stale.sh: server did not listen" >&2; kill "$pid" 2>/dev/null; exit 3; }
cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1 &
cpid=$!
for i in $(seq 1800); do grep -q "^hud: " "$out/client.log" 2>/dev/null && break; ps -p $cpid > /dev/null || break; sleep 0.1; done    # frame 300
sleep 0.8    # frame ~350: before the holds (frames 401-447)
kill -STOP "$pid"
( sleep 2.5; kill -CONT "$pid" 2>/dev/null ) &    # frame ~500: after the second hold's wait, before the shot (570) and quit (600)
wait "$cpid"; rc=$?
kill -CONT "$pid" 2>/dev/null; kill "$pid" 2>/dev/null
echo "client rc $rc; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
exit $rc
