#!/bin/bash
# kenshi-k16.2.353.1: the RTS camera tilts up past the original's +11.5 degrees (to +80); the follow camera's collision boom (boom_step) shortens the orbit
# as the ground comes between the focus and the eye and eases it back out. Usage: run.sh OUT MODE   (PORT, BUILD, SCEN = scenario name, default order_sounds)
#   shots  `set pitch` 11.5 / 30 / 60 / 80 / 0, a shot each (the .txt beside it carries the camera line: pivot, dist, pitch); "rtsboom:" per frame
#   tilt   the tilt keys held: , to the +80 stop, then . down past 0 to -30; "rtsboom:" lines (pitch, boom length, eye height, ground) per frame
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
mode=${2:?shots|tilt}
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"
server=$SERVER
scen=${SCEN:-order_sounds}
d=$out/$mode-$scen
mkdir -p "$d"
{
	echo "10 set animate 1"
	echo "100 key 1 down"; echo "101 key 1 up"
	echo "110 key Z down"; echo "111 key Z up"
	echo "150 set rtsboom 1"
	if [ "$mode" = shots ]; then
		f=200
		for p in 0 11.5 30 60 80 0; do
			echo "$f set pitch $p"; echo "$((f + 60)) screenshot $d/p$p-$f.png"; f=$((f + 120))
		done
		echo "$((f + 20)) quit"
	else
		echo "200 set pitch 0"
		echo "220 key COMMA down"; echo "1000 key COMMA up"
		echo "1100 key PERIOD down"; echo "2200 key PERIOD up"
		echo "2300 quit"
	fi
} > "$d/script.txt"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$tree/scenarios/$scen.kscen" --debug-view > "$d/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$d/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
server_listening "$d/server.log" $pid $port
(cd "$tree" && timeout 580 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/script.txt" > "$d/client.log" 2>&1)
rc=$?
kill $pid 2>/dev/null; wait $pid 2>/dev/null
echo "rtsboom lines: $(grep -c '^rtsboom:' "$d/client.log"); [sg][error]: $(grep -c '\[sg\]\[error' "$d/client.log")"
if [ "$mode" = tilt ]; then
	# kenshi-k16.2.353.2: no frame's length step may exceed 40 u while the pitch moves by under 1 deg; no frame with the eye < 2 u over the ground
	awk '/^rtsboom:/ { p=$3; l=$5; d=$7; e=$12; g=$15; n++
		if (n>1) { dp=p-pp; if (dp<0) dp=-dp; st=l-pl; if (st<0) st=-st; if (dp<1 && st>mx) mx=st; if (dp<1 && st>40) bad++ }
		if (e-g<2) low++; if (n==1||p<pmin) pmin=p; if (n==1||p>pmax) pmax=p; pp=p; pl=l; ld=l; dd=d }
		END { printf "rtstilt: frames %d pitch %.1f..%.1f max length step %.2f (<1 deg pitch), steps>40: %d, eye<2u over ground: %d, final len %.1f dist %.1f\n", n, pmin, pmax, mx, bad, low, ld, dd
		exit (bad>0 || low>0 || n==0) }' "$d/client.log" | tee "$d/check.txt"; chk=${PIPESTATUS[0]}
	[ "$chk" = 0 ] && echo "rtstilt check PASS" || echo "rtstilt check FAIL"
fi
. "$here/../runcheck.sh"
run_status "$rc" "$d/script.txt"
