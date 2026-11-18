# Sourced by the test runners (client/tests/*/run.sh) and the check tools (client/tools/*.sh): a free TCP port on 127.0.0.1, so two sessions
# (desktop and ds share one machine) never run their servers on the same fixed default (kenshi-k16.2.307).
#   port=$(pick_port "${PORT:-}") || exit 3   PORT set: kept if nothing listens there, else "port busy" (stderr) and failure.
#                                             PORT unset: a free port from KENSHI_PORT_RANGE (default 48100-48499), random start.
#   server_listening LOG PID PORT             after the start loop: stops the script (exit 3) with a "did not listen" line when the server
#                                             log lacks "^listening": the bind failed (port taken in the meantime) or the server died.
#   start_server_on_free_port LOG CMD...      start a server and wait until it listens; the literal word @PORT@ in CMD is replaced by the port. Sets
#                                             SERVER_PID and SERVER_PORT. Use it for every server start of a runner that starts several (a port
#                                             picked once at the top can be taken by another session between two cases, kenshi-k16.2.358):
#                                             each start picks a fresh port (PORT set: that port only, busy = exit 3, as before; START_PORT overrides PORT per start), and a bind
#                                             failure ("Address already in use") picks another port and starts again, at most 3 tries, each
#                                             logged on stderr ("port retry: ..."). Any other failure to listen stops the script (exit 3).
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
	read -r lo hi <<< "${KENSHI_PORT_RANGE:-48100 48499}"
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
start_server_on_free_port()
{
	local log=$1 want=${START_PORT:-${PORT:-}} try p i a cmd
	shift
	for try in 1 2 3
	do
		p=$(pick_port "$want") || exit 3
		cmd=(); for a in "$@"; do cmd+=("${a//@PORT@/$p}"); done
		"${cmd[@]}" > "$log" 2>&1 &
		SERVER_PID=$!; SERVER_PORT=$p
		for i in $(seq 240); do grep -q "^listening" "$log" && break; ps -p $SERVER_PID > /dev/null || break; sleep 0.5; done
		grep -q "^listening" "$log" && return 0
		if [ -z "$want" ] && [ "$try" -lt 3 ] && grep -q "Address already in use" "$log"
		then
			echo "port retry: 127.0.0.1:$p taken between picking and binding (try $try of 3), picking another" >&2
			wait "$SERVER_PID" 2>/dev/null
			continue
		fi
		break
	done
	server_listening "$log" "$SERVER_PID" "$SERVER_PORT"
}
if [ "${BASH_SOURCE[0]}" = "$0" ]; then pick_port "${1:-}"; fi
