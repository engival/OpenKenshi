#!/bin/bash
# Combat idle check (kenshi-k16.2.162.1, rwh101_idle_pick_static.md section 3): the combat picker is the first maximum over the idle list in the original's order (the sid hash walk),
# which must reproduce the two picks logged earlier: a sabre holder draws guard5 (over guard4 main), a blunt weapon holder guard 1h. Scene characters (--character), `dump idle`.
# Usage: client/tests/sneakidle/combat.sh OUT. Exit 0 = pass, 1 = fail (loud).
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
printf '200 dump idle\n215 quit\n' > "$out/script.txt"
cd "$tree" && timeout 300 client/tools/headless.sh -- "$tree/client/build/kclient" --debug --character "move=0;combat;weapon=Desert Sabre;drawn;at=0,0" --character "move=0;combat;weapon=Iron Club;drawn;at=5,0" \
	--character "move=0;combat;weapon=Katana;drawn;at=10,0" --script "$out/script.txt" > "$out/client.log" 2>&1
mapfile -t L < <(grep '^idle ' "$out/client.log")
fail=0
[[ "${L[0]:-}" == *"category 1"*": guard5" ]] || { echo "FAIL sabre: ${L[0]:-no idle line}"; fail=1; }
[[ "${L[1]:-}" == *"category 2"*": guard 1h" ]] || { echo "FAIL blunt: ${L[1]:-no idle line}"; fail=1; }
[ $fail = 0 ] && echo "PASS: sabre -> guard5, blunt -> guard 1h (${L[2]:-?})"
exit $fail
