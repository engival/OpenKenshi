#!/usr/bin/env bash
# First-run folder picker (kenshi-379.28.1): datadir::resolve with a fake zenity first in PATH, a temp HOME / XDG_CONFIG_HOME, and no game data.
# Builds a tiny driver from the real datadir.cpp + datapicker.cpp + tinyfiledialogs (no dev KENSHI_DATA default, so the miss path is reachable).
# Cases: no dialog (KCLIENT_NO_DIALOG, no display, KCLIENT_CONFIG=none), wrong folder then a good one (saved, second start silent), install
# root accepted, cancel, no zenity at all.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

c++ -std=c++20 -w -I"$HERE/src" -I"$HERE/third_party" -x c++ "$HERE/third_party/tinyfiledialogs/tinyfiledialogs.c" -x c++ "$HERE/src/app/datapicker.cpp" \
	"$HERE/src/app/datadir.cpp" "$HERE/tests/datapicker_main.cpp" -o "$T/drv" || { echo "FAIL build"; exit 1; }

mkdir -p "$T/good/data" "$T/bad" "$T/bin" "$T/home" "$T/cfg"
touch "$T/good/data/gamedata.base"
# A fake zenity: call n prints the nth line of $FAKE_ANSWERS; "CANCEL" exits 1. Every call is logged with its title.
cat > "$T/bin/zenity" <<'FAKE'
#!/bin/sh
# tinyfd probes the tool (--version); only --file-selection is a dialog.
case "$*" in *--file-selection*) ;; *) echo 3.44.0; exit 0 ;; esac
n=$(cat "$FAKE_DIR/n" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$FAKE_DIR/n"
echo "$*" >> "$FAKE_DIR/calls"
ans=$(sed -n "${n}p" "$FAKE_DIR/answers")
[ "$ans" = CANCEL ] && exit 1
[ -n "$ans" ] && echo "$ans"
exit 0
FAKE
chmod +x "$T/bin/zenity"

# Only the tools the driver and tinyfd need, without zenity/kdialog/python/dialog, so a "no tool" run cannot find a real one.
mkdir -p "$T/basebin"; for t in sh bash cat sed ls which uname grep tr; do p=$(command -v $t) && ln -sf "$p" "$T/basebin/$t"; done

# run NAME PATHVAR ANSWERS... ; sets $out (stdout), $err, $rc; resets the fake's state
run() {
	local name=$1 pathv=$2; shift 2
	rm -f "$T/n"; : > "$T/calls"; : > "$T/answers"; for a in "$@"; do echo "$a" >> "$T/answers"; done
	out=$(env -i HOME="$T/home" XDG_CONFIG_HOME="$T/cfg" PATH="$pathv" FAKE_DIR="$T" ${DISPLAY_VAR-DISPLAY=:99} $EXTRA_ENV "$T/drv" 2>"$T/err" </dev/null); rc=$?
	err=$(cat "$T/err"); calls=$(wc -l < "$T/calls")
}
FAKEPATH="$T/bin:$T/basebin"
NOTOOL="$T/basebin"

# 1. KCLIENT_NO_DIALOG=1: today's message, exit 1, the fake never called.
EXTRA_ENV="KCLIENT_NO_DIALOG=1" run nodialog "$FAKEPATH" "$T/good"
{ [ $rc -eq 1 ] && [ "$calls" -eq 0 ] && grep -q "cannot find the Kenshi game data" <<<"$err"; } && ok "KCLIENT_NO_DIALOG=1: message, exit 1, no dialog" || bad "KCLIENT_NO_DIALOG=1 (rc=$rc calls=$calls)"
# 2. No display.
DISPLAY_VAR="X=1" EXTRA_ENV="" run nodisplay "$FAKEPATH" "$T/good"
{ [ $rc -eq 1 ] && [ "$calls" -eq 0 ] && grep -q "cannot find the Kenshi game data" <<<"$err"; } && ok "no DISPLAY: message, exit 1, no dialog" || bad "no display (rc=$rc calls=$calls)"
# 3. KCLIENT_CONFIG=none.
EXTRA_ENV="KCLIENT_CONFIG=none" run confignone "$FAKEPATH" "$T/good"
{ [ $rc -eq 1 ] && [ "$calls" -eq 0 ]; } && ok "KCLIENT_CONFIG=none: no dialog" || bad "KCLIENT_CONFIG=none (rc=$rc calls=$calls)"
# 4. No zenity at all: NULL = cancel = today's message.
EXTRA_ENV="" run notool "$NOTOOL"
{ [ $rc -eq 1 ] && grep -q "cannot find the Kenshi game data" <<<"$err"; } && ok "no dialog tool: message, exit 1" || bad "no dialog tool (rc=$rc): $err"
# 5. Cancel.
EXTRA_ENV="" run cancel "$FAKEPATH" CANCEL
{ [ $rc -eq 1 ] && [ "$calls" -eq 1 ] && grep -q "cannot find the Kenshi game data" <<<"$err" && [ ! -e "$T/cfg/kenshi-client/data.conf" ]; } && ok "cancel: message, exit 1, nothing saved" || bad "cancel (rc=$rc calls=$calls)"
# 6. Wrong folder, then the install root (data/ inside): retry with the reason, saved, data folder reported.
EXTRA_ENV="" run retry "$FAKEPATH" "$T/bad" "$T/good"
second=$(sed -n 2p "$T/calls")
{ [ $rc -eq 0 ] && [ "$calls" -eq 2 ] && [ "$out" = "data=$T/good/data" ] && grep -q "No gamedata.base in $T/bad" <<<"$second"; } && ok "wrong then good: retried with the reason, picked $T/good/data" || bad "retry (rc=$rc calls=$calls out=$out second=$second)"
[ "$(cat "$T/cfg/kenshi-client/data.conf" 2>/dev/null)" = "data=$T/good/data" ] && ok "data=PATH saved in the temp config" || bad "config content: $(cat "$T/cfg/kenshi-client/data.conf" 2>&1)"
# 7. Second start: found by the saved setting, no dialog.
EXTRA_ENV="" run second "$FAKEPATH" CANCEL
{ [ $rc -eq 0 ] && [ "$calls" -eq 0 ] && [ "$out" = "data=$T/good/data" ]; } && ok "second start: saved setting, no dialog" || bad "second start (rc=$rc calls=$calls out=$out)"
# 8. The data folder itself is accepted too.
rm -rf "$T/cfg"; EXTRA_ENV="" run datafolder "$FAKEPATH" "$T/good/data"
{ [ $rc -eq 0 ] && [ "$out" = "data=$T/good/data" ]; } && ok "the data folder itself is accepted" || bad "data folder (rc=$rc out=$out)"
# 9. Optional: the read-only real game path, from KENSHI_REAL_DATA (the install folder; skipped when unset or not a directory).
G=${KENSHI_REAL_DATA:-}
if [ -z "$G" ]; then echo "skip: real game path (set KENSHI_REAL_DATA to the Kenshi install folder)"; fi
if [ -n "$G" ] && [ -d "$G" ]; then rm -rf "$T/cfg"; EXTRA_ENV="" run real "$FAKEPATH" "$T/bad" "$G"; { [ $rc -eq 0 ] && [ "$calls" -eq 2 ]; } && ok "real game path accepted ($out)" || bad "real game path (rc=$rc out=$out)"; fi

[ $fail -eq 0 ] && echo "datapicker_check: all passed" || echo "datapicker_check: FAILED"
exit $fail
