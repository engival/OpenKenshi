#!/usr/bin/env python3
"""Run ksim_fight on a fixture and gate the hero's distribution (a ctest, docs/reference/SCENARIO_IMPORT.md "First comparison").

  fight_gate.py KSIM_FIGHT SCENARIO NAVWORLD|- OUTDIR [--runs N] [--max NAME=VALUE ...] [--min NAME=VALUE ...]

NAME is a mean over the runs of: ko (hero KO time, s), swings_at (hits_taken), her_swings (hits_landed), her_landed (att hit),
hero_loss (sum of part_loss).  Exit 77 (ctest skip) when the scenario file is absent (the converted fixtures are game-derived and
live in kenshi-inspect) or when ksim_fight itself skips (no navworld); 1 when a gate fails.
NAVWORLD "-" runs ksim_fight on its default, the runtime mesh; a directory selects that bake.
"""
import json, os, subprocess, sys, glob


def main():
	args = sys.argv[1:]
	exe, scenario, navworld, out = args[:4]
	runs, gates, i = 10, [], 4
	while i < len(args):
		if args[i] == '--runs':
			runs = int(args[i + 1]); i += 2
		elif args[i] in ('--max', '--min'):
			name, value = args[i + 1].split('=')
			gates.append((args[i] == '--max', name, float(value))); i += 2
		else:
			sys.exit('unknown argument ' + args[i])
	if not os.path.exists(scenario):
		print('fight_gate: %s absent, skipped' % scenario)
		return 77
	os.makedirs(out, exist_ok=True)
	r = subprocess.run([exe, scenario] + ([] if navworld == '-' else ['--navworld', navworld]) + ['--runs', str(runs), '--out', out])
	if r.returncode != 0:
		return r.returncode
	rows = []
	for f in sorted(glob.glob(os.path.join(out, 'r*_summary.json'))):
		j = json.load(open(f))
		chars = j['chars'].values() if isinstance(j['chars'], dict) else j['chars']
		hero = [c for c in chars if c.get('player')][0]
		rows.append({'ko': hero['ko'] if hero['ko'] is not None else j['fight_s'], 'swings_at': hero['hits_taken'], 'her_swings': hero['hits_landed'],
			'her_landed': hero['att']['hit'], 'hero_loss': sum(hero['part_loss'].values())})
	if len(rows) != runs:
		print('fight_gate: %d summaries, expected %d' % (len(rows), runs))
		return 1
	mean = lambda k: sum(r[k] for r in rows) / len(rows)
	bad = 0
	for k in rows[0]:
		print('fight_gate: %-10s mean %.2f' % (k, mean(k)))
	for is_max, name, value in gates:
		m = mean(name)
		if (m > value) if is_max else (m < value):
			print('fight_gate: FAIL %s mean %.2f %s %.2f' % (name, m, 'above' if is_max else 'below', value))
			bad = 1
	return bad


sys.exit(main())
