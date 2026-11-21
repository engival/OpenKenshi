#!/bin/bash
# Scene regression test: renders every scene of client/scenes.txt headless, compares with the baseline.
#   client/tools/scene_test.sh [--bless] [--keep-all] [--scenes NAME,NAME] [OUTDIR]
# Only changed / new / FAIL scenes keep an image in OUTDIR; a passing scene is one "unchanged: NAME = <baseline file>" line (summary.txt):
# --keep-all keeps every image. Images are lossless .webp (Linux; screenshot.format=png for .png); baselines may be either.
# KCLIENT=<binary> renders with another kclient (the Debug build: client/build-debug/kclient); never --bless from it.
# KCLIENT_SCENE_ARGS="--set render.thread=lockstep" adds options to the client's command line (lockstep_check.sh).
# KCLIENT_SCENE_REFS=<dir> holds the reference screenshots named by the ref=NAME fields of scenes.txt; unset = scenes run without a reference.
# Baselines live outside git: ~/.cache/kclient/scene-baseline/ (NAME.png or NAME.webp + NAME.json), one directory per card: a run
# pinned to a card (headless.sh --gpu, e.g. the Intel test card 8086:e223) uses scene-baseline-<vendor:device>, since
# drivers differ by a few counts over the whole frame (kenshi-k16.2.207). Exit 0 = rendered
# cleanly (no sg errors, nothing left loading); pixel changes are reported, not failed.
# Renders the display base: no post.* effect is on by default (k16.2.82), so the baselines match the original.
HERE=$(cd "$(dirname "$0")" && pwd)
GPU=$("$HERE/headless.sh" --gpu)
BASE="${KCLIENT_SCENE_BASELINE:-$HOME/.cache/kclient/scene-baseline${GPU:+-$GPU}}"
BLESS=0
KEEP=0
SCENES=
OUT=
while [ $# -gt 0 ]
do
	case "$1" in
		--bless) BLESS=1; shift ;;
		--keep-all) KEEP=1; shift ;;
		--scenes) SCENES=$2; shift 2 ;;
		*) OUT=$1; shift ;;
	esac
done
[ -n "$OUT" ] || OUT=$(mktemp -d /tmp/scene-test.XXXXXX) || exit 1
mkdir -p -- "$OUT" "$BASE" || exit 1
ARGS=(--scene-test "$OUT" --debug)	# the scenes are a development feature (client --debug)
[ -z "$SCENES" ] || ARGS+=(--scenes "$SCENES")
# shellcheck disable=SC2206
[ -z "${KCLIENT_SCENE_ARGS:-}" ] || ARGS+=($KCLIENT_SCENE_ARGS)
nice -n 10 "$HERE/headless.sh" -- "${KCLIENT:-$HERE/../build/kclient}" "${ARGS[@]}" 2>&1 | tee "$OUT/run.log" | grep -E '^scene |\[sg\]|kclient:' >&2
RC=${PIPESTATUS[0]}
python3 -I "$HERE/scene_compare.py" "$OUT" "$BASE" "$BLESS" "$HERE/../scenes.txt" "$KEEP" || RC=1
echo "output: $OUT  (summary: $OUT/summary.txt; overview: $(ls "$OUT"/overview.* 2>/dev/null | head -1))"
exit $RC
