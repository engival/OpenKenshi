#!/bin/bash
# The floor view, live (kenshi-k16.2.236.1 .. 236.10). Every case asserts on what the user sees, not on which rule wrote it (the fable review
# kenshi-k16.2.236.9, REVIEW.md section 5): the SEQUENCE of shown levels, read from the one line WorldBuildings::updateFocus prints per change
# ("floor: view A -> B (reason)", dev builds with --debug; the reason is for the reader, never asserted), and the invariant: at each dump
# `dump cut id=<subject>` says hidden 0 for the label and the body, unless the script itself stepped the level (R / F: those cases dump the cut only
# before the step, and say so). The old rules' own lines (auto threshold, stair hotspot, snap on engage) are no longer asserted on.
# A case whose sequence is wrong today is a KNOWN-FAIL: the observed sequence and the reason print, the run does not fail on it, and it turns into a
# plain PASS when a later step of the migration (kenshi-k16.2.236.11 / .12) fixes it. Remove the reason then.
# The follows the original has (camera_rts.md 5): V (the hover / drive camera, `follow char`) and Z (focus_char: the RTS camera centred on the
# character; F is floor_down, ui/keys.cpp). The Storm House: the front door sets floor 0, the stairs' top step floor 1.
# Usage: run.sh OUT   (OUT: output dir). PORT (default: a free port), BUILD (default client/build of this tree).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
fail=0

# --- harness -------------------------------------------------------------------------------------------------------------------------------
# start a scenario's server and run $d/script.txt against it; server.log and client.log land in $d
run_live()
{
	local d=$1 scen=${2:-order_sounds} pid i port
	start_server_on_free_port "$d/server.log" "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:@PORT@ --scenario-file "$tree/scenarios/$scen.kscen" --debug-view	# a fresh port per start (kenshi-k16.2.358)
	pid=$SERVER_PID; port=$SERVER_PORT
	(cd "$tree" && timeout 580 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --set key.floor_up=Period --set key.floor_down=Comma --script "$d/script.txt" > "$d/client.log" 2>&1)
	kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}
# The wire id of a scenario's character (the sim's instance counter: it shifts with whatever the world load creates first, kenshi-k16.2.257), read once
# per scenario from a `dump chars`. `dump cut` takes an id, not a name.
idof()
{
	local scen=$1 name=$2 d=$out/ids_$1
	if [ ! -f "$d/client.log" ]; then
		mkdir -p "$d"
		printf '10 set animate 1\n300 dump chars\n320 quit\n' > "$d/script.txt"
		run_live "$d" "$scen"
	fi
	grep -o "^char [0-9]* \"$name\"" "$d/client.log" | head -1 | grep -o '[0-9]*'
}
# The shown level's sequence: the start level (the session starts on 3) and each "view A -> B" target, e.g. "3 0 1".
views() { { echo 3; grep -o "floor: view [0-9] -> [0-9]" "$1/client.log" | sed 's/.*-> //'; } | tr '\n' ' ' | sed 's/ $//'; }
# the number of cut dump lines that say a character is hidden (label or body), and how many dumps there were
hiddenCount() { grep -E "^  (label feet|body \(Characters pos\))" "$1/client.log" | grep -c "hidden 1$"; }
# the frames the followed subject was not drawn (WorldBuildings' per-frame line "floor: subject hidden", kenshi-k16.2.236.15): zero outside a floor step (which ends the follow)
frameHidden() { grep -c "^floor: subject hidden" "$1/client.log"; }
cutDumps() { grep -c "^cut id " "$1/client.log"; }
lastFloor() { grep -o "floor: [A-Z][A-Za-z ]* shown .*" "$1/client.log" | tail -1; }	# any named building (the Small Shack too, kenshi-k16.2.355); the '- shown' no-lead default never matches
# finish NAME DIR WANT_REGEX LAST_GLOB [KNOWN_REASON] [NO_INVARIANT]: WANT_REGEX matches the sequence, LAST_GLOB the last `dump floor` line, the invariant holds
# (cut dumps present and none hidden; NO_INVARIANT=1 skips it for a case that steps the level itself). A KNOWN_REASON turns a miss into KNOWN-FAIL.
finish()
{
	local name=$1 d=$2 want=$3 lastglob=$4 known=${5:-} noinv=${6:-0} seq last hid dumps bad="" fh
	seq=$(views "$d"); last=$(lastFloor "$d"); hid=$(hiddenCount "$d"); dumps=$(cutDumps "$d"); fh=$(frameHidden "$d")
	[[ "$seq" =~ $want ]] || bad="$bad sequence '$seq' is not /$want/;"
	[[ "$last" == $lastglob ]] || bad="$bad last '$last' is not '$lastglob';"
	if [ "$noinv" != 1 ]; then
		[ "$hid" = 0 ] || bad="$bad the followed character was hidden in $hid cut line(s);"
		[ "$fh" = 0 ] || bad="$bad the followed character was hidden on $fh frame(s);"
		[ "$dumps" -ge 1 ] || bad="$bad no cut dump ran;"
	fi
	if [ -z "$bad" ]; then echo "PASS $name: $seq (hidden frames $fh)"
	elif [ -n "$known" ]; then echo "KNOWN-FAIL $name: observed '$seq', hidden $hid/$dumps;$bad known: $known"
	else echo "FAIL $name: observed '$seq', hidden $hid/$dumps;$bad"; fail=1; fi
}
# script lines: dumps every STEP frames from A to B, each a floor dump plus the subject's cut dump (the invariant's evidence)
dumps() { local a=$1 b=$2 step=$3 id=$4 f; for f in $(seq "$a" "$step" "$b"); do echo "$f dump floor"; if [ -n "$id" ]; then echo "$((f+1)) dump cut id=$id"; fi; done; }
# the shared opening: the first character selected and centred, followed ($1: the follow line), walked in the front door (125, 400); climb: up the stairs (700)
open() { echo "10 set animate 1"; echo "100 key 1 down"; echo "101 key 1 up"; echo "110 ${1:-follow char Wanderer}"; echo "125 move -50616 -1130"; echo "400 move -50616 -1200"; }
openZ() { open "key Z down"; echo "112 key Z up"; }	# the RTS camera centred on the lead (the follow of L1-L5; `follow char` is direct drive, where a step is ignored, D2)
climb() { echo "700 move -50590 -1230"; }

# ONLY="case ..." runs just those cases (a rerun of a subset); unset, every case runs
want_case() { [ -z "${ONLY:-}" ] || [[ " $ONLY " == *" $1 "* ]]; }

# --- order_sounds / bar_lure_hostile: the door then the stairs, V and Z -----------------------------------------------------------------------
# order_sounds (nobody hostile): the start centring snaps 3 -> 0, the front door keeps 0, the stairs' top step makes 1: 3 0 1, ending following on 1/1.
# bar_lure_hostile (a fight with the raiders, so not deterministic): 0 at least once, 1 at most once, ending following.
run_case()
{
	local name=$1 scen=$2 follow=$3 d=$out/$1 lead f want last
	want_case "$name" || return 0
	lead=$(idof "$scen" Wanderer)
	mkdir -p "$d"
	{
		echo "10 set animate 1"
		echo "100 key 1 down"
		echo "101 key 1 up"
		echo "$follow"
		echo "125 move -50616 -1130"
		for f in $(seq 150 60 1000); do echo "$f dump floor"; if [ $((f % 120)) = 30 ]; then echo "$((f+1)) dump cut id=$lead"; fi; done
		echo "400 move -50616 -1200"
		echo "700 move -50590 -1230"
		echo "1100 dump floor"
		echo "1101 dump cut id=$lead"
		echo "1110 quit"
	} > "$d/script.txt"
	run_live "$d" "$scen"
	if [ "$scen" = order_sounds ]; then want='^3 0 1$'; last='*shown 1/1*(following)*'
	else want='^3 0( 1)?( 0)*$'; last='*(following)*'; fi
	finish "$name" "$d" "$want" "$last"
}
run_case order_sounds_V order_sounds "110 follow char Wanderer"
run_case order_sounds_Z order_sounds "110 key Z down
112 key Z up"
run_case bar_lure_hostile_V bar_lure_hostile "110 follow char Wanderer"
run_case bar_lure_hostile_Z bar_lure_hostile "110 key Z down
112 key Z up"

# --- snap on engage (kenshi-k16.2.236.2, .5) ------------------------------------------------------------------------------------------------
# The lead is walked up the stairs to floor 1 with the camera free (`camera char`: no subject); the view is set (KEYS: one F press each, stepping it
# down; none = the start level 3, out of range for the 0..1 building); the camera then engages by the route in ENGAGE. The view must end on 1.
run_snap()
{
	local name=$1 lead=$2 keys=$3 engage=$4 start=$5 prefollow=$6 want=$7 d=$out/$1 id pre
	want_case "$name" || return 0
	id=$(idof order_sounds Wanderer)
	mkdir -p "$d"
	{
		echo "10 set animate 1"
		echo "100 key 1 down"
		echo "101 key 1 up"
		[ -n "$start" ] && echo "$start" | sed "s#ID#$lead#g"
		echo "125 move -50616 -1130"
		echo "400 move -50616 -1200"
		echo "700 move -50590 -1230"
		local t=1192 k
		for k in $keys; do echo "$t key COMMA down"; echo "$((t+1)) key COMMA up"; t=$((t+3)); done
		echo "$t dump floor"
		echo "$engage" | sed "s#ID#$lead#g; s#T0#$((t+10))#g; s#T1#$((t+11))#g; s#T2#$((t+12))#g; s#T3#$((t+13))#g"
		echo "$((t+30)) dump floor"
		echo "$((t+31)) dump cut id=$id"
		echo "$((t+40)) quit"
	} > "$d/script.txt"
	run_live "$d"
	pre=$(grep -o "floor: Storm House.*" "$d/client.log" | tail -2 | head -1)	# before the engage: the lead on floor 1, the view as set
	if [[ "$pre" != *"lead 1 $prefollow"* ]] || [[ "$pre" == *"level 1" ]]; then echo "FAIL $name: the fixture is not the case (want lead 1 $prefollow, level not 1): $pre"; fail=1; return; fi
	finish "$name" "$d" "$want" '*shown 1/1*(following)*level 1'
}
for view in 3 0; do
	keys=""; [ $view = 0 ] && keys="a b c"
	run_snap snap_V_$view Wanderer "$keys" "T0 follow char ID" "105 camera char ID" "(not following)" '^3 0 1$'
	run_snap snap_Z_$view Wanderer "$keys" "T0 key Z down
T1 key Z up" "105 camera char ID" "(not following)" '^3 0 1$'
	run_snap snap_double_$view Wanderer "$keys" "T0 key 1 down
T1 key 1 up
T2 key 1 down
T3 key 1 up" "105 camera char ID" "(not following)" '^3 0 1$'
done
# kenshi-k16.2.236.5: the session start's centring is an engage (no camera op: the scenario start centres on the selected lead and follows it), and a later
# explicit engage of that same, already-centred lead snaps too (the playtest bug: a double click on the start-centred character did nothing). The lead is
# walked up to floor 1, the view stepped down (Comma = floor_down) to 0 (the step ends the follow, L4/L5: the free level 0), then Z / the double 1 re-engages it: 3, 0 (start), 1, 0 (step), 1.
run_snap reengage_Z Wanderer "a" "T0 key Z down
T1 key Z up" "" "(not following)" '^3 0 1 0 1$'
run_snap reengage_double Wanderer "a" "T0 key 1 down
T1 key 1 up
T2 key 1 down
T3 key 1 up" "" "(not following)" '^3 0 1 0 1$'
# the start's own snap: the start centring engages, so the level 3 snaps to the lead's outdoor floor 0 (the first change in the sequence)
if ! want_case reengage_Z; then :
elif [[ "$(views "$out/reengage_Z")" == "3 0 "* ]]; then echo "PASS start_engage_snap: $(views "$out/reengage_Z")"; else echo "FAIL start_engage_snap: $(views "$out/reengage_Z")"; fail=1; fi

# --- the stairs ----------------------------------------------------------------------------------------------------------------------------
# stairs_walk: up once (0 -> 1 as the feet climb the top steps), a walk across floor 1 and a long stand beside the flight: no further change.
run_stairs_walk()
{
	local d=$out/stairs_walk id
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; climb
		dumps 900 1440 60 "$id"
		echo "1480 move -50578 -1246"	# a walk across the deck: no level change either
		dumps 1560 1860 60 "$id"
		dumps 1900 2800 60 "$id"	# a long stand on the deck beside the flight: nothing flips
		echo "2860 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish stairs_walk "$d" '^3 0 1$' '*shown 1/1*(following)*'
}
# stairs_click: a click on the stairs' top steps changes nothing by itself. The character is walked up (level 1, left on the deck); the free camera goes
# over the top steps and a right click there, the move order, leaves the level on 1; the walk to the click arrives on the top tread (a click one tread in:
# pick y 1134.7, k16.2.236.8.4.1) and that alone does not flip; the view drops to 0 only when the character walks on down, a rise under the top tread, and stays.
run_stairs_click()
{
	local d=$out/stairs_click id
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; climb
		dumps 900 1440 60 "$id"
		echo "1500 set pos -50548.7,-1274.7"
		echo "1501 set alt 100"
		echo "1502 set yaw 0"
		echo "1503 set pitch -89"
		echo "1510 mouse pos 640 360"
		echo "1515 dump pick"
		echo "1520 dump floor"
		echo "1525 mouse down right"
		echo "1527 mouse up right"
		echo "1528 dump floor"
		echo "1530 dump floor"
		echo "1540 follow char Wanderer"	# the free camera back on the character: its walk to the click arrives on the top tread
		dumps 1560 2400 60 "$id"
		echo "2440 move -50558 -1275"	# on down the flight to its foot
		dumps 2500 3200 60 "$id"
		echo "3260 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish stairs_click "$d" '^3 0 1 0$' '*level 0'
}
# stairs_down: walked down from the deck: 0, 1, 0, and no change on the way to the foot or in the long stand there.
# The descent ends at the flight's foot and the lead stays there (the old second move -50616 -1200 is a deck-level goal: its route climbs back up out
# of the stairwell, findings/kenshi-k16.2.236.8.4.1/tmp3/client.log, so the lead ended on floor 1).
run_stairs_down()
{
	local d=$out/stairs_down id f
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; climb
		echo "1480 move -50578 -1246"
		dumps 1560 1860 60 "$id"
		echo "1900 move -50558 -1275"	# down the flight to its foot
		dumps 1960 2100 60 "$id"
		for f in 1960 2040 2120 2200; do echo "$f screenshot $d/walkdown-$f.png"; done	# the descent: the flip frame
		dumps 2200 3000 60 "$id"	# standing at the foot: the view stays on 0
		echo "3060 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish stairs_down "$d" '^3 0 1 0$' '*level 0'
}
# stairs_shots (kenshi-7f72): a shot sweep through the up flip (the climb) and the down flip (the descent); each shot's sidecar carries the floor latch row (y against up_line / low_line).
run_stairs_shots()
{
	local d=$out/stairs_shots id f
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; climb
		for f in $(seq 800 8 1100); do echo "$f screenshot $d/climb-$f.png"; done
		echo "1480 move -50578 -1246"
		echo "1900 move -50558 -1275"
		for f in $(seq 1960 8 2260); do echo "$f screenshot $d/descent-$f.png"; done
		echo "2290 dump floor"
		echo "2300 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish stairs_shots "$d" '^3 0 1 0$' '*level 0' "" 1
}
# deck_edge (kenshi-k16.2.236.8.4.1, .8.3): the lead rests on the deck at the stair top, then walks the length of the flight's edge right beside it (its
# sim path dips into the zone rect's east side at deck height). Nothing changes: the view stays 1 and the lead stays drawn. (On the base the same walk
# flipped to 0 at 1.6 u ABOVE the top tread and left the lead hidden: the user's 2026-11-15 22:23 state.)
# The goal (kenshi-k16.2.375.1): the first goal, -50545 -1290, lies beyond the flight's far side (the flight's footprint is z -1288 .. -1271), so the sim
# snapped it onto the flight itself, 0.1 u from the descent line (1 rise below the top tread: local y 62.6; the lead settled at 62.1 .. 62.7). Whether the
# interpolated feet dipped under the line on the way decided the flip 1 -> 0 (flight) and the model's half-rise hold kept it: 5 of 5 failures on 2026-11-19.
# The goal now is a deck point east of the flight's top end (local x 44.3, the footprint ends at 38.3), still reached along the flight's edge, never on it.
run_deck_edge()
{
	local d=$out/deck_edge id
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; climb
		echo "1000 move -50550 1140 -1266"	# rest on the deck at the stair top (the user's pre-click spot)
		dumps 1060 1440 60 "$id"
		echo "1500 move -50538 -1280"	# the deck edge right beside the zone
		dumps 1560 2100 60 "$id"
		echo "2240 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish deck_edge "$d" '^3 0 1$' '*shown 1/1*(following)*level 1'
}
# deck_click (user 2026-11-15 22:23): the lead is walked up to the upper deck; the free camera looks straight down on the deck at the stair top from the
# deck side and a right click there (the user's order: pick -50549.5 1140.2 -1266.5) walks the lead across the deck to the stair top. The click is not a way
# down: the view stays 1 and the lead stays drawn. A click one tread in (a pick on the top step itself) holds at 0.4 u below the top tread and flips only
# at 4.0 u below it, a real rise (findings/kenshi-k16.2.236.8.4.1/probe/fix5-client.log).
run_deck_click()
{
	local d=$out/deck_click id
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; climb
		echo "900 dump floor"
		echo "950 screenshot $d/before.png"
		echo "1000 set pos -50549.5,-1266.5"	# straight down on the deck at the stair top from the deck side (the user's pick: deck y 1140.2)
		echo "1001 set alt 100"
		echo "1002 set yaw 0"
		echo "1003 set pitch -89"
		echo "1010 mouse pos 640 360"
		echo "1015 dump pick"
		echo "1025 mouse down right"
		echo "1027 mouse up right"
		echo "1040 follow char Wanderer"
		dumps 1060 1580 40 "$id"
		echo "1600 screenshot $d/after.png"
		echo "2200 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish deck_click "$d" '^3 0 1$' '*shown 1/1*(following)*level 1'
	echo "deck_click pick: $(grep -o 'pick (640, 360): .*' "$d/client.log" | head -1)"
}
# stop_midflight_up / _down: an order whose goal is mid-flight, then a long stand. At most one change after the way there (the feet are on one side of
# the line or the other, never churning), and the lead is drawn at every dump. MID is the flight's middle: the flight runs along +x at z -1280 from the ground (x -50616, y 1083) to the deck (x -50543, y 1137), the wire floor turns over at x -50564 (probe/client.log).
MID="-50598 1097 -1280"
run_stop_midflight_up()
{
	local d=$out/stop_midflight_up id
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; echo "700 move $MID"
		dumps 760 2000 60 "$id"	# the walk to the mid-flight goal, then the stand
		echo "2060 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish stop_midflight_up "$d" '^3 0( 1)?$' '*(following)*'
}
run_stop_midflight_down()
{
	local d=$out/stop_midflight_down id
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; climb
		dumps 900 1440 60 "$id"
		echo "1500 move $MID"	# from the deck down to the mid-flight goal
		dumps 1560 2400 60 "$id"
		echo "2460 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish stop_midflight_down "$d" '^3 0 1( 0)?$' '*(following)*'
}
# reverse_midflight: up past the up line, then back down the same flight (the goal is the ground floor again): 0, 1, 0.
run_reverse_midflight()
{
	local d=$out/reverse_midflight id
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; echo "700 move -50553 1131 -1275"	# onto the flight past the up line (wire y 1131, floor 1, in the zone: the view flips up), a stand
		dumps 760 1200 60 "$id"
		echo "1260 move -50616 1082 -1200"	# and back down to the ground floor (the height: the same xz is also a deck point, y 1146)
		dumps 1320 2400 60 "$id"
		echo "2460 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish reverse_midflight "$d" '^3 0 1 0$' '*(following)*'
}
# step_ends_follow (L4/L5; was manual_hold, the model's manual level is gone): following, on the deck (1), a floor step to 0 ends the follow: the level is the free
# one (F2), so it stays 0 while the lead walks the deck, down the flight to its foot and back up (the follow's thresholds no longer take it back): 3, 0, 1, 0.
run_step_ends_follow()
{
	local d=$out/step_ends_follow id f
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		openZ; climb
		dumps 900 1200 60 "$id"	# the invariant before the step
		echo "1260 key COMMA down"; echo "1261 key COMMA up"
		echo "1300 dump floor"
		echo "1320 move -50578 -1246"	# a walk across the deck: the level stays 0
		for f in $(seq 1380 60 1800); do echo "$f dump floor"; done
		echo "1860 move -50558 -1275"	# down the flight to its foot, then back up to the deck
		for f in $(seq 1920 60 2300); do echo "$f dump floor"; done
		echo "2400 move -50590 -1230"
		for f in $(seq 2460 60 3200); do echo "$f dump floor"; done
		echo "3260 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish step_ends_follow "$d" '^3 0 1 0$' '*(not following)*level 0' "" 1
}
# step_up_ends_follow: Period steps the view to 1 while the lead is still on floor 0 (inside): the follow ends, and the free level 1 stays while the lead
# climbs. 3, 0, 1 (the step), nothing else.
run_step_up_ends_follow()
{
	local d=$out/step_up_ends_follow id
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		openZ
		echo "650 key PERIOD down"; echo "651 key PERIOD up"
		climb
		dumps 900 1800 60 "$id"
		echo "1860 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish step_up_ends_follow "$d" '^3 0 1$' '*shown 1/1*(not following)*level 1'
}
# two_subjects: Wanderer climbs to the deck, Beep stays on the ground outside. Engage Beep, Wanderer, Beep: 3, 0 (start), 1 (the climb), 0, 1, 0.
run_two_subjects()
{
	local d=$out/two_subjects wid bid
	wid=$(idof order_sounds Wanderer); bid=$(idof order_sounds Beep); mkdir -p "$d"
	{
		open; climb
		dumps 900 1200 60 "$wid"
		echo "1260 follow char Beep"
		dumps 1320 1500 60 "$bid"
		echo "1560 follow char Wanderer"
		dumps 1620 1800 60 "$wid"
		echo "1860 follow char Beep"
		dumps 1920 2100 60 "$bid"
		echo "2160 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish two_subjects "$d" '^3 0 1 0 1 0$' '*(following)*level 0'
}
# follow_lost: the free camera (camera char) while the lead is on floor 1, the lead walks down: the view stays 1 (not following, the level is the player's alone).
# The lead is below the cut on floor 0 at the end, so the cut dump after the walk would say hidden: no invariant after follow off.
run_follow_lost()
{
	local d=$out/follow_lost id f
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; climb
		dumps 900 1200 60 "$id"
		echo "1260 camera char Wanderer"
		echo "1320 move -50558 -1275"
		for f in $(seq 1380 60 2400); do echo "$f dump floor"; done
		echo "2460 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish follow_lost "$d" '^3 0 1$' '*(not following)*level 1'
}
# start_indoors: a session that starts with the lead inside (sunshafts_bar: the player alone in the Storm House at floor 0): the start centring is an engage
# and gives the lead's floor, 3 -> 0, and the lead is drawn.
run_start_indoors()
{
	local d=$out/start_indoors id
	id=$(idof sunshafts_bar Wanderer); mkdir -p "$d"
	{
		echo "10 set animate 1"
		dumps 100 400 60 "$id"
		echo "460 quit"
	} > "$d/script.txt"
	run_live "$d" sunshafts_bar
	finish start_indoors "$d" '^3 0$' '*(following)*level 0'
}
# other_building (review section 5; kenshi-k16.2.236.14): out of one floorable building and into a second. order_sounds' Storm House stands alone
# (the nearest other building with floors, the Hub's, is ~4000 u north), so this case plays in the Hub (journey_walk: the Wanderer alone at the
# new-game start): into the Bar (a Storm House, -51181 2639), the step key (Period) takes the view up to 1 while the lead is on its floor 0 (the step ends the
# follow: the free level 1 stays), out of the Bar and into the Small Shack 926 u north (-51271 3561, maxFloor 1; floor 0, the view stays 0).
# The Storm House 550 u north of the Bar (-51192 3192) is a RUIN on this data (destroyed meshes, no interior, maxFloor 0): the client cuts nothing
# there and the floor line is the empty default, so it is no fixture (kenshi-k16.2.355; `dump floorables X Z R` lists the floorable buildings near a point).
# The shack is further, so the walk's dump window runs to 2800 (~926 u at walking pace after the Bar's door at ~1060).
# 3, 0 (start), 1 (the step); the wire names the second building at the end, and the lead is never hidden.
run_other_building()
{
	local d=$out/other_building id a b
	id=$(idof journey_walk Wanderer); mkdir -p "$d"
	{
		echo "10 set animate 1"; echo "100 key 1 down"; echo "101 key 1 up"; echo "110 key Z down"; echo "112 key Z up"
		echo "125 move -51200 2640"	# into the Bar
		dumps 400 880 60 "$id"
		echo "900 key PERIOD down"; echo "901 key PERIOD up"
		dumps 940 1000 60 "$id"
		echo "1020 move -51271 3550"	# out of the Bar and into the Small Shack
		dumps 1080 2800 60 "$id"
		echo "2860 quit"
	} > "$d/script.txt"
	run_live "$d" journey_walk
	a=$(grep -o "floortrace .* building [0-9]* floor" "$d/client.log" | grep -v ' building 0 ' | head -1 | grep -o 'building [0-9]*')
	b=$(grep -o "floortrace .* building [0-9]* floor" "$d/client.log" | tail -1 | grep -o 'building [0-9]*')
	if [ -z "$a" ] || [ -z "$b" ] || [ "$a" = "$b" ] || [ "$b" = "building 0" ]; then echo "FAIL other_building: the fixture is not the case (want the wire in one building, then another): first '$a', last '$b'"; fail=1; return; fi
	finish other_building "$d" '^3 0 1$' '*shown 1/*(not following)*level 1'
}
# stairs_stop_click (kenshi-k16.2.236.8.4.3; the user's 236.8.4.2 FAIL): the lead rests on the deck at the stair top, then is ordered to the user's seq 29 pick
# (sim goal -50548.6 1134.76 -1276.2, a tread in and about 1.5 u under the descent line) and stands there. A flip of the view down and back is the flicker: a
# fired hotspot stays cold until the feet are well clear. Dumps every 20 frames through the walk; the "floor: latch" lines carry the feet's height against the lines.
run_stairs_stop_click()
{
	local d=$out/stairs_stop_click id
	id=$(idof order_sounds Wanderer); mkdir -p "$d"
	{
		open; climb
		echo "1000 move -50550 1140 -1266"
		dumps 1060 1440 60 "$id"
		echo "1500 move -50548.6 1134.76 -1276.2"
		dumps 1500 1700 20 "$id"
		dumps 1700 2400 100 "$id"
		echo "2460 quit"
	} > "$d/script.txt"
	run_live "$d"
	finish stairs_stop_click "$d" '^3 0 1$' '*shown 1/1*(following)*level 1'	# base: 3 0 1 0 1 (the flicker; findings/kenshi-k16.2.236.8.4.3/before)
}
for c in stairs_walk stairs_shots stairs_down stairs_click stairs_stop_click deck_click deck_edge stop_midflight_up stop_midflight_down reverse_midflight step_ends_follow step_up_ends_follow two_subjects follow_lost start_indoors other_building; do
	if want_case "$c"; then run_$c; fi
done

# kenshi-k16.2.284 (roof guard, k16.2.274): the roof cut needs a collision shape under the feet. The Wanderer stands on open ground (terrain, wire indoors bit
# clear, floor 0) inside the box roofBuilding tests for the dune-sunk Longhouse beside it (68394 1230.8 -71032, the spot id 1159 was hidden at for 74 frames in
# k16.2.274's walk4). The floor selector is stepped down to the ground floor (Comma twice, the floor_down key here: 0/2), so the cut hides everything above floor 0 (cutHides 1) and the
# roof branch is the only thing that could hide the character: shown (hidden 0) with the fix, hidden 1 without it (2709e082 reverted).
# The other side of that rule (a guard standing on a roof deck at wire floor 0 with the indoors bit clear is hidden, user 2026-11-05) has no case here: the sim
# snaps a scenario character to the navmesh, where the wire's indoors bit is set (measured on the Longhouse roof, 6 placements), so it needs a walk onto a roof.
# Not a level case (it asserts the cut guard on a stepped view), so it keeps its own assertion.
run_roof_open_ground()
{
	local name=roof_open_ground d=$out/roof_open_ground lead
	lead=$(idof roof_guard Wanderer)	# the lead's wire id moves with the scenario (a probe run of roof_guard: kenshi-k16.2.313)
	[ -n "$lead" ] || { echo "FAIL $name: no Wanderer in roof_guard's dump chars"; fail=1; return; }
	mkdir -p "$d"
	{
		echo "10 set animate 1"
		echo "20 set pos 68394,-71032"
		echo "21 set alt 1300"
		echo "22 set pitch -60"
		echo "300 key COMMA down"
		echo "301 key COMMA up"
		echo "304 key COMMA down"
		echo "305 key COMMA up"
		echo "310 dump chars"
		echo "320 dump cut id=$lead"
		echo "330 dump floor"
		echo "340 quit"
	} > "$d/script.txt"
	run_live "$d" roof_guard
	local actual feet cut
	actual=$(grep -o '^char [0-9]* "Wanderer"' "$d/client.log" | head -1 | grep -o '[0-9]*')	# the lead's wire id is the sim's instance counter (k16.2.257): the check names the id it used
	feet=$(grep -A1 "^cut id $lead " "$d/client.log" | grep "label feet")
	cut=$(grep -A1 "^cut id $lead " "$d/client.log" | head -1)
	if [ "$actual" != "$lead" ]; then echo "FAIL $name: the Wanderer's wire id is '$actual', the script uses $lead (the idof probe and the case ran different worlds)"; fail=1
	elif [[ "$cut" != *"indoors 0"* ]] || [[ "$cut" != *"floor 0 "* ]] || [[ "$feet" != *"cutHides 1;"* ]] || [[ "$feet" != *"ray no hit within 8 u"* ]] || [[ "$feet" == *"roof -1;"* ]]
	then echo "FAIL $name: the fixture is not the case (want indoors 0, floor 0, cutHides 1, no shape under the feet, a roofBuilding match): $cut | $feet"; fail=1
	elif [[ "$feet" == *"hidden 0" ]]; then echo "PASS $name"
	else echo "FAIL $name: the character on open ground beside a low building is hidden: $feet"; fail=1; fi
}
want_case roof_open_ground && run_roof_open_ground
[ $fail = 0 ] && echo "floor_threshold: PASS" || echo "floor_threshold: FAIL"
exit $fail
