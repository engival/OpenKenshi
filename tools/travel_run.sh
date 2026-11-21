#!/bin/bash
# travel_run.sh NAME PORT WALLSECS SPEED DX: a KSIM_PROFILE server on a new game "Guy with a dog" driven by travel_probe.py (one far move_to DX
# units east, the view following the leader), kenshi-k16.6.604. Writes NAME.{ticks.csv,server.log,probe.txt,meta} in the current directory.
# Run under the worker lock (heavy runs one at a time): nice -n 10 tools/travel_run.sh ...
N=$1; P=$2; S=$3; V=$4; DX=$5
T=$(cd "$(dirname "$0")/.." && pwd)
KSIM_PROFILE_TICKS=$PWD/$N.ticks.csv KSIM_PROFILE_JSON=30 $T/cpp/build-prof/OpenKenshi-server --listen 127.0.0.1:$P \
	--scenario newgame --start "${KSIM_START:-47924-rebirth.mod}" --navworld "${KSIM_NAVWORLD:-$HOME/git/kenshi/navworld}" ${KSIM_SERVER_ARGS} > $N.server.log 2>&1 &
SP=$!
sleep 25 # the world load
echo "load before: $(cat /proc/loadavg)" > $N.meta
C0=$(awk '{print $14+$15}' /proc/$SP/stat)
python3 -I $T/tools/travel_probe.py $P $S $V $DX > $N.probe.txt 2>&1
C1=$(awk '{print $14+$15}' /proc/$SP/stat)
echo "load after: $(cat /proc/loadavg)" >> $N.meta
echo "server cpu (1/100 s) during the run: $((C1-C0)), wall $S s" >> $N.meta
kill -TERM $SP
