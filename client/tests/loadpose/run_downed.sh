#!/bin/bash
# loadpose downed (kenshi-k16.2.144): a OpenKenshi-server with downed.kscen and a client script that attacks the Boss, gets the player put down, then
# quickloads the F5 save taken before the fight (the same player id; SAVE=file: that save as `quicksave` instead). Usage: run_downed.sh OUT KEYFRAME: KEYFRAME is the script frame of the Ctrl+F9 (the player must be
# down by then; read it from a first run's dump chars). Port: PORT (47831). SCRIPT=downed_save.txt: the F5 is taken while the player is down (acceptance 2, KEYFRAME 4300).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../charids.sh"
. "$(dirname "${BASH_SOURCE[0]}")/../runcheck.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
key=${2:?keyframe}
port=${PORT:-47831}
build=${BUILD:-$tree/client/build}
. "$tree/client/tools/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$tree" "$build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
server=$SERVER
export XDG_DATA_HOME="$out/xdg"
mkdir -p "$XDG_DATA_HOME/ksim/saves"
[ -n "${SAVE:-}" ] && cp "$SAVE" "$XDG_DATA_HOME/ksim/saves/quicksave"
{ cat "$here/${SCRIPT:-downed.txt}"
  echo "$key key CTRL down"; echo "$((key+2)) key F9 down"; echo "$((key+4)) key F9 up"; echo "$((key+6)) key CTRL up"
  # the camera follows the player after the load (the load puts it back where the save had it), and the flags / pose are dumped with each shot
  for d in 4 20 40 80 200 400; do echo "$((key+d+6)) camera char Wanderer"; echo "$((key+d+6)) dump chars"; echo "$((key+d+6)) screenshot $out/after_$d.png"; done
  echo "$((key+500)) quit"; } > "$out/script.tmpl"
"$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:$port --scenario-file "$here/downed.kscen" --save-dir "$XDG_DATA_HOME/ksim/saves" > "$out/server.log" 2>&1 &
pid=$!
for i in $(seq 240); do grep -q "^listening" "$out/server.log" && break; ps -p $pid > /dev/null || break; sleep 0.5; done
grep -q "^listening" "$out/server.log" || { echo "run_downed.sh: server did not listen" >&2; kill "$pid" 2>/dev/null; exit 3; }
if ! probe_char_ids "$build" "$tree" "$port" "$out/ids.txt"; then echo "run_downed.sh: id probe found no characters (see $out/idprobe.log)" >&2; kill "$pid" 2>/dev/null; exit 4; fi
charids_fill "$out/script.tmpl" "$out/ids.txt" "$out" "$out/script.txt" || { kill "$pid" 2>/dev/null; exit 4; }
cd "$tree" && timeout 400 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
ps -o args= -p "$pid" 2>/dev/null | grep -q -- "--listen 127.0.0.1:$port" && kill "$pid"
echo "client rc $rc; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client.log")"
run_status "$rc" "$out/script.txt"
