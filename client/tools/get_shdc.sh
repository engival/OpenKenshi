#!/bin/bash
# Fetch the prebuilt sokol-shdc (Linux, macOS arm64 / x64, Windows x64 under MSYS2 / Git Bash) from floooh/sokol-tools-bin into client/bin/.
# Pinned commit: sokol-tools-bin build "(97)", 2428a421, which pairs with the vendored sokol_gfx.h
# 93aa187b (SG_MAX_SAMPLER_BINDSLOTS 14, skip-next-draw fix #1644).
set -e
PIN=2428a421
DIR="$(cd "$(dirname "$0")/.." && pwd)/bin"
mkdir -p "$DIR"
case "$(uname -s)-$(uname -m)" in
	Linux-x86_64) SUB=linux ;;
	Linux-aarch64) SUB=linux_arm64 ;;
	Darwin-arm64) SUB=osx_arm64 ;;
	Darwin-x86_64) SUB=osx ;;
	MINGW*-x86_64|MSYS*-x86_64|UCRT64*-x86_64|CYGWIN*-x86_64) SUB=win32; EXE=.exe ;;	# kenshi-379.31.1
	*) echo "get_shdc.sh: no prebuilt sokol-shdc for $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
URL="https://github.com/floooh/sokol-tools-bin/raw/${PIN}/bin/${SUB}/sokol-shdc${EXE}"
LINK="$DIR/sokol-shdc${EXE}"
# Shared per-host copy keyed by the pin (kenshi-3dwk): a pin bump is a new filename, the old copy stays. Hosts without
# $HOME/git/kenshi (CI, Windows, macOS) download straight into client/bin as before.
SHARED_DIR="$HOME/git/kenshi/bin"
if [ -n "$HOME" ] && [ -d "$HOME/git/kenshi" ]; then
	mkdir -p "$SHARED_DIR"
	SHARED="$SHARED_DIR/sokol-shdc-${PIN}${EXE}"
	if [ ! -x "$SHARED" ]; then
		TMP="$(mktemp "$SHARED_DIR/.sokol-shdc-${PIN}.XXXXXX")"
		trap 'rm -f "$TMP"' EXIT
		curl -fsSL "$URL" -o "$TMP"
		chmod +x "$TMP"
		mv -f "$TMP" "$SHARED"	# atomic: a concurrent configure never sees half a file
	fi
	rm -f "$LINK"
	case "$EXE" in
		.exe) cp "$SHARED" "$LINK" ;;
		*) ln -s "$SHARED" "$LINK" ;;
	esac
else
	curl -fsSL "$URL" -o "$LINK"
	chmod +x "$LINK"
fi
"$LINK" --help | head -3
