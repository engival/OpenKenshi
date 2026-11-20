#!/bin/bash
# Venge beams' ground fire (kenshi-h2v.14.9.1; kenshi-rwh.175 2.6): in the original the Flame1 patch is the same burst's technique,
# emitted at the handler point for its 9 s and dead ~2 s later, so it rides the beam's foot (parented) and never outlives the beam's
# ~12 s burst (the strip's last particle). This runs the pinned midday scenario against the sim's BEAMS frame, frames one beam and
# shoots its whole life, and dumps `beamfeet` once a second over a minute: every placed beam's foot with its SkyBeam system's
# techniques (material, live count, centroid and farthest particle offset). The op itself fails a fire farther than 400 u from its
# beam's foot (its Flame1 Circle radius 140 u, a 2 s particle at the beam's 84 u/s drift, and slack) or one still burning 12 s after
# the spawn. Exit 1 on any FAIL line (or a run that saw no fire at all: a vacuous check).
# usage: beam_foot_check.sh OUT_DIR [SCENARIO] [PORT]
#   OUT_DIR gets server.log, client.log, check.txt (the generated client script), beam_00..beam_19.png/.txt (one beam framed and re-aimed
#   at its foot, 1 s apart over ~20 s of its life) and beam_sheet.png (their contact sheet, 5 x 4). SCENARIO defaults to
#   scenarios/venge_beams.kscen. PORT must be 47830-47839.
#   Exit: 0 every ground fire sat at its beam's foot inside its burst; 1 the check failed (see the FAIL lines); 2/3/4 setup, server or
#   shot failure.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CLIENT=$(cd "$HERE/.." && pwd)	# the client tree (client/)
TREE=$(cd "$CLIENT/.." && pwd)	# the kenshi tree (its cpp/ builds the server)
OUT=${1:?usage: beam_foot_check.sh OUT_DIR [SCENARIO] [PORT]}
SCENARIO=${2:-scenarios/venge_beams.kscen}
. "$HERE/ports.sh"
WANT=${3:-}
case "$SCENARIO" in /*) ;; *) SCENARIO=$TREE/$SCENARIO ;; esac
. "$HERE/server_bin.sh"	# pick_server: KSIM_SERVER, else the shared master build, else this build dir's (kenshi-k16.2.306.1)
pick_server "$TREE" "$CLIENT/build/cpp/OpenKenshi-server"
KCLIENT=$CLIENT/build/kclient
[ -x "$KCLIENT" ] || { echo "beam_foot_check: no $KCLIENT (build target kclient)" >&2; exit 2; }
[ -x "$SERVER" ] || { echo "beam_foot_check: no OpenKenshi-server (build target ksim_server)" >&2; exit 2; }
[ -f "$SCENARIO" ] || { echo "beam_foot_check: no scenario $SCENARIO" >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
export XDG_DATA_HOME=$OUT/data
SCRIPT=$OUT/check.txt
# The client script: a wind-up beam is framed at frame 120 (the BEAMS frame is in by then), then `camera beam same` re-aims at its
# moving foot before each shot, 20 frames (1 s) apart for 12 s (a wind-up pick has a TTL of at least 10 s and the client holds an ended
# beam another 12.5 s, so the shots never outlive the pick). `dump beamfeet` runs once a second from frame 60 to 3600 (~59 s).
{
	echo "2 set particles 0.0166667"	# a scripted run freezes the particle sim (dt 0) unless told: 1/60 s a frame keeps its clock on the sim's
	# frame 730 (~12 s in, when the load's first beams end and their slots' next ones are in their 3 s lead): `camera beam id=0` cannot
	# fail while any beam is held (a wind-up beam wins the pick, else the nearest live one), so the sheet starts at or near a strike.
	echo "730 camera beam id=0"
	for i in $(seq 0 19)
	do
		f=$((730 + 20 * i))
		echo "$f camera beam same"
		echo "$((f + 1)) screenshot $OUT/beam_$(printf %02d "$i").png"
	done
	for f in $(seq 600 60 3600)
	do
		echo "$f dump beamfeet"
	done
	echo "3660 quit"
} > "$SCRIPT"
START_PORT=$WANT start_server_on_free_port "$OUT/server.log" env -C "$TREE" "$SERVER" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen "127.0.0.1:@PORT@" --scenario-file "$SCENARIO" \
	${KENSHI_NAVWORLD:+--navworld "$KENSHI_NAVWORLD"}
pid=$SERVER_PID; PORT=$SERVER_PORT
trap 'kill $pid 2>/dev/null || true' EXIT
nice -n 10 "$HERE/headless.sh" -- "$KCLIENT" --debug --simlink "127.0.0.1:$PORT" --script "$SCRIPT" > "$OUT/client.log" 2>&1 || true
kill $pid 2>/dev/null || true
trap - EXIT
# the report: the op's own FAIL lines, then the counters over every dump ("beamfeet: N placed beams, M ground fires, K fails")
grep '^  FAIL' "$OUT/client.log" > "$OUT/fails.txt" || true
read -r dumps beams fires fails < <(awk '/^beamfeet: /{ dumps++; beams += $2; fires += $5; fails += $8 } END { print dumps + 0, beams + 0, fires + 0, fails + 0 }' "$OUT/client.log")
printf 'beamfeet summary: dumps %s, placed-beam rows %s, ground fires %s, fails %s\n' "$dumps" "$beams" "$fires" "$fails" | tee "$OUT/summary.txt"
# Info (not a fail): how far the strip's particles trail the foot in XZ. With keep_local the column rides the system, so the remaining lag is
# only what a particle drifts between two placements (a few u); before it, each particle froze where it was emitted.
awk '/^  tech "Beam" n [1-9]/ { l = sqrt($6 * $6 + $8 * $8); n++; s += l; if (l > mx) mx = l } END { if (n) printf "strip XZ lag from the foot: %d samples, mean %.0f u, max %.0f u\n", n, s / n, mx }' "$OUT/client.log" | tee -a "$OUT/summary.txt"
echo "beam_foot_check: [sg][error] lines: $(grep -c '\[sg\]\[error\]' "$OUT/client.log" || true)"
grep '^kclient: script: no beam\|^kclient: script: ' "$OUT/client.log" | head -3 || true
for i in $(seq 0 19)
do
	f=$(printf "beam_%02d" "$i")
	[ -f "$OUT/$f.png" ] || { echo "beam_foot_check: no $f.png (the run ended early: see client.log)" >&2; exit 4; }
done
montage -label '%f' "$OUT"/beam_*.png -tile 5x4 -geometry 320x180+2+2 "$OUT/beam_sheet.png"
if [ -s "$OUT/fails.txt" ]
then
	echo "beam_foot_check: FAIL: $(wc -l < "$OUT/fails.txt") foot-fire lines ($OUT/fails.txt):" >&2
	head -10 "$OUT/fails.txt" >&2
	exit 1
fi
if [ "$beams" -eq 0 ] || [ "$fires" -eq 0 ]
then
	echo "beam_foot_check: FAIL: the run saw no placed beam or no ground fire (beams $beams, fires $fires): the check proved nothing" >&2
	exit 1
fi
echo "beam_foot_check: ok: every ground fire sat within 400 u of its beam's foot and inside the 12 s burst"
