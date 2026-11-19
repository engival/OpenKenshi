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
curl -fsSL "https://github.com/floooh/sokol-tools-bin/raw/${PIN}/bin/${SUB}/sokol-shdc${EXE}" -o "$DIR/sokol-shdc${EXE}"
chmod +x "$DIR/sokol-shdc${EXE}"
"$DIR/sokol-shdc${EXE}" --help | head -3
