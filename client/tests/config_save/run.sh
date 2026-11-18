#!/bin/bash
# config_save (kenshi-k16.2.243.1): config.cpp's save() drops a line whose value equals the option's static compiled-in default
# (so a changed default reaches the user), keeps a comment, an unknown key, a window.<layout>.* line and any away value, and does not
# write a dropped key back. run.sh builds the check twice against src/config.cpp, the second time with a different t.int default, and
# runs the base build's file through it (an away value outlives a changed default). Then `kclient --print-defaults`: exit 0 with no
# window and no game data, the static default of a key printed although the config file sets another value, and the file untouched.
# Needs no game data and no display. Usage: run.sh OUT (CXX, BUILD as for the other checks).
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
cxx=${CXX:-g++}
build=${BUILD:-$tree/client/build}
srcdir=$tree/client/src
mkdir -p "$out/bin"
rc=0
for kind in base changed; do
	extra=(-DT_INT_DEFAULT=5)
	[ "$kind" = changed ] && extra=(-DT_INT_DEFAULT=6)
	"$cxx" -std=c++20 -O1 -Wall -Wextra -I "$srcdir" "${extra[@]}" "$here/config_save.cpp" "$srcdir/config.cpp" \
		-o "$out/bin/config_save-$kind" || { echo "run.sh: cannot build $out/bin/config_save-$kind" >&2; exit 2; }
done
export KCLIENT_CONFIG="$out/kclient.cfg"
timeout 60 "$out/bin/config_save-base" drop > "$out/drop.log" 2>&1 || rc=1
cat "$out/drop.log"
timeout 60 "$out/bin/config_save-changed" survive > "$out/survive.log" 2>&1 || rc=1
cat "$out/survive.log"
cp "$KCLIENT_CONFIG" "$out/kclient.cfg.final"
kclient=$build/kclient
if [ -x "$kclient" ]
then
	# A config file with an away value and an unknown key: neither may reach the listing, and nothing may be written.
	printf 'ui.scale=250\nkey.debug.walk=Shift+Z\n' > "$out/print-defaults.cfg"
	cp "$out/print-defaults.cfg" "$out/print-defaults.cfg.before"
	XDG_CONFIG_HOME="$out/xdg" XDG_DATA_HOME="$out/xdg" KCLIENT_CONFIG="$out/print-defaults.cfg" \
		timeout 120 "$kclient" --print-defaults > "$out/print-defaults.txt" 2> "$out/print-defaults.log"
	prc=$?
	opts=$(grep -cE '^[A-Za-z][A-Za-z0-9_.]*=' "$out/print-defaults.txt")
	pfail=0
	[ $prc -eq 0 ] || { echo "FAIL: --print-defaults exited $prc" >&2; pfail=1; }
	[ "$opts" -ge 100 ] || { echo "FAIL: --print-defaults printed $opts options, expected 100+" >&2; pfail=1; }
	grep -qx 'ui.scale=100' "$out/print-defaults.txt" || { echo "FAIL: ui.scale is not its static default 100 (the file sets 250)" >&2; pfail=1; }
	grep -qE '^window\.[^.]+\.(rect|monitor|fullscreen|maximized)=' "$out/print-defaults.txt" && { echo "FAIL: a detected window.<layout>.* line reached the listing" >&2; pfail=1; }
	grep -q '^ui.scale=250' "$out/print-defaults.txt" && { echo "FAIL: the config file's value reached the listing" >&2; pfail=1; }
	cmp -s "$out/print-defaults.cfg" "$out/print-defaults.cfg.before" || { echo "FAIL: --print-defaults wrote the config file" >&2; pfail=1; }
	if [ $pfail -eq 0 ]; then echo "print-defaults: PASS ($opts options, exit 0, file untouched)"; else rc=1; fi
	[ -s "$out/print-defaults.log" ] && { echo "-- stderr --"; cat "$out/print-defaults.log"; }
else
	echo "SKIP: --print-defaults needs the built client ($kclient); build target kclient to run it"
fi
[ $rc -eq 0 ] && echo "config_save: PASS" || echo "config_save: FAIL" >&2
exit $rc
