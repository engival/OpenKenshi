#!/bin/bash
# kenshi-xm7v (k16.2.236.13 Q4): camera follow only locks onto the player's own squad. progress_bars: Wanderer/Lockpicker (squad), Captive (an NPC).
#   - a cast-row double click (portrait_focus) and the cast FOLLOW button on the NPC set the watch (inspect) but start no camera follow;
#   - Z / F with the NPC as the inspect focus follows the selection (a squad member), never the NPC;
#   - the same routes on a squad member do follow.
# Usage: run.sh OUT. PORT, BUILD as follow_target.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"
server=$SERVER
d=$out
{
	echo "130 dump chars"
	echo "140 cast focus id=@Captive@"
	echo "150 follow state"		# S0: no follow on the NPC
	echo "160 cast follow id=@Captive@"
	echo "170 follow state"		# S1: still none
	echo "180 inspect id=@Captive@"
	echo "190 follow on"
	echo "200 follow state"		# S2: follows the selection (a squad member), not the NPC
	echo "210 follow off"
	echo "220 cast follow id=@Lockpicker@"
	echo "230 follow state"		# S3: the squad member
	echo "240 quit"
} | awk '{ $1 += 400; print }' > "$d/script.in"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/progress_bars.kscen" --debug-view > "$d/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$d/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$d/server.log" $pid $port
. "$here/../charids.sh"
probe_char_ids "$build" "$tree" "$port" "$d/ids.txt" || { echo "FAIL id probe"; kill $pid; exit 4; }
charids_fill "$d/script.in" "$d/ids.txt" "$d" "$d/script.txt" || { kill $pid; exit 4; }
(cd "$tree" && timeout 300 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/script.txt" > "$d/client.log" 2>&1)
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
mapfile -t st < <(grep -o "follow state: .*" "$d/client.log")
[ ${#st[@]} = 4 ] || { echo "FAIL expected 4 state lines, got ${#st[@]}"; exit 1; }
drv() { echo "${st[$1]}" | sed -n 's/.* drive \([0-9]*\) .*/\1/p'; }
# the NPC's id is in the script's cast lines
npc=$(sed -n 's/^[0-9]* cast focus id=\([0-9]*\)/\1/p' "$d/script.txt" | head -1)
lp=$(sed -n 's/^[0-9]* cast follow id=\([0-9]*\)/\1/p' "$d/script.txt" | tail -1)
fail=0
chk() { if [ "$2" = "$3" ]; then echo "PASS $1"; else echo "FAIL $1: got '$2' want '$3'"; fail=1; fi; }
chk "cast focus on an NPC: no camera follow" "$(drv 0)" 0
chk "cast FOLLOW on an NPC: no camera follow" "$(drv 1)" 0
[ "$(drv 2)" != "$npc" ] && echo "PASS F with an NPC inspected does not follow it" || { echo "FAIL F followed the NPC"; fail=1; }
chk "cast FOLLOW on a squad member" "$(drv 3)" "$lp"
exit $fail
