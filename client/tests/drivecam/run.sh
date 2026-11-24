#!/bin/bash
# kenshi-k16.2.289.1: the direct-drive camera boom, scripted. Usage: run.sh OUT MODE   (PORT 47830, BUILD)
#   spot  the user's shot spot (Wanderer on the Storm House porch, "B" sign between camera and character): shots + dump cuts
#   orbit D held: the camera turns round the standing character; "boom:" per frame
#   custom  SCRIPT=file: script lines after the follow setup (dump shapes X Z R thin lists thin props; set dist D sets the follow distance)
#   walk  W held 4 s ("boom:" per frame); SCEN=other.kscen puts the character elsewhere / facing elsewhere
# The followed character is the Wanderer: the script names it @Wanderer@ and a probe run (charids.sh) fills its id in (exit 4 when the scenario has none).
set -u
export KCLIENT_FRAMELOG=boom	# the "boom" log lines this test parses are opt-in (kenshi-jvhw)
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
mode=${2:?mode}
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
scen=${SCEN:-$here/drivecam.kscen}
d=$out/$mode
mkdir -p "$d"
{
	echo "10 set animate 1"
	echo "100 select id=@Wanderer@"
	echo "110 follow char id=@Wanderer@"
	if [ "$mode" = custom ]; then
		# SCRIPT=file: its lines follow the follow-char setup (frame numbers from 120)
		cat "${SCRIPT:?SCRIPT file}"
	elif [ "$mode" = spot ]; then
		echo "180 set dist ${DIST:-66.5}"
		echo "181 set pitch ${PITCH:--25.8}"
		for i in 0 1 2; do echo "$((260 + i * 30)) screenshot $d/spot$i.png"; done
		echo "290 dump occluders"
		echo "400 quit"
	elif [ "$mode" = orbit ]; then
		# turn in place: D held, the camera goes round the standing character (the boom meets whatever is behind it); a shot every 8 frames
		echo "181 set pitch ${PITCH:--25.8}"
		echo "200 key D down"
		echo "420 key D up"
		for i in $(seq 0 8 230); do echo "$((196 + i)) screenshot $d/o$(printf %03d $i).png"; done
		echo "520 quit"
	else
		# walk: W held for 4 s from the start of the scenario (the character's yaw says which way; the camera follows behind)
		echo "181 set pitch ${PITCH:--25.8}"
		echo "200 key W down"
		echo "440 key W up"
		for i in $(seq 0 12 300); do echo "$((196 + i)) screenshot $d/w$(printf %03d $i).png"; done
		echo "560 quit"
	fi
} > "$d/script.tmpl"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$scen" --debug-view > "$d/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$d/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$d/server.log" $pid $port
if ! probe_char_ids "$build" "$tree" "$port" "$d/ids.txt"; then echo "run.sh: id probe found no characters (see $d/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$d/script.tmpl" "$d/ids.txt" "$d" "$d/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
(cd "$tree" && timeout 400 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/script.txt" > "$d/client.log" 2>&1)
rc=$?
kill $pid 2>/dev/null; wait $pid 2>/dev/null
grep -c "^boom:" "$d/client.log"
. "$here/../runcheck.sh"
run_status "$rc" "$d/script.txt"
