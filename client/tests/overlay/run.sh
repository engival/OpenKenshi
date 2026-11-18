#!/bin/bash
# overlay (render sitting 11, kenshi-k16.2.205.12.57): run.sh OUT. The boxselect scenario + a scripted headless client (ov.txt) with the world
# overlay's logic-recorded blocks on: the nav overlay, trails, sim markers, the --debug HUD and minimap, the full map. Shots in OUT, the client log in
# OUT/client.log. KCLIENT=<binary> runs another build (a base build to compare with); ARGS adds client arguments (e.g. "--set render.thread=lockstep").
# The script names Charlie as @Charlie@ (charids.sh: a probe run fills the id in; exit 4 when missing). Port: PORT (47852). Exit 0 when the client ran to its quit with no sg error and the three shots exist.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/../boxselect/boxselect.kscen" \
	--navworld "${KENSHI_NAVWORLD:-$HOME/git/kenshi/navworld}" > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
if ! probe_char_ids "$tree/client/build" "$tree" "$port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$here/ov.txt" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
# shellcheck disable=SC2086
cd "$tree" && nice -n 10 timeout 300 client/tools/headless.sh -- stdbuf -oL "${KCLIENT:-$tree/client/build/kclient}" --simlink 127.0.0.1:$port --debug \
	--set ui.style=flat ${ARGS:-} --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
kill "$pid" 2>/dev/null
for i in $(seq 100); do ps -p "$pid" > /dev/null || break; sleep 0.1; done
sg=$(grep -c '\[sg\]\[error\]' "$out/client.log")
echo "sg errors: $sg"
grep -E "^sgl: " "$out/client.log"
ok=1
for s in overlay overlay_later fullmap; do [ -f "$out/$s.png" ] || { echo "overlay: no $s.png"; ok=0; }; done
[ "$rc" -eq 0 ] && [ "$sg" -eq 0 ] && [ $ok -eq 1 ] || { echo "overlay: FAIL (client rc $rc)"; exit 1; }
echo "overlay: PASS"
