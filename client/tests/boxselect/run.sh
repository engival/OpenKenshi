#!/bin/bash
# boxselect (kenshi-k16.2.230.1; the original's rules, kenshi-k16.2.230.3): run.sh OUT [SCRIPT.txt]. boxselect.kscen server + a scripted headless
# client (OUT/client.log, shots in OUT); OUT/check.txt says which of the expected results failed and the exit code is 1 then, 0 with
# "boxselect: PASS". STYLE=original draws the game's own box (ui.style=original: the original's Kenshi_SelectionPanel, k16.2.230.3) into the
# same shots; the default is ours. Port: PORT (47831). The script names characters as @Name@ (charids.sh: a probe run's `dump chars` fills
# their ids in; exit 4 when one is missing).
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
script="$here/${2:-box.txt}"
[ -f "$script" ] || { echo "run.sh: no script $script" >&2; exit 2; }
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/boxselect.kscen" \
	--navworld "${KENSHI_NAVWORLD:-$HOME/git/kenshi/navworld}" > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
if ! probe_char_ids "$tree/client/build" "$tree" "$port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$script" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
cd "$tree" && nice -n 10 timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug \
	--set "ui.style=${STYLE:-flat}" --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
kill "$pid" 2>/dev/null
for i in $(seq 100); do ps -p "$pid" > /dev/null || break; sleep 0.1; done
echo "sg errors: $(grep -c '\[sg\]\[error\]' "$out/client.log")"
grep -E "^(box select|sim click|sim menu|cursor:|clock|char )" "$out/client.log"
awk -f "$here/check.awk" "$out/client.log" > "$out/check.txt"
check=$?
cat "$out/check.txt"
[ "$check" -eq 0 ] && [ "$rc" -eq 0 ] || { echo "boxselect: FAIL (client rc $rc)"; exit 1; }
echo "boxselect: PASS"
