#!/bin/bash
# sunshafts_air (kenshi-rlbr): run.sh OUT. FAILS when the noon sun shaft or the dust motes draw nothing in the air of the Hub bar. air.txt: the camera inside the bar at the doorway
# column, noon, each air term alone (`light sun_shafts|dust_motes solo`: nothing else lights the scene, so people cannot move the picture). Runs the on state (render.sun_shafts
# ON_SHAFTS, default 1; render.dust_motes ON_MOTES, default 1) and the off state (both 0), in the overlap mode and, with LOCK=1 (default), in lockstep.
# Measure: the mean grey of the 1280x720 shot's air (rows 0..500, the minimap box masked) must beat the off state by MIN_SUN (default 8) for the shaft and MIN_MOTES (default 2) for the motes.
# ON_SHAFTS=0 ON_MOTES=0 forces the "broken" state: the check must then FAIL. Needs a game-data tree (SKIP otherwise). Exit 1 on a failure.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$here/../need_data.sh"
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"
pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"
server=$SERVER
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$tree/client/tools/ports.sh"
minsun=${MIN_SUN:-8}; minmotes=${MIN_MOTES:-2}
modes="on off"; threads="overlap"; [ "${LOCK:-1}" = 1 ] && threads="overlap lockstep"
for th in $threads; do for mode in $modes; do
	d="$out/${mode}_$th"; mkdir -p "$d"
	sed "s#OUTDIR#$d#g" "$here/air.txt" > "$d/script.txt"
	if [ $mode = on ]; then extra=(--set render.sun_shafts=${ON_SHAFTS:-1} --set render.dust_motes=${ON_MOTES:-1}); else extra=(--set render.sun_shafts=0 --set render.dust_motes=0); fi
	[ $th = lockstep ] && extra+=(--set render.thread=lockstep)
	start_server_on_free_port "$d/server.log" "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:@PORT@ --scenario-file "$tree/scenarios/bar_lure.kscen" --debug-view
	pid=$SERVER_PID; port=$SERVER_PORT
	(cd "$tree" && timeout 300 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug "${extra[@]}" --script "$d/script.txt" > "$d/client.log" 2>&1)
	ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
	for n in sun dust; do for k in 0 1; do grep -q "^time: 12:00" "$d/$n$k.txt" 2>/dev/null && { cp "$d/$n$k.png" "$d/$n.png"; break; }; done; [ -f "$d/$n.png" ] || cp "$d/${n}1.png" "$d/$n.png" 2>/dev/null; done
done; done
fail=0
for th in $threads; do
	for t in "sun $minsun" "dust $minmotes"; do
		set -- $t
		if [ -f "$out/on_$th/$1.png" ] && [ -f "$out/off_$th/$1.png" ]; then
		d=$(python3 -I - "$out/off_$th/$1.png" "$out/on_$th/$1.png" <<'P'
import sys
from PIL import Image
import numpy as np
def g(p):
    a = np.asarray(Image.open(p).convert('L')).astype(float)[:500]
    a[:240, 1100:] = 0	# the minimap
    return a
print('%.2f' % (g(sys.argv[2]).mean() - g(sys.argv[1]).mean()))
P
)
		else d=-999; fi
		name=$1; [ $1 = dust ] && name=motes
		if python3 -I -c "import sys; sys.exit(0 if float('$d') >= float('$2') else 1)"; then echo "sunshafts_air $th $name: PASS air diff $d >= $2"; else echo "sunshafts_air $th $name: FAIL air diff $d < $2 (nothing drawn in the air)"; fail=1; fi
	done
done
exit $fail
