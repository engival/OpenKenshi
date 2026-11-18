#!/bin/bash
# weatherfx (kenshi-k16.2.11.3): the weather EFFECT classes the item adds or fixes, in the offline viewer (no sim needed), headless.
#   ash       Ashlands (100000,100000, over the open ash), weather Kenshi_Ash-Flakes: type 1 camera flakes.
#   mist      Fog Islands (Mongrel), weather "fog islands": the type 4 global volume at the pivot, terrain + 100.
#   mistoff   the same frame with --no-particles (the A/B half of the mist).
#   dust      The Desert (60000,-85000), weather "Dust Storm Approach": the type 4 Sand-Stream at the pivot; its world instances
#             (types 3) need game time, so 30 s of frames.
#   twister   The same weather with two `debug weff` pins ahead of the pivot: Twister-Chuff_Desert01 and DesertCloudStorm (dev aid, README).
#   detritus  weather "Desert-dust-Light Detritus01": the type 10 GlobalPoint band refill (Local-swirl-devil).
#   lightning weather "Heavy_Rain deadlands": the type 7 bolts (with no player character in the viewer they keep the random cell
#             point, so the dump is the evidence; see README).
#   off       ash again with --no-particles: nothing of the above is drawn.
# Usage: run.sh MODE OUT. KCLIENT=<binary> renders with another build. XDG_DATA_HOME is OUT/xdg (events, no user files touched).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
mode=${1:?ash|mist|mistoff|dust|twister|detritus|lightning|off}
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
build=${BUILD:-$tree/client/build}
export XDG_DATA_HOME="$out/xdg"
mkdir -p "$XDG_DATA_HOME"
case "$mode" in
	ash) set -- "Kenshi_Ash-Flakes" 100000 100000 300 0 -18 "" 10 ;;
	mist) set -- "fog islands" -82559 -37630 80 200 10 "" 7 ;;
	mistoff) set -- "fog islands" -82559 -37630 80 200 10 "--no-particles" 7 ;;
	dust) set -- "Dust Storm Approach" 60000 -85000 120 -20 -12 "" 12 ;;
	twister) set -- "Dust Storm Approach" 60000 -85000 120 -20 -12 "" 12 ;;
	detritus) set -- "Desert-dust-Light Detritus01" 60000 -85000 100 -20 -8 "" 12 ;;
	lightning) set -- "Heavy_Rain deadlands" 6000 -10343 100 0 -8 "" 18 ;;
	off) set -- "Kenshi_Ash-Flakes" 100000 100000 300 0 -18 "--no-particles" 10 ;;
	*) echo "run.sh: unknown mode $mode" >&2; exit 2 ;;
esac
weather=$1 x=$2 z=$3 alt=$4 yaw=$5 pitch=$6 extra=$7 hour=$8
script="$here/$mode.txt"
[ -f "$script" ] || { echo "run.sh: no script $script" >&2; exit 2; }
sed "s#OUTDIR#$out#g" "$script" > "$out/script-$mode.txt"
cd "$tree" || exit 2
# shellcheck disable=SC2086
timeout 400 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --debug ${extra:+$extra} --pos "$x,$z" --alt "$alt" --yaw "$yaw" --pitch "$pitch" \
	--weather "$weather" --time "$hour" --script "$out/script-$mode.txt" > "$out/client-$mode.log" 2>&1
rc=$?
echo "client rc $rc; [sg][error]: $(grep -c '\[sg\]\[error' "$out/client-$mode.log"); shots: $(ls "$out"/*.png 2>/dev/null | wc -l)"
exit $rc
