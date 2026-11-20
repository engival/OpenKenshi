#!/usr/bin/env bash
# The plant predicate check (kenshi-k16.2.420.6.1): builds src/world/plants.cpp + plants_main.cpp against libkdata
# (KDATA_LIB, default client/build/cpp/libkdata.a), then asserts on the shipped game data (KENSHI_DATA, default the
# CMake path) that the user's rock and the other rocks, wrecks and props are static, that palms, trees, bushes and
# grass clumps bend, and that every id in client/data/foliage_plants.txt exists in the game data.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
CLIENT=$(cd "$HERE/.." && pwd)
BUILD=$(dirname "${KDATA_LIB:-$CLIENT/build/cpp/libkdata.a}")/..
DATA=${KENSHI_DATA:-$(sed -n 's/^KENSHI_DATA:PATH=//p' "$BUILD/CMakeCache.txt" 2>/dev/null)}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

c++ -std=c++20 -w -O1 -I"$CLIENT/src" -I"$CLIENT/../cpp/include" \
	"$CLIENT/src/world/plants.cpp" "$HERE/plants/plants_main.cpp" \
	"${KDATA_LIB:-$CLIENT/build/cpp/libkdata.a}" -o "$T/plants_main" || { echo "FAIL build"; exit 1; }
[ -d "$DATA" ] || { echo "FAIL no game data at $DATA (set KENSHI_DATA)"; exit 1; }

"$T/plants_main" "$DATA" "$CLIENT/data" > "$T/out.txt" 2> "$T/err.txt"
rc=$?
cat "$T/out.txt"
[ -s "$T/err.txt" ] && { echo "--- stderr ---"; cat "$T/err.txt"; }
[ $rc -eq 0 ] && echo "plants_check: all passed" || echo "plants_check: FAILED"
exit $rc
