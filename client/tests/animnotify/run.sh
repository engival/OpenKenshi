#!/bin/bash
# Animation notify sounds (kenshi-h2v.29.6): run.sh walkrun|fight OUT. walkrun: bar_lure_stealth_arrows, the Wanderer walks / runs (sneak toggled, RUN clicked);
# fight: dismember_fight, the Wanderer attacks the bandit (its id comes from a probe run, charids.sh: the script names it @Dust bandit@; exit 4 when the scenario has no such character). OUT gets client.log (`notify:` lines and the audio
# thread's voices) and OUT/mix.wav (KAUDIO_WAV); peaks.py WAV LOG prints each world voice's L/R peak. Port: PORT (default: a free port).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
mode=${1:?walkrun or fight}
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
kscen=bar_lure_stealth_arrows; [ "$mode" = fight ] && kscen=dismember_fight
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/$kscen.kscen" --debug-view > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$out/server.log" $pid $port
if ! probe_char_ids "$tree/client/build" "$tree" "$port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$here/$mode.txt" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
cd "$tree" && KAUDIO_WAV="$out/mix.wav" timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --set debug.audio_trace=1 --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
n=$(grep -c '^notify' "$out/client.log")
echo "notify lines: $n"
[ "$rc" -eq 0 ] && [ "$n" -gt 0 ] || { echo "animnotify: FAIL (client rc $rc, $n notify lines)"; exit 1; }
