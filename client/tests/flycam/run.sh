#!/bin/bash
# flycam (kenshi-wlql): run.sh OUT. A headless `--fly --debug --at Hub` client driven by fly.txt; the per-frame camera log is checked:
#   A  W held at pitch -60: the height is constant (no dive)
#   B  R held: x and z constant, y rises;  C  F held: x, z constant, y falls;  D  Space held: x, z constant, y rises
#   E  a dive (pitch -80, F and W held): y >= ground + clearance on every frame, and the clearance is reached
#   keys: while flying C / R / Space are swallowed by the camera; after Ctrl+F (no longer flying) they fire their game actions
#   G  (game phase, boxselect.kscen server, game.txt; kenshi-2n7o) in a game Space pauses and the height is unchanged, R still lifts the camera
# Prints "flycam: PASS" or exits 1. The --fly phases need no server.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
cd "$tree" && KCLIENT_FLYLOG=1 nice -n 10 timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --fly --debug --at Hub --script "$here/fly.txt" > "$out/client.log" 2>&1
rc=$?
grep -E "^flycam: frame" "$out/client.log" > "$out/cam.txt"
echo "sg errors: $(grep -c '\[sg\]\[error\]' "$out/client.log"), camera frames: $(grep -c . "$out/cam.txt")"
fail=0
res=$(awk '
function dev(a, b) { return (a > b ? a - b : b - a) }
{ f = $3; x = $5; y = $6; z = $7; g = $9; X[f] = x; Y[f] = y; Z[f] = z; G[f] = g }
END {
	# phase ranges (frames): the log has one line per frame
	p["A"] = "80 128"; p["B"] = "150 188"; p["C"] = "205 238"; p["D"] = "255 288"
	for (k in p) { split(p[k], r, " "); lo = r[1]; hi = r[2]
		if (!(lo in Y) || !(hi in Y)) { print "missing frames for " k; bad = 1; continue }
		dy = Y[hi] - Y[lo]; dh = sqrt((X[hi] - X[lo]) ^ 2 + (Z[hi] - Z[lo]) ^ 2)
		if (k == "A" && (dev(Y[hi], Y[lo]) > 0.01 || dh < 1)) { print "A: height moved " dy " or no travel " dh; bad = 1 }
		if (k != "A" && dh > 0.01) { print k ": moved horizontally " dh; bad = 1 }
		if ((k == "B" || k == "D") && dy < 1) { print k ": did not rise " dy; bad = 1 }
		if (k == "C" && dy > -1) { print k ": did not fall " dy; bad = 1 }
	}
	minc = 1e9
	for (f = 310; f <= 515; f++) { if (!(f in Y)) { print "E: missing frame " f; bad = 1; break } c = Y[f] - G[f]; if (c < minc) minc = c }
	if (minc < 1.99) { print "E: dived under the terrain clearance, min " minc; bad = 1 }
	if (minc > 2.5) { print "E: never reached the ground " minc; bad = 1 }
	print "E: min clearance " minc
	exit bad
}' "$out/cam.txt")
[ $? -eq 0 ] || fail=1
echo "$res"
sw=$(grep -c "^flykey: .* swallowed" "$out/client.log"); fi=$(grep -c "^flykey: .* fires" "$out/client.log")
echo "flykeys: swallowed $sw fires $fi"
for a in toggle_stats floor_up pause; do
	grep -q "^flykey: $a swallowed" "$out/client.log" || { echo "flycam: $a was not swallowed while flying"; fail=1; }
	grep -q "^flykey: $a fires" "$out/client.log" || { echo "flycam: $a did not fire outside the free camera"; fail=1; }
done
[ "$rc" -eq 0 ] || { echo "flycam: client rc $rc"; fail=1; }
# game phase
. "$tree/client/tools/server_bin.sh"; pick_server "$tree" "$tree/client/build/cpp/OpenKenshi-server"
. "$here/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
"$SERVER" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/../boxselect/boxselect.kscen" --navworld "${KENSHI_NAVWORLD:-$HOME/git/kenshi/navworld}" > "$out/server.log" 2>&1 &
spid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $spid > /dev/null || break; sleep 0.5; done
cd "$tree" && KCLIENT_FLYLOG=1 nice -n 10 timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --simlink 127.0.0.1:$port --debug --script "$here/game.txt" > "$out/game.log" 2>&1
grc=$?
kill "$spid" 2>/dev/null
grep -E "^flycam: frame" "$out/game.log" > "$out/gcam.txt"
gy() { awk -v f="$1" '$3 == f { print $6 }' "$out/gcam.txt"; }
y0=$(gy 165); y1=$(gy 198); y2=$(gy 265); y3=$(gy 298)
echo "game: y at 165 $y0, 198 $y1 (Space), 265 $y2, 298 $y3 (R); clock: $(grep -c '^clock' "$out/game.log")"
awk -v a="$y0" -v b="$y1" -v c="$y2" -v d="$y3" 'BEGIN { exit !(a != "" && b != "" && c != "" && d != "" && (a - b < 0.01 && b - a < 0.01) && d - c > 1) }' || { echo "flycam: game: Space moved the camera or R did not lift it"; fail=1; }
awk '/^clock/ { n++; if ($0 ~ /paused bits [0-9]*[13579],/) p[n] = 1 } END { exit !(p[2] && !p[1]) }' "$out/game.log" || { echo "flycam: game: Space did not pause the sim"; fail=1; }
[ "$grc" -eq 0 ] || { echo "flycam: game client rc $grc"; fail=1; }
[ "$fail" -eq 0 ] || { echo "flycam: FAIL"; exit 1; }
echo "flycam: PASS"
