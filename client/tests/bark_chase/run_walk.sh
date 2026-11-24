#!/bin/bash
# bark_chase run_walk (kenshi-jtnm): run_walk.sh OUT. The user's 1js4 walk: newgame at the Hub, a far move order to Stack's west gate, the sim sped up, a bounded headless run.
# The client's `speech:` / `speech_end:` lines (with the sim tick) go to OUT/client.log; count them with count.py. Env: KSIM_SERVER (the server binary, default the shared one / this build),
# TEMPLATE (a script template beside this file, default walk.txt; walk_attack.txt adds the crime and the run), REORDER_UNTIL (frame), SHOT_FROM (frame), REORDER (frames: re-issue the far move order every N frames, default off), SIMSPEED (default 8), FRAMES (script length in frames, default 14000), SHOT_EVERY (frames, default 1500), WALL (wall-clock bound in s, default 420), WANDERER (id, default: probed).
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
mkdir -p "$base/shots"
srv() { start_server_on_free_port "$1" "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:@PORT@ --scenario newgame --debug-view; }
# the Wanderer's id (newgame spawns shift it between trees): a probe run on a throwaway server, as run.sh does
if [ -z "${WANDERER:-}" ]
then
	srv "$base/probe_server.log"
	# a newgame loads more slowly than a scenario: dump chars late (probe_char_ids' frames 30-90 are too early)
	printf '240 dump chars\n400 dump chars\n450 quit\n' > "$base/idprobe.txt"
	(cd "$tree" && timeout 200 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$SERVER_PORT --debug --script "$base/idprobe.txt" > "$base/idprobe.log" 2>&1)
	sed -n 's/^char \([0-9][0-9]*\) "\([^"]*\)".*/\1\t\2/p' "$base/idprobe.log" | sort -n -u > "$base/ids.tsv"; [ -s "$base/ids.tsv" ]; rc=$?
	kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null
	[ $rc = 0 ] || { echo "probe failed"; exit 4; }
	# the newgame player: the highest-id char with flags 0x41244 (its name is random: Wanderer, Hesric, ...); PLAYER_NAME picks by name instead
	if [ -n "${PLAYER_NAME:-}" ]; then WANDERER=$(char_id "$base/ids.tsv" "$PLAYER_NAME") || exit 4
	else WANDERER=$(sed -n 's/^char \([0-9]*\) .* flags 0x41244 at .*/\1/p' "$base/idprobe.log" | sort -n | tail -1); [ -n "$WANDERER" ] || { echo "no player char (flags 0x41244)"; exit 4; }; fi
fi
frames=${FRAMES:-14000}
dumps=; shots=; moves=
for ((f = 900; f < frames; f += 600)); do dumps+="$f dump chars"$'\n'; done
r=${REORDER:-0}
if [ "$r" -gt 0 ]; then for ((f = 320 + r; f < ${REORDER_UNTIL:-$frames}; f += r)); do moves+="$f move -56300 -13188"$'\n'; done; fi
for ((f = ${SHOT_FROM:-${SHOT_EVERY:-1500}}; f < frames; f += ${SHOT_EVERY:-1500})); do shots+="$f screenshot $base/shots/f$(printf %05d $f).png"$'\n'; done
awk -v d="$dumps" -v s="$shots" -v m="$moves" -v q="$((frames + 50))" -v sp="${SIMSPEED:-8}" -v id="${WANDERER}" \
	'$0 == "DUMPS" { printf "%s", d; next } $0 == "MOVES" { printf "%s", m; next } $0 == "SHOTS" { printf "%s", s; next } { sub(/\$\{QUIT\}/, q); sub(/\$\{SIMSPEED\}/, sp); gsub(/@Wanderer@/, id); print }' "$here/${TEMPLATE:-walk.txt}" > "$base/script.txt"
srv "$base/server.log"
port=$SERVER_PORT; pid=$SERVER_PID
(cd "$tree" && HEADLESS_TIMEOUT=${WALL:-420} nice -n 10 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$base/script.txt" 2>&1 \
	| perl -MTime::HiRes=time -e '$|=1; $t0=time; while (<>) { printf "%8.2f %s", time-$t0, $_ }' > "$base/client.log")
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
echo "server: $server; speech lines: $(grep -c ' speech: ' "$base/client.log")"
