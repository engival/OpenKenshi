#!/bin/bash
# The launcher: named launch modes for kclient, so nobody has to remember the flags (kenshi-k16.2.87, user 2026-11-10:
# "these cli options are getting nuts. I either need a launcher, or some more launch scripts").
# usage: client/tools/play.sh [MODE [ARGS]] [-- KCLIENT_ARGS...]
#   play.sh                      list the modes
#   play.sh fly [--at PLACE|x,z] the debug free camera over a place, no title screen (default: the Hub); e.g. play.sh fly --at Squin
#   play.sh title [--playtest] [--bead ID]  the title screen, which starts its own OpenKenshi-server; the journey start (kenshi-k16.2.94);
#                                --playtest shows SCENARIOS on the title and pause menus without --debug, to tag a journey playtest run
#                                (kenshi-k16.2.110); --bead ID also posts the run's notes, verdict and F12 shots to that beads item
#   play.sh game                 a new game: demo.sh's newgame at the Hub with a OpenKenshi-server behind it; e.g. play.sh game
#   play.sh scenario [NAME]      a scenario fixture from scenarios/ (no NAME: list them); e.g. play.sh scenario bar_lure
#   play.sh spectate [NAME]      a player-less spectator fixture (the spectate_* files); e.g. play.sh spectate spectate_fight
#                                scenario / spectate start the fixture's recorded playtest run (kclient --play: NOTES, run file)
#                                --bead ID (any of title / scenario / spectate): the run is pinned to its playtest (no SCENARIOS button; scenario /
#                                spectate: NAME's; title: the item or a parent must be a playtests/*/versions.md bead:, else an error and no run);
#                                the notes, verdict and F12 shots are also comments on that beads item; e.g. play.sh scenario bar_lure --bead kenshi-h2v.1.7
#   play.sh --help               the same list
# Arguments after -- go to kclient (e.g. play.sh fly --at Hub -- --alt 200). Builds are not run here: a missing binary
# prints the command that builds it. game / scenario / spectate run demo.sh, which starts the OpenKenshi-server and joins it;
# title runs plain kclient (no --simlink, no --debug) and kclient starts the server itself (session.server_args), so a
# scripted/headless run gets the title and its server as well (--local-server).
# KCLIENT: the client command, word-split (default client/build/kclient), for every mode (game / scenario / spectate hand it to demo.sh): e.g.
#   KCLIENT="client/tools/headless.sh -- $PWD/client/build/kclient" play.sh fly --at Hub -- --script shot.txt
# The title's server: KSIM_SERVER=<path>, else the shared master build (client/tools/shared_server.sh), else
# client/build/cpp/OpenKenshi-server (kenshi-k16.2.306.1); a shared one goes to kclient as --set session.server=PATH.
# game / scenario / spectate leave the choice to demo.sh, which they run.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$ROOT/client/tools/server_bin.sh"	# pick_server (kenshi-k16.2.306.1)
CLIENT="$ROOT/client/build/kclient"
SERVER="$ROOT/client/build/cpp/OpenKenshi-server"	# replaced by pick_server in the title mode
DEMO="$ROOT/client/tools/demo.sh"
SCEN="$ROOT/scenarios"

usage()
{
	cat <<'EOF'
play.sh: the named launch modes for kclient (the Kenshi runtime rewrite client)

  play.sh fly [--at PLACE|x,z]  the debug free camera over a place, no title screen (default: the Hub)   e.g. play.sh fly --at Squin
  play.sh title [--playtest]    the title screen with its own OpenKenshi-server (New Game starts a game)       e.g. play.sh title
                                --bead ID: pinned to the item's playtest (the item or a parent is the bead: of a playtests/*/versions.md,
                                else an error and no run): no SCENARIOS button, the playtest's window and Notes dock up at once; notes, verdict and shots also as comments there
                                --playtest: the SCENARIOS button (tag a journey playtest run), no --debug
  play.sh game                  a new game: the Wanderer start at the Hub, a OpenKenshi-server behind it      e.g. play.sh game
  play.sh scenario [NAME]       a scenario fixture from scenarios/ (no NAME: list them)                 e.g. play.sh scenario bar_lure
  play.sh spectate [NAME]       a player-less spectator fixture (spectate_*) (no NAME: list them)        e.g. play.sh spectate spectate_fight
  play.sh --help                this list

--bead ID (title, scenario, spectate): the run's notes, verdict and F12 shots are also posted as comments on that beads item (the
run file is still written under playtests/); an F12 shot is copied to findings/asks/ID/. The run is pinned to its playtest: scenario /
spectate to NAME; title walks the item's parents to a playtests/*/versions.md bead: (none: an error, no run).
Arguments after -- go to kclient. Builds are not run here; a missing binary prints the build command.
KCLIENT overrides the client command in every mode (word-split): KCLIENT="client/tools/headless.sh -- $PWD/client/build/kclient".
EOF
}

missing_client()
{
	echo "play.sh: missing $CLIENT" >&2
	echo "  build: flock $ROOT/client/build/.build.lock nice -n 10 cmake --build $ROOT/client/build -j4 --target kclient" >&2
	exit 1
}

missing_server()
{
	echo "play.sh: missing $SERVER" >&2
	echo "  build: flock $ROOT/client/build/.build.lock nice -n 10 cmake --build $ROOT/client/build -j4 --target ksim_server" >&2
	exit 1
}

# The scenario fixtures, one per line (NAME, the file's stem). $1 filters: empty = every non-spectate fixture, "spectate_" = those.
list_scenarios()
{
	local prefix=$1 f name
	local -a found=()
	for f in "$SCEN"/*.kscen
	do
		[ -e "$f" ] || continue
		name=$(basename "$f" .kscen)
		if [ "$prefix" = spectate_ ]
		then
			case "$name" in spectate_*) found+=("$name") ;; esac
		else
			case "$name" in spectate_*) ;; *) found+=("$name") ;; esac
		fi
	done
	if [ ${#found[@]} -eq 0 ]
	then
		echo "play.sh: no scenarios in $SCEN" >&2
		exit 1
	fi
	printf '  %s\n' "${found[@]}"
}

# The playtest a --bead item belongs to (kenshi-k16.2.136.1): walks ID and its parents (bd show --json, .parent) until one is the
# `bead:` line of a playtests/*/versions.md; prints that playtest's name. A loud error and exit 2 when none is (no run).
resolve_playtest()
{
	local id=$1 hop name json
	command -v jq >/dev/null || { echo "play.sh: --bead needs jq to walk the item's parents" >&2; exit 2; }
	for hop in 1 2 3 4 5 6 7 8 9 10
	do
		[ -n "$id" ] || break
		name=$(grep -lx "bead: $id" "$ROOT"/playtests/*/versions.md 2>/dev/null | head -n 1)
		if [ -n "$name" ]
		then
			basename "$(dirname "$name")"
			return 0
		fi
		json=$(bd -C "$ROOT" show "$id" --json 2>/dev/null) || { echo "play.sh: bd show $id failed (is it an item id?)" >&2; exit 2; }
		id=$(printf '%s' "$json" | jq -r '(if type == "array" then .[0] else . end).parent // empty')
	done
	echo "play.sh: --bead $1: neither it nor any parent is the 'bead:' of a playtests/*/versions.md, so no playtest to pin the run to; no run started" >&2
	exit 2
}

MODE=${1:-}
[ $# -gt 0 ] && shift

case "$MODE" in
	''|--help|-h|help)
		usage
		exit 0
		;;
	fly)
		PLACE=Hub
		EXTRA=()
		while [ $# -gt 0 ]
		do
			case "$1" in
				--at) [ $# -ge 2 ] || { echo "play.sh: --at needs PLACE or x,z" >&2; exit 2; }; PLACE=$2; shift 2 ;;
				--at=*) PLACE=${1#--at=}; shift ;;
				--) shift; EXTRA+=("$@"); break ;;
				*) EXTRA+=("$1"); shift ;;	# any other flag goes to kclient (--alt, --time, ...)
			esac
		done
		CLIENT_CMD=(${KCLIENT:-$CLIENT})
		[ -x "${CLIENT_CMD[0]}" ] || missing_client
		# 400 up, looking down 35 degrees: the place fills the frame (the fly-around defaults; --alt / --pitch after -- override)
		exec "${CLIENT_CMD[@]}" --debug --at "$PLACE" --alt 400 --pitch -35 ${EXTRA[@]+"${EXTRA[@]}"}
		;;
	title)
		EXTRA=()
		PT=()
		BEAD=()
		while [ $# -gt 0 ]
		do
			case "$1" in
				--playtest) PT=(--playtest); shift ;;
				--bead) [ $# -ge 2 ] || { echo "play.sh: --bead needs an item id" >&2; exit 2; }; BEAD=(--bead "$2"); shift 2 ;;
				--bead=*) BEAD=(--bead "${1#--bead=}"); shift ;;
				--) shift; EXTRA=("$@"); break ;;
				*) echo "play.sh: title takes only --playtest and --bead ID (kclient args go after --)" >&2; exit 2 ;;
			esac
		done
		CLIENT_CMD=(${KCLIENT:-$CLIENT})
		[ -x "${CLIENT_CMD[0]}" ] || missing_client
		pick_server "$ROOT" "$SERVER"	# 'server: shared PATH' (KSIM_SERVER or the shared master build) / 'server: built (reason)'
		SERVER_ARGS=()
		if [ "$SHARED" = 1 ]
		then
			SERVER_ARGS=(--set "session.server=$SERVER")	# kclient starts this one for the title (start_local_server); empty: the one beside kclient
			[ ${#SRV_SIMDATA[@]} -eq 0 ] || SERVER_ARGS+=(--set "session.server_args=--scenario newgame ${SRV_SIMDATA[*]}")	# the deploy has no simdata beside it
		fi
		[ -x "$SERVER" ] || missing_server	# kclient starts it itself; the path is beside kclient (client/build/cpp) unless --set names one
		# Plain kclient: no --simlink, no --debug. --local-server keeps the title in a scripted/headless run too
		# (scripted runs skip the title otherwise); the title starts the server (start_local_server).
		PLAY=()
		if [ ${#BEAD[@]} -gt 0 ]; then PN=$(resolve_playtest "${BEAD[1]}") || exit 2; PLAY=(--play "$PN"); fi	# pinned to the item's playtest: no SCENARIOS button
		exec "${CLIENT_CMD[@]}" --local-server ${SERVER_ARGS[@]+"${SERVER_ARGS[@]}"} ${PT[@]+"${PT[@]}"} ${PLAY[@]+"${PLAY[@]}"} ${BEAD[@]+"${BEAD[@]}"} ${EXTRA[@]+"${EXTRA[@]}"}
		;;
	game)
		EXTRA=()
		while [ $# -gt 0 ]
		do
			case "$1" in
				--) shift; EXTRA=("$@"); break ;;
				*) echo "play.sh: game takes no options (kclient args go after --)" >&2; exit 2 ;;
			esac
		done
		CLIENT_CMD=(${KCLIENT:-$CLIENT})
		[ -x "${CLIENT_CMD[0]}" ] || missing_client
		exec "$DEMO" --scenario newgame ${EXTRA[@]+-- "${EXTRA[@]}"}
		;;
	scenario|spectate)
		NAME=
		EXTRA=()
		BEAD=()
		while [ $# -gt 0 ]
		do
			case "$1" in
				--bead) [ $# -ge 2 ] || { echo "play.sh: --bead needs an item id" >&2; exit 2; }; BEAD=(--bead "$2"); shift 2 ;;
				--bead=*) BEAD=(--bead "${1#--bead=}"); shift ;;
				--) shift; EXTRA=("$@"); break ;;
				-*) echo "play.sh: $MODE takes a NAME, --bead ID and no other options (kclient args go after --)" >&2; exit 2 ;;
				*) NAME=$1; shift ;;
			esac
		done
		FILTER=
		[ "$MODE" = spectate ] && FILTER=spectate_
		if [ -z "$NAME" ]
		then
			echo "play.sh: $MODE fixtures in $SCEN:"
			list_scenarios "$FILTER"
			exit 0
		fi
		FILE="$SCEN/$NAME.kscen"
		if [ ! -f "$FILE" ] && [ "$MODE" = spectate ] && [ -f "$SCEN/spectate_$NAME.kscen" ]
		then
			FILE="$SCEN/spectate_$NAME.kscen"
		fi
		if [ ! -f "$FILE" ]
		then
			echo "play.sh: no scenario $FILE; the $MODE fixtures are:" >&2
			list_scenarios "$FILTER" >&2
			exit 2
		fi
		CLIENT_CMD=(${KCLIENT:-$CLIENT})
		[ -x "${CLIENT_CMD[0]}" ] || missing_client
		# --play: the client starts the fixture's recorded run itself (Notes, MARK, verdict, run file); the server's boot load sends no
		# SCENARIO_STARTED, so without it no run began (kenshi-k16.2.126). The run's LOAD reloads the same file at its start.
		STEM=$(basename "$FILE" .kscen)
		exec "$DEMO" --scenario-file "$FILE" -- --play "$STEM" ${BEAD[@]+"${BEAD[@]}"} ${EXTRA[@]+"${EXTRA[@]}"}
		;;
	*)
		echo "play.sh: unknown mode '$MODE'" >&2
		usage >&2
		exit 2
		;;
esac
