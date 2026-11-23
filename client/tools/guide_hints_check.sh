#!/bin/bash
# kenshi-86qh (control hints, DESIGN 5): the guide coverage check. `hints kinds` (a script op) prints every registered HINT_KIND as `kind <id> "<phrase>"`;
# each phrase must appear (case-insensitive, whitespace-folded) in docs/PLAYER_GUIDE.md section 5 ("Orders, selection and the camera (the mouse)").
# A kind whose phrase is missing fails the check, so a new hover action cannot land without its guide line.
# Usage: guide_hints_check.sh [OUT]   (OUT: output dir, default a temp dir). BUILD (default client/build of this tree).
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../.." && pwd)
build=${BUILD:-$tree/client/build}
out=$(mkdir -p "${1:-${TMPDIR:-/tmp}/guide_hints_check.$$}" && cd "${1:-${TMPDIR:-/tmp}/guide_hints_check.$$}" && pwd)
printf '20 hints kinds\n40 quit\n' > "$out/kinds.txt"
(cd "$tree" && timeout 120 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --debug --script "$out/kinds.txt" > "$out/kinds.log" 2>&1)
grep '^kind ' "$out/kinds.log" > "$out/kinds.list"
if ! grep -q '^hints kinds: ' "$out/kinds.log" || [ ! -s "$out/kinds.list" ]; then echo "guide_hints_check: no kinds printed (see $out/kinds.log)" >&2; exit 2; fi
# section 5 of the guide: from its "## 5." heading to the next "## "
awk '/^## 5\./ { on = 1; next } /^## / { on = 0 } on' "$tree/docs/PLAYER_GUIDE.md" | tr '\n' ' ' | tr -s ' ' | tr 'A-Z' 'a-z' > "$out/section5.txt"
[ -s "$out/section5.txt" ] || { echo "guide_hints_check: PLAYER_GUIDE.md has no section 5" >&2; exit 2; }
bad=0
while IFS= read -r line
do
	id=${line#kind }; id=${id%% *}
	phrase=${line#*\"}; phrase=${phrase%\"}
	want=$(printf '%s' "$phrase" | tr '\n' ' ' | tr -s ' ' | tr 'A-Z' 'a-z')
	if grep -qF -- "$want" "$out/section5.txt"; then echo "ok   $id"; else echo "MISSING $id: \"$phrase\" is not in docs/PLAYER_GUIDE.md section 5"; bad=$((bad + 1)); fi
done < "$out/kinds.list"
[ "$bad" -eq 0 ] && { echo "guide_hints_check: PASS ($(wc -l < "$out/kinds.list") kinds)"; exit 0; }
echo "guide_hints_check: FAIL ($bad of $(wc -l < "$out/kinds.list") kinds missing)"; exit 1
