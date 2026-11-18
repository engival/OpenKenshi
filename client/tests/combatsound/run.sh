#!/bin/bash
# Combat sounds (kenshi-h2v.29.7): run.sh fight|beak|shot|female|goat OUT. fight: dismember_fight (the Wanderer attacks the Dust bandit);
# beak: beak_open_ground (the Wanderer attacks a Beak Thing: animal voices, death); shot: cage_prisoner (the Wanderer attacks the Slaver boss, the crossbow
# guards shoot); female: female_fight (the bandit is female: VO_Races=Human_Female, kenshi-h2v.29.28); goat: goat_fight (a weak animal the player can kill:
# VO_Creature_Take_Damage and VO_Creature_Die). Port: PORT
# (default 47831). OUT gets client.log (`combat sound:` lines, the audio thread's voices) and mix.wav (KAUDIO_WAV). The script names characters
# as @Name@ (charids.sh: a probe run's `dump chars` of the fresh scenario; a name it lacks exits 4); exit 0 needs a clean client and at least
# one `combat sound` line (kenshi-k16.2.308); beak with none prints a GAP line and exits 1. The beak world fights on its own, so its ids are
# probed on a throwaway server: a probe on the measured server would spend the world's only hit before the measured client exists
# (kenshi-k16.2.315).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
mode=${1:?fight, beak, shot, female or goat}
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
case $mode in fight) kscen=dismember_fight;; beak) kscen=beak_open_ground;; shot) kscen=cage_prisoner;; female) kscen=female_fight;; goat) kscen=goat_fight;; *) echo "unknown mode $mode"; exit 2;; esac
scenario=${KSCEN:-$tree/scenarios/$kscen.kscen}
start_server()	# start_server LOG: the server in the background on a fresh free port and listened (SERVER_PID, SERVER_PORT; kenshi-k16.2.358)
{
	start_server_on_free_port "$1" "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:@PORT@ --scenario-file "$scenario" --debug-view
}
# kenshi-k16.2.315: the beak world fights on its own - the Beak Things are hostile wild animals 20 u off and their first hit downs the
# Wanderer inside the world's first second. The id probe is a client session too, and the sim ticks while it is connected, so a probe on
# the measured server spends that hit before the measured client exists (0 `combat sound` lines, the 308 GAP). The beak's ids come from a
# throwaway server instead (the same scenario file, so the same spawn ids) and the measured run then gets the world from its own first
# session. The other modes start their fight from the script's own attack, so their probe stays on the measured server.
if [ "$mode" = beak ]
then
	PORT= start_server "$out/probe-server.log"
	probe_pid=$SERVER_PID; probe_port=$SERVER_PORT
	if ! probe_char_ids "$tree/client/build" "$tree" "$probe_port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$probe_pid" 2>/dev/null; exit 4; fi
	kill "$probe_pid" 2>/dev/null; wait "$probe_pid" 2>/dev/null
fi
start_server "$out/server.log"
pid=$SERVER_PID; port=$SERVER_PORT
if [ "$mode" != beak ] && ! probe_char_ids "$tree/client/build" "$tree" "$port" "$out/ids.txt"; then echo "run.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$here/$mode.txt" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
cd "$tree" && KAUDIO_WAV="$out/mix.wav" timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --set debug.audio_trace=1 --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
n=$(grep -c '^combat sound' "$out/client.log")
echo "client rc $rc; combat sound lines: $n"
if [ "$n" -eq 0 ] && [ "$mode" = beak ]; then	# k16.2.308: beak counts the Beak Thing's hits on the Wanderer; none voiced is a gap, reported, not exempted
	echo "GAP: no combat sound line in the beak run: the measured client's session saw no hit event on the Wanderer (and the Wanderer is down, $(grep -c '^hud: Wanderer.* down' "$out/client.log") hud lines): the world's first hit must arrive in a client's first session (kenshi-k16.2.315)" >&2
fi
[ "$rc" -eq 0 ] && [ "$n" -gt 0 ]
