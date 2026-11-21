#!/bin/bash
# Headless follow-mode run against the fake agent (kenshi-cym.40.2.5): tests/followstream/fake_agent.py plays RECORDS to a kclient started with
# --follow-stream / --import-save / --import-report, under tools/headless.sh, bounded by timeout 300.
#
# usage: client/tests/followstream/run.sh SAVEDIR RECORDS OUTDIR [PORT]
#   SAVEDIR  an original quick-save folder (the fixture: ksim_save_import_test --write-fixture DIR)
#   RECORDS  the agent's record file (tests/followstream/records_fixture.txt)
#   OUTDIR   logs: client.log, agent.log, report.json; env KCLIENT_FOLLOW_LOG=1 gives one follow-cam line per frame
# env: RESTART=records2 plays that second record file on a new agent after the first agent is killed (the reconnect check; KILL_AT seconds in, default 30)
# The server child is the shared master build when client/tools/shared_server.sh has one (KSIM_SERVER overrides), else client/build/cpp/OpenKenshi-server.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
SAVE=$1 REC=$2 OUT=$3 PORT=${4:-47830}
mkdir -p "$OUT"
. "$ROOT/client/tools/server_bin.sh"
pick_server "$ROOT" "$ROOT/client/build/cpp/OpenKenshi-server" > "$OUT/server.txt"; cat "$OUT/server.txt"
SRVSET=(--set "session.server=$SERVER")
[ "$SHARED" = 1 ] && SRVSET+=(--set "session.server_args=--scenario newgame --sim-data $ROOT/cpp/data")
export KCLIENT_FOLLOW_LOG=${KCLIENT_FOLLOW_LOG:-1}
rm -f "$OUT/report.json" "$OUT/agent.log" "$OUT/client.log"
python3 "$ROOT/client/tests/followstream/fake_agent.py" --port "$PORT" --records "$REC" --log "$OUT/agent.log" --report "$OUT/report.json" --linger 8 &
AGENT=$!
if [ -n "${RESTART:-}" ]
then
	( sleep "${KILL_AT:-30}"; kill "$AGENT" 2>/dev/null; echo "agent killed" >> "$OUT/agent.log"; sleep 4
	  python3 "$ROOT/client/tests/followstream/fake_agent.py" --port "$PORT" --records "$RESTART" --log "$OUT/agent.log" --report "$OUT/report.json" --linger 8 ) &
	WATCH=$!
fi
HEADLESS_TIMEOUT=${HEADLESS_TIMEOUT:-300} nice -n 10 "$ROOT/client/tools/headless.sh" -- "$ROOT/client/build/kclient" --debug \
	--follow-stream "127.0.0.1:$PORT" --import-save "$SAVE" --import-report "$OUT/report.json" "${SRVSET[@]}" "${@:5}" > "$OUT/client.log" 2>&1
echo "kclient exit $?" | tee -a "$OUT/client.log"
kill "$AGENT" ${WATCH:-} 2>/dev/null
exit 0
