#!/bin/bash
# stealcaught: a witnessed take out of a foreign bar container, live, and the original's "Got caught stealing." message bar line
# (kenshi-k16.2.300.6.1). Two headless runs, each through client/tools/live.sh (its own OpenKenshi-server on the bar_lure scenario):
#   probe: dump chars / dump things -> the Wanderer's id and the Large Basket nearest it (they move with the build's world data;
#          41091 was the basket on kenshi-k16.2.300.4)
#   run:   steal.txt with those ids: the camera on the basket, the selection walked there, ALT held, right click on the basket,
#          transfer take 0; a shot every 0.5 s for 4 s after the take (the message lasts that long, ui::message)
# Pass: client.log holds the theft ("theft event: ...") and the caught_stealing line ("caught stealing: thief / witness"),
# and at least one shot of the take burst was written. Exit 0 pass, 1 fail (the log says why), 3 the tooling did.
# usage: run.sh OUT_DIR [CLIENT_SECONDS]	(PORT/... stay live.sh's: it picks a free port)
#   BASKET="id=N|NAME" overrides the probe's basket; WITNESS_CHECK=0 skips nothing (the events are the check)
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?usage: run.sh OUT_DIR [CLIENT_SECONDS]}" && cd "$1" && pwd)
secs=${2:-400}
live="$tree/client/tools/live.sh"
scen="$tree/scenarios/bar_lure.kscen"
fail() { echo "stealcaught: FAIL: $*"; exit 1; }

# 1. the probe: the ids (three dumps: the first world frames can run before the first snapshot, charids.sh's note)
mkdir -p "$out/probe"
printf '60 dump chars\n90 dump things\n120 dump chars\n150 quit\n' > "$out/probe/probe.txt"
nice -n 10 "$live" "$tree" "$out/probe" "$out/probe/probe.txt" 200 -- --scenario-file "$scen" || { echo "stealcaught: probe run failed ($out/probe)"; exit 3; }
wanderer=$(sed -n 's/^char \([0-9][0-9]*\) "Wanderer".*/\1/p' "$out/probe/client.log" | head -1)
[ -n "$wanderer" ] || fail "no Wanderer in $out/probe/client.log (the bar_lure load did not come up?)"
read -r wx wz < <(sed -n "s/^char $wanderer \"Wanderer\".* at \(-\{0,1\}[0-9.]*\) [-\0-9.]* \(-\{0,1\}[0-9.]*\) .*/\1 \2/p" "$out/probe/client.log" | head -1)
echo "stealcaught: Wanderer $wanderer at $wx,$wz"
# the Large Basket nearest the Wanderer (BASKET="id=N" / BASKET="NAME" overrides the name)
want=${BASKET:-}
name="Large Basket"
case "$want" in id=*) basket=${want#id=} ;; "") ;; *) name=$want ;; esac
if [ -z "${basket:-}" ]
then
	awk -v wx="${wx:-0}" -v wz="${wz:-0}" -v want="$name" '
		/^thing [0-9]+ kind [0-9]+ record [0-9]+ "/ {
			name = $0; sub(/^thing [0-9]+ kind [0-9]+ record [0-9]+ "/, "", name); sub(/".*/, "", name)
			if (name != want) { next }
			id = $2; at = index($0, " at "); if (!at) { next }
			rest = substr($0, at + 4); split(rest, p, " ")
			dx = p[1] - wx; dz = p[3] - wz
			d = dx * dx + dz * dz
			if (best == "" || d < bd) { best = id; bd = d }
		}
		END { print best }' "$out/probe/client.log" > "$out/probe/basket.txt"
	basket=$(cat "$out/probe/basket.txt")
fi
[ -n "${basket:-}" ] || fail "no \"$name\" in $out/probe/client.log (dump things)"
echo "stealcaught: basket $basket ($name nearest the Wanderer)"

# 2/3. the run and its check. One run, no retry. The subject is event 51 and its message line: the sim's witness check
# (recordCaughtStealing) fires on every witnessed take, so 51 is required on every run. Event 14 is NOT: a caught steal roll
# aborts the take and sends only 51 (cpp/src/ksim/law/crime.cpp, stealRollCaught); only a landed take sends 14. So 14 is
# checked for consistency with the items: "theft event:" logged <=> the basket's total fell and the Wanderer's rose by the same
# count; not logged <=> both unchanged (dump inv before and after the take, steal.txt).
# The message box line is in the shots: ui::message lasts 4 s, the burst covers 4 s at 0.5 s.
d="$out/run"
mkdir -p "$d"
sed -e "s|OUTDIR|$d|g" -e "s|@Wanderer@|$wanderer|g" -e "s|@Basket@|$basket|g" "$here/steal.txt" > "$d/script.txt"
nice -n 10 "$live" "$tree" "$d" "$d/script.txt" "$secs" -- --scenario-file "$scen" || { echo "stealcaught: live.sh failed, rc $? ($d)"; exit 3; }
log="$d/client.log"
theft=$(grep -m1 '^theft event:' "$log")
caught=$(grep -m1 '^caught stealing:' "$log")
shot=$(ls "$d"/*_take.png 2>/dev/null | head -1)
total() { grep "^inv $1: " "$log" | sed -n "$2p" | sed -n 's/^inv [0-9]*: held total \([0-9]*\):.*/\1/p'; }
cb=$(total "$basket" 1); ca=$(total "$basket" 2); wb=$(total "$wanderer" 1); wa=$(total "$wanderer" 2)
printf '%s\n' "${theft:-theft event: (none)}" "${caught:-caught stealing: (none)}" "${shot:-shot: (none)}" "basket $basket total ${cb:-?} -> ${ca:-?}; Wanderer $wanderer total ${wb:-?} -> ${wa:-?}"
[ -n "$caught" ] || fail "no caught_stealing (51): refusal lines: $(grep -c 'REJECTED\|not sent\|script: ' "$log") ($log)"
[ -n "$shot" ] || fail "no shot of the take burst ($d)"
[ -n "$cb" ] && [ -n "$ca" ] && [ -n "$wb" ] && [ -n "$wa" ] || fail "an inventory dump is missing ($log)"
if [ -n "$theft" ]
then
	[ "$ca" -lt "$cb" ] && [ $((cb - ca)) -eq $((wa - wb)) ] || fail "event 14 logged but the items did not move basket $cb -> $ca, Wanderer $wb -> $wa"
	echo "stealcaught: PASS: caught stealing 51 and a landed theft 14 (the items moved), shots under $d"
else
	[ "$ca" -eq "$cb" ] && [ "$wa" -eq "$wb" ] || fail "no event 14 but the items moved basket $cb -> $ca, Wanderer $wb -> $wa"
	echo "stealcaught: PASS: caught stealing 51, the steal roll failed (no 14, the items stayed), shots under $d"
fi
exit 0
