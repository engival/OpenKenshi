#!/bin/bash
# Fetch the prebuilt Linux sokol-shdc from floooh/sokol-tools-bin into client/bin/.
# Pinned commit: sokol-tools-bin build "(97)", 2428a421, which pairs with the vendored sokol_gfx.h
# 93aa187b (SG_MAX_SAMPLER_BINDSLOTS 14, skip-next-draw fix #1644).
set -e
PIN=2428a421
DIR="$(cd "$(dirname "$0")/.." && pwd)/bin"
mkdir -p "$DIR"
curl -fsSL "https://github.com/floooh/sokol-tools-bin/raw/${PIN}/bin/linux/sokol-shdc" -o "$DIR/sokol-shdc"
chmod +x "$DIR/sokol-shdc"
"$DIR/sokol-shdc" --help | head -3
