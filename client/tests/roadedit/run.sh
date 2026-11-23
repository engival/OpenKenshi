#!/bin/bash
# roadedit (kenshi-ix9r): run.sh OUT. A headless `--fly --debug --at Hub` client, the overlay on, `roads next`: the dumped suspect must be the Hub spike
# (road 1542 (424-Newwworld) marker 3, at -51032.7 1539.6 2782.9, the top of desktop-findings/roads_export/audit/road_suspects.tsv), and the second `roads next`
# then `roads prev` must come back to it. Shots in OUT. No server. Prints "roadedit: PASS" or exits 1.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
sed "s#OUTDIR#$out#g" "$here/roads.txt" > "$out/script.txt"
cd "$tree" && nice -n 10 timeout 300 client/tools/headless.sh -- stdbuf -oL "$tree/client/build/kclient" --fly --debug --at Hub --script "$out/script.txt" > "$out/client.log" 2>&1
rc=$?
grep -E "^(roads)" "$out/client.log"
echo "sg errors: $(grep -c '\[sg\]\[error\]' "$out/client.log")"
dumps=$(grep -E "^roads suspect" "$out/client.log")
first=$(echo "$dumps" | sed -n 1p)
fail=0
case "$first" in *"marker 3/"*"(-51032."*", 1539."*", 2782."*) ;; *) echo "roadedit: the first suspect is not the Hub spike: $first"; fail=1 ;; esac
[ "$(echo "$dumps" | grep -c .)" -ge 6 ] || { echo "roadedit: missing dump lines"; fail=1; }
# 6 lines: each next / prev prints once itself and once for the dump; the 1st and the 5th (after prev) describe the same marker
[ "$(echo "$dumps" | sed -n 1p)" = "$(echo "$dumps" | sed -n 5p)" ] || { echo "roadedit: next, next, prev did not come back to the first suspect"; fail=1; }
# step 1b (kenshi-tqtd): the list row, the marks, the scripted click, the kinds (item 8), the click on empty ground
all=$(grep -E "^roads" "$out/client.log")
echo "$all" | grep -qE "^roads suspect 1/.*road #1542 .*marker 3/.*kind reversal mark BAD" || { echo "roadedit: row 0 / mark bad: the Hub spike is not the marked top reversal"; fail=1; }
echo "$all" | grep -qE "^roads mark #1542 .* marker 3: BAD" || { echo "roadedit: the marks dump lacks the spike as BAD"; fail=1; }
echo "$all" | grep -qE "^roads marker: road #1542 .*marker 2/" || { echo "roadedit: the scripted click on 1542 marker 2 did not select it"; fail=1; }
echo "$all" | grep -qE "^roads mark #1542 .* marker 2: UNSURE" || { echo "roadedit: the marks dump lacks marker 2 as UNSURE"; fail=1; }
echo "$all" | grep -E "^roads suspect" | grep -E "road #1022 .*marker 3/" | grep -q "kind overlap" || { echo "roadedit: 1886-Newwworld #3 (index 1022) is not kind overlap"; fail=1; }
echo "$all" | grep -E "^roads suspect" | grep -E "road #1932 .*marker 5/" | grep -q "kind T-junction" || { echo "roadedit: 837-Newwworld #5 (index 1932) is not kind T-junction"; fail=1; }
[ "$(echo "$all" | grep -E "^roads: none selected" | wc -l)" -ge 1 ] || { echo "roadedit: a click on empty ground did not clear the selection"; fail=1; }
for f in roads_spike roads_second roads_panel roads_off; do [ -s "$out/$f.png" ] || [ -s "$out/$f.webp" ] || { echo "roadedit: no shot $f"; fail=1; }; done
[ "$rc" -eq 0 ] || { echo "roadedit: client rc $rc"; fail=1; }
[ "$fail" -eq 0 ] || { echo "roadedit: FAIL"; exit 1; }
echo "roadedit: PASS"
