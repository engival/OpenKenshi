#!/bin/bash
# The title's held click (kenshi-k16.2.210.4; user 2026-11-14: "The main menu lets you hit 'new game' before the engine is ready, and it just
# eats the input"): with the client's own server still starting, a NEW GAME click is not run but held (MainMenu::setPending draws the button
# pushed) and runs once the server is connected, opening the new-game panel. Dev builds only: the client script's `serverhold N`
# (KCLIENT_DEV_TOOLS) keeps the server 'starting' for N scripted frames, and `dump title` reports the title's state (held click, offline
# line, the new-game panel) for the checks.
#
# usage: client/tools/title_pending_check.sh [OUT_DIR] [--port N]
#   OUT_DIR   the client script, its log and the shots (default client/build/title-pending-check)
#   --port N  the local server's port (default: a free port)
# Exit 0: the click was held while the server started, then ran and the new-game panel opened; 1: a check failed (the log path is printed);
# 2: usage (no kclient, no data dir). Run it under tools/headless.sh's GPU selection like every headless run: the tool does that itself.
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CLIENT=$(cd "$HERE/.." && pwd)	# the client tree (client/)
TREE=$(cd "$CLIENT/.." && pwd)	# the kenshi tree (client/tools/server_bin.sh picks the server from it)
BUILD="$CLIENT/build"
OUT="$BUILD/title-pending-check"
. "$(dirname "${BASH_SOURCE[0]}")/ports.sh"
PORT=
while [ $# -gt 0 ]
do
	case "$1" in
		--port) [ $# -ge 2 ] || { echo "usage: $0 [OUT_DIR] [--port N]" >&2; exit 2; }; PORT=$2; shift 2 ;;
		-*) echo "usage: $0 [OUT_DIR] [--port N]" >&2; exit 2 ;;
		*) OUT=$1; shift ;;
	esac
done
PORT=$(pick_port "$PORT") || exit 3
mkdir -p "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd)

fail()
{
	echo "title_pending_check: FAIL: $*" >&2
	echo "  client log: $OUT/client.log" >&2
	exit 1
}

KCLIENT="$BUILD/kclient"
[ -x "$KCLIENT" ] || { echo "title_pending_check: no $KCLIENT (build target kclient, with the dev tools)" >&2; exit 2; }
. "$HERE/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$TREE" "$BUILD/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
SETSRV=()	# the client starts the title's server itself (--local-server): pin it when that is not the one beside kclient
if [ "$SHARED" = 1 ]; then SETSRV=(--set "session.server=$SERVER"); fi
SIMARG=	# the deploy has no simdata beside it: session.server_args below carries it
[ ${#SRV_SIMDATA[@]} -eq 0 ] || SIMARG=" ${SRV_SIMDATA[*]}"
[ -x "$SERVER" ] || { echo "title_pending_check: no $SERVER (build target ksim_server)" >&2; exit 2; }
DATA=${KENSHI_DATA:-}
if [ -z "$DATA" ] && [ -f "$BUILD/CMakeCache.txt" ]; then DATA=$(sed -n 's/^KENSHI_DATA:[A-Z]*=//p' "$BUILD/CMakeCache.txt" | head -n 1); fi
[ -n "$DATA" ] || { echo "title_pending_check: no data dir (export KENSHI_DATA or configure client/build)" >&2; exit 2; }

# run_case NAME: the client with $OUT/NAME.txt as its script; the log is $OUT/NAME.log
run_case()
{
	local name=$1
	# The client starts this server itself (start_local_server): the scenario and the port are the `session.server_args` option.
	"$HERE/headless.sh" -- "$KCLIENT" --data "$DATA" --debug --local-server ${SETSRV[@]+"${SETSRV[@]}"} \
		--set "session.server_args=--scenario newgame --listen 127.0.0.1:$PORT$SIMARG" --script "$OUT/$name.txt" > "$OUT/$name.log" 2>&1
	[ $? = 0 ] || { echo "title_pending_check: FAIL: the $name run exited nonzero" >&2; echo "  client log: $OUT/$name.log" >&2; exit 1; }
	if grep -q '\[sg\]\[error\]' "$OUT/$name.log"; then echo "title_pending_check: FAIL: $name: sokol logged [sg][error]" >&2; exit 1; fi
}
# want NAME REGEX MESSAGE: the log must match
want()
{
	grep -q "$2" "$OUT/$1.log" || { echo "title_pending_check: FAIL: $1: $3" >&2; echo "  client log: $OUT/$1.log" >&2; exit 1; }
}
# 600 held frames (10 s at 60 fps): the click and both held dumps land inside it, the release and the panel well after. NEW GAME is at (433, 201)
# at 720p; `mouse pos` is framebuffer pixels.
cat > "$OUT/newgame.txt" <<EOT
# the title's held NEW GAME click (kenshi-k16.2.210.4)
0 serverhold 600
5 mouse pos 433 201
6 mouse down left
7 mouse up left
30 dump title
40 screenshot $OUT/held.png
560 dump title
620 dump title
700 dump title
710 screenshot $OUT/panel.png
720 quit
EOT
run_case newgame
want newgame 'title: holding NEW GAME: Starting the game server\.\.\.' "the NEW GAME click was not held while the server started"
want newgame 'title: shown 1, pending 1, status "Starting the game server\.\.\.", new game 0' "while held: expected the NEW GAME button pending and no new-game panel"
want newgame 'title: running the held NEW GAME' "the held click did not run once the server connected"
want newgame 'title: shown 1, pending -1, status "", new game 1' "after the connect: expected the new-game panel open and nothing pending"
[ -s "$OUT/held.png" ] || { echo "title_pending_check: FAIL: no $OUT/held.png" >&2; exit 1; }
[ -s "$OUT/panel.png" ] || { echo "title_pending_check: FAIL: no $OUT/panel.png" >&2; exit 1; }

# CONTINUE and LOAD GAME: the held click runs once the server connects (a held one needs a save list the starting server cannot give, so `titlehold`)
for b in 0 2
do
	n=$([ $b = 0 ] && echo CONTINUE || echo "LOAD GAME")
	cat > "$OUT/hold$b.txt" <<EOT
0 serverhold 600
5 titlehold $b
30 dump title
620 dump title
640 quit
EOT
	run_case hold$b
	want hold$b "title: holding $n: Starting" "the $n click was not held"
	want hold$b 'title: shown 1, pending '$b', status "Starting' "$n: expected it pending while the server started"
	want hold$b "title: held $n ran" "the held $n did not run once the server connected"
	want hold$b 'title: shown 1, pending -1, status ""' "$n: nothing should be pending after the connect"
done

# A failed start: the held click is dropped, the failure line stays, a click after it is not held
for b in 0 2
do
	n=$([ $b = 0 ] && echo CONTINUE || echo "LOAD GAME")
	cat > "$OUT/fail$b.txt" <<EOT
0 serverhold 600
5 titlehold $b
20 dump title
30 serverfail
40 dump title
50 titlehold $b
60 dump title
80 quit
EOT
	run_case fail$b
	want fail$b 'title: shown 1, pending '$b', status "Starting' "$n: expected it pending before the failure"
	want fail$b "title: dropping the held $n: The game server did not start: dev: forced failure" "the failed start did not drop the held $n"
	want fail$b 'title: shown 1, pending -1, status "The game server did not start: dev: forced failure' "$n: after the failure expected nothing pending and the failure line showing"
	want fail$b "title: not holding $n: The game server did not start" "a $n click after the failure was held"
	if grep -q "held $n ran" "$OUT/fail$b.log"; then echo "title_pending_check: FAIL: fail$b: the dropped $n ran" >&2; exit 1; fi
	[ "$(grep -c 'title: shown 1, pending -1, status "The game server did not start' "$OUT/fail$b.log")" -ge 2 ] || { echo "title_pending_check: FAIL: fail$b: a click after the failure left the button pushed" >&2; exit 1; }
done

echo "title_pending_check: ok   NEW GAME / CONTINUE / LOAD GAME held while the server started, then ran; a failed start dropped the held click and kept the line"
echo "  held:  $OUT/held.png"
echo "  panel: $OUT/panel.png"
echo "  logs:  $OUT/*.log"
