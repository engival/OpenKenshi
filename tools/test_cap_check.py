#!/usr/bin/env python3
"""The 15 s test cap (kenshi-k16.15.55; user 2026-11-23, docs/INTENT.md). Runs as the ctest `ksim_test_cap_check`, last in a run.
  test_cap_check.py --log test_cpu.tsv --exceptions cpp/tests/test_cap_exceptions.tsv --names ctest_names.txt
The log (tools/test_cpu_wrap.py, "<test>\t<cpu_s>\t<ctest pid>" per run) holds CPU seconds; only lines of this ctest's pid count, so a
reused build dir's old lines never fail a later run. Fails on:
  - a test over 15 CPU-s that has no line in the exceptions file AND is in the touched set (rewrite it, or add the line with why + item);
  - an exception line naming no ctest;
  - a malformed or duplicate line.
The touched set (kenshi-k16.15.74) is the new or changed tests of the change under test: a file of ctest names, one per line, named by
the env var KSIM_CAP_TOUCHED (the test queue sets it from its changed files). An unlisted test over the cap that is NOT in the set (a cold
nav cache made untouched tests slow) is only a "NOTE <test> ... over the cap"; with no set (CI's full run, a manual ctest) nothing
over the cap fails, notes only.
A listed test that ran under 12 CPU-s is only a note (NOTE ... delete the line): a warm nav cache makes some tests fast (knav_bake_hub
16.5 s cold, 0.1 s warm), so it is never a failure. --selftest runs the cases below against temp files.
Exceptions columns: test, cpu_s (k16.15.54 solo number), why, item. '#' lines are comments."""
import argparse, os, sys

CAP, FLOOR = 15.0, 12.0	# FLOOR: only a note
if "--selftest" in sys.argv:
	import subprocess, tempfile
	with tempfile.TemporaryDirectory(prefix="capcheck") as d:
		def run(log, exc, touched):
			for n, t in (("log", log), ("exc", exc), ("names", "slow\nfast\n")):
				open(os.path.join(d, n), "w").write(t)
			env = dict(os.environ, KSIM_CTEST_PID="7")
			env.pop("KSIM_CAP_TOUCHED", None)
			if touched is not None:
				open(os.path.join(d, "touched"), "w").write(touched)
				env["KSIM_CAP_TOUCHED"] = os.path.join(d, "touched")
			r = subprocess.run([sys.executable, "-I", os.path.abspath(__file__), "--log", os.path.join(d, "log"), "--exceptions", os.path.join(d, "exc"), "--names", os.path.join(d, "names")],
				env=env, capture_output=True, text=True)
			return r.returncode, r.stdout
		cases = [("touched, over the cap, unlisted fails", "slow\t20.0\t7\n", "", "slow\n", 1, "FAIL slow"),
			("untouched, over the cap is a note", "slow\t20.0\t7\n", "", "fast\n", 0, "NOTE slow took 20.0"),
			("no set given: notes only", "slow\t20.0\t7\n", "", None, 0, "NOTE slow took 20.0"),
			("touched, over the cap, listed passes", "slow\t20.0\t7\n", "slow\t20\twhy\tit\n", "slow\n", 0, ""),
			("listed but fast is a note, not a failure", "slow\t0.1\t7\n", "slow\t20\twhy\tit\n", None, 0, "NOTE slow"),
			("a stale line of another ctest run is ignored", "slow\t99.0\t3\n", "", "slow\n", 0, ""),
			("an exception naming no ctest fails", "", "gone\t20\twhy\tit\n", None, 1, "no ctest")]
		bad_cases = 0
		for what, log, exc, touched, rc, text in cases:
			got, out = run(log, exc, touched)
			ok = got == rc and text in out
			bad_cases += not ok
			print("selftest %s: %s" % ("ok" if ok else "FAILED", what))
		sys.exit(1 if bad_cases else 0)
ap = argparse.ArgumentParser()
ap.add_argument("--log", required=True)
ap.add_argument("--exceptions", required=True)
ap.add_argument("--names", required=True)
a = ap.parse_args()
names = set(open(a.names).read().split())
exc, bad, notes = {}, [], []
for n, l in enumerate(open(a.exceptions), 1):
	if not l.strip() or l.startswith("#") or l.startswith("test\t"):
		continue
	c = l.rstrip("\n").split("\t")
	if len(c) != 4 or not all(x.strip() for x in c):
		bad.append("%s:%d: want 4 columns test, cpu_s, why, item" % (a.exceptions, n))
	elif c[0] in exc:
		bad.append("%s:%d: duplicate %s" % (a.exceptions, n, c[0]))
	elif c[0] not in names:
		bad.append("%s:%d: %s is no ctest (delete the line)" % (a.exceptions, n, c[0]))
	else:
		exc[c[0]] = c
cpu = {}
me = os.environ.get("KSIM_CTEST_PID", "")	# set by test_cpu_wrap.py: only the lines of this ctest run count
touched = None	# None: no set given, nothing over the cap fails
if os.environ.get("KSIM_CAP_TOUCHED"):
	try:
		touched = set(open(os.environ["KSIM_CAP_TOUCHED"]).read().split())
	except OSError as e:
		bad.append("KSIM_CAP_TOUCHED unreadable: %s" % e)
try:
	for l in open(a.log):
		c = l.rstrip("\n").split("\t")
		if len(c) == 3 and c[2] == me:
			cpu[c[0]] = float(c[1])
except OSError:
	pass
for t, s in sorted(cpu.items()):
	if s > CAP and t not in exc and (touched is None or t not in touched):
		notes.append("NOTE %s took %.1f CPU-s over the cap %g, untouched by this change (not a failure; a cold nav cache is the usual cause)" % (t, s, CAP))
	elif s > CAP and t not in exc:
		bad.append("%s took %.1f CPU-s (cap %g): rewrite it, or add a line to cpp/tests/test_cap_exceptions.tsv (test, cpu_s, why, item)" % (t, s, CAP))
	elif s < FLOOR and t in exc:
		notes.append("NOTE %s took %.1f CPU-s, under the cap (a cold cache may need it): delete its line from cpp/tests/test_cap_exceptions.tsv if it is always fast" % (t, s))
print("test cap: %d tests timed, %d exceptions listed, %d problems" % (len(cpu), len(exc), len(bad)))
for b in notes + ["FAIL " + b for b in bad]:
	print(b)
sys.exit(1 if bad else 0)
