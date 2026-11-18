#!/bin/bash
# lampbeams (kenshi-jj3.28.1): run.sh OUT. Five headless runs of bar.txt on the bar_lure scenario: render.lamp_beams 0, 0.3, 1 (OUT/b0, b0.3, b1), 1 with render.sun_shafts 1 (OUT/b1_sun)
# and, with LAMPS=1, render.lamp_occlusion alone as the null (OUT/occ). Each writes night.png and day.png. Port: PORT (47834).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"
server=$SERVER
base=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
for mode in ${MODES:-b0 b0.3 b1 b1_sun}
do
	out="$base/$mode"; mkdir -p "$out"
	sed "s#OUTDIR#$out#g" "${SCRIPT:-$here/bar.txt}" > "$out/script.txt"
	case $mode in
		b0) extra=(--set render.lamp_beams=0) ;;
		b0.3) extra=(--set render.lamp_beams=0.3) ;;
		b1) extra=(--set render.lamp_beams=1) ;;
		b1_lock) extra=(--set render.lamp_beams=1 --set render.thread=lockstep) ;;
		b0_lock) extra=(--set render.lamp_beams=0 --set render.thread=lockstep) ;;
		b1_sun) extra=(--set render.lamp_beams=1 --set render.sun_shafts=1) ;;
		sun) extra=(--set render.sun_shafts=1) ;;
		occ) extra=(--set render.lamp_occlusion=true) ;;
	esac
	"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/bar_lure.kscen" --debug-view > "$out/server.log" 2>&1 &
	pid=$!
	for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
	server_listening "$out/server.log" $pid $port
	(cd "$tree" && timeout 300 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug "${extra[@]}" ${CLIENT_ARGS[@]+"${CLIENT_ARGS[@]}"} --script "$out/script.txt" > "$out/client.log" 2>&1)
	for tm in night:01:00 day:12:00; do n=${tm%%:*}; h=${tm#*:}; for k in 0 1 2 3 4; do grep -q "^time: $h" "$out/$n$k.txt" 2>/dev/null && { cp "$out/$n$k.png" "$out/$n.png"; break; }; done; [ -f "$out/$n.png" ] || echo "$mode: no $n shot at the right hour"; done
	ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
	echo "$mode: [sg][error] $(grep -c '\[sg\]\[error' "$out/client.log")"
done
