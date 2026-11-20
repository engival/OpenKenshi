#!/bin/bash
# Trails + trade window check (kenshi-k16.19.6.1, the k16.19.6 second case): with the debug trails drawing, open the trade window at the Hub
# (scenarios/journey_sell.kscen), sell the Copper, buy it back, close, re-open and hover. Before the fix the trails shared the default sgl
# context, so once they passed ~65,536 vertices the trade window's grid lost its items and slots (the later commands of the frame dropped
# silently); after it the trails have their own context and every part of the window draws.
# usage: trails_trade_check.sh OUT_DIR [PORT] [KCLIENT]
#   OUT_DIR gets client.log (every `dump trade` and `dump trails` line), server.log, check.txt and trade_open/trade_sold/trade_bought/
#   trade_closed/trade_reopened .png. KCLIENT defaults to client/build/kclient; point it at an unfixed build to see the failure.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CLIENT=$(cd "$HERE/.." && pwd)	# the client tree (client/)
TREE=$(cd "$CLIENT/.." && pwd)	# the kenshi tree (its cpp/ builds the server)
OUT=${1:?usage: trails_trade_check.sh OUT_DIR [PORT] [KCLIENT]}
. "$(dirname "${BASH_SOURCE[0]}")/ports.sh"
WANT=${2:-}
KCLIENT=${3:-$CLIENT/build/kclient}
SCENARIO=$TREE/scenarios/journey_sell.kscen
. "$HERE/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$TREE" "$CLIENT/build/cpp/OpenKenshi-server"	# 'server: shared PATH' / 'server: built (reason)'
[ -x "$KCLIENT" ] || { echo "trails_trade_check: no $KCLIENT" >&2; exit 2; }
[ -x "$SERVER" ] || { echo "trails_trade_check: no OpenKenshi-server (build target ksim_server)" >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
export XDG_DATA_HOME=$OUT/data
SCRIPT=$OUT/check.txt
# The Hub barman is character 4679 of the playtest save (indoors at -51247 2662): `interact id=4679 6` talks to him, the Wanderer walks up and the
# trade opens. The Copper is the first cell of the player's main grid; a right-click on it moves it to the trader's stock (a sale), it lands at
# container cell 7,10, and a right-click there takes it back (a purchase). Escape closes the window; talking again re-opens it.
cat > "$SCRIPT" <<EOF
5 set animate 1
6 trails all
10 set pos -51187,2627
14 set alt 200
18 set yaw 20
22 set pitch -50
40 set simspeed 5
60 interact id=4679 6
400 dump trade
420 dump trails
440 screenshot $OUT/trade_open.png
1600 dump trails
1620 screenshot $OUT/trade_open_late.png
1700 mouse gear grid main 0 0
1702 mouse down right
1704 mouse up right
2000 dump trade
2020 dump trails
2040 screenshot $OUT/trade_sold.png
2200 mouse gear grid container 7 10
2202 mouse down right
2204 mouse up right
2600 dump trade
2620 screenshot $OUT/trade_bought.png
2700 key ESCAPE down
2704 key ESCAPE up
2800 screenshot $OUT/trade_closed.png
2900 interact id=4679 6
3400 dump trade
3420 dump trails
3440 mouse gear grid main 0 0
3460 mouse pos 640 360
3480 mouse pos 660 380
3520 screenshot $OUT/trade_reopened.png
3600 quit
EOF
START_PORT=$WANT start_server_on_free_port "$OUT/server.log" env -C "$TREE" "$SERVER" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen "127.0.0.1:@PORT@" --scenario-file "$SCENARIO" \
	--navworld "${KENSHI_NAVWORLD:?set KENSHI_NAVWORLD to a navworld bake dir}" --bake "$KENSHI_NAVWORLD"
pid=$SERVER_PID; PORT=$SERVER_PORT
trap 'kill $pid 2>/dev/null || true' EXIT
nice -n 10 "$HERE/headless.sh" -- "$KCLIENT" --debug --simlink "127.0.0.1:$PORT" --script "$SCRIPT" > "$OUT/client.log" 2>&1 || true
kill $pid 2>/dev/null || true
trap - EXIT
echo "trails_trade_check: [sg][error] lines: $(grep -c '\[sg\]\[error\]' "$OUT/client.log" || true)"
echo "trails_trade_check: sgl lines:"
grep '^sgl: ' "$OUT/client.log" || echo "  (none)"
echo "trails_trade_check: trade and trails lines:"
grep -E '^(trade|trails): ' "$OUT/client.log" || true
for f in trade_open trade_sold trade_bought trade_closed trade_reopened
do
	[ -f "$OUT/$f.png" ] || { echo "trails_trade_check: no $f.png (the run ended early: see client.log)" >&2; exit 4; }
done
if grep -q '^sgl: .* in the .* context' "$OUT/client.log"
then
	echo "trails_trade_check: FAIL: an sgl context filled" >&2
	exit 1
fi
echo "trails_trade_check: ok: no sgl context filled"
