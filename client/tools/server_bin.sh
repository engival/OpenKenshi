#!/bin/bash
# Sourced by the client's run scripts (client/tools/*.sh and client/tests/*/run.sh), not run:
# pick the OpenKenshi-server for a run (kenshi-k16.2.306.1) without building ksim_server in every worktree.
#
# pick_server TREE FALLBACK_PATH
#   KSIM_SERVER=<path>   wins: an explicit binary (a debug build, a KSIM_PROFILE build, the shared deploy by hand).
#        It is not this run's fallback, so it gets the same --data/--sim-data as the shared one: this tree's.
#   else TREE's shared master build from client/tools/shared_server.sh, when that tree has one and this branch
#        changes nothing the server is built from (it says why when it does not).
#   else FALLBACK_PATH    the server built in the run's own build dir (client/build/cpp/OpenKenshi-server, or
#        <tree>/cpp/build/OpenKenshi-server for live.sh).
#   Sets SERVER (the path), SHARED (1 when SERVER is not the fallback) and SRV_ARGS, the arguments a binary from
#   outside this build dir needs with it: --data DIR (the game data; $KENSHI_DATA, else the KENSHI_DATA the client was
#   configured with) and --sim-data DIR (TREE/cpp/data; the deploy carries no simdata/ beside it). Both are empty for
#   the fallback, whose build has them baked in or beside it. SRV_SIMDATA is the sim-data part alone, for a server the
#   CLIENT starts (--local-server: session.server_args; the client passes --data itself).
#   Prints the one line the run logs: 'server: shared PATH' or 'server: built (REASON)', the reason being
#   shared_server.sh's decline ('cpp/ changed', 'no shared binary for <hash8>', ...).
pick_server()
{
	local tree=$1 fallback=$2 line reason data sim cache
	if [ -n "${KSIM_SERVER:-}" ]
	then
		SERVER=$KSIM_SERVER SHARED=1
		echo "server: shared $SERVER (KSIM_SERVER)"
	else
		line=
		if [ -x "$tree/client/tools/shared_server.sh" ]
		then
			line=$("$tree/client/tools/shared_server.sh" --explain)
		fi
		case $line in
			shared\ *)
				SERVER=${line#shared } SHARED=1
				echo "server: $line"
				;;
			*)
				SERVER=$fallback SHARED=0
				reason=${line#built (}
				reason=${reason%)}
				echo "server: built (${reason:-this tree has no shared_server.sh})"
				;;
		esac
	fi
	SRV_ARGS=() SRV_SIMDATA=()
	if [ "$SHARED" = 1 ]
	then
		# The deploy is the client lead's build of master (kenshi-k16.2.306): its baked game data and sim data are that
		# machine's paths, unreadable from a ds worktree, so both come on the command line from this tree.
		data=${KENSHI_DATA:-}
		if [ -z "$data" ] || [ ! -d "$data" ]
		then
			cache=${fallback%/cpp/OpenKenshi-server}/CMakeCache.txt
			[ -f "$cache" ] || cache=$tree/client/build/CMakeCache.txt
			data=$(sed -n 's/^KENSHI_DATA:[A-Z]*=//p' "$cache" 2>/dev/null | head -n 1)
		fi
		sim=$tree/cpp/data
		if [ -f "$sim/ksim_ai_behaviours.tsv" ] && [ -d "$sim/behaviour" ]
		then
			SRV_SIMDATA=(--sim-data "$sim")
		else
			sim=
		fi
		if [ -n "$data" ] && [ -d "$data" ]
		then
			SRV_ARGS=(--data "$data")
		fi
		SRV_ARGS+=(${SRV_SIMDATA[@]+"${SRV_SIMDATA[@]}"})
		echo "server: args ${SRV_ARGS[*]:-(none)}" >&2
	fi
}
