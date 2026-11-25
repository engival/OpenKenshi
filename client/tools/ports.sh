# Sourced by the test runners (client/tests/*/run.sh) and the check tools (client/tools/*.sh): a free TCP port on 127.0.0.1, so two sessions
# (desktop and ds share one machine) never run their servers on the same fixed default (kenshi-k16.2.307).
#   port=$(pick_port "${PORT:-}") || exit 3   PORT set: kept if nothing listens there, else "port busy" (stderr) and failure.
#                                             PORT unset: a free port from KENSHI_PORT_RANGE (default 29100-29499, below the kernel's ephemeral range
#                                             32768-60999, where an outgoing connection can hold the port and ss -lt does not show it; kenshi-k16.2.417.1), random start.
#   server_listening LOG PID PORT             after the start loop: stops the script (exit 3) with a "did not listen" line when the server
#                                             log lacks "^listening": the bind failed (port taken in the meantime) or the server died.
#   start_server_on_free_port LOG CMD...      start a server and wait until it listens; the literal word @PORT@ in CMD is replaced by the port. Sets
#                                             SERVER_PID and SERVER_PORT. Use it for every server start of a runner that starts several (a port
#                                             picked once at the top can be taken by another session between two cases, kenshi-k16.2.358):
#                                             each start picks a fresh port (PORT set: that port only, busy = exit 3, as before; START_PORT overrides PORT per start), and a bind
#                                             failure ("Address already in use") picks another port and starts again, at most 3 tries, each
#                                             logged on stderr ("port retry: ..."). Any other failure to listen stops the script (exit 3).
#   launch_server LOG WANT CMD... / bind_retry LOG WANT TRY   the two halves of that, for a caller with its own wait loop (demo.sh).
# Also runnable: client/tools/ports.sh [PORT]  prints the port.
port_busy() { ss -Hltn "sport = :$1" 2>/dev/null | grep -q .; }
pick_port()
{
	local want=${1:-} lo hi p n i
	if [ -n "$want" ]
	then
		if port_busy "$want"; then echo "port busy: 127.0.0.1:$want already has a listener (unset PORT to pick a free one)" >&2; return 1; fi
		echo "$want"; return 0
	fi
	read -r lo hi <<< "${KENSHI_PORT_RANGE:-29100 29499}"
	n=$((hi - lo + 1))
	for ((i = 0; i < n; i++))
	do
		p=$((lo + (RANDOM * 32768 + RANDOM + i) % n))
		port_busy "$p" || { echo "$p"; return 0; }
	done
	echo "no free port in $lo-$hi" >&2; return 1
}
server_listening()
{
	local log=$1 pid=$2 port=$3
	grep -q "^listening" "$log" && return 0
	echo "server did not listen on 127.0.0.1:$port (port busy, or the server died): tail of $log:" >&2
	tail -3 "$log" >&2
	kill "$pid" 2>/dev/null
	exit 3
}
# launch_server LOG WANT CMD...: pick a port (WANT as for pick_port), start CMD with @PORT@ replaced, in the background, output to LOG.
# Sets SERVER_PID and SERVER_PORT. Exits 3 when no port can be picked. The caller waits for "^listening" itself.
launch_server()
{
	local log=$1 want=$2 p a cmd
	shift 2
	p=$(pick_port "$want") || exit 3
	cmd=(); for a in "$@"; do cmd+=("${a//@PORT@/$p}"); done
	"${cmd[@]}" > "$log" 2>&1 &
	SERVER_PID=$!; SERVER_PORT=$p
}
# bind_retry LOG WANT TRY: after a launch_server whose server did not listen: true (0) when the failure was a bind failure on a port we picked
# ourselves (WANT empty) and tries remain (TRY < 3); logs "port retry: ..." and reaps the dead server. The caller then launches again.
bind_retry()
{
	local log=$1 want=$2 try=$3
	[ -z "$want" ] && [ "$try" -lt 3 ] && grep -q "Address already in use" "$log" || return 1
	echo "port retry: 127.0.0.1:$SERVER_PORT taken between picking and binding (try $try of 3), picking another" >&2
	wait "$SERVER_PID" 2>/dev/null
	return 0
}
start_server_on_free_port()
{
	local log=$1 want=${START_PORT-${PORT:-}} try t0 tp sz n
	shift
	for try in 1 2 3
	do
		launch_server "$log" "$want" "$@"
		# A cold nav cache makes the start slow (a zone's ground took 102 s; kenshi-oi32): wait while the server is alive and still printing (any new log
		# output resets a 150 s stall timer), under an overall bound of 600 s.
		t0=$SECONDS tp=$SECONDS sz=0
		while :
		do
			grep -q "^listening" "$log" && break
			ps -p $SERVER_PID > /dev/null || break
			n=$(stat -c %s "$log" 2>/dev/null || echo 0)
			if [ "$n" != "$sz" ]; then sz=$n tp=$SECONDS; fi
			if [ $((SECONDS - tp)) -ge 150 ] || [ $((SECONDS - t0)) -ge 600 ]; then break; fi
			sleep 0.5
		done
		grep -q "^listening" "$log" && return 0
		bind_retry "$log" "$want" "$try" && continue
		break
	done
	server_listening "$log" "$SERVER_PID" "$SERVER_PORT"
}
if [ "${BASH_SOURCE[0]}" = "$0" ]; then pick_port "${1:-}"; fi
