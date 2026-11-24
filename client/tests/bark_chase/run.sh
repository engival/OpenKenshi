#!/bin/bash
# bark_chase (kenshi-jtnm): run.sh OUT [SCENARIO.kscen]. A Holy Nation patrol chases the Wanderer over open ground (scenarios/hn_chase_barks.kscen) and the client logs every speech
# bubble (`speech:` / `speech_end:` lines, each stamped with the wall seconds since the run started). The same run on a pre-fix and a fixed sim:
#   KSIM_SERVER=<server binary> LABEL=prefix client/tools/../tests/bark_chase/run.sh OUT/prefix
# then python3 client/tests/bark_chase/count.py OUT/prefix/client.log [...]. Port: PORT (a free one). SHOT_EVERY (frames, default 240), SHOT_FROM (default 240), SHOT_TO (3600).
# A probe run on a throwaway server resolves the Wanderer's id first: a scenario that fights on its own loses the fight before the measured client joins (the id probe runs the world).
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tests/need_data.sh" 2>/dev/null
. "$tree/client/tools/server_bin.sh"
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"
server=$SERVER
. "$tree/client/tools/ports.sh"
. "$tree/client/tests/charids.sh"
base=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
scen=${2:-$tree/scenarios/hn_chase_barks.kscen}
mkdir -p "$base/shots"
srv() { start_server_on_free_port "$1" "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:@PORT@ --scenario-file "$scen" --debug-view; }
srv "$base/probe_server.log"
probe_char_ids "$tree/client/build" "$tree" "$SERVER_PORT" "$base/ids.tsv"; rc=$?
kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null
[ $rc = 0 ] || { echo "probe failed"; exit 4; }
shots=
for ((f = ${SHOT_FROM:-240}; f < ${SHOT_TO:-3600}; f += ${SHOT_EVERY:-240})); do shots+="$f screenshot $base/shots/f$(printf %05d $f).png"$'\n'; done
awk -v s="$shots" '$0 == "SHOTS" { printf "%s", s; next } { print }' "$here/chase.txt" > "$base/chase.in"
charids_fill "$base/chase.in" "$base/ids.tsv" "$base" "$base/script.txt" || exit 4
srv "$base/server.log"
port=$SERVER_PORT; pid=$SERVER_PID
(cd "$tree" && timeout 420 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$base/script.txt" 2>&1 \
	| perl -MTime::HiRes=time -e '$|=1; $t0=time; while (<>) { printf "%8.2f %s", time-$t0, $_ }' > "$base/client.log")
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
echo "server: $server; speech lines: $(grep -c ' speech: ' "$base/client.log")"
