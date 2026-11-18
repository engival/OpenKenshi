#!/usr/bin/env python3
# kenshi-k16.2.183: per member from the `log gait players` lines of a formation run: walk straightness (max / mean distance of the drawn position from the
# start -> end line, u), yaw vs velocity (dy = drawn yaw - heading of the wire velocity, deg, over frames with wire speed > 20 u/s: mean abs, p95, max),
# and the facing on arrival (drawn yaw 1 s before the stop vs at the end). Usage: analyze_formation.py client.log
import math, re, sys, collections
rows = collections.defaultdict(list)
for l in open(sys.argv[1], errors="replace"):
    m = re.match(r"gait \S+ \S+ id (\d+) tick (\d+) simyaw (\S+) interp (\S+) drawn (\S+) vhead (\S+) dy (\S+) wv (\S+).* at (\S+) (\S+) \|", l)
    if not m: continue
    i, tick, sy, ip, dr, vh, dy, wv, x, z = m.groups()
    rows[int(i)].append((int(tick), float(dr), float(vh), float(dy), float(wv), float(x), float(z), float(sy)))
for i, r in sorted(rows.items()):
    mv = [k for k in r if k[4] > 20 and not math.isnan(k[3])]
    if not mv: print(i, "never moved"); continue
    x0, z0 = r[0][5], r[0][6]; x1, z1 = r[-1][5], r[-1][6]
    first = next(k for k in r if k[4] > 20); last = [k for k in r if k[4] > 20][-1]
    ax, az, bx, bz = first[5], first[6], last[5], last[6]
    L = math.hypot(bx - ax, bz - az) or 1
    dev = [abs((bx - ax) * (az - k[6]) - (ax - k[5]) * (bz - az)) / L for k in mv]
    d = sorted(abs(k[3]) for k in mv)
    p95 = d[int(len(d) * .95)]
    arr = r[-1]
    pre = [k for k in r if k[0] <= last[0] and k[4] > 20][-1]
    print(f"id {i}: moved {L:.1f} u ({len(mv)} frames); straight dev max {max(dev):.2f} mean {sum(dev)/len(dev):.2f} u; yaw-vs-velocity |dy| mean {sum(d)/len(d):.1f} p95 {p95:.1f} max {d[-1]:.1f} deg; "
          f"heading of last walk frame {last[2]:.1f}, drawn then {last[1]:.1f}, drawn at end {arr[1]:.1f} (change {((arr[1]-last[1]+180)%360)-180:.1f} deg); start ({x0:.1f},{z0:.1f}) end ({x1:.1f},{z1:.1f})")
