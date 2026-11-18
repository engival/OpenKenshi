# Sourced by the client/tests/*/run.sh scripts: these checks play the game, so they need the original game data.
# Without it the test is skipped (exit 0, a SKIP line), which is how the public tree runs them.
# The data dir is $KENSHI_DATA, else the KENSHI_DATA the client was configured with (client/build/CMakeCache.txt).
_nd_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
_nd=${KENSHI_DATA:-}
[ -n "$_nd" ] || _nd=$(sed -n 's/^KENSHI_DATA:[A-Z]*=//p' "${BUILD:-$_nd_root/build}/CMakeCache.txt" 2>/dev/null | head -1)
if [ -z "$_nd" ] || [ ! -d "$_nd" ]
then
	echo "SKIP: $(basename "$(dirname "${BASH_SOURCE[1]}")") needs the Kenshi game data; set KENSHI_DATA (or configure with -DKENSHI_DATA=...) to run it" >&2
	exit 0
fi
unset _nd _nd_root
