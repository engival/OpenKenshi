#!/bin/bash
# One-command SIMLINK demo: starts OpenKenshi-server, then kclient connected to it.
# Usage: client/tools/demo.sh [--scenario newgame|duel|squad] [--port N] [--navworld DIR|none] [--ai DIR|none]
#          [--bandits] [--scenario-file FILE.kscen] [--server-arg ARG]... [--build] [-- KCLIENT_ARGS...]
#   --scenario    server scenario (default newgame: a new game of the Wanderer start at the Hub, fresh stats;
#                 squad = the old test squad with the duel-log Engival next to bandits)
#   --navworld    world bake for the streamed navmesh, opt-in; default none = no nav args: the server's runtime mesh (shipped navtiles +
#                 patch cache), which playtests use (kenshi-k16.19.12; user 2026-11-04, kenshi-k16.7). navworld_v11 is a frozen validation bake.
#   --ai          world bake for the squad AI, the server's --bake, opt-in; default none = the AI routes on the runtime mesh ('navigator navmesh')
#   --bandits     a Dust Bandit squad at the old den by the Hub (-48880, -823), for fight tests (OpenKenshi-server --place-squad)
#   --scenario-file  a KSCEN save import (docs/reference/SCENARIO_IMPORT.md); its FILE.kclient_args sidecar (camera) goes to kclient
#                 (without one, the camera starts on the file's source centre=, as --pos)
#   --server-arg  one extra OpenKenshi-server argument, repeatable (e.g. --server-arg --log-world-squads --server-arg 30)
#   --port        TCP port on 127.0.0.1 (default: a free port)
#   --build       build kclient, and OpenKenshi-server when this tree must build it, first (under flock client/build/.build.lock)
#   KSIM_SERVER=<path>  the server binary; else the shared master build (client/tools/shared_server.sh); else this build
#                 dir's client/build/cpp/OpenKenshi-server. The choice prints as 'server: shared PATH' / 'server: built (why)'
#                 (kenshi-k16.2.306.1), and a shared one skips the ksim_server build and its stale-source check.
#   KCLIENT=<cmd>  the client command, word-split like play.sh's (default client/build/kclient): KCLIENT="client/tools/headless.sh -- $PWD/client/build/kclient"
#                 runs the client headless (kenshi-k16.2.314); play.sh scenario / game / spectate pass it through.
# The server always runs with --debug-view (the debug frames: weather, time, spawns; loopback only, as here).
# On client exit or Ctrl-C only the server started here is killed.
set -eu

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$ROOT/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, the shared master build, else this build dir's (kenshi-k16.2.306.1)
SERVER="$ROOT/client/build/cpp/OpenKenshi-server"	# unless pick_server below replaces it (the sim target ksim_server, one build dir per checkout)
CLIENT="$ROOT/client/build/kclient"
CLIENT_CMD=(${KCLIENT:-$CLIENT})	# KCLIENT: the client command, word-split (e.g. headless.sh -- PATH); unset = $CLIENT
SCENARIO=newgame
. "$(dirname "${BASH_SOURCE[0]}")/ports.sh"
PORT=		# --port N, else a free port (kenshi-k16.2.307)
BUILD=0
# The runtime mesh by default (kenshi-k16.19.12): no --navworld / --bake, so the server streams the shipped tiles with its patch cache and the
# squad AI routes on the same mesh. A bake is opt-in (--navworld DIR, --ai DIR); the one used is printed.
NAVWORLD=none
AI=none
EXTRA=()
SCENFILE=
CAMARGS=()

while [ $# -gt 0 ]
do
	case "$1" in
		--scenario) SCENARIO=${2:?--scenario needs newgame|duel|squad}; shift 2 ;;
		--port) PORT=${2:?--port needs a number}; shift 2 ;;
		--navworld) NAVWORLD=${2:?--navworld needs a directory or none}; shift 2 ;;
		--ai) AI=${2:?--ai needs a directory or none}; shift 2 ;;
		--bandits) EXTRA+=(--place-squad 1302-gamedata.base -48880 -823); shift ;;
		--scenario-file) SCENFILE=${2:?--scenario-file needs a .kscen file}; shift 2 ;;
		--server-arg) EXTRA+=("${2?--server-arg needs a value}"); shift 2 ;;
		--build) BUILD=1; shift ;;
		--) shift; break ;;
		-h|--help) sed -n '2,19p' "$0"; exit 0 ;;
		*) echo "demo.sh: unknown option '$1' (kclient args go after --)" >&2; exit 2 ;;
	esac
done

case "$SCENARIO" in newgame|duel|squad) ;; *) echo "demo.sh: scenario must be newgame, duel or squad" >&2; exit 2 ;; esac
case "$PORT" in *[!0-9]*) echo "demo.sh: bad port '$PORT'" >&2; exit 2 ;; esac
WANT_PORT=$PORT	# --port N: kept or fails; unset: picked at each server start (retried on a bind failure, kenshi-k16.2.417.1)

pick_server "$ROOT" "$SERVER"	# prints 'server: shared PATH' / 'server: built (reason)'; SHARED=1 for the first two sources
# A binary older than the last commit touching its sources is rebuilt (2026-11-07: an old OpenKenshi-server ignored the scenario wire silently
# and the client sat on the load screen). Uncommitted edits don't count; --build always builds. A shared server is master's build, not this
# tree's: it has no stale check here, and the build below is kclient only.
stale()
{
	[ -x "$1" ] || return 1
	local t
	t=$(git -C "$ROOT" log -1 --format=%ct -- "${@:2}" 2>/dev/null) || return 1
	[ -n "$t" ] && [ "$(stat -c %Y "$1")" -lt "$t" ]
}
TARGETS=(ksim_server kclient)
if [ "$SHARED" = 1 ]
then
	TARGETS=(kclient)
	if [ "$BUILD" != 1 ] && stale "$CLIENT" client/src client/shaders client/CMakeLists.txt cpp/src cpp/include
	then
		echo "demo.sh: kclient is older than the last commit to its sources: building it" >&2
		BUILD=1
	fi
elif [ "$BUILD" != 1 ] && { stale "$SERVER" cpp/src cpp/include cpp/CMakeLists.txt || stale "$CLIENT" client/src client/shaders client/CMakeLists.txt cpp/src cpp/include; }
then
	echo "demo.sh: a binary is older than the last commit to its sources: building both" >&2
	BUILD=1
fi
if [ "$BUILD" = 1 ]
then
	# Player-initiated build (a playtest): the player waits on it, so all cores and no nice. Every build in
	# client/build takes this flock; a second one waits for the first and then finds the targets fresh.
	mkdir -p "$ROOT/client/build"
	LOCK="$ROOT/client/build/.build.lock"
	exec 9>"$LOCK"
	if ! flock -n 9
	then
		echo "demo.sh: another build holds $LOCK; waiting for it to finish" >&2
		flock 9
	fi
	nice -n 10 cmake --build "$ROOT/client/build" --target "${TARGETS[@]}" -j4	# half the cores or less, niced (project build rule)
	exec 9>&-
	if [ "$SHARED" != 1 ] && [ -x "$SERVER" ]
	then
		"$ROOT/client/tools/shared_server.sh" --cache "$SERVER" >&2	# a clean cpp/ tree adds this build to the shared cache (kenshi-k16.2.306.6.1)
	fi
fi

if [ ! -x "$SERVER" ]
then
	echo "demo.sh: missing $SERVER" >&2
	echo "  build: flock $ROOT/client/build/.build.lock nice -n 10 cmake --build $ROOT/client/build --target ksim_server -j8" >&2
	exit 1
fi
if [ ! -x "${CLIENT_CMD[0]}" ]
then
	echo "demo.sh: missing ${CLIENT_CMD[0]}" >&2
	echo "  build: flock $ROOT/client/build/.build.lock nice -n 10 cmake --build $ROOT/client/build --target kclient -j8" >&2
	exit 1
fi

NAVARGS=()
if [ "$NAVWORLD" != none ]
then
	if [ ! -d "$NAVWORLD" ]
	then
		echo "demo.sh: missing navworld bake $NAVWORLD (see docs/RUNBOOK.md --navworld, or pass --navworld none)" >&2
		exit 1
	fi
	NAVARGS=(--navworld "$NAVWORLD")
fi
if [ "$AI" != none ]
then
	if [ ! -d "$AI" ]
	then
		echo "demo.sh: missing AI bake $AI (see docs/RUNBOOK.md --bake, or pass --ai none)" >&2
		exit 1
	fi
	NAVARGS+=(--bake "$AI")
fi

SCENARGS=(--scenario "$SCENARIO")
if [ -n "$SCENFILE" ]
then
	if [ ! -f "$SCENFILE" ]
	then
		echo "demo.sh: missing scenario file $SCENFILE" >&2
		exit 1
	fi
	SCENFILE=$(realpath "$SCENFILE")
	SCENARGS=(--scenario-file "$SCENFILE")
	if [ -f "${SCENFILE%.kscen}.kclient_args" ]
	then
		read -r -a CAMARGS <"${SCENFILE%.kscen}.kclient_args"
	else
		# No camera sidecar: start the camera on the file's `source ... centre=X,Z`. A player-less (spectate_*) fixture has no squad to centre
		# on, and kclient's default position is far away: spectate_fight opened in the Deadlands (kenshi-k16.2.111). A --pos after -- still wins.
		CENTRE=$(sed -n 's/^source .*centre=\(-\{0,1\}[0-9.]*,-\{0,1\}[0-9.]*\).*/\1/p' "$SCENFILE" | head -1)
		if [ -n "$CENTRE" ]
		then
			CAMARGS=(--pos "$CENTRE")
		fi
	fi
fi

TMP=$(mktemp -d /tmp/kdemo.XXXXXX)
LOG="$TMP/server.log"
SPID=

cleanup()
{
	trap - EXIT INT TERM
	if [ -n "$SPID" ]
	then
		kill "$SPID" 2>/dev/null || true
		wait "$SPID" 2>/dev/null || true
	fi
	if [ "${CSTATUS:-0}" -ge 128 ]
	then
		echo "demo.sh: client died (status $CSTATUS); server log kept: $LOG" >&2
	else
		rm -rf -- "/tmp/kdemo.${TMP##*.}"
	fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

for try in 1 2 3
do
	# from the repo root, as it is run in development
	launch_server "$LOG" "$WANT_PORT" env -C "$ROOT" "$SERVER" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen "127.0.0.1:@PORT@" --debug-view "${SCENARGS[@]}" "${NAVARGS[@]}" ${EXTRA[@]+"${EXTRA[@]}"}
	SPID=$SERVER_PID; PORT=$SERVER_PORT
	echo "demo.sh: server pid $SPID (${SCENFILE:-$SCENARIO}, port $PORT, navworld $NAVWORLD, AI bake $AI), waiting for world + navmesh bake..."
	# A cold nav cache makes the start slow (one zone's ground takes 10-100 s; kenshi-oi32): keep waiting while the server is alive and still printing.
	# Any new log output (a `nav zone` or stage line) resets the stall timer (150 s: a zone's line comes only when it finishes, and one took 102 s); 600 s is the overall bound.
	WAIT_START=$SECONDS WAIT_PROGRESS=$SECONDS WAIT_SIZE=0 WAIT_WHY=
	while :
	do
		grep -q '^listening ' "$LOG" && break
		kill -0 "$SPID" 2>/dev/null || break
		size=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
		if [ "$size" != "$WAIT_SIZE" ]; then WAIT_SIZE=$size WAIT_PROGRESS=$SECONDS; fi
		if [ $((SECONDS - WAIT_PROGRESS)) -ge 150 ]; then WAIT_WHY="no new server output for 150 s"; break; fi
		if [ $((SECONDS - WAIT_START)) -ge 600 ]; then WAIT_WHY="not up after the overall bound of 600 s"; break; fi
		sleep 0.5
	done
	grep -q '^listening ' "$LOG" && break
	if ! kill -0 "$SPID" 2>/dev/null
	then
		if bind_retry "$LOG" "$WANT_PORT" "$try"; then SPID=; continue; fi
		SPID=
		echo "demo.sh: server exited early; log tail:" >&2
		tail -n 20 "$LOG" >&2
		exit 1
	fi
	echo "demo.sh: server not listening ($WAIT_WHY); log tail:" >&2
	tail -n 20 "$LOG" >&2
	exit 1
done

echo "demo.sh: server up, starting client"
CSTATUS=0
"${CLIENT_CMD[@]}" --debug --simlink "127.0.0.1:$PORT" ${CAMARGS[@]+"${CAMARGS[@]}"} "$@" || CSTATUS=$?	# --debug: the development windows and keys
exit "$CSTATUS"
