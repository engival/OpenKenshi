#!/bin/bash
# The camera's decided rules (user 2026-11-22, kenshi-k16.2.236.13; docs/CLIENT.md "Camera and control modes", Governing rules), kenshi-i6gs:
#   follow_step   (L4, L5) follow (Z) + F: the follow ends, the camera stays where it is (floating mode), the shown level is the stepped one
#                 (0), and a later follow shows the character's floor again; follow + R (a step the level cannot take: top) also ends it
#   follow_wasd   (L4) follow + W: the follow ends, the shown level is unchanged
#   drive_step    (D2) drive (`follow char`, V) + F and R: no floor step, the drive stays
#   drive_leave   (D3) V out of the drive: the camera is in follow on that character (centre = subject = it), the floor follows it (L3)
# order_sounds: the Wanderer is walked into the Storm House and up the stairs to the deck (floor 1), as tests/floor_threshold does. A shot at each change.
# Usage: run.sh OUT   (OUT: output dir). PORT (default: a free port), BUILD (default client/build of this tree).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"
server=$SERVER
fail=0

run_live()
{
	local d=$1 pid port
	start_server_on_free_port "$d/server.log" "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:@PORT@ --scenario-file "$tree/scenarios/order_sounds.kscen" --debug-view
	pid=$SERVER_PID; port=$SERVER_PORT
	(cd "$tree" && timeout 580 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/script.txt" > "$d/client.log" 2>&1)
	kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}
# the walk: the first character selected, then in the front door and up the stairs to the deck (floor 1): done by frame ~1000
walk() { echo "10 set animate 1"; echo "100 key 1 down"; echo "101 key 1 up"; echo "125 move -50616 -1130"; echo "400 move -50616 -1200"; echo "700 move -50590 -1230"; }
# N-th "follow state" / "floor:" dump line of a run, and a field of the state line
st() { grep "^follow state:" "$1/client.log" | sed -n "$2p"; }
fld() { echo "$1" | sed -n "s/.* $2 \(-\?[0-9.]*\) .*/\1/p"; }
mode() { echo "$1" | sed -n 's/^follow state: mode \([a-z]*\) .*/\1/p'; }
fl() { grep "^floor: [A-Z][A-Za-z ]* shown " "$1/client.log" | sed -n "$2p"; }
chk() { if [ "$2" = "$3" ]; then echo "PASS $1"; else echo "FAIL $1: got '$2' want '$3'"; fail=1; fi; }
has() { if [[ "$2" == *"$3"* ]]; then echo "PASS $1"; else echo "FAIL $1: '$2' lacks '$3'"; fail=1; fi; }

# --- follow (Z): F, R, W ---------------------------------------------------------------------------------------------------------------------
d=$out/follow; mkdir -p "$d"
{
	walk
	echo "110 key Z down"; echo "112 key Z up"
	echo "1000 follow state"; echo "1001 dump floor"; echo "1005 screenshot $d/follow_0_before.png"
	echo "1098 follow state"
	echo "1100 key F down"; echo "1101 key F up"
	echo "1105 screenshot $d/follow_1_after_F.png"
	echo "1110 follow state"; echo "1111 dump floor"
	echo "1300 key Z down"; echo "1302 key Z up"
	echo "1330 follow state"; echo "1331 dump floor"
	echo "1400 key R down"; echo "1401 key R up"
	echo "1405 screenshot $d/follow_2_after_R.png"
	echo "1410 follow state"; echo "1411 dump floor"
	echo "1500 key Z down"; echo "1502 key Z up"
	echo "1530 follow state"
	echo "1540 key W down"; echo "1556 screenshot $d/follow_3_W_held.png"; echo "1560 key W up"
	echo "1570 follow state"; echo "1571 dump floor"
	echo "1600 quit"
} > "$d/script.txt"
run_live "$d"
S0=$(st "$d" 1); SB=$(st "$d" 2); S1=$(st "$d" 3); S2=$(st "$d" 4); S3=$(st "$d" 5); S4=$(st "$d" 6); S5=$(st "$d" 7)
D0=$(fl "$d" 1); D1=$(fl "$d" 2); D2=$(fl "$d" 3); D3=$(fl "$d" 4); D5=$(fl "$d" 5)
id=$(fld "$S0" center)
[ -n "$id" ] && [ "$id" != 0 ] || { echo "FAIL follow: the fixture is not the case (no centre after Z): $S0"; fail=1; }
has "follow: on the deck the level is 1, following" "$D0" "level 1"; has "follow: floor line says following" "$D0" "(following)"
chk "follow + F: the follow ends (centre 0)" "$(fld "$S1" center)" 0
chk "follow + F: the camera stays in the floating mode" "$(mode "$S1")" rts
chk "follow + F: the viewport stays (focus x)" "$(fld "$S1" focus)" "$(fld "$SB" focus)"
has "follow + F: the shown level is the stepped one (0)" "$D1" "level 0"; has "follow + F: free level, not following" "$D1" "(not following)"
chk "follow again: centre is the character" "$(fld "$S2" center)" "$id"; has "follow again: the character's floor (1), following" "$D2" "level 1"
chk "follow + R (at the top): the follow ends" "$(fld "$S3" center)" 0; has "follow + R: level 1, not following" "$D3" "(not following)"
chk "follow again: centre is the character" "$(fld "$S4" center)" "$id"
chk "follow + W: the follow ends" "$(fld "$S5" center)" 0; has "follow + W: the level is unchanged (1), not following" "$D5" "level 1"; has "follow + W: not following" "$D5" "(not following)"

# --- drive (V / follow char): F, R ignored; V leaves into follow ------------------------------------------------------------------------------
d=$out/drive; mkdir -p "$d"
{
	walk
	echo "110 follow char Wanderer"
	echo "1000 follow state"; echo "1001 dump floor"; echo "1005 screenshot $d/drive_0_before.png"
	echo "1100 key F down"; echo "1101 key F up"
	echo "1105 screenshot $d/drive_1_after_F.png"
	echo "1110 follow state"; echo "1111 dump floor"
	echo "1200 key R down"; echo "1201 key R up"
	echo "1210 follow state"; echo "1211 dump floor"
	echo "1300 key V down"; echo "1302 key V up"
	echo "1305 screenshot $d/drive_2_after_V.png"
	echo "1330 follow state"; echo "1331 dump floor"
	echo "1400 quit"
} > "$d/script.txt"
run_live "$d"
S0=$(st "$d" 1); S1=$(st "$d" 2); S2=$(st "$d" 3); S3=$(st "$d" 4)
D0=$(fl "$d" 1); D1=$(fl "$d" 2); D2=$(fl "$d" 3); D3=$(fl "$d" 4)
id=$(fld "$S0" drive)
[ -n "$id" ] && [ "$id" != 0 ] || { echo "FAIL drive: the fixture is not the case (no drive target): $S0"; fail=1; }
has "drive: on the deck the level is 1" "$D0" "level 1"
chk "drive + F: the drive stays" "$(fld "$S1" drive)" "$id"; has "drive + F: no floor step (level 1)" "$D1" "level 1"; has "drive + F: still following" "$D1" "(following)"
chk "drive + R: the drive stays" "$(fld "$S2" drive)" "$id"; has "drive + R: level 1" "$D2" "level 1"
chk "leaving drive: no drive" "$(fld "$S3" drive)" 0
chk "leaving drive: the camera follows the character (centre)" "$(fld "$S3" center)" "$id"
chk "leaving drive: the camera follows the character (subject)" "$(fld "$S3" subject)" "$id"
has "leaving drive: the floor follows it (following, level 1)" "$D3" "(following)"

[ $fail = 0 ] && echo "camera_rules: PASS" || echo "camera_rules: FAIL"
exit $fail
