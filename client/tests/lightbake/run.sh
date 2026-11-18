#!/bin/bash
# lightbake (kenshi-jj3.1.1): run.sh OUT. Two headless runs of storm.txt (OUT/run1: bakes; OUT/run2: the cache), debug.light_probes on, no sim. Each prints the
# `light_bake:` lines; run1 must say "(baked)", run2 "(from the cache)". run1 starts without the cache (XDG_CACHE_HOME = OUT/cache). run3 is the same
# scene with the switch off: its screenshot must equal the same frame of a build without the change (scene_test covers it).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
base=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
real="${XDG_CACHE_HOME:-$HOME/.cache}/kclient"
export XDG_CACHE_HOME="$base/cache"
# a clean cache for the bake (run1 bakes, run2 loads), but the client's other caches (the colour map: minutes to rebuild) are linked in, so no frame stalls on them
mkdir -p "$XDG_CACHE_HOME/kclient"
for f in "$real"/*; do case "$f" in */lightbake.*) ;; *) ln -sf "$f" "$XDG_CACHE_HOME/kclient/" ;; esac; done
for run in run1 run2
do
	out="$base/$run"; mkdir -p "$out"
	sed "s#OUTDIR#$out#g" "$here/storm.txt" > "$out/script.txt"
	(cd "$tree" && timeout 280 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --debug --script "$out/script.txt" > "$out/client.log" 2>&1)
	echo "$run: rc $? [sg][error] $(grep -c '\[sg\]\[error' "$out/client.log")"
	grep -a '^light_bake' "$out/client.log" | uniq | cut -c1-400
	grep -a -A30 '^light_slice' "$out/client.log" | grep -a '^light_slice\|^[#0-9]*$' > "$out/slices.txt"
done
