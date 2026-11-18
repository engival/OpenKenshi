#!/usr/bin/env python3
# kenshi-k16.2.183: target changes per NPC from the `dump chars` runs of lock.txt (10 Hz): per NPC the target field of the AI tail ("ai goal .. target N")
# sampled each dump, the number of changes, the changes per second while it had a target, and the shortest time a target was kept before a re-pick (0 = none).
# Usage: analyze_lock.py client.log   (frame numbers: the "frame" lines are not in dump chars, so dumps are counted; 6 frames = 0.1 s at 60 fps)
import re, sys, collections
dumps = []; cur = None; cid = None
for l in open(sys.argv[1], errors="replace"):
    m = re.match(r'char (\d+) "([^"]*)"', l)
    if m:
        if cur is None or int(m.group(1)) in cur:
            if cur is not None: dumps.append(cur)
            cur = {}
        cid = int(m.group(1)); cur.setdefault(cid, [m.group(2), None]); continue
    m = re.match(r"\s+ai goal \S+ source \d+ step \S+ \d+/\d+ target (\d+)", l)
    if m and cid is not None and cur is not None: cur[cid][1] = int(m.group(1))
if cur: dumps.append(cur)
STEP = 0.1
print(len(dumps), "dumps", f"({len(dumps)*STEP:.0f} s at 10 Hz)")
names = {}
seq = collections.defaultdict(list)
for d in dumps:
    for i, (n, t) in d.items(): names[i] = n; seq[i].append(t)
for i, s in sorted(seq.items()):
    runs = []  # (target, samples)
    for t in s:
        if t is None: continue
        if runs and runs[-1][0] == t: runs[-1][1] += 1
        else: runs.append([t, 1])
    held = [r for r in runs if r[0] != 0]
    ch = max(len(runs) - 1, 0)
    tt = sum(r[1] for r in runs) * STEP
    short = min((r[1] * STEP for r in runs[1:-1]), default=0)
    print(f"{i} {names[i]!r}: {len(runs)} target runs, {ch} changes in {tt:.1f} s ({ch/tt if tt else 0:.2f}/s); shortest kept {short:.1f} s; targets {[r[0] for r in runs][:12]}")
