"""Summarise a KSIM_PROFILE_TICKS per-tick CSV: heft_ticks.py FILE.csv BUDGET_US (docs/PROFILING.md "Heft under live load")."""
import csv, sys

name, budget = sys.argv[1], float(sys.argv[2])
rows = list(csv.DictReader(open(name)))
# drop the startup ticks before the probe connected (paused sim yields none) and the first 100 ticks (cold caches)
rows = rows[100:]
tick = [float(r["tick_us"]) for r in rows]
n = len(tick)
s = sorted(tick)
q = lambda f: s[int(f * (n - 1))]
print(f"{name}: {n} ticks ({rows[0]['tick']}..{rows[-1]['tick']}); mean {sum(tick)/n:.0f} p50 {q(.5):.0f} p90 {q(.9):.0f} p99 {q(.99):.0f} p99.9 {q(.999):.0f} max {s[-1]:.0f} us")
over = sum(1 for t in tick if t > budget)
print(f"budget {budget:.0f} us: {over} ticks over ({100*over/n:.2f}%); sim cost sum {sum(tick)/1e6:.1f} s for {n*0.05:.0f} s sim")
secs = ["setup","systems","weather","wild","ai_strategic","ai_squad","tokens","combat","interact","producers","tasks","residents","crime","goals","economy","nav","tail"]
subs = [k for k in rows[0] if k.startswith("sub_")]
tot = sum(tick)
print("section        mean   p99    max   share  (us)")
for k in secs + subs:
	v = sorted(float(r[k]) for r in rows)
	m = sum(v) / n
	print(f"  {k:16s}{m:8.1f}{v[int(.99*(n-1))]:8.0f}{v[-1]:9.0f}{100*m*n/tot:6.1f}%")
for k in ("characters", "squads", "path_queries", "path_us"):
	v = [float(r[k]) for r in rows]
	print(f"  {k}: mean {sum(v)/n:.1f} max {max(v):.0f}")
# the 1 Hz pass: ticks where tick % 20 == 0, vs the rest
on = [float(r["tick_us"]) for r in rows if int(r["tick"]) % 20 == 0]
off = [float(r["tick_us"]) for r in rows if int(r["tick"]) % 20 != 0]
print(f"tick%20==0: n {len(on)} mean {sum(on)/len(on):.0f} p99 {sorted(on)[int(.99*(len(on)-1))]:.0f}; others: mean {sum(off)/len(off):.0f} p99 {sorted(off)[int(.99*(len(off)-1))]:.0f}")
print("over-budget ticks by tick%20 phase 0 / other:", sum(1 for r in rows if float(r["tick_us"]) > budget and int(r["tick"]) % 20 == 0), "/", sum(1 for r in rows if float(r["tick_us"]) > budget and int(r["tick"]) % 20 != 0))
print("worst 10 ticks (tick, tick%20, tick_us, top sections / subs):")
for r in sorted(rows, key=lambda r: -float(r["tick_us"]))[:10]:
	parts = sorted(((float(r[k]), k) for k in secs + subs), reverse=True)[:5]
	print(f"  {r['tick']} %20={int(r['tick'])%20:2d} %400={int(r['tick'])%400:3d} {float(r['tick_us']):9.0f}  " + " ".join(f"{k}={v:.0f}" for v, k in parts))
