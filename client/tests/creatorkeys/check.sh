#!/bin/bash
# Reads the `creator: shown ...` dump lines of a creatorkeys run (client.log): PASS when the typed names took (member 1 and the dog) and no game
# panel opened under the creator, typing with a box focused or with nothing focused (kenshi-k16.2.472.1). Usage: check.sh client.log
log=${1:?client.log}
mapfile -t L < <(grep '^creator: shown' "$log")
fail=0
bad() { echo "FAIL [dump $1]: $2"; fail=1; }
[ ${#L[@]} -eq 8 ] || { echo "FAIL: ${#L[@]} creator dump lines, want 8"; exit 1; }
none='panels: stats 0, inventory 0, options 0, build 0, help 0'
for i in 0 1 2 3 4 5 6 7; do
	[[ ${L[$i]} == *"shown 1,"* ]] || bad $i "creator not shown"
	[[ ${L[$i]} == *"$none"* ]] || bad $i "a game panel opened: ${L[$i]#*panels: }"
done
[[ ${L[0]} == *"member 1/2"* ]] || bad 0 "not member 1/2"
[[ ${L[1]} == *'name "Rex", box "Rex"'* ]] || bad 1 "member 1's typed name did not take: ${L[1]}"
[[ ${L[2]} == *'box "Rex'?*'", box focused 1'* ]] || bad 2 "focused box did not take C and I: ${L[2]}"
[[ ${L[3]} == *'box "Rex'?*'", box focused 0'* ]] || bad 3 "the box lost its text: ${L[3]}"
[[ ${L[4]} == *"member 2/2"* ]] || bad 4 "not member 2/2"
[[ ${L[5]} == *'name "Fido", box "Fido"'* ]] || bad 5 "the dog's typed name did not take: ${L[5]}"
[[ ${L[6]} == *'box "Fido'?*'", box focused 1'* ]] || bad 6 "the dog's focused box did not take C: ${L[6]}"
[[ ${L[7]} == *'name "Fido'?*'", box "Fido'?*'", box focused 0'* ]] || bad 7 "the dog's name was lost: ${L[7]}"
grep -q "^squads: .*Rexci.* Fidoc" "$log" || bad end "the spawned squad does not carry both typed names (the sim dropped the dog's?): $(grep '^squads:' "$log" | tail -1)"
[ $fail = 0 ] && echo "creatorkeys PASS" || echo "creatorkeys FAIL"
exit $fail
