#!/usr/bin/env python3
# kenshi-k16.2.183: target lock from the wire's EVENT 1 hits (`log hits on`): the wire carries an NPC's target only for the characters the client asked CHAR_STATS
# for (the player's), so the proxy is each attacker's victim sequence. Per attacker: hits, target changes (victim differs from the attacker's previous hit),
# the time from a hit on a victim to the first hit on a different one (the lock held at least that long, s; ticks at 20/s), the shortest such gap, how many
# gaps are under 1.0 s. Also the Wanderer's own AI-tail target per dump (lock.txt's dump chars). Usage: analyze_hits.py client.log
import re, sys, collections
T = 20.0
hits = collections.defaultdict(list)
for l in open(sys.argv[1], errors="replace"):
    m = re.match(r"hit tick (\d+) victim (\d+) attacker (\d+) result (\d+)", l)
    if m: hits[int(m.group(3))].append((int(m.group(1)), int(m.group(2)), int(m.group(4))))
tot = sum(len(v) for v in hits.values())
print(f"{tot} hits, {len(hits)} attackers")
allgaps = []
for a, h in sorted(hits.items()):
    h.sort()
    ch = []; first = h[0]
    for p, q in zip(h, h[1:]):
        if q[1] != p[1]: ch.append((q[0] - first[0]) / T); first = q   # time since the first hit on the previous victim
    span = (h[-1][0] - h[0][0]) / T
    allgaps += ch
    print(f"attacker {a}: {len(h)} hits over {span:.1f} s, victims {sorted(set(x[1] for x in h))}, target changes {len(ch)}"
          f" ({len(ch)/span if span else 0:.2f}/s); first-hit-to-re-pick on the previous victim, s: {[round(c,1) for c in ch][:10]}")
if allgaps: print(f"all re-picks {len(allgaps)}; shortest {min(allgaps):.1f} s; under 1.0 s: {sum(1 for g in allgaps if g < 1.0)}")
