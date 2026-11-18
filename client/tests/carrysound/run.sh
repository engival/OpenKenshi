#!/bin/bash
# Carry sounds (kenshi-h2v.29.8): run.sh OUT. slavers_downed: the Wanderer attacks the Slaver boss, is put down and carried by the caravan. OUT/client.log has the `combat sound:`
# lines (every SimView sound cue: Door_Open, Door_Shut, Character_*), the audio thread's `audio: Enter_Building ...` / `Leave_Building` lines and the
# voices; OUT/mix.wav is the KAUDIO_WAV render. Port: PORT (default: a free port). The script names the boss as @Slaver boss@ (charids.sh: a probe
# run fills its id in; exit 4 when the scenario has no such character).
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
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/slavers_downed.kscen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
if ! probe_char_ids "$tree/client/build" "$tree" "$port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$here/slavers.txt" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
cd "$tree" && KAUDIO_WAV="$out/mix.wav" timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --set debug.audio_trace=1 --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
grep -a 'combat sound\|^audio: \(Enter\|Leave\)' "$out/client.log" | cut -c1-140
[ "$rc" -eq 0 ] || { echo "carrysound: FAIL (client rc $rc)"; exit 1; }
