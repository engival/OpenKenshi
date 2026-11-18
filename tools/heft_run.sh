#!/bin/bash
# heft_run.sh NAME PORT WALLSECS SPEED: a KSIM_PROFILE server at Heft (cage_prisoner) driven by heft_probe.py like the client;
# writes NAME.{ticks.csv,server.log,probe.txt,meta} in the current directory. docs/PROFILING.md "Heft under live load".
N=$1; P=$2; S=$3; V=$4
T=$(cd "$(dirname "$0")/.." && pwd)
KSIM_PROFILE_TICKS=$PWD/$N.ticks.csv KSIM_PROFILE_JSON=30 $T/cpp/build-prof/OpenKenshi-server --listen 127.0.0.1:$P \
	--scenario-file $T/scenarios/cage_prisoner.kscen --navworld "${KSIM_NAVWORLD:-$HOME/git/kenshi/navworld}" > $N.server.log 2>&1 &
SP=$!
sleep 25 # the world load
echo "load before: $(cat /proc/loadavg)" > $N.meta
C0=$(awk '{print $14+$15}' /proc/$SP/stat)
python3 -I $T/tools/heft_probe.py $P $S $V > $N.probe.txt 2>&1
C1=$(awk '{print $14+$15}' /proc/$SP/stat)
echo "load after: $(cat /proc/loadavg)" >> $N.meta
echo "server cpu (1/100 s) during the run: $((C1-C0)), wall $S s" >> $N.meta
kill -TERM $SP
