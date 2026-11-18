#!/bin/bash
# kenshi-k16.2.300.5.1: five cage interacts, no toast. Usage: run.sh OUT. Uses the cagemenu runner (cage_alone.kscen); prints the reply and toast counts (0 toasts expected).
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
out=${1:?out dir}
cp "$here/interact5.txt" "$here/../cagemenu/_interact5.txt"
"$here/../cagemenu/run.sh" _interact5 "$out"; rc=$?
rm -f "$here/../cagemenu/_interact5.txt"
log="$out/client.log"
echo "orders sent: 5; sim reply: interact lines: $(grep -c '^sim reply: interact' "$log"); toast lines: $(grep -c '^toast:' "$log")"
grep -m3 '^sim reply' "$log"
exit $rc
