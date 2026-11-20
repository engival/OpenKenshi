#!/bin/bash
# kenshi-k16.2.353.1/.4: the RTS camera tilts up past the original's +11.5 degrees (to +80); the collision boom (boom_step, shared with the drive camera)
# shortens the orbit as the ground comes between the focus and the eye, keeps a minimum orbit length outside the followed body (kenshi-k16.2.353.4) and
# eases the length through the direction window (no jump when hills swing in behind). Usage: run.sh OUT MODE   (PORT, BUILD, SCEN = scenario, default order_sounds)
#   shots  `set pitch` 0 / 11.5 / 30 / 60 / 80 / 0 (the user's F12 replay, 353.3), a shot each (the .txt beside it carries the camera line); "rtsboom:" per frame
#   tilt   the tilt keys held: , to the +80 stop, then . down past 0 to -30; "rtsboom:" lines (pitch, boom length, eye, ground, bounds, snaps) per frame
#   sweep  Q held through a full 405 deg at pitch 25, rtsdist 200 (DIST/PITCH override): the boom meets the Hub's walls and roofs and the hills
#          behind them, and the direction window's length has no frame change above the cap
#   jitter `set yaw` +-5 deg noise each frame over the same (rtsdist 200, pitch 25): the length moves only a little (no safety snap)
#   indoor sunshafts_bar (SCEN is ignored): the Wanderer alone in the Storm House (floor cut), tilted up; the eye stays under the roof
# Every mode's "rtsboom:" check (kenshi-k16.2.353.4): the eye is outside the followed character's hull box (bounds > 0, the distance to it logged) and at
# least kGroundClear (2 u) over the ground, and no frame's length changes by more than the ease's cap (kClearRate 1200 u/s at the 1/60 s scripted frame =
# 20 u; 22 allows the pitch blend's own term). "snap" is a jump the ease could not deliver in a frame (the window missed a fast move; a normal-speed
# rotation expects 0); "clamp" is the length held at the view direction's own allow (never past the world), which every frame of a held tilt key is
# (the target moves under the ease by ~0.2 u a frame - a smooth curve, not a jump).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
mode=${2:?shots|tilt|sweep|jitter|indoor}
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"
server=$SERVER
scen=${SCEN:-order_sounds}
[ "$mode" = indoor ] && scen=sunshafts_bar
d=$out/$mode-$scen
mkdir -p "$d"
{
	echo "10 set animate 1"
	echo "100 key 1 down"; echo "101 key 1 up"
	echo "110 key Z down"; echo "111 key Z up"
	echo "150 set rtsboom 1"
	case "$mode" in
	shots)
		f=200
		for p in 0 11.5 30 60 80 0; do
			echo "$f set pitch $p"; echo "$((f + 60)) screenshot $d/p$p-$f.png"; f=$((f + 120))
		done
		echo "$((f + 20)) quit"
		;;
	tilt)
		echo "200 set pitch 0"
		echo "220 key COMMA down"; echo "1000 key COMMA up"
		echo "1100 key PERIOD down"; echo "2200 key PERIOD up"
		echo "2300 quit"
		;;
	sweep)
		# a normal rotation: the orbit key's 54 deg/s through 405 deg (7.5 s), at pitch 25 and the wheel's first notch in (200 u: the boom reaches
		# the Hub's walls and roofs, and farther out the hills). The setup steps come before the log starts: they are teleports, and the boom snaps in
		# for them by design (the very jump the window cannot smooth)
		echo "140 set pitch ${PITCH:-25}"; echo "141 set rtsdist ${DIST:-200}"
		echo "310 key Q down"; echo "760 key Q up"
		echo "800 quit"
		;;
	jitter)
		# the same spot, the yaw set every frame, +-5 deg of noise (a 1 s period), around one heading (the setup before the log, as in sweep)
		echo "140 set pitch ${PITCH:-25}"; echo "141 set rtsdist ${DIST:-200}"; echo "142 set yaw 180"
		for i in $(seq 310 610); do
			y=$(awk -v i="$i" 'BEGIN { printf "%.2f", 180 + 5 * sin(i * 0.1047) }')
			echo "$i set yaw $y"
		done
		echo "650 quit"
		;;
	indoor)
		echo "200 set pitch 0"
		echo "220 key COMMA down"; echo "900 key COMMA up"
		for i in $(seq 0 4); do echo "$((240 + i * 120)) screenshot $d/in$i.png"; done
		echo "1000 quit"
		;;
	esac
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
	# kenshi-k16.2.353.2/.4: no frame's length step above the ease's cap while the pitch moves by under 1 deg; the eye always outside the
	# body (bounds > 0, its distance to the hull box) and over the ground (>= 2 u); the safety snaps reported.
	awk '/^rtsboom:/ { p=$3; l=$5; d=$7; e=$12; g=$15; bd=$17; sn=$19; cl=$21; n++
		if (n>1) { dp=p-pp; if (dp<0) dp=-dp; st=l-pl; if (st<0) st=-st; if (st>mx) mx=st; if (dp<1 && st>22) bad++ }
		if (e-g<2) low++; if (bd<=0) body++; snaps+=sn; clamps+=cl
		if (n==1||p<pmin) pmin=p; if (n==1||p>pmax) pmax=p; pp=p; pl=l; ld=l; dd=d }
		END { printf "rtstilt: frames %d pitch %.1f..%.1f max length step %.2f (<1 deg pitch, cap 22), steps>22: %d, eye<2u over ground: %d, eye at/inside the body: %d, safety snaps %d, allow clamps %d, final len %.1f dist %.1f\n", n, pmin, pmax, mx, bad, low, body, snaps, clamps, ld, dd
		exit (bad>0 || low>0 || body>0 || n==0) }' "$d/client.log" | tee "$d/check.txt"; chk=${PIPESTATUS[0]}
	[ "$chk" = 0 ] && echo "rtstilt check PASS" || echo "rtstilt check FAIL"
else
	# the new modes: the same eye checks at every frame, no per-frame length change above the cap, and the snaps printed
	# a mode whose pitch is set in a step (shots) cannot hold the per-frame cap: it checks the eye only
	capok=1; [ "$mode" = shots ] && capok=0
	awk -v m="$mode" -v capok="$capok" '/^rtsboom:/ { l=$5; e=$12; g=$15; bd=$17; n++
		if (n>1) { st=l-pl; if (st<0) st=-st; if (st>mx) mx=st; if (capok && st>22) big++ }
		if (e-g<2) low++; if (bd<=0) body++; snaps+=$19; clamps+=$21; pl=l }
		END { printf "rtstilt %s: frames %d max length step %.2f (cap 22), eye<2u over ground: %d, eye at/inside the body: %d, safety snaps %d, allow clamps %d, steps>22: %d\n", m, n, mx, low, body, snaps, clamps, big
		exit (low>0 || body>0 || big>0 || n==0) }' "$d/client.log" | tee "$d/check.txt"; chk=${PIPESTATUS[0]}
	[ "$chk" = 0 ] && echo "rtstilt check PASS" || echo "rtstilt check FAIL"
fi
. "$here/../runcheck.sh"
run_status "$rc" "$d/script.txt"
