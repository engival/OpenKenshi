#!/bin/bash
# lightning (kenshi-k16.2.177.1): the type 7 bolt against the original's live capture (kenshi-rwh.120).
# A squad scenario at the Hub pins the weather to "Heavy_Rain deadlands"; spawnBolt relocates the first bolt to a fixed
# point near the player character (world -52235.4, 1328.3, 3138.3, terrain + 10) on frame ~62, so one frame a shot gives
# the whole flash and its tail. The particle step is pinned to 1/60 s (`set particles 0.01667`) so the frame numbers are
# game time at 60 fps.
#   run.sh MODE OUT      MODE = wide | mid | close | tail | all
#     wide   the k16.2.11.1 follow framing (yaw -99, pitch 5), the bolt column in the distance.
#     mid    the original capture's framing (pivot ~ base + 500, camera 900 u away, pitch -8): the montage comparison.
#     close  a free camera 250 u south of the base: the fireball at the foot and the SPARKS.
#     tail   the same camera, one shot every 20 frames to ~7 s after the spawn: the SPARKS outlive the 1 s handler and the
#            psys (its particle count is in each shot's `particles N drawn: world Lightning_Bolt x1.00 M` line) runs out.
# OUT holds the PNGs (fNNN.png), the generated script, the client log (the framestats line prints the live particle
# counts) and OUT/xdg. PORT must stay in this worker's range (47830-47839).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
mode=${1:?wide|mid|close|all}
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
build=${BUILD:-$tree/client/build}
# PORT names the sim server's port; unset, take the first free one in this worker's range (two sessions share the machine)
free_port()
{
	local p
	for p in $(seq 47830 47839); do
		if ! (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then echo "$p"; return 0; fi
	done
	return 1
}
port=${PORT:-$(free_port)} || { echo "run.sh: no free port in 47830-47839" >&2; exit 2; }
export XDG_DATA_HOME="$out/xdg"
mkdir -p "$XDG_DATA_HOME"

# The deterministic bolt's base (k16.2.143: spawnBolt near the squad scenario's character; terrain 1318.3 + 10).
BX=-52235.4
BZ=3138.3
BY=1328.3

# frame list: from before the handler spawns to well past the flash, one shot a frame
write_script()
{
	local name=$1 first=$2 last=$3 cam=$4 step=${5:-1}
	local f
	{
		echo "# lightning $name: one dump weather + one shot every $step frame(s) from $first to $last."
		echo "0 set particles 0.01667"
		echo "20 set pitch 5"
		echo "20 set yaw -99"
		case $cam in
		mid)
			# the original's capture framing: play distance (900 u), eye ~150 u over the bolt base, pitch -8. The hub slope
			# rises +z, so the camera goes north (z 2238, terrain 1219.2) looking +z at the base (yaw 180), alt 250 -> eye 1469.
			# frame 25: after the follow camera auto-centres (frame ~20), before the handler spawns (~62)
			printf '%s\n' "25 set pos $BX,2238" "25 set alt 250" "25 set yaw 180" "25 set pitch -8"
			;;
		close|tail)
			# 250 u south of the base, 40 u over it: the foot with the fireball and the sparks
			printf '%s\n' "25 set pos $BX,3388" "25 set alt 40" "25 set yaw 0" "25 set pitch 5"
			;;
		esac
		for ((f = first; f <= last; f += step)); do
			echo "$f dump weather"
			echo "$f screenshot $out/$name/f$(printf '%03d' "$f").png"
		done
		echo "$last quit"
	} > "$out/script-$name.txt"
}

run_mode()
{
	local name=$1 first=$2 last=$3 cam=$4 step=${5:-1}
	mkdir -p "$out/$name"
	write_script "$name" "$first" "$last" "$cam" "$step"
	"$build/cpp/ksim_server" --listen 127.0.0.1:$port --scenario squad > "$out/server-$name.log" 2>&1 &
	local spid=$!
	local up=0
	for _ in $(seq 240); do grep -q "^listening" "$out/server-$name.log" && { up=1; break; }; sleep 0.5; done
	if [ "$up" = 0 ] || ! kill -0 $spid 2>/dev/null
	then
		echo "$name: the sim server did not listen on 127.0.0.1:$port:" >&2
		sed 's/^/  /' "$out/server-$name.log" >&2
		kill $spid 2>/dev/null
		return 2
	fi
	cd "$tree" || exit 2
	nice -n 10 timeout 900 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --debug \
		--weather "Heavy_Rain deadlands" --simlink 127.0.0.1:$port --script "$out/script-$name.txt" > "$out/client-$name.log" 2>&1
	local rc=$?
	kill $spid 2>/dev/null
	echo "$name: client rc $rc; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client-$name.log"); shots: $(ls "$out/$name"/*.png 2>/dev/null | wc -l)"
	return $rc
}

rc=0
case $mode in
	wide)  run_mode wide  55 190 follow || rc=$? ;;
	mid)   run_mode mid   40 190 mid    || rc=$? ;;
	close) run_mode close 40 190 close  || rc=$? ;;
	tail)  run_mode tail  90 440 tail 20 || rc=$? ;;
	midtail) run_mode midtail 150 350 mid 20 || rc=$? ;;
	all)   run_mode wide 55 190 follow && run_mode mid 40 190 mid && run_mode close 40 190 close || rc=$? ;;
	*) echo "run.sh: unknown mode $mode (wide|mid|close|tail|midtail|all)" >&2; exit 2 ;;
esac
exit $rc
