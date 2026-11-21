#!/bin/bash
# A container's pick box sits on the mesh the renderer draws (kenshi-k16.2.260.1, k16.2.260; user 2026-11-15: "I clicked on the lamp ... it opened
# up a 'water tank'"): every drawn container in the Heft Storm House and in the Hub bar must have its pick volume on its drawn mesh.
# `dump pickboxes R` (dev) reports, per container, the instance the pick tests (WorldBuildings::instanceRay's frame: the uploaded mesh bounds + the
# part's offset, scaled, turned, at the instance origin) against the decoded mesh's triangles through the transform the instance shader uses
# (mesh.glsl inst_vs). Per part it prints the two world boxes and `frame` (how far a drawn vertex lies outside the pick box in the mesh frame), and a
# `box summary` line: the owner building, floor and shown floor, part count, the worst gap / frame and `inside` (every drawn vertex, and the drawn
# box's centre, inside the pick box).
# Pass: every summary line of the watched owner has parts >= 1, frame <= TOL and inside 1, and the case measured at least MIN of them.
# The Hub's bar is a Storm House record (the bar_lure run's 14 drawn containers are all owner "Storm House"), so both cases watch that owner.
# usage: container_pick_check.sh OUT_DIR [PORT]   (PORT one of this worker's 47830-47839; the runs are sequential: two clients at once time the
#        server handshake out). TOL (mesh units, default 0.05) and MIN (default 1) are env.
# The dump decodes every part's mesh in one frame (dev only): ~9 s of stall for the 146 containers the Heft case prints.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: container_pick_check.sh OUT_DIR [PORT]}
PORTARG=(); [ -n "${2:-}" ] && PORTARG=(--port "$2")	# no PORT: demo.sh picks a free one and retries on a bind failure (kenshi-k16.2.417.3)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
OWNER=${OWNER:-Storm House}
TOL=${TOL:-0.05}
MIN=${MIN:-1}
fail=0

run_case()	# NAME SCENARIO CAMERA_ARGS...
{
	local name=$1 scen=$2 d
	shift 2
	d="$OUT/$name"
	mkdir -p "$d"
	cat > "$d/script.txt" <<EOS
600 dump pickboxes 1000
620 quit
EOS
	HEADLESS_TIMEOUT=420 nice -n 10 "$HERE/headless.sh" -W 1280 -H 720 -- "$HERE/demo.sh" ${PORTARG[@]+"${PORTARG[@]}"} --scenario-file "$TREE/scenarios/$scen.kscen" \
		-- --play "$scen" --script "$d/script.txt" "$@" > "$d/run.log" 2>&1 || true
	grep -E '^(pickboxes|container|box) ' "$d/run.log" > "$d/boxes.txt" || true
	echo "--- $name: $(grep -c '^box summary' "$d/boxes.txt" || true) drawn containers within the dump's radius"
}

watch()	# CASE_DIR NAME
{
	local d=$1 name=$2 l n=0 bad=""
	[ -f "$d/boxes.txt" ] || { echo "container_pick_check: FAIL: $name: no dump (run.log: $d/run.log)"; fail=1; return; }
	while IFS= read -r l
	do
		case "$l" in "box summary "*) ;; *) continue ;; esac
		printf '%s\n' "$l" | grep -q "owner \"$OWNER\"" || continue
		local parts frame inside
		parts=$(printf '%s\n' "$l" | sed -n 's/.* parts \([0-9]*\).*/\1/p')
		frame=$(printf '%s\n' "$l" | sed -n 's/.* frame \([0-9.]*\).*/\1/p')
		inside=$(printf '%s\n' "$l" | sed -n 's/.* inside \([0-9]*\).*/\1/p')
		n=$((n + 1))
		awk -v f="${frame:-0}" -v t="$TOL" 'BEGIN { exit !(f <= t) }' || bad="$bad  $l"$'\n'
		[ "${inside:-0}" = 1 ] && [ "${parts:-0}" -ge 1 ] || bad="$bad  $l"$'\n'
	done < "$d/boxes.txt"
	if [ "$n" -lt "$MIN" ]
	then
		echo "container_pick_check: FAIL: $name: $n drawn containers of \"$OWNER\" (want >= $MIN: the run did not reach the building?)"
		fail=1
	elif [ -n "$bad" ]
	then
		printf 'container_pick_check: FAIL: %s: the pick box is off the drawn mesh:\n%s' "$name" "$bad"
		fail=1
	else
		echo "container_pick_check: ok   $name: $n drawn containers of \"$OWNER\", pick box on the drawn mesh (frame <= $TOL)"
	fi
}

# The reported shot's camera over the Heft Storm House (kclient-20261115-001410's recreate line); the Hub bar from bar_lure's start.
run_case heft cage_prisoner --camera-pos 68228.8 1385.5 -70842.7 --camera-yaw -33.2 --camera-pitch -51.0 --camera-dist 172.0
watch "$OUT/heft" "the Heft Storm House"
run_case hub bar_lure
watch "$OUT/hub" "the Hub bar"
[ $fail = 0 ] && echo "container_pick_check: PASS" || { echo "container_pick_check: FAIL (logs under $OUT)"; exit 1; }
