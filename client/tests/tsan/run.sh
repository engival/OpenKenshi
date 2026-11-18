#!/bin/bash
# ThreadSanitizer run of kclient under render.thread=lockstep (kenshi-k16.2.205.12.29; docs/RENDER_THREAD.md 7.1 rule 9: overlap is enabled only
# when a TSan run is clean). Configures and builds client/build-tsan (-DKCLIENT_TSAN=ON, CMakeLists.txt), then runs two headless checks under it:
#   scene test      every scene of client/scenes.txt (client/tools/scene_test.sh), compared against the normal build's baseline for the card;
#   scripted        client/tests/tsan/scenario.txt: movement, camera modes, weather and scene jumps, the debug toggles, a resize, the scripted
#                   stalls (the hand-off's bounded waits), screenshots.
# and summarises the reports: the count per race kind, and each unique race once by its SUMMARY line (kind, top frame, site).
#
#   client/tests/tsan/run.sh [--mode off|lockstep|overlap] [--scenes NAME,NAME] [--only both|scenes|scenario|live] [--jobs N] [--port N] [--no-build] [OUT]
#
# TSAN_SCENARIO=NAME (kenshi-k16.2.205.12.110): the live scenario's server scenario (scenarios/NAME.kscen, default bar_lure; the Heft crowd: cage_prisoner with tests/tsan/heft.txt).
# --only live (kenshi-k16.2.205.12.46): a third check, not part of `both`: ksim_server (scenarios/bar_lure.kscen, started by this script, killed by its own
#   $!) plus the TSan kclient --simlink under client/tests/tsan/live.txt, so app.sim.active() is true and the idle hook polls the link (pumpLink)
#   whenever a held render makes logic's 100 ms wait expire. The summary counts those polls. Needs client/build/cpp/OpenKenshi-server (target
#   ksim_server; the normal build tree) and a navworld bake (KENSHI_NAVWORLD, default ~/git/kenshi/navworld). --port N: the server's port (default: a free one).
#
# render.thread comes from a config file the script writes (client/src/app/config.cpp KCLIENT_CONFIG), so both the scene test and the scripted run get
# it: the scene test takes no extra kclient arguments. --mode overlap is accepted for when it lands; until then the client prints its loud
# "overlap IS NOT IMPLEMENTED" line and runs lockstep, and the summary counts the logic thread that ran.
# OUT defaults to client/build-tsan/tsan-run (in the worker's tree, gitignored); OUT/tsan/ holds the TSan reports (TSAN_OPTIONS log_path, one file
# per process), OUT/scenes/ and OUT/shots/ the runs, OUT/summary.txt the result. TSan is slow: scene test plus scenario take minutes, not seconds.
# HEADLESS_TIMEOUT (default 1800 s here) bounds each run.
# Exit 0 = the TSan build was built and both runs finished (races are reported, not failed: they are the result). 1 = the build failed, the binary
# is not TSan-linked, a run crashed, timed out, or a scene never finished loading.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
build=${BUILD:-$tree/client/build-tsan}
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
mode=lockstep
scenes=
jobs=${TSAN_JOBS:-4}
do_build=1
only=both
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=${TSAN_PORT:-}
out=
while [ $# -gt 0 ]
do
	case "$1" in
		--mode) mode=$2; shift 2 ;;
		--scenes) scenes=$2; shift 2 ;;
		--jobs) jobs=$2; shift 2 ;;
		--no-build) do_build=0; shift ;;
		--only) only=$2; shift 2 ;;
		--port) port=$2; shift 2 ;;
		*) out=$1; shift ;;
	esac
done
case "$mode" in off|lockstep|overlap) ;; *) echo "usage: $0 [--mode off|lockstep|overlap] [--scenes NAME,NAME] [--only both|scenes|scenario|live] [--jobs N] [--port N] [--no-build] [OUT]" >&2; exit 2 ;; esac
port=$(pick_port "$port") || exit 3	# TSAN_PORT / --port wins; else a free port (kenshi-k16.2.307)
case "$only" in both|scenes|scenario|live) ;; *) echo "usage: --only both|scenes|scenario|live" >&2; exit 2 ;; esac
# The game data (client/tests/need_data.sh: KENSHI_DATA, else the KENSHI_DATA of the build the run uses; SKIP+exit 0 without it, the public tree).
BUILD=$build
. "$here/../need_data.sh"
data=${KENSHI_DATA:-}
[ -n "$data" ] || data=$(sed -n 's/^KENSHI_DATA:[A-Z]*=//p' "$build/CMakeCache.txt" 2>/dev/null | head -1)
[ -n "$data" ] || data=$(sed -n 's/^KENSHI_DATA:[A-Z]*=//p' "$tree/client/build/CMakeCache.txt" 2>/dev/null | head -1)
[ -n "$data" ] || { echo "kenshi tsan: no game data dir; set KENSHI_DATA" >&2; exit 1; }

[ -n "$out" ] || out=$build/tsan-run
out=$(mkdir -p "$out" && cd "$out" && pwd) || exit 1
mkdir -p "$out/tsan" "$out/scenes" "$out/shots" || exit 1
rm -rf "$out/tsan"/* || true	# the last run's reports, not two runs mixed
bin=$build/kclient
log=$out/tsan-run.log
: > "$log"
say() { echo "$@" | tee -a "$log"; }

# --- the TSan build -----------------------------------------------------------------------------------------------------------------------------
if [ $do_build = 1 ]
then
	say "configure $build (KCLIENT_TSAN=ON, RelWithDebInfo)"
	if ! nice -n 10 cmake -S "$tree/client" -B "$build" -DKENSHI_DATA="$data" -DKCLIENT_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >> "$log" 2>&1
	then
		echo "kenshi tsan: configure failed; $log" >&2; exit 1
	fi
	say "build kclient -j$jobs (nice)"
	if ! nice -n 10 cmake --build "$build" -j "$jobs" --target kclient >> "$log" 2>&1
	then
		echo "kenshi tsan: build failed; $log" >&2; exit 1
	fi
fi
grep -q '^KCLIENT_TSAN:BOOL=ON' "$build/CMakeCache.txt" 2>/dev/null || { echo "kenshi tsan: $build is not configured with -DKCLIENT_TSAN=ON (a normal build's report would be empty and mean nothing)" >&2; exit 1; }
[ -x "$bin" ] || { echo "kenshi tsan: no binary at $bin" >&2; exit 1; }
ldd "$bin" 2>/dev/null | grep -q 'libtsan' || { echo "kenshi tsan: $bin does not link libtsan (stale or non-sanitizer build)" >&2; exit 1; }

# --- the options both runs share ----------------------------------------------------------------------------------------------------------------
cfg=$out/kclient.cfg
printf '# client/tests/tsan/run.sh: the render thread mode under test\nrender.thread=%s\n' "$mode" > "$cfg"
[ "$mode" != overlap ] || printf 'debug.overlap_scripted=1\n' >> "$cfg"	# scripted runs fall back to lockstep unless this is set (overlap review A3, kenshi-k16.2.205.12.99)
export KCLIENT_CONFIG=$cfg	# headless.sh keeps a set KCLIENT_CONFIG (defaults to "none"); the run reads this file
export XDG_DATA_HOME=$out/xdg	# the playtest events and other run state stay in OUT, not the user's ~/.local
mkdir -p "$XDG_DATA_HOME"
# TSan: halt_on_error=0 collects every report instead of stopping at the first; exitcode=0 keeps the client's own status (the race count is the
# result here, and scene_test/scene_compare must not read a report as a scene failure); detect_deadlocks adds lock-order inversions;
# log_path writes one file per process into OUT/tsan (a prefix: TSan appends .pid). second_deadlock_stack names both threads of a deadlock.
# ignore_noninstrumented_modules=1: Mesa's driver is not instrumented and its own worker thread (`kclient:gdrv0`) races with the sapp thread
# inside libgallium through intercepted libc calls (free, calloc, memcpy, mmap); without the flag every run reports ~22 of those and no client frame
# (the reports of 2026-11-15 are in the findings of kenshi-k16.2.205.12.29). The client's own code is instrumented on both threads, so a logic/render
# race is still reported; only an access made inside an uninstrumented library is ignored.
export TSAN_OPTIONS="log_path=$out/tsan/tsan halt_on_error=0 exitcode=0 second_deadlock_stack=1 detect_deadlocks=1 ignore_noninstrumented_modules=1 suppressions=$here/tsan.supp:history_size=4"
export HEADLESS_TIMEOUT=${HEADLESS_TIMEOUT:-1800}

# A TSan-linked run that quit the way the check expects: 0 from the check, 124 the wrapper's timeout, 137/134/139 a kill or a crash. Anything
# else with a nonzero status came from the client or the wrapper; report it, do not guess.
note_rc() {	# NAME RC
	if [ "$2" = 0 ]; then say "$1: rc 0"
	elif [ "$2" = 124 ]; then say "$1: TIMEOUT (HEADLESS_TIMEOUT=${HEADLESS_TIMEOUT}s); a hung client ends the run"
	else say "$1: rc $2 (see its log)"
	fi
}
rc_all=0
rc_scenes=skip
rc_scenario=skip

# --- 1. the scene test -------------------------------------------------------------------------------------------------------------------------
if [ "$only" = both ] || [ "$only" = scenes ]
then
	say "scene test (render.thread=$mode)"
	scene_args=()
	[ -z "$scenes" ] || scene_args=(--scenes "$scenes")
	KCLIENT=$bin nice -n 10 "$tree/client/tools/scene_test.sh" "${scene_args[@]}" "$out/scenes" > "$out/scenes.log" 2>&1
	rc_scenes=$?
	note_rc "scene test" $rc_scenes
	tail -3 "$out/scenes.log" | sed 's/^/  /' | tee -a "$log" >&2
	[ $rc_scenes -le 1 ] || rc_all=1	# scene_test: 0 (or 1 = compare failed / a scene did not load) are results; 124/134/139 a dead run
	say "  scenes changed against the baseline: $(grep -cE ' changed ' "$out/scenes.log") of $(grep -cE '^[a-z0-9_]+ +(pass|changed|new) ' "$out/scenes.log")"
fi

# --- 2. the scripted scenario -------------------------------------------------------------------------------------------------------------------
if [ "$only" = both ] || [ "$only" = scenario ]
then
	say "scripted scenario (render.thread=$mode)"
	sed -e "s#SHOTDIR#$out/shots#g" "$here/scenario.txt" > "$out/scenario.txt"
	nice -n 10 "$tree/client/tools/headless.sh" -- stdbuf -oL "$bin" --debug --script "$out/scenario.txt" > "$out/scenario.log" 2>&1
	rc_scenario=$?
	note_rc "scenario" $rc_scenario
	[ $rc_scenario = 0 ] || rc_all=1
	say "  screenshots: $(ls "$out/shots" | wc -l), sg errors: $(grep -c '\[sg\]\[error' "$out/scenario.log")"
fi

# --- 2b. the live scenario ----------------------------------------------------------------------------------------------------------------------
rc_live=skip
if [ "$only" = live ]
then
	say "live scenario (render.thread=$mode, ksim_server ${TSAN_SCENARIO:-bar_lure} on 127.0.0.1:$port)"
	pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
	server=$SERVER
	nav=${KENSHI_NAVWORLD:-$HOME/git/kenshi/navworld}
	[ -x "$server" ] || { echo "kenshi tsan: no $server (build target ksim_server in client/build)" >&2; exit 1; }
	[ -d "$nav" ] || { echo "kenshi tsan: no navworld bake $nav (KENSHI_NAVWORLD)" >&2; exit 1; }
	sed -e "s#SHOTDIR#$out/shots#g" "${TSAN_LIVE_SCRIPT:-$here/live.txt}" > "$out/live.txt"	# TSAN_LIVE_SCRIPT: another script (tests/tsan/soak.txt)
	# docs: a hand-started server dies with its own $!, never by a name match (other sessions' servers match too).
	(cd "$tree" && exec "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen "127.0.0.1:$port" --scenario-file "$tree/scenarios/${TSAN_SCENARIO:-bar_lure}.kscen" --navworld "$nav" --bake "$nav") > "$out/server.log" 2>&1 &
	spid=$!
	trap 'kill $spid 2>/dev/null || true' EXIT
	for _ in $(seq 240)
	do
		grep -q '^listening ' "$out/server.log" && break
		kill -0 $spid 2>/dev/null || { echo "kenshi tsan: the server died:" >&2; tail -5 "$out/server.log" >&2; exit 1; }
		sleep 0.5
	done
	grep -q '^listening ' "$out/server.log" || { echo "kenshi tsan: the server is not listening on $port" >&2; exit 1; }
	nice -n 10 "$tree/client/tools/headless.sh" -- stdbuf -oL "$bin" --debug --simlink "127.0.0.1:$port" --script "$out/live.txt" > "$out/live.log" 2>&1
	rc_live=$?
	kill $spid 2>/dev/null || true
	wait $spid 2>/dev/null || true
	trap - EXIT
	note_rc "live scenario" $rc_live
	[ $rc_live = 0 ] || rc_all=1
	say "  screenshots: $(ls "$out/shots" | wc -l), sg errors: $(grep -c '\[sg\]\[error' "$out/live.log")"
	live_polls=$(grep -ho '[0-9][0-9]* link poll' "$out/live.log" | awk '{s += $1} END { print s + 0 }')
	live_expiries=$(grep -ho 'ran out [0-9][0-9]* time' "$out/live.log" | awk '{s += $3} END { print s + 0 }')
	say "  pumpLink: $live_polls idle link polls over $live_expiries expiries (>0 polls = the sim was active; 0 = the path was not reached)"
fi

# --- 3. the reports -----------------------------------------------------------------------------------------------------------------------------
threads_started=$(grep -hc 'logic thread started' "$out/scenes/run.log" "$out/scenario.log" "$out/live.log" 2>/dev/null | awk '{s += $1} END { print s + 0 }')
say "logic thread started in the logs: $threads_started (0 = the runs never left loading; a clean TSan run then proves nothing about the thread)"
reports=$(cat "$out"/tsan/tsan.* 2>/dev/null)
summary=$out/summary.txt
{
	echo "ThreadSanitizer run, kclient, render.thread=$mode ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
	echo "build: $build  binary: $bin"
	echo "scene test rc $rc_scenes (scenes: ${scenes:-all}); scripted scenario rc $rc_scenario; live scenario rc $rc_live"
	[ "$rc_live" = skip ] || echo "live: pumpLink $live_polls idle link polls over $live_expiries expiries"
	echo
	echo "reports by kind:"
	printf '%s\n' "$reports" | grep '^WARNING: ThreadSanitizer' | sed 's/ (pid=[0-9]*)//' | sort | uniq -c | sort -rn
	echo
	echo "unique races (count, kind and top frame at the site):"
	printf '%s\n' "$reports" | grep '^SUMMARY: ThreadSanitizer' | sort | uniq -c | sort -rn
	echo
	echo "report files: $out/tsan/"
} > "$summary"
say ""
sed 's/^/  /' "$summary" | tee -a "$log" >&2
say "summary: $summary"
exit $rc_all
