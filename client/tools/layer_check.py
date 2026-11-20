#!/usr/bin/env python3
"""The layering check (kenshi-k16.2.369.15.1; docs/CLIENT.md "Code map").

usage: layer_check.py SRC_DIR LAYERS_FILE
LAYERS_FILE (written by client/CMakeLists.txt at configure) has one "dir:library:dep,dep" row per library. A cross-directory
#include "dir/file" in SRC_DIR/<dir>/ must name a directory whose library <dir>'s library declares in its DEPS, so an upward or
sideways include fails the build here, and the library link order (static libraries, declared deps) fails the symbols the headers
don't show. Exit 1 with one line per offending include."""
import os, re, sys

src, layers = sys.argv[1], sys.argv[2]
lib_dir, rows = {}, []
for line in open(layers):
    line = line.strip()
    if not line: continue
    d, lib, deps = line.split(':')
    lib_dir[lib] = d
    rows.append((d, [x for x in deps.split(',') if x]))
allowed = {}
for d, deps in rows:
    allowed[d] = {lib_dir[x] for x in deps if x in lib_dir}
inc = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
bad = 0
for d in sorted(allowed):
    for dp, _, fs in os.walk(os.path.join(src, d)):
        for f in sorted(fs):
            if not f.endswith(('.cpp', '.hpp', '.h', '.mm', '.inl')): continue
            p = os.path.join(dp, f)
            for i, l in enumerate(open(p, errors='ignore'), 1):
                m = inc.match(l)
                if not m: continue
                t = m.group(1).split('/')[0]
                if t == d or t not in allowed: continue
                if t not in allowed[d]:
                    print(f"layer_check: {os.path.relpath(p, src)}:{i}: {d}/ includes {m.group(1)}, but {d}'s library does not declare {t}/ as a dependency", file=sys.stderr)
                    bad += 1
if bad:
    print(f"layer_check: {bad} include(s) break the layering (client/CMakeLists.txt kc_layer DEPS; docs/CLIENT.md \"Code map\")", file=sys.stderr)
    sys.exit(1)
