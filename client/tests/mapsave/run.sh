#!/bin/bash
# mapsave (kenshi-k16.2.435): the world map's view (the MAP page of the overview, M) and the camera sidecar that carries it. live: one
# OpenKenshi-server, a scenario Wanderer at the Hub; M opens the default overview (zoom only since k16.2.435.5: the map opens centred on the camera), the wheel zooms,
# M closes and reopens the same view, a quicksave at that zoom and a quickload bring it back. oldsave: the live run's sidecar with its map_ lines
# stripped (an older client's), loaded from a fresh client's title LOAD GAME; the map must open at the default overview.
# Checks: the `dump map` lines of live (the reopened view equals the one before the close; the loaded view equals the saved one and differs from
# the one moved away from) and oldsave's view equals live's default. Usage: run.sh live OUT, then run.sh oldsave OUT. Port: PORT (47837).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
mode=${1:?live|oldsave}
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/../../tools/ports.sh"
port=$(pick_port "${PORT:-}") || exit 3
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
SETSRV=()	# --local-server's own server: pin it to the chosen binary when that is not the one beside kclient (a shared one)
if [ "$SHARED" = 1 ]
then
	SETSRV=(--set "session.server=$server")
	[ ${#SRV_SIMDATA[@]} -eq 0 ] || SETSRV+=(--set "session.server_args=--scenario newgame ${SRV_SIMDATA[*]}")	# the deploy has no simdata beside it
fi
export XDG_DATA_HOME="$out/xdg"
mkdir -p "$XDG_DATA_HOME/ksim/saves"
case "$mode" in live) name=save_load ;; centre) name=centre ;; oldsave) name=oldsidecar ;; *) echo "run.sh: mode $mode (live|oldsave|centre)" >&2; exit 2 ;; esac
script="$here/$name.txt"
[ -f "$script" ] || { echo "run.sh: no script $script" >&2; exit 2; }
sed "s#OUTDIR#$out#g" "$script" > "$out/script-$mode.txt"
# The map's view at each dump, in the script's order: "zoom Z cx X cz Z".
# k16.2.435.5: the centre is the camera's at each open and never kept: the checks compare the zoom (the dump's first field pair).
views() { grep -o '^map view: zoom [^ ]* cx [^ ]* cz [^ ]*' "$1" | sed 's/^map view: //'; }
zoomof() { echo "${1%% cx *}"; }
rc=0
if [ "$mode" = live ] || [ "$mode" = centre ]
then
	"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/mapsave.kscen" --save-dir "$XDG_DATA_HOME/ksim/saves" > "$out/server-live.log" 2>&1 &
	pid=$!
	for i in $(seq 240); do grep -q "^listening" "$out/server-live.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
	server_listening "$out/server-live.log" $pid $port
	cd "$tree" && timeout 400 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script-$mode.txt" > "$out/client-$mode.log" 2>&1
	rc=$?
	ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
	mapfile -t v < <(views "$out/client-$mode.log")
	echo "views: ${#v[@]}: ${v[*]:-none}"
	ok=1
	if [ "$mode" = centre ]
	then
		# the centre is the camera's each open (within a unit); the zoom is one value throughout
		near() { awk -v a="$1" -v b="$2" 'BEGIN { d = a - b; exit !(d < 1 && d > -1) }'; }
		at() { echo "${v[$1]}" | awk -v f="$2" '{ print $f }'; }	# "zoom Z cx X cz Z": 2 zoom, 4 cx, 6 cz
		[ "${#v[@]}" -eq 4 ] || { echo "expected 4 dump map lines, got ${#v[@]}" >&2; ok=0; }
		if [ "${#v[@]}" -eq 4 ]
		then
			near "$(at 0 4)" -40000 && near "$(at 0 6)" 1000 || { echo "first open not on the camera A: ${v[0]}" >&2; ok=0; }
			near "$(at 1 4)" -40000 && near "$(at 1 6)" 1000 && { echo "the drag did not pan: ${v[1]}" >&2; ok=0; }
			near "$(at 2 4)" 20000 && near "$(at 2 6)" -30000 || { echo "second open not on the camera B: ${v[2]}" >&2; ok=0; }
			near "$(at 3 4)" 20000 && near "$(at 3 6)" -30000 || { echo "reopen not on the camera B: ${v[3]}" >&2; ok=0; }
			for i in 1 2 3; do [ "$(at 0 2)" = "$(at $i 2)" ] || { echo "the zoom changed: ${v[0]} -> ${v[$i]}" >&2; ok=0; }; done
		fi
		[ $ok = 1 ] || rc=1
	else
	[ "${#v[@]}" -eq 6 ] || { echo "expected 6 dump map lines, got ${#v[@]}" >&2; ok=0; }
	if [ "${#v[@]}" -eq 6 ]
	then
		case "${v[0]}" in "zoom 0.000000 "*) echo "the default view is the fitted whole world (${v[0]})" >&2; ok=0 ;; esac
		[ "$(zoomof "${v[1]}")" = "$(zoomof "${v[2]}")" ] || { echo "close/reopen changed the view: ${v[1]} -> ${v[2]}" >&2; ok=0; }
		[ "$(zoomof "${v[3]}")" = "$(zoomof "${v[5]}")" ] || { echo "the load did not restore the saved view: ${v[3]} -> ${v[5]}" >&2; ok=0; }
		[ "$(zoomof "${v[3]}")" != "$(zoomof "${v[4]}")" ] || { echo "the view before the load was not moved away from the save's (${v[4]})" >&2; ok=0; }
		grep -q '^map view: default zoom' "$out/client-live.log" || { echo "no default view line" >&2; ok=0; }
		grep -q "^map view: saved \"quicksave\": $(zoomof "${v[3]}")\$" "$out/client-live.log" || { echo "the sidecar's map view is not ${v[3]}" >&2; ok=0; }
	fi
	[ $ok = 1 ] || rc=1
	fi
else
	side=$(find "$XDG_DATA_HOME/kclient/cameras" -name 'quicksave.camera' 2>/dev/null | head -1)
	[ -n "$side" ] || { echo "run.sh: no quicksave camera sidecar under $XDG_DATA_HOME (run live first)" >&2; exit 2; }
	grep -q '^map_zoom=' "$side" || { echo "run.sh: $side has no map view (run live first)" >&2; exit 2; }
	grep -v '^map_' "$side" > "$side.old" && mv "$side.old" "$side"	# the map_ keys of an older client's sidecar: gone
	echo "sidecar: $side, map keys left: $(grep -c '^map_' "$side")"
	cd "$tree" && timeout 400 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --local-server ${SETSRV[@]+"${SETSRV[@]}"} --debug --script "$out/script-oldsave.txt" > "$out/client-oldsave.log" 2>&1
	rc=$?
	mapfile -t v < <(views "$out/client-oldsave.log")
	echo "view: ${v[*]:-none}"
	ok=1
	[ "${#v[@]}" -eq 1 ] || { echo "expected 1 dump map line, got ${#v[@]}" >&2; ok=0; }
	live=$(grep -m1 -o '^map view: zoom [^ ]* cx [^ ]* cz [^ ]*' "$out/client-live.log" | sed 's/^map view: //')
	if [ "${#v[@]}" -eq 1 ] && [ -n "$live" ] && [ "$(zoomof "${v[0]}")" != "$(zoomof "$live")" ]
	then
		echo "an old sidecar did not get live's default view: ${v[0]} (live's default: $live)" >&2; ok=0
	fi
	case "${v[0]:-}" in "zoom 0.000000 "*) echo "an old sidecar's map was fitted, not the default (${v[0]})" >&2; ok=0 ;; esac
	grep -q '^map view: default zoom' "$out/client-oldsave.log" || { echo "no default view line" >&2; ok=0; }
	[ $ok = 1 ] || rc=1
fi
echo "client rc $rc; map view lines: $(grep -c '^map view:' "$out/client-$mode.log"); [sg][error]: $(grep -c '\[sg\]\[error' "$out/client-$mode.log")"
. "$here/../runcheck.sh"
run_status "$rc" "$out/script-$mode.txt" 1 || exit 1
exit "$rc"
