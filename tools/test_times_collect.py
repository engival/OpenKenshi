#!/usr/bin/env python3
"""Turns tools/test_times.sh's <outdir>/raw into <outdir>/times.tsv (kenshi-k16.15.54). Usage: test_times_collect.py <outdir> <build-dir> [<outdir2>]
With outdir2 (a later run of a subset, same layout), a test found there takes its phase columns and cpu2_s / wall2_s from it."""
import json, os, re, subprocess, sys

out, build = sys.argv[1], sys.argv[2]
out2 = sys.argv[3] if len(sys.argv) > 3 else None
meta = json.loads(subprocess.check_output(["ctest", "--test-dir", build, "--show-only=json-v1"]))
labels = {}
for t in meta["tests"]:
	for p in t.get("properties", []):
		if p["name"] == "LABELS":
			labels[t["name"]] = p["value"]
PH = ["data", "world", "nav", "combat", "tick", "teardown", "other", "total"]
cols = ["test", "status", "wall_s", "cpu_s", "load_pre", "load_post"] + [p + "_cpu_s" for p in PH if p != "total"] + ["ticks", "proc_cpu_s", "wall2_s", "cpu2_s", "labels", "quick"]
rows = []
for name in open(os.path.join(out, "tests.txt")).read().split():
	raw = os.path.join(out, "raw", name)
	try:
		lines = open(raw + ".time").read().split("\n")
	except OSError:
		rows.append([name, "NOT_RUN"] + [""] * (len(cols) - 2))
		continue
	nums = [l.split() for l in lines if re.fullmatch(r"[\d.]+ [\d.]+ [\d.]+", l.strip())]
	e, u, s = (float(x) for x in nums[0]) if nums else (0.0, 0.0, 0.0)
	log = open(raw + ".log", errors="replace").read()
	status = "Passed" if re.search(r"\bPassed\b", log) else "Timeout" if "Timeout" in log else "Skipped" if re.search(r"Skipped|\*\*\*Not Run|SKIPPED", log) else "Failed"
	ph = {p: [0.0, 0] for p in PH}
	w2 = c2 = ""
	phraw = raw
	if out2 and os.path.exists(os.path.join(out2, "raw", name + ".time")):
		phraw = os.path.join(out2, "raw", name)
		n2 = [l.split() for l in open(phraw + ".time").read().split("\n") if re.fullmatch(r"[\d.]+ [\d.]+ [\d.]+", l.strip())]
		if n2:
			w2, c2 = "%.2f" % float(n2[0][0]), "%.2f" % (float(n2[0][1]) + float(n2[0][2]))
	try:
		for l in open(phraw + ".phases"):
			m = re.match(r"PHASE (\w+) wall=([\d.]+) cpu=([\d.]+) n=(\d+)", l)
			if m:
				ph[m[1]][0] += float(m[3]); ph[m[1]][1] += int(m[4])
	except OSError:
		pass
	def load(suffix):
		try: return open(raw + suffix).read().split()[0]
		except OSError: return ""
	lab = labels.get(name, [])
	lab = ";".join(lab) if isinstance(lab, list) else lab
	rows.append([name, status, "%.2f" % e, "%.2f" % (u + s), load(".load.pre"), load(".load.post")]
		+ ["%.2f" % ph[p][0] for p in PH if p != "total"] + [str(ph["tick"][1]), "%.2f" % ph["total"][0], w2, c2, lab, "no" if "slow" in lab.split(";") else "yes"])
with open(os.path.join(out, "times.tsv"), "w") as f:
	f.write("\t".join(cols) + "\n")
	for r in rows:
		f.write("\t".join(r) + "\n")
print(len(rows), "rows")
