#!/bin/bash
# Regression check (kenshi-k16.2.160.1): a sneaking standing character, armed, draws `crouch idle` (the `stealth idle` record), loaded sneaking and after a
# sneak off / on; unarmed-off shows the standing clip. Fixture scenarios/bar_lure_stealth_arrows (the Wanderer sneaks with a sword).
# Usage: client/tests/sneakidle/run.sh OUT   Port: PORT (default: a free port). Exit 0 = pass, 1 = fail (loud). Shots in OUT.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
cat > "$out/script.txt" <<EOS
200 camera char Wanderer
300 dump anim
305 screenshot $out/a_loaded.png
310 key X down
315 key X up
400 dump anim
405 screenshot $out/b_off.png
410 key X down
415 key X up
500 dump anim
505 screenshot $out/c_on.png
510 quit
EOS
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/bar_lure_stealth_arrows.kscen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
cd "$tree" && timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
mapfile -t L < <(grep '^anim ' "$out/client.log")
fail=0
[[ "${L[0]:-}" == *"crouch idle"* ]] || { echo "FAIL loaded sneaking: ${L[0]:-no anim line}"; fail=1; }
[[ "${L[1]:-}" != *"crouch idle"* && "${L[1]:-}" == *"stand 1"* ]] || { echo "FAIL sneak off: ${L[1]:-no anim line}"; fail=1; }
[[ "${L[2]:-}" == *"crouch idle"* ]] || { echo "FAIL sneak back on: ${L[2]:-no anim line}"; fail=1; }
[ $fail = 0 ] && echo "PASS: crouch idle loaded sneaking, stand 1 sword off, crouch idle on again"
exit $fail
