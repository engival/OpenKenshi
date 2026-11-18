#!/bin/bash
# Run a command inside a headless gamescope: GPU-accelerated GL/Vulkan, no
# window on the desktop. The child gets a nested X display as $DISPLAY.
# No synthetic input: this gamescope is built without libei, so XTEST (and
# xdotool) is unavailable inside. Drive the client with its own scripted input.
#
#   client/tools/headless.sh [-W 1280] [-H 720] -- build/kclient --screenshot /tmp/x.png
# HEADLESS_GPU=8086:e223 (vendor:device) pins gamescope and the child to that card (Vulkan device select,
# DRI_PRIME for GL), leaving the desktop's card alone. Unset: the Intel Arc test card (8086:e223) when this
# machine has it, so test runs stay off the user's playtest card (kenshi-k16.2.207); else the default device.
# HEADLESS_GPU=default forces the default device. `headless.sh --gpu` prints the card a run would use (empty = default).
# HEADLESS_TIMEOUT (seconds, default 600, 0 = no bound) bounds the run: on expiry gamescope and the client are killed
# together and headless.sh exits 124 with a "TIMEOUT" line, so a hung run ends the call instead of the job (kenshi-k16.2.219).
W=1280
H=720
GPU_ONLY=0
while [ $# -gt 0 ]
do
	case "$1" in
		-W) W=$2; shift 2 ;;
		-H) H=$2; shift 2 ;;
		--gpu) GPU_ONLY=1; shift ;;
		--) shift; break ;;
		*) break ;;
	esac
done
[ $# -gt 0 ] || [ $GPU_ONLY = 1 ] || { echo "usage: $0 [-W w] [-H h] -- command [args]" >&2; exit 2; }
TIMEOUT=${HEADLESS_TIMEOUT:-600}
# kui_demo runs until it exits: --screenshot takes its shot and quits, and a Q key does (event()), but nothing can press
# a key inside this gamescope (no synthetic input above), so a run without --screenshot only ends by the timeout.
if [ $GPU_ONLY = 0 ] && [ "$(basename -- "$1" 2>/dev/null)" = kui_demo ]
then
	SELF_EXIT=0
	for a in "$@"; do [ "$a" = --screenshot ] && SELF_EXIT=1; done
	[ $SELF_EXIT = 1 ] || printf '%s\n' "headless.sh: warning: kui_demo without --screenshot never exits on its own (nothing can press Q here); $([ "$TIMEOUT" = 0 ] && echo 'HEADLESS_TIMEOUT=0: it runs until killed' || echo "it is killed after ${TIMEOUT} s (HEADLESS_TIMEOUT)")" >&2
fi
# Headless runs are silent (docs/AUDIO_ENGINE.md): no kclient audio unless KAUDIO_WAV renders it to a file or KCLIENT_NOAUDIO=0 is set.
[ -n "$KAUDIO_WAV" ] || export KCLIENT_NOAUDIO=${KCLIENT_NOAUDIO-1}
# No first-run folder picker (src/datapicker.cpp): a headless run never shows a dialog.
export KCLIENT_NO_DIALOG=1
# Deterministic: options at their defaults, the user's kclient.cfg neither read nor written (config.hpp), unless KCLIENT_CONFIG names a file.
export KCLIENT_CONFIG=${KCLIENT_CONFIG-none}
# gamescope exits 0 whatever the child returns, so the child's status goes
# through a file. stderr is filtered synchronously (a >( ) filter can lose the
# last lines): drop gamescope's info/warn chatter and its known-harmless
# startup errors (PipeWire, libei, taken X socket, xkbcomp keysyms, shutdown
# EPIPE, the Intel card's high-priority queue fallback, the reaper's notice when a killed run takes its children
# down); keep other errors and everything the child prints. --line-buffered: grep into a pipe or file
# writes 4 KB blocks, which split a stderr line around the child's stdout lines in a shared log (k16.2.239.2 boxselect).
# stdbuf -oL: the child's stdout (and its children's: demo.sh's kclient) is line-buffered too, else a block-buffered stdout
# line is split by a stderr line (pick_drawn_check's 'sim click ... move_to' cut by a stall line, k16.2.205.12.49).
PREFER=()
if [ -z "${HEADLESS_GPU:-}" ]
then
	for d in /sys/bus/pci/devices/*
	do
		[ "$(cat "$d/vendor" "$d/device" 2>/dev/null)" = $'0x8086\n0xe223' ] && HEADLESS_GPU=8086:e223
	done
fi
[ "${HEADLESS_GPU:-}" != default ] || HEADLESS_GPU=
[ $GPU_ONLY = 0 ] || { echo "${HEADLESS_GPU:-}"; exit 0; }
if [ -n "${HEADLESS_GPU:-}" ]
then
	PREFER=(--prefer-vk-device "$HEADLESS_GPU")
	export MESA_VK_DEVICE_SELECT=$HEADLESS_GPU MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1 DRI_PRIME=$HEADLESS_GPU
fi
RC=$(mktemp /tmp/headless-rc.XXXXXX) || exit 1
{
	# HEADLESS_TIMEOUT: timeout puts gamescope in a process group of its own, so its TERM (and the KILL after
	# --kill-after, for a child that ignores it) reaches the client gamescope started too, not just gamescope.
	timeout --kill-after=10 "$TIMEOUT" gamescope "${PREFER[@]}" --backend headless -W "$W" -H "$H" -w "$W" -h "$H" -- \
		stdbuf -oL sh -c '"$@"; echo $? > "$0"' "$RC" "$@" 2>&1 1>&3 3>&- \
		| grep --line-buffered -v -E '\[gamescope\].*(Info|Warn)|gamescopereaper|pipewire|PipeWire|without libei|Failed to bind socket|xkbcomp|^> Warning: +Could not resolve keysym|failed to read Wayland events: Broken pipe|vkCreateDevice failed with a high-priority queue|^\s*$' >&2
	RUN=${PIPESTATUS[0]}	# gamescope's, or timeout's 124 / 137; grep's status says nothing
} 3>&1
STATUS=$(cat -- "$RC" 2>/dev/null)
rm -f -- "/tmp/headless-rc.${RC##*.}"
if [ "$RUN" = 124 ] || [ "$RUN" = 137 ]
then
	echo "headless.sh: TIMEOUT: killed the run after ${TIMEOUT} s (HEADLESS_TIMEOUT) - it did not exit on its own" >&2
	exit 124
fi
# Empty status: the child never finished (gamescope failed or was killed).
exit "${STATUS:-125}"
