#!/bin/bash
# Usage: run.sh SCRIPT OUT (SCRIPT: a .txt here without .txt). PORT (default 47838). Runs scenarios/order_sounds against a headless kclient (client/tests/containeropen/double_open.txt).
#
# Ids move with the build's world data (kenshi-k16.2.304.2): a script with the placeholders __WALKER__, __A__ (the shop counter) and __B__
# (a small barrel) has them resolved by a probe run first (`dump chars` / `dump things` into $OUT/probe.log), so paths.txt runs on any build.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
script="$here/${1:?script name}.txt"

run()	# run SCRIPTFILE LOG: one headless session against the order_sounds scenario
{
	cd "$tree" && HEADLESS_TIMEOUT=300 nice -n 10 timeout 300 client/tools/headless.sh -W 1280 -H 720 -- client/tools/demo.sh --port "${PORT:-47838}" \
		--scenario-file "$tree/scenarios/order_sounds.kscen" -- --play order_sounds --script "$1" > "$2" 2>&1
	echo "rc $?"
}

if grep -q '__WALKER__\|__A__\|__B__' "$script"
then
	printf '400 dump chars\n410 dump things\n500 quit\n' > "$out/probe.txt"
	run "$out/probe.txt" "$out/probe.log"
	walk=$(sed -n 's/^char \([0-9][0-9]*\) "Wanderer".*/\1/p' "$out/probe.log" | head -1)
	a=$(sed -n 's/^thing \([0-9][0-9]*\) .*"Shop Counter".*/\1/p' "$out/probe.log" | head -1)
	b=$(sed -n 's/^thing \([0-9][0-9]*\) .*"Small Barrel" at.*lock 0 .*/\1/p' "$out/probe.log" | head -1)
	if [ -z "$walk" ] || [ -z "$a" ] || [ -z "$b" ]
	then
		echo "run.sh: no ids in $out/probe.log (walker '$walk', counter '$a', barrel '$b')" >&2
		exit 1
	fi
	echo "ids: walker $walk, A (Shop Counter) $a, B (Small Barrel) $b"
else
	walk=; a=; b=
fi
sed -e "s#OUTDIR#$out#g" -e "s#__WALKER__#${walk}#g" -e "s#__A__#${a}#g" -e "s#__B__#${b}#g" "$script" > "$out/script.txt"
run "$out/script.txt" "$out/run.log"
grep -E "^(container|body|trade):|sim click|window" "$out/run.log" | cut -c1-200
