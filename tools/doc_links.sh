#!/bin/bash
# Link check: every docs/... or findings/... .md path named in the docs must exist.
# Run from anywhere; prints "BROKEN <path>" per missing target, nothing when clean.
cd "$(dirname "$0")/.." || exit 1
grep -rhoE '(docs|findings)/[A-Za-z0-9_./-]+\.md' CLAUDE.md client/CLAUDE.md docs ~/git/kenshi/kenshi-inspect/CLAUDE.md ~/git/kenshi/kenshi-inspect/docs | sort -u | while read p; do [ -e "$p" ] || [ -e "$HOME/git/kenshi/kenshi-inspect/$p" ] || echo "BROKEN $p"; done
