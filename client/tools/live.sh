#!/bin/bash
# One live run: a private OpenKenshi-server on a free port, the headless client with a script, then the server stopped.
# usage: [KCLIENT_ARGS="..."] live.sh SERVER_TREE OUT_DIR CLIENT_SCRIPT [client timeout s] -- [server args...]
#   SERVER_TREE: a kenshi tree with cpp/build/OpenKenshi-server (the shared tree or a private worktree)
#   OUT_DIR gets server.log, client.log, saves/; --listen and --save-dir are added here.
# The server binary (kenshi-k16.2.306.1): KSIM_SERVER=<path>, else SERVER_TREE's shared master build when its branch changes
# nothing the server is built from (client/tools/shared_server.sh), else SERVER_TREE/cpp/build/OpenKenshi-server. It prints as
# 'server: shared PATH' / 'server: built (reason)'.
# Fails (exit 3) when the server does not reach "listening" (bind errors: another server holds the port).
root=$(cd "$(dirname "$0")/../.." && pwd)	# the kenshi tree this script is in (its client build runs)
. "$root/client/tools/server_bin.sh"	# pick_server (kenshi-k16.2.306.1)
tree=$1 out=$2 script=$3 secs=235
shift 3
if [ "$1" != "--" ]; then secs=$1; shift; fi
shift
pick_server "$tree" "$tree/cpp/build/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
mkdir -p "$out"
port=""
for p in $(seq 47960 48060)
do
	ss -ltn | grep -q "127.0.0.1:$p " || { port=$p; break; }
done
[ -n "$port" ] || { echo "live.sh: no free port" >&2; exit 3; }
# The server is started by exec from a subshell whose own & makes $! the server itself
# ('cd X && (exec ...) &' backgrounds a wrapper shell and $! is the wrapper).
(cd "$tree" && exec "$SERVER" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --save-dir "$out/saves" "$@") > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 180)
do
	grep -q "^listening" "$out/server.log" && break
	grep -q "bind\|error" "$out/server.log" && break
	ps -p $pid > /dev/null || break
	timeout 1 tail -f /dev/null
done
stop()
{
	ps -o args= -p $pid 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill $pid
}
if ! grep -q "^listening" "$out/server.log"
then
	echo "live.sh: server did not listen on $port:" >&2; tail -3 "$out/server.log" >&2; stop; exit 3
fi
echo "live.sh: server pid $pid port $port"
cd "$root" && HEADLESS_TIMEOUT=$secs client/tools/headless.sh -- $PWD/client/build/kclient --debug --simlink 127.0.0.1:$port --script "$script" $KCLIENT_ARGS > "$out/client.log" 2>&1	# --debug: the scripts' dump / log / scene ops; KCLIENT_ARGS: extra client flags (word-split; e.g. --playtests COPY, never the repo's)
rc=$?	# 124: the client hit its timeout (headless.sh kills gamescope and the client together; an outer timeout would miss them, kenshi-k16.2.219)
stop
echo "live.sh: client rc $rc; [sg][error] lines: $(grep -c '\[sg\]\[error\]' "$out/client.log")"
exit $rc
