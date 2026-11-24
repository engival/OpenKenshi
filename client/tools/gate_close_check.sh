#!/bin/bash
# Heft gate close through a character in the gateway (kenshi-eolb, ksim k16.6.562 / kenshi-47pf). scenarios/gate_heft_occupant.kscen: the Wanderer
# and an Occupant (both player, United Cities, so either may Close the gate). The Occupant walks to the gate door's centre (the gateway) and
# stays; the Wanderer is ordered `interact <door> 2` (Close). The check logs the Occupant's and Wanderer's position and the door
# state every 30 frames for ~45 s and takes a frame sequence of the close (free camera over the gateway). The sim under test is KSIM_SERVER.
# usage: KSIM_SERVER=<server> gate_close_check.sh OUT_DIR [PORT]     (Wanderer 40842 / Occupant 40843 / door 1099511627900 are this save's ids)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); TREE=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: gate_close_check.sh OUT_DIR [PORT]}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
PORTARG=(); [ -n "${2:-}" ] && PORTARG=(--port "$2")
WAN=${WAN:-40842}; OCC=${OCC:-40843}; DOOR=${DOOR:-1099511627900};	# env overrides: master's sim numbers them 40844 / 40845 / 1099511627902
 GX=67187; GY=1279; GZ=-72193
{
	echo "300 select id=$OCC"; echo "301 move $GX $GY $GZ"
	# the free camera: CAM=x,z (default over the door), ALT, YAW (0 looks toward -z), PITCH
	if [ -n "${CAMOPS:-}" ]; then echo "500 $CAMOPS"	# CAMOPS="camera char id=N" etc.: e.g. follow the Occupant (then YAW / PITCH / DIST set on top)
		echo "501 set yaw ${YAW:-0}"; echo "502 set pitch ${PITCH:--35}"; echo "503 set dist ${DIST:-150}"
	else echo "500 set pos ${CAM:-$GX,$GZ}"; echo "501 set alt ${ALT:-160}"; echo "502 set yaw ${YAW:-0}"; echo "503 set pitch ${PITCH:--55}"; fi
	echo "700 dump chars"; echo "700 dump doors"; echo "701 screenshot $OUT/pre.png"
	echo "710 select id=$WAN"; echo "711 interact id=$DOOR 2"
	# WALKOFF=out|in: while the leaf is shut (frame 1200) the Occupant is ordered to a point 40 u outside / inside the gateway (n = -0.83,0.56)
	case "${WALKOFF:-}" in
	out) echo "1200 select id=$OCC"; echo "1201 move 67154 1262 -72171";;
	west) echo "1200 select id=$OCC"; echo "1201 move 67150 1280 -72193";;
	east) echo "1200 select id=$OCC"; echo "1201 move 67225 1275 -72193";;
	in) echo "1200 select id=$OCC"; echo "1201 move 67220 1279 -72215";;
	esac
	f=720
	while [ $f -le 2800 ]; do
		echo "$f dump chars"; echo "$f dump doors"
		[ $f -le 1500 ] && echo "$f screenshot $OUT/seq_$(printf %04d $f).png"
		f=$((f+30))
	done
	echo "2810 quit"
} > "$OUT/script.txt"
HEADLESS_TIMEOUT=900 nice -n 10 "$HERE/headless.sh" -W 960 -H 540 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} --scenario-file "$TREE/scenarios/gate_heft_occupant.kscen" -- --play gate_heft_occupant --script "$OUT/script.txt" > "$OUT/run.log" 2>&1 || true
grep -E "^char ($WAN|$OCC) |^door $DOOR " "$OUT/run.log" | sed 's/ race [^ ]* ([A-Za-z]*)//' > "$OUT/positions.txt" || true
wc -l "$OUT/positions.txt"
