#!/bin/bash
# gamespeed (kenshi-k16.2.181.1, the 5x stutter check): run.sh OUT [SPEEDS]. One bar_lure server + scripted client run per game speed
# (default 1 2 5), each in OUT/speed<N>/: the Wanderer followed and walked south of the Hub, `set simspeed` at frame 170, `log pos all` per
# frame (client/src/dev/poslog.cpp: sim and drawn position, render clock, newest snapshot, the frame's wall time). Then summary.awk per run:
# per character the drawn step's median and max over moving frames, the jump and stall counts, the render clock's lag behind the newest
# snapshot and its per-frame advance, and the harness share of the worst step (a scripted frame is a fixed 1/60 s of sim clock whatever it
# really took, so a slow frame's catch-up is not the stutter a live run shows). table.txt collects the runs' RUN lines plus their frame
# wall time and snapshot rate. Shots: none. Ports: PORT.. (default: a free one, one per speed).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
speeds=${2:-1 2 5}
frames=${FRAMES:-1200}
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port0=$(pick_port "${PORT:-}") || exit 3
size=${SIZE:-"640 360"}	# the headless window: smaller and without the shadow / foliage passes, the client keeps a frame rate near the live one
read -r width height <<< "$size"
client=$tree/client/build/kclient
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
if [ -n "${SIM:-}" ]
then
	server=$SIM	# SIM: another OpenKenshi-server (the KSIM_PROFILE build, with KSIM_PROFILE_JSON=<s> in the env)
	echo "server: built (SIM: $server)"
else
	pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
	server=$SERVER
fi
: > "$out/table.txt"
n=0
for speed in $speeds; do
	dir=$out/speed$speed
	mkdir -p "$dir"
	if [ -n "${PORT:-}" ]; then port=$((port0 + n)); else port=$(pick_port ""); fi; n=$((n + 1))
	sed -e "s#OUTDIR#$dir#g" -e "s#@SPEED@#$speed#g" -e "s#@FRAMES@#$frames#g" "$here/script.txt" > "$dir/script.txt"
	nice -n 10 "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/bar_lure.kscen" \
		--navworld "${KENSHI_NAVWORLD:?set KENSHI_NAVWORLD to a navworld bake dir}" > "$dir/server.log" 2>&1 &
	pid=$!
	for i in $(seq 240); do grep -q "^listening" "$dir/server.log" && break; ps -p "$pid" > /dev/null || break; sleep 0.5; done
	server_listening "$dir/server.log" $pid $port
	(cd "$tree" && XDG_DATA_HOME="$dir/xdg" nice -n 10 timeout 900 "$tree/client/tools/headless.sh" -W "$width" -H "$height" -- \
		stdbuf -oL "$client" --simlink 127.0.0.1:$port --debug --script "$dir/script.txt" --no-shadows --no-grass --no-foliage --no-fog-volumes \
		> "$dir/client.log" 2>&1)
	rc=$?
	ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
	events=$(find "$dir/xdg/kclient/events" -name 'events-*.txt' 2>/dev/null | sort | tail -1)
	[ -n "$events" ] && cp "$events" "$dir/events.txt"
	counts=$(grep -o ' tick [-0-9.]* [a-z]*:' "$dir/events.txt" 2>/dev/null | awk '{print $3}' | sort | uniq -c | awk '{printf "%s %s, ", $2, $1}')
	echo "speed $speed: client exit $rc, sg errors $(grep -c '\[sg\]\[error\]' "$dir/client.log"), pos lines $(grep -c '^pos ' "$dir/client.log"), events: ${counts:-none}"
	player=$(grep -m1 '^player ' "$dir/client.log" | awk '{print $2}')
	awk -v speed="$speed" -v player="${player:-}" -f "$here/summary.awk" "$dir/client.log" > "$dir/summary.txt"
	cat "$dir/summary.txt"
	# the harness: the player's logged frames against the last frame's wall clock (a scripted frame is a fixed 1/60 s of sim clock), and
	# the snapshot rate the client received against the sim's own (20 x speed ticks a wall second)
	harness=$(awk -v speed="$speed" '
		$1 == "pos" && $2 != t {
			t = $2; n++
			if (n == 1) { fnew = $10; fwall = $24 }
			lnew = $10; lwall = $24
			if (prev >= 0) { d = $26 - prev; if (d < 0) { d = 0 }; arr[d > 3 ? 3 : d]++ }
			prev = $26
		}
		END {
			if (n < 2 || lwall <= fwall) { printf "harness: no frames\n"; exit }
			printf "harness: frames %d, mean frame wall %.1f ms (a scripted frame is 16.7 ms of sim clock), snapshots/frame 0:%d 1:%d 2:%d 3+:%d, snapshot rate %.1f ticks/wall s of the sim\x27s %.0f, client clock deficit %.2f ticks/frame\n",
				n, lwall / n * 1000, arr[0], arr[1], arr[2], arr[3], (lnew - fnew) / (lwall - fwall), 20 * speed,
				speed / 3 * (lwall / n * 60 - 1)
		}' "$dir/client.log")
	echo "$harness" | tee "$dir/harness.txt"
	grep '^RUN ' "$dir/summary.txt" >> "$out/table.txt"
	echo "$harness" >> "$out/table.txt"
done
echo
echo "== table (OUT/table.txt) =="
cat "$out/table.txt"
