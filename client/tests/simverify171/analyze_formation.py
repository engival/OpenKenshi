#!/usr/bin/env python3
# kenshi-k16.2.183 / k16.2.407: per member from the `log gait players` lines of a formation run: walk straightness (max / mean distance of the drawn position from the
# start -> end line, u), and yaw vs heading on ONE time base each (deg, over frames with wire speed > 20 u/s: mean abs, p95, max):
#   drawn : the drawn yaw against the heading of the drawn segment (the drawn position's displacement over +-W frames). The drawn yaw is interpolated about
#           3 ticks behind the newest snapshot, so it must not be set against vhead (the newest tick's heading): that reads every turn as a lag (k16.6.133).
#   sim   : the sim yaw (simyaw, the newest tick) against the wire heading of that same tick (vhead).
# Each is split: steady (walk), start (first START_S s of walking), arrival (last ARRIVE_S s of walking); the facing on arrival (drawn yaw 1 s before the stop vs at the end).
# Usage: analyze_formation.py client.log
import math, re, sys, collections
W = 3          # frames each side for the drawn heading
START_S = 1.0  # seconds counted as the start-off
ARRIVE_S = 1.0 # seconds counted as the arrival
rows = collections.defaultdict(list)
for l in open(sys.argv[1], errors="replace"):
    m = re.match(r"gait (\S+) \S+ id (\d+) tick (\d+) simyaw (\S+) interp (\S+) drawn (\S+) vhead (\S+) dy (\S+) wv (\S+).* at (\S+) (\S+) \|", l)
    if not m: continue
    t, i, tick, sy, ip, dr, vh, dy, wv, x, z = m.groups()
    rows[int(i)].append(dict(t=float(t), tick=int(tick), sy=float(sy), dr=float(dr), vh=float(vh), wv=float(wv), x=float(x), z=float(z)))

def wrap(a): return (a + 180) % 360 - 180

def stats(d):
    if not d: return "n/a"
    d = sorted(d)
    return f"{len(d)}f mean {sum(d)/len(d):.1f} p95 {d[int(len(d) * .95)]:.1f} max {d[-1]:.1f}"

for i, r in sorted(rows.items()):
    mv = [k for k in r if k["wv"] > 20 and not math.isnan(k["vh"])]
    if not mv: print(i, "never moved"); continue
    # drawn heading: displacement of the drawn position over the frames around k
    for n, k in enumerate(r):
        a, b = r[max(0, n - W)], r[min(len(r) - 1, n + W)]
        dx, dz = b["x"] - a["x"], b["z"] - a["z"]
        k["dh"] = math.degrees(math.atan2(dx, dz)) if math.hypot(dx, dz) > 0.5 * (b["t"] - a["t"]) * 20 else float("nan")
    first, last = mv[0], mv[-1]
    ax, az, bx, bz = first["x"], first["z"], last["x"], last["z"]
    L = math.hypot(bx - ax, bz - az) or 1
    dev = [abs((bx - ax) * (az - k["z"]) - (ax - k["x"]) * (bz - az)) / L for k in mv]
    def split(key, ref):
        s, a, st = [], [], []
        for k in mv:
            if math.isnan(k[ref]): continue
            e = abs(wrap(k[key] - k[ref]))
            (s if k["t"] - first["t"] < START_S else a if last["t"] - k["t"] < ARRIVE_S else st).append(e)
        return s, st, a
    ds, dst, da = split("dr", "dh")
    ss, sst, sa = split("sy", "vh")
    pre = last
    end = r[-1]
    print(f"id {i}: moved {L:.1f} u ({len(mv)} frames); straight dev max {max(dev):.2f} mean {sum(dev)/len(dev):.2f} u")
    print(f"   drawn yaw vs drawn heading : steady {stats(dst)} | start {stats(ds)} | arrival {stats(da)}")
    print(f"   sim yaw vs wire heading    : steady {stats(sst)} | start {stats(ss)} | arrival {stats(sa)}")
    print(f"   arrival: heading of last walk frame {last['vh']:.1f}, drawn then {last['dr']:.1f}, drawn at end {end['dr']:.1f} (change {wrap(end['dr'] - last['dr']):.1f} deg); "
          f"start ({r[0]['x']:.1f},{r[0]['z']:.1f}) end ({end['x']:.1f},{end['z']:.1f})")
