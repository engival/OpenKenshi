#!/bin/bash
# Door and building sounds (kenshi-h2v.29.8): run.sh OUT. bar_lure: the Wanderer walks into the Hub bar through its door. OUT/client.log has the `combat sound:`
# lines (every SimView sound cue: Door_Open, Door_Shut, Character_*), the audio thread's `audio: Enter_Building ...` / `Leave_Building` lines and the
# voices; OUT/mix.wav is the KAUDIO_WAV render. Port: PORT (default: a free port).
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
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/bar_lure.kscen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
cd "$tree" && KAUDIO_WAV="$out/mix.wav" timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$here/bar.txt" > "$out/client.log" 2>&1
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
grep -a 'combat sound\|^audio: \(Enter\|Leave\)' "$out/client.log" | cut -c1-140
