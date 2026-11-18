#!/bin/bash
# options_groups (kenshi-k16.2.244.1): every declared option has a section (a known tab) and a group. `kclient --check-options` lists the
# options that have none and exits 1; run.sh also checks that --print-defaults is written under "# Section / Group" headers and that no key
# line comes before the first header. Needs the built client, no game data, no display. Usage: run.sh OUT (BUILD as for the other checks).
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
kclient=${BUILD:-$tree/client/build}/kclient
[ -x "$kclient" ] || { echo "options_groups: needs the built client ($kclient)" >&2; exit 2; }
export KCLIENT_CONFIG=none XDG_CONFIG_HOME="$out/xdg" XDG_DATA_HOME="$out/xdg"
rc=0
timeout 60 "$kclient" --check-options > "$out/check.log" 2>&1 || rc=1
cat "$out/check.log"
timeout 60 "$kclient" --print-defaults > "$out/print-defaults.txt" 2>/dev/null || rc=1
first=$(grep -nE '^# [A-Z][A-Za-z-]* / ' "$out/print-defaults.txt" | head -1 | cut -d: -f1)
firstkey=$(grep -nE '^[a-z][a-z0-9_.]*=' "$out/print-defaults.txt" | head -1 | cut -d: -f1)
if [ -z "$first" ] || [ "$first" -gt "$firstkey" ]; then echo "FAIL: --print-defaults has a key before the first '# Section / Group' header" >&2; rc=1; fi
[ $rc -eq 0 ] && echo "options_groups: PASS" || echo "options_groups: FAIL" >&2
exit $rc
