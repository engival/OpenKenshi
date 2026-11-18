# Sourced by the test runners (client/tests/*/run.sh) and the check tools (client/tools/*.sh): a free TCP port on 127.0.0.1, so two sessions
# (desktop and ds share one machine) never run their servers on the same fixed default (kenshi-k16.2.307).
#   port=$(pick_port "${PORT:-}") || exit 3   PORT set: kept if nothing listens there, else "port busy" (stderr) and failure.
#                                             PORT unset: a free port from KENSHI_PORT_RANGE (default 48100-48499), random start.
#   server_listening LOG PID PORT             after the start loop: stops the script (exit 3) with a "did not listen" line when the server
#                                             log lacks "^listening": the bind failed (port taken in the meantime) or the server died.
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
if [ "${BASH_SOURCE[0]}" = "$0" ]; then pick_port "${1:-}"; fi
