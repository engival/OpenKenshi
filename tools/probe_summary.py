#!/usr/bin/env python3
"""Summarise the scheduling probes (kenshi-k16.6.305, docs/PROFILING.md "Scheduling measurements").
Usage: [TICK_MIN=n] [TICK_MAX=n] probe_summary.py NAME.ticks.csv [NAME.routes.csv] [BUDGET_US]
Prints the cs_update think / act split, the island columns, and the per-route stage statistics and re-query rates."""
import csv, os, sys
from collections import defaultdict


def q(v, f):
	return v[int(f * (len(v) - 1))] if v else 0.0


def stat(name, v):
	v = sorted(v)
	print(f"  {name:22s} mean {sum(v)/len(v):9.2f} p50 {q(v,.5):9.2f} p99 {q(v,.99):9.2f} max {v[-1]:9.2f}")


BRANCHES = ["dead", "down", "walk", "follow", "drive", "idle", "fight", "other"]
SUBS = ["carry_prep", "bar_depots", "squad_step", "delivery_log", "world_squads", "stragglers", "token_fights", "stored_medical", "corpses", "drop_gone", "intruders", "crime_goals", "slave_trade", "slave_state", "corpse_pot", "world_tasks",
	"torture", "gate_crews", "player_areas", "residents", "routines", "rearm", "shopping", "stealth_ko", "wt_prelude", "wt_orders", "wt_in_hand", "wt_fight_goals", "combat_pre", "combat_step", "combat_mirror", "cs_index", "cs_sense", "cs_env", "cs_separate", "cs_update", "cs_tail", "(after the last mark)"]
TICKS = [0, 0]
LO, HI = int(os.environ.get("TICK_MIN", 0)), int(os.environ.get("TICK_MAX", 1 << 60))  # a tick window, e.g. the walking phase of the travel run


def ticks(path):
	rows = [r for r in csv.DictReader(open(path))][100:]
	rows = [r for r in rows if LO <= int(r["tick"]) <= HI]
	n = len(rows)
	TICKS[:] = [int(rows[0]["tick"]), int(rows[-1]["tick"])]
	col = lambda k: [float(r[k]) for r in rows]
	print(f"{path}: {n} ticks, {rows[0]['tick']}..{rows[-1]['tick']}")
	stat("tick_us", col("tick_us"))
	stat("simlink_us (encode)", col("simlink"))
	stat("combat_us", col("combat"))
	stat("sub_combat_step", col("sub_combat_step"))
	stat("sub_cs_sense", col("sub_cs_sense"))
	stat("sub_cs_update", col("sub_cs_update"))
	print("cs_update split (per tick, us):")
	for k in ("upd_us", "think_us", "think_route_us", "scan_us", "upd_route_us"):
		stat(k, col(k))
	upd, think, tr, ur = col("upd_us"), col("think_us"), col("think_route_us"), col("upd_route_us")
	act = [u - t - (r - x) for u, t, r, x in zip(upd, think, ur, tr)]  # update minus pick minus the route time outside the pick
	stat("act_us (upd-think-other routes)", act)
	stat("think_us excl. routes", [t - x for t, x in zip(think, tr)])
	stat("route_in_update_us", ur)
	for k in ("think_calls", "scan_calls", "upd_members"):
		stat(k, col(k))
	st = col("steps")
	stat("combat steps per tick", st)
	print("islands, per combat step (sight range 1500 u, usable characters; uf_us is per tick):")
	for k in ("isl_active", "isl_n", "isl_max", "isl_2nd", "isl_single", "isl_top_share", "tgt_n", "tgt_max", "tgt_nonsingle"):
		stat(k, [v / s for v, s in zip(col(k), st) if s > 0])
	stat("isl_uf_us (per tick)", col("isl_uf_us"))
	if "b_dead_n" in rows[0]:
		arms = lambda r: sum(float(r[f"b_{b}_{d}"]) for b in BRANCHES for d in "nmf")
		print("arena member updates by branch x band (kenshi-k16.6.631; per update loop, mean over ticks; n 0-2000 u, m 2000-3250 u, f beyond):")
		tot = sum(arms(r) for r in rows) or 1.0
		for b in BRANCHES:
			v = [sum(float(r[f"b_{b}_{d}"]) for d in "nmf") for r in rows]
			bands = [sum(float(r[f"b_{b}_{d}"]) for r in rows) / n for d in "nmf"]
			print(f"  {b:7s} mean {sum(v)/n:8.2f} ({100*sum(v)/tot:5.1f}%)  n {bands[0]:8.2f} m {bands[1]:8.2f} f {bands[2]:8.2f}")
		for d, name in (("n", "near (0-2000)"), ("m", "mid (2000-3250)"), ("f", "far")):
			print(f"  band {name}: {100*sum(float(r[f'b_{b}_{d}']) for r in rows for b in BRANCHES)/tot:5.1f}% of member updates")
		print(f"  members per tick (updates, both steps): mean {tot/n:.1f}; near share {100*sum(float(r[f'b_{b}_n']) for r in rows for b in BRANCHES)/tot:.1f}%")
		print("pickTarget calls by caller state (per tick):")
		for st in ("attack", "defend", "between", "hub", "circle", "wait", "stumble", "done", "approach"):
			stat("pick_" + st, col("pick_" + st))
	one = sum(1 for a, m in zip(col("isl_active"), col("isl_max")) if a > 0 and m == a)
	print(f"  ticks where the sight graph is ONE component: {one}/{n} ({100*one/n:.1f}%)")
	print(f"  ticks with isl_n>=2: {sum(1 for v in col('isl_n') if v>=2)}; with tgt_max>=2 (some fight): {sum(1 for v in col('tgt_max') if v>=2)}")


def routes(path):
	rows = [r for r in csv.DictReader(open(path)) if LO <= int(r["tick"]) <= HI]
	if not rows:
		print("no routes")
		return
	print(f"{path}: {len(rows)} routeFrom calls")
	ms = sorted(float(r["wall_us"]) / 1000 for r in rows)
	print(f"  wall ms mean {sum(ms)/len(ms):.3f} p50 {q(ms,.5):.3f} p99 {q(ms,.99):.3f} max {ms[-1]:.1f}; total {sum(ms)/1000:.1f} s")
	print(f"  found {sum(1 for r in rows if r['found']=='1')}, not found {sum(1 for r in rows if r['found']=='0')}, plain_failed {sum(1 for r in rows if r['plain_failed']=='1')}")
	by = defaultdict(list)
	for r in rows:
		by[(r["tag"], r["radius"] != "0.00")].append(r)
	print("  by tag (1 combat pathLength, 2 combat replan, 3 blocked-target test, 0 other) x clearance radius>0:")
	for (tag, rad), rs in sorted(by.items()):
		w = sorted(float(r["wall_us"]) / 1000 for r in rs)
		f = sum(1 for r in rs if r["found"] == "1")
		print(f"    tag {tag} radius>0={rad}: n {len(rs)} found {f} wall mean {sum(w)/len(w):.3f} p99 {q(w,.99):.3f} max {w[-1]:.1f} ms; total {sum(w)/1000:.1f} s")
	print("  stage nodes (all calls / the slow calls > 5 ms):")
	slow = [r for r in rows if float(r["wall_us"]) > 5000]
	for name, rs in (("all", rows), ("slow", slow)):
		if not rs:
			continue
		for k in ("searches", "find_paths", "probe_nodes", "clear_nodes", "again_nodes", "clear_runs"):
			v = sorted(float(r[k]) for r in rs)
			print(f"    {name:4s} {k:12s} mean {sum(v)/len(v):9.1f} p99 {q(v,.99):9.0f} max {v[-1]:9.0f}")
	print(f"  slow (>5 ms): {len(slow)}; of them found {sum(1 for r in slow if r['found']=='1')}, not found {sum(1 for r in slow if r['found']=='0')}; time share {100*sum(float(r['wall_us']) for r in slow)/max(1,sum(float(r['wall_us']) for r in rows)):.1f}%")
	for name, rs in (("not found", [r for r in rows if r["found"] == "0"]), ("radius-0 retries (radius 0 after a radius>0 miss; approx = radius 0 calls)", [r for r in rows if r["radius"] == "0.00"])):
		if rs:
			w = [float(r["wall_us"]) / 1000 for r in rs]
			print(f"  {name}: n {len(rs)} wall total {sum(w)/1000:.1f} s mean {sum(w)/len(w):.3f} ms")
	print("  by caller stretch (the profiler stage the call ran in = the sub after the last mark passed): n, total s")
	byl = defaultdict(lambda: [0, 0.0])
	for r in rows:
		byl[r["last_sub"]][0] += 1
		byl[r["last_sub"]][1] += float(r["wall_us"]) / 1e6
	for k, (n, t) in sorted(byl.items(), key=lambda kv: -kv[1][1])[:8]:
		print(f"    {SUBS[min(int(k) + 1, len(SUBS) - 1)]}: {n} calls {t:.2f} s")
	nf = defaultdict(lambda: [0, 0.0])
	for r in rows:
		if r["found"] == "0":
			nf[SUBS[min(int(r["last_sub"]) + 1, len(SUBS) - 1)]][0] += 1
			nf[SUBS[min(int(r["last_sub"]) + 1, len(SUBS) - 1)]][1] += float(r["wall_us"]) / 1e6
	print("  NOT-found calls by caller stretch:", {k: (v[0], round(v[1], 2)) for k, v in nf.items()})
	print("  slow calls (>5 ms) by outcome: ", end="")
	groups = defaultdict(lambda: [0, 0.0])
	for r in slow:
		key = ("found" if r["found"] == "1" else "NOT found", "probe hit the 32768-node pool" if float(r["probe_nodes"]) >= 32768 else "probe under the pool", "clearance ran" if float(r["clear_runs"]) > 0 else "no clearance")
		groups[key][0] += 1
		groups[key][1] += float(r["wall_us"]) / 1e6
	print({k: (v[0], round(v[1], 2)) for k, v in sorted(groups.items(), key=lambda kv: -kv[1][1])})
	# re-query rate: calls per (tag, who) with who != 0; per second of sim (20 ticks/s)
	t0, t1 = TICKS
	secs = max(1.0, (t1 - t0) / 20.0)
	print(f"  sim seconds covered {secs:.0f}")
	for tag in ("1", "2", "3"):
		per = defaultdict(list)
		for r in rows:
			if r["tag"] == tag and r["who"] != "0":
				per[r["who"]].append(int(r["tick"]))
		if not per:
			continue
		counts = sorted(len(v) for v in per.values())
		print(f"  tag {tag}: {len(per)} members queried; calls per member mean {sum(counts)/len(counts):.1f} p99 {q(counts,.99):.0f} max {counts[-1]}; per member-second max {counts[-1]/secs:.2f}")
		# runs of consecutive-tick re-queries by one member (gap <= 2 ticks)
		longest = 0
		for v in per.values():
			run = 1
			for a, b in zip(v, v[1:]):
				run = run + 1 if b - a <= 2 else 1
				longest = max(longest, run)
		print(f"    longest run of re-queries by one member, gap <=2 ticks: {longest}")
	# same goal re-queried (all tags), goal quantised to 4 u, and same from-cell+goal
	goal = defaultdict(int)
	for r in rows:
		goal[(round(float(r["tox"]) / 4), round(float(r["toz"]) / 4))] += 1
	top = sorted(goal.values(), reverse=True)[:5]
	print(f"  most-repeated goals (4 u cells): {top} of {len(goal)} distinct goals")
	per_tick = defaultdict(int)
	for r in rows:
		per_tick[r["tick"]] += 1
	pt = sorted(per_tick.values())
	print(f"  routes per tick (ticks with any): n {len(pt)} mean {sum(pt)/len(pt):.2f} p99 {q(pt,.99)} max {pt[-1]}")


if __name__ == "__main__":
	ticks(sys.argv[1])
	if len(sys.argv) > 2:
		routes(sys.argv[2])
