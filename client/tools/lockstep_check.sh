#!/bin/bash
# The merge check for the render thread (kenshi-k16.2.205.12.92; overlap review kenshi-k16.2.205.12.85 A4, C6). Render features keep landing
# on `client`; the sokol owner assert is dev-only and no other gate runs the logic thread, so a new sg_* call on the logic half breaks
# render.thread=lockstep silently. Three checks, run it in the merge cycle after building kclient:
#   1. a grep: sg_query_* / sg_isvalid beyond an audited per-file count (they read GL state through the unhooked sokol; kenshi-k16.2.205.12.32)
#   2. scene_test.sh with render.thread=lockstep against the same baseline: every scene "pass", none "changed", no [sg] error
#   3. a new game (server on the `newgame` scenario, --simlink) with render.thread=lockstep --sg-owner-logic: reaches frame 300, 0 [sg][error]
#      (--sg-owner-logic aborts at the first sg_* call from the logic half, and prints the stack)
#
# usage: client/tools/lockstep_check.sh [--skip-scenes] [--port N] [OUT_DIR]
#   --skip-scenes  run only 1 and 3 (the scene test is the slow part)
#   OUT_DIR        logs (default client/build/lockstep-check)
# Needs client/build/kclient with the dev tools and the scene baseline (scene_test.sh --bless from the base build when missing or stale).
# Exit 0: all passed; 1: a check failed (the log path is printed); 2: usage / no kclient. Two game runs at most, one after the other.
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CLIENT=$(cd "$HERE/.." && pwd)
TREE=$(cd "$CLIENT/.." && pwd)
BUILD="$CLIENT/build"
OUT="$BUILD/lockstep-check"
PORT=
SCENES=1
while [ $# -gt 0 ]
do
	case "$1" in
		--skip-scenes) SCENES=0; shift ;;
		--port) [ $# -ge 2 ] || { echo "usage: $0 [--skip-scenes] [--port N] [OUT_DIR]" >&2; exit 2; }; PORT=$2; shift 2 ;;
		-*) echo "usage: $0 [--skip-scenes] [--port N] [OUT_DIR]" >&2; exit 2 ;;
		*) OUT=$1; shift ;;
	esac
done
. "$HERE/ports.sh"
mkdir -p "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd)
fail()
{
	echo "lockstep_check: FAIL: $*" >&2
	exit 1
}

# 1. sg_query_* / sg_isvalid (kenshi-k16.2.205.12.32): sokol has no owner hook on them, so --sg-owner-logic cannot see a call from the logic
# thread. sg_query_backend() is a constant after sg_setup and is not counted; src/dev/ is not counted (dev tools, owner layer). The rest is
# the audited set below, file -> number of lines (the draw side, init on the render side, or a main-thread-only caller): a file over its
# count, or a new file, fails. A new call that really is render-side raises the count here, in the same commit, after the reviewer has
# looked at which thread runs it (that is the point: it is looked at).
# Audit kenshi-k16.2.205.12.107 (table: findings/kenshi-k16.2.205.12.107/audit.md): none of these touch GL (sokol reads _sg.limits /
# _sg.features / the pool slot state, no backend call), and none runs on the logic thread. Per entry:
#   colourmap.cpp 2   GroundColour::upload: sg_query_limits + image_state, a load step (before the logic thread starts, rule 7)
#   detail.cpp 2      :272 prepare() on a std::async worker, reads sg_query_limits, a constant after sg_setup; :344 uploadPart, a load step
#   worldbuildings 2  :247 init() in a load step; :920 the texture's finish(), inside a render-side rq job (frame or stream lane)
#   grass.cpp 1       makePipeline's sprite loop, a render-side rq job ("grass-pipeline")
#   particles.cpp 1   makeTexture, a render-side rq job ("particle-texture")
#   worldmap.cpp 1    WorldMap::init, a load step
#   dds.cpp 2         :76 sg_query_limits (a constant) in dds_levels, called from render-side jobs / load steps / workers;
#                     :168 inside the dds_queue_image render-side job
#   main.cpp 2        :4430 / :4489 in init() on the sapp (render) thread, before the logic thread exists; the logic half reads the value
#                     portraitTable.setOrigin stored (logic asks sokol nothing)
# Only image_state (a pool-slot read, racy against a concurrent make on another thread) matters, and no caller of it is on the logic thread.
declare -A KNOWN=([client/src/colourmap.cpp]=2 [client/src/detail.cpp]=2 [client/src/worldbuildings.cpp]=2 [client/src/grass.cpp]=1
	[client/src/particles.cpp]=1 [client/src/worldmap.cpp]=1 [client/src/dds.cpp]=2 [client/src/main.cpp]=2)
cd "$TREE" || exit 2
BAD=0
while read -r f n
do
	[ -n "$f" ] || continue
	if [ "$n" -gt "${KNOWN[$f]:-0}" ]
	then
		echo "lockstep_check: $f has $n sg_query_* / sg_isvalid lines, audited: ${KNOWN[$f]:-0}" >&2
		grep -nE '\bsg_(query_(features|limits|image|buffer|view|sampler|shader|pipeline|desc|pixelformat|frame|buffer_info|image_info)|query_[a-z_]+|isvalid)\b' "$f" | grep -v 'sg_query_backend' | cut -c1-140 >&2
		BAD=1
	fi
done < <(grep -rcE '\bsg_(query_[a-z_]+|isvalid)\b' client/src --include='*.cpp' --include='*.hpp' --include='*.h' \
	| grep -v '^client/src/dev/' | grep -v ':0$' | awk -F: '{print $1, $2}' \
	| while read -r f n; do m=$(grep -E '\bsg_(query_[a-z_]+|isvalid)\b' "$f" | grep -vc 'sg_query_backend'); [ "$m" -gt 0 ] && echo "$f $m"; done)
[ "$BAD" = 0 ] || fail "sg_query_* / sg_isvalid beyond the audited set (above): on the logic thread they read GL state with no owner; keep them on the render side or in a renderqueue job, or raise KNOWN in this script after checking which thread runs the call"
echo "lockstep_check: ok   no sg_query_* / sg_isvalid beyond the audited set"

KCLIENT="${KCLIENT:-$BUILD/kclient}"
[ -x "$KCLIENT" ] || { echo "lockstep_check: no $KCLIENT (build target kclient, with the dev tools)" >&2; exit 2; }
DATA=${KENSHI_DATA:-}
if [ -z "$DATA" ] && [ -f "$BUILD/CMakeCache.txt" ]; then DATA=$(sed -n 's/^KENSHI_DATA:[A-Z]*=//p' "$BUILD/CMakeCache.txt" | head -n 1); fi
[ -n "$DATA" ] || { echo "lockstep_check: no data dir (export KENSHI_DATA or configure client/build)" >&2; exit 2; }

# 2. scene_test in lockstep against the baseline
if [ "$SCENES" = 1 ]
then
	KCLIENT="$KCLIENT" KCLIENT_SCENE_ARGS="--set render.thread=lockstep" "$HERE/scene_test.sh" "$OUT/scenes" >"$OUT/scenes.log" 2>&1
	RC=$?
	[ "$RC" = 0 ] || { tail -25 "$OUT/scenes.log" >&2; fail "scene_test in lockstep exited $RC, see $OUT/scenes.log"; }
	grep -q 'render.thread=lockstep: the logic thread starts' "$OUT/scenes/run.log" || fail "the scene run never started the logic thread: render.thread=lockstep did not reach the client, see $OUT/scenes/run.log"
	if grep -E '^[a-z0-9_.-]+ +(changed|new|FAIL)' "$OUT/scenes.log" >&2
	then
		fail "scene_test in lockstep: scenes changed / new / FAIL (above), see $OUT/scenes.log. Compare with an off run before blaming lockstep; a stale baseline is --bless from the base build"
	fi
	echo "lockstep_check: ok   scene_test in lockstep: every scene passes against the baseline ($OUT/scenes/overview.png)"
fi

# 3. a new game in lockstep with the logic half owning no sokol
. "$HERE/server_bin.sh"
pick_server "$TREE" "$BUILD/cpp/OpenKenshi-server"
[ -x "$SERVER" ] || { echo "lockstep_check: no $SERVER (build target ksim_server)" >&2; exit 2; }
PORT=$(pick_port "$PORT") || exit 3
(cd "$TREE" && exec "$SERVER" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen "127.0.0.1:$PORT" --scenario newgame) >"$OUT/server.log" 2>&1 &
SPID=$!
trap 'kill "$SPID" 2>/dev/null; wait "$SPID" 2>/dev/null' EXIT
for ((i = 0; i < 120; i++))
do
	grep -q '^listening ' "$OUT/server.log" && break
	kill -0 "$SPID" 2>/dev/null || fail "the server exited early, see $OUT/server.log"
	sleep 0.5
done
grep -q '^listening ' "$OUT/server.log" || fail "the server is not listening after 60 s, see $OUT/server.log"
printf '300 quit\n' > "$OUT/newgame.txt"
nice -n 10 "$HERE/headless.sh" -- "$KCLIENT" --data "$DATA" --debug --simlink "127.0.0.1:$PORT" --set render.thread=lockstep --sg-owner-logic \
	--script "$OUT/newgame.txt" >"$OUT/newgame.log" 2>&1
RC=$?
if grep -q 'sokol thread-owner assert' "$OUT/newgame.log"
then
	grep -E -A12 'sokol thread-owner assert' "$OUT/newgame.log" | head -20 >&2
	fail "an sg_* call from the logic half (the stack is above, full log $OUT/newgame.log)"
fi
[ "$RC" = 0 ] || fail "the new-game run exited $RC, see $OUT/newgame.log"
grep -q 'render.thread=lockstep: the logic thread starts' "$OUT/newgame.log" || fail "the logic thread never started: render.thread=lockstep did not reach the client, see $OUT/newgame.log"
grep -q '^first_world_frame' "$OUT/newgame.log" || fail "no first_world_frame, see $OUT/newgame.log"
! grep -q '\[sg\]\[error\]' "$OUT/newgame.log" || fail "[sg][error] in the new-game run, see $OUT/newgame.log"
echo "lockstep_check: ok   a lockstep new game with --sg-owner-logic ran its script to 300 frames, 0 [sg][error] ($OUT/newgame.log)"
echo "lockstep_check: PASS (logs in $OUT)"
