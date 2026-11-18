#!/bin/bash
# The fence's test (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153): builds kclient with the dev tools and the debug view OFF
# in client/build-devoff (git-ignored) and checks that the game still starts: the title screen, then a new game up to its first world
# frame. It also checks that the OFF binary holds no playtest / beadpost symbols and no home-directory paths (kenshi-379.13), and passes --data
# to every run: an OFF build carries no built-in data path.
#
# usage: client/tools/dev_off_check.sh [--no-build] [--port N] [--out DIR]
#   --no-build  skip configure + build (the binaries in client/build-devoff are used as they are)
#   --port N    the new-game step's OpenKenshi-server port (default: a free port)
#   --out DIR   where the logs and shots go (default client/build-devoff/check)
# The OFF build has no dev script ops: both steps are driven by the options every build has. The title step is `--local-server
# --screenshot` (the title with its own server, a shot, quit); the new-game step starts a OpenKenshi-server on the `newgame` scenario, joins it
# with `--simlink HOST:PORT --screenshot` (the shot and the quit come 3 frames after the first world frame). Both run under
# tools/headless.sh. Exit 0: all passed; 1: a check failed (the log paths are printed); 2: usage.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="$ROOT/client/build-devoff"
OUT="$BUILD/check"
PORT=
DOBUILD=1
while [ $# -gt 0 ]
do
	case "$1" in
		--no-build) DOBUILD=0; shift ;;
		--port) [ $# -ge 2 ] || { echo "dev_off_check: --port needs a number" >&2; exit 2; }; PORT=$2; shift 2 ;;
		--out) [ $# -ge 2 ] || { echo "dev_off_check: --out needs a directory" >&2; exit 2; }; OUT=$2; shift 2 ;;
		*) echo "usage: $0 [--no-build] [--port N] [--out DIR]" >&2; exit 2 ;;
	esac
done

. "$(dirname "${BASH_SOURCE[0]}")/ports.sh"
fail()
{
	echo "dev_off_check: FAIL: $*" >&2
	exit 1
}

KCLIENT="$BUILD/OpenKenshi-client"	# the OFF build names target kclient's file OpenKenshi-client (kenshi-379.11.1)
SERVER="$BUILD/cpp/OpenKenshi-server"
mkdir -p "$OUT"

# The data dir the runs pass: an OFF build carries no built-in one (kenshi-379.13), so it goes on the command line, as a release's client does.
DATA=${KENSHI_DATA:-}
if [ -z "$DATA" ] && [ -f "$ROOT/client/build/CMakeCache.txt" ]
then
	DATA=$(sed -n 's/^KENSHI_DATA:[A-Z]*=//p' "$ROOT/client/build/CMakeCache.txt" | head -n 1)
fi
[ -n "$DATA" ] || fail "KENSHI_DATA is not set (export it, or configure client/build first)"

if [ "$DOBUILD" = 1 ]
then
	cmake -S "$ROOT/client" -B "$BUILD" -DKENSHI_DATA="$DATA" -DKCLIENT_DEV_TOOLS=OFF -DKCLIENT_DEBUG_VIEW=OFF >"$OUT/configure.log" 2>&1 \
		|| fail "configure failed, see $OUT/configure.log"
	nice -n 10 cmake --build "$BUILD" -j4 --target kclient ksim_server >"$OUT/build.log" 2>&1 \
		|| fail "build failed, see $OUT/build.log"
	"$ROOT/client/tools/shared_server.sh" --cache "$SERVER" >>"$OUT/build.log" 2>&1 || true	# a clean cpp/ tree is cached (kenshi-k16.2.306.6.1)
fi
[ -x "$KCLIENT" ] || fail "missing $KCLIENT (run without --no-build)"
[ -x "$SERVER" ] || fail "missing $SERVER (run without --no-build)"

# 1. no dev symbols in the OFF binary
if command -v nm >/dev/null
then
	N=$(nm -C "$KCLIENT" | grep -ci 'playtest\|beadpost')
	[ "$N" = 0 ] || fail "$N playtest/beadpost symbols in the OFF kclient (nm -C $KCLIENT | grep -i 'playtest\\|beadpost')"
	echo "dev_off_check: ok   no playtest/beadpost symbols in kclient"
fi
# kenshi-379.13: the OFF binary carries no developer path; the game data is resolved at runtime.
if command -v strings >/dev/null
then
	N=$(strings "$KCLIENT" | grep -c '/hom[e]/')
	[ "$N" = 0 ] || fail "$N home-directory strings in the OFF kclient (strings $KCLIENT | grep /hom[e]/)"
	echo "dev_off_check: ok   no home-directory paths in kclient"
fi

HEADLESS="$ROOT/client/tools/headless.sh"

# 2. the title screen (its own server), a shot, quit
rm -f "$OUT/title.png"
"$HEADLESS" -- "$KCLIENT" --data "$DATA" --local-server --screenshot "$OUT/title.png" >"$OUT/title.log" 2>&1
RC=$?
[ "$RC" = 0 ] || fail "the title run exited $RC, see $OUT/title.log"
[ -s "$OUT/title.png" ] || fail "no title shot, see $OUT/title.log"
echo "dev_off_check: ok   the title screen ($OUT/title.png)"

# 2b. a bare --screenshot (no server, no sim): the world path draws no characters and the shot is written (kenshi-k16.2.265: it used to
# segfault in Characters::render, the shadow atlas pointer being set only in the chars init phase)
rm -f "$OUT/bare.png"
"$HEADLESS" -- "$KCLIENT" --data "$DATA" --screenshot "$OUT/bare.png" >"$OUT/bare.log" 2>&1
RC=$?
[ "$RC" = 0 ] || fail "the bare --screenshot run exited $RC, see $OUT/bare.log"
[ -s "$OUT/bare.png" ] || fail "no bare --screenshot shot, see $OUT/bare.log"
echo "dev_off_check: ok   a bare --screenshot with no server ($OUT/bare.png)"

# 3. a new game: a server on the newgame scenario, the client joins it and reaches its first world frame
[ -n "$PORT" ] || PORT=$(pick_port "") || fail "no free port"
if ss -Hltn "sport = :$PORT" 2>/dev/null | grep -q .
then
	fail "port busy: $PORT is in use (--port N, or leave it out)"
fi
(cd "$ROOT" && exec "$SERVER" --data "$DATA" --listen "127.0.0.1:$PORT" --scenario newgame) >"$OUT/server.log" 2>&1 &
SPID=$!
trap 'kill "$SPID" 2>/dev/null; wait "$SPID" 2>/dev/null' EXIT
for ((i = 0; i < 120; i++))
do
	grep -q '^listening ' "$OUT/server.log" && break
	kill -0 "$SPID" 2>/dev/null || fail "the server exited early, see $OUT/server.log"
	sleep 0.5
done
grep -q '^listening ' "$OUT/server.log" || fail "the server is not listening after 60 s, see $OUT/server.log"
rm -f "$OUT/newgame.png"
"$HEADLESS" -- "$KCLIENT" --data "$DATA" --simlink "127.0.0.1:$PORT" --screenshot "$OUT/newgame.png" >"$OUT/newgame.log" 2>&1
RC=$?
[ "$RC" = 0 ] || fail "the new-game run exited $RC, see $OUT/newgame.log"
grep -q '^first_world_frame' "$OUT/newgame.log" || fail "no first_world_frame, see $OUT/newgame.log"
[ -s "$OUT/newgame.png" ] || fail "no new-game shot, see $OUT/newgame.log"
echo "dev_off_check: ok   a new game up to its first world frame ($OUT/newgame.png)"
echo "dev_off_check: PASS (logs in $OUT)"
