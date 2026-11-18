#!/usr/bin/env python3
"""Compare fight distributions: the original's captures against ksim_fight's.

  fight_compare.py DIR_A [DIR_B ...] [--hero=NAME] [--label=A,B]
  fight_compare.py DIR_A [DIR_B ...] --crowd=SIDE_A_REGEX [--label=A,B]

Each DIR holds rN_summary.json files in the common schema (kinspect's tools/fight_summary.py writes it from a combatlog; `ksim_fight` writes the same
fields, docs/reference/SCENARIO_IMPORT.md "Summary format").  Prints, per directory, a table in the layout of kinspect's SUMMARY.md (one row per run,
then mean +- sd lines) and then one side-by-side table of the means.  Stdlib only; our own code (kinspect's script defines the measures, it is not copied).

Measures (the hero is the one character of the side that is fought; default: the char with `player` true, else the --hero name):
  KO time        hero `ko` (seconds from the trigger; kinspect: real seconds at speed 1, ours: sim seconds, the same unit: 1 game minute = 1.818 s)
  swings at her  hero hits_taken (every resolved swing: through + blocked + dodged); through / blocked / dodged = hero def hit / block / dodge
  her swings     hero hits_landed; landed = hero att hit
  bandits that swung   other chars with att hit + block + dodge > 0
  part loss      sum of part_loss; top parts = the three largest; bandit part loss = sum over the other chars; bandits KO = others with ko or dead set
"""
import glob, json, math, os, re, sys

CROWD_DOC = """
--crowd=REGEX (a fight with no single hero, e.g. bar_crowd): side A = the characters whose squad matches REGEX (the attackers); only squads that the FIRST directory
logged are compared (kinspect's log has a radius, the scenario holds more).  Each directory needs rN_summary.json and rN_joiners.json (kinspect's tools/fight_joiners.py
writes the second from a combatlog; ksim_fight writes both when the scenario has `order` records).  Measures:
  end s            the last side-A KO / down second of the run (the "bandits_down" end of SUMMARY.md), runs where not everyone of side A went down are left out
  A KO             side-A characters with a ko, down or dead time
  fought           side-B characters that joined (first targeted a side-A character; the order targets included)
  fought KO        of them, with a ko / down / dead time
  deaths           dead in the whole run
  joiner table     per (name, squad): runs in which it joined, first join second mean (min-max)
"""


def load(d):
	runs = []
	for p in sorted(glob.glob(os.path.join(d, 'r*_summary.json')), key=lambda p: int(re.search(r'r(\d+)_summary', p).group(1))):
		runs.append((int(re.search(r'r(\d+)_summary', p).group(1)), json.load(open(p))))
	return runs


def measures(run, hero_name):
	chars = run['chars']
	hero = next((c for c in chars.values() if c.get('player')), None)
	if hero is None and hero_name:
		hero = next((c for c in chars.values() if c['name'] == hero_name), None)
	others = [c for c in chars.values() if c is not hero]
	loss = hero.get('part_loss', {})
	top = sorted(loss.items(), key=lambda kv: -kv[1])[:3]
	swung = sum(1 for c in others if sum(c['att'].values()) > 0)
	return {
		'ko': hero.get('ko'), 'at': hero['hits_taken'], 'through': hero['def']['hit'], 'blocked': hero['def']['block'], 'dodged': hero['def']['dodge'],
		'hers': hero['hits_landed'], 'landed': hero['att']['hit'], 'swung': swung, 'loss': sum(loss.values()), 'max_loss': max(loss.values()) if loss else 0.0,
		'top': ', '.join('%s %d' % (k, round(v)) for k, v in top), 'bloss': sum(sum(c.get('part_loss', {}).values()) for c in others),
		'bko': sum(1 for c in others if c.get('ko') is not None or c.get('dead') is not None),
	}


STATS = [('ko', 'KO time s'), ('at', 'swings at her'), ('through', '... through'), ('blocked', '... blocked'), ('dodged', '... dodged'), ('swung', 'bandits that swung'),
	('loss', 'her part loss'), ('max_loss', 'her largest part loss'), ('hers', 'her swings'), ('landed', '... landed'), ('bloss', 'bandit part loss'), ('bko', 'bandits KO')]


def mean_sd(xs):
	xs = [x for x in xs if x is not None]
	if not xs: return None
	m = sum(xs) / len(xs)
	sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1)) if len(xs) > 1 else 0.0
	return m, sd, min(xs), max(xs), len(xs)


def table(label, runs, hero):
	rows = [(n, measures(r, hero)) for n, r in runs]
	out = ['## %s (%d runs)' % (label, len(rows)), '',
		'| run | hero KO (s) | swings at her | through | blocked | dodged | her swings | landed | bandits that swung | her part loss | top parts | bandit part loss | bandits KO |',
		'|---|---|---|---|---|---|---|---|---|---|---|---|---|']
	for n, m in rows:
		out.append('| %d | %s | %d | %d | %d | %d | %d | %d | %d | %.1f | %s | %.1f | %d |' % (n, '-' if m['ko'] is None else '%.2f' % m['ko'], m['at'], m['through'],
			m['blocked'], m['dodged'], m['hers'], m['landed'], m['swung'], m['loss'], m['top'], m['bloss'], m['bko']))
	out.append('')
	summary = {}
	for key, name in STATS:
		s = mean_sd([m[key] for _, m in rows])
		summary[key] = s
		out.append('- %s: %s' % (name, 'none' if s is None else '%.1f +- %.1f (min %.1f, max %.1f, n=%d)' % s))
	return '\n'.join(out), summary


def crowd_run(summary, joiners, side_re, squads):
	rx = re.compile(side_re)
	chars = {c['name'] + '|' + c['squad'] + '|' + k: c for k, c in summary['chars'].items() if c.get('squad') in squads}
	a = [c for c in chars.values() if rx.search(c['squad'] or '')]
	def down(c):
		ts = [t for t in (c.get('ko'), c.get('down'), c.get('dead')) if t is not None]
		return min(ts) if ts else None
	a_down = [down(c) for c in a]
	end = max(a_down) if a and all(t is not None for t in a_down) else None
	join = {}
	for j in joiners['joiners']:
		if j['squad'] in squads:
			join[j['id']] = j
	# joiners are keyed by the char id of the summary (kinspect: hex id string; ours: decimal)
	fought = [j for j in join.values()]
	byid = {k: c for k, c in summary['chars'].items()}
	fought_ko = sum(1 for j in fought if byid.get(j['id'], {}).get('ko') is not None or byid.get(j['id'], {}).get('dead') is not None or j.get('ever_down') is not None)
	joins = {}
	for j in fought:
		joins.setdefault((j['name'], j['squad']), []).append(j['join_s'])
	return {'end': end, 'a_ko': sum(1 for t in a_down if t is not None), 'a_n': len(a), 'fought': len(fought), 'fought_ko': fought_ko,
		'deaths': sum(1 for c in chars.values() if c.get('dead') is not None), 'joins': joins}


def crowd(dirs, labels, side_re):
	loaded = []
	for d in dirs:
		runs = load(d)
		if not runs: sys.exit('no rN_summary.json in ' + d)
		loaded.append([(n, r, json.load(open(os.path.join(d, 'r%d_joiners.json' % n)))) for n, r in runs])
	squads = {c['squad'] for _, r, _ in loaded[0] for c in r['chars'].values() if c.get('squad')}
	results = [[crowd_run(r, j, side_re, squads) for _, r, j in runs] for runs in loaded]
	rows = [('end s (side A all down)', 'end'), ('side A KO', 'a_ko'), ('side B that fought', 'fought'), ('... of them KO / down', 'fought_ko'), ('deaths', 'deaths')]
	print('## Crowd fight, side A = /%s/ (%d squads compared)\n' % (side_re, len(squads)))
	print('| measure | ' + ' | '.join('%s (%d runs)' % (l, len(r)) for l, r in zip(labels, results)) + ' |')
	print('|---|' + '---|' * len(labels))
	for name, key in rows:
		cells = []
		for res in results:
			m = mean_sd([r[key] for r in res])
			cells.append('-' if m is None else '%.1f +- %.1f (%g-%g, n=%d)' % m)
		print('| %s | ' % name + ' | '.join(cells) + ' |')
	pooled = lambda res, k: [t for r in res for t in r['joins'].get(k, [])]
	names = sorted({k for res in results for r in res for k in r['joins']}, key=lambda k: (min(pooled(results[0], k) or [1e9]), k))
	print('\n| joiner (name, squad; same-name characters pooled) | ' + ' | '.join('%s: joins / runs, join s mean (min-max)' % l for l in labels) + ' |')
	print('|---|' + '---|' * len(labels))
	for k in names:
		cells = []
		for res in results:
			ts = pooled(res, k)
			cells.append('%d/%d' % (len(ts), len(res)) + (' %.1f (%.1f-%.1f)' % (sum(ts) / len(ts), min(ts), max(ts)) if ts else ''))
		print('| %s, %s | ' % k + ' | '.join(cells) + ' |')


def main():
	args = [a for a in sys.argv[1:] if not a.startswith('--')]
	hero = None
	labels = None
	side = None
	for a in sys.argv[1:]:
		if a.startswith('--hero='): hero = a[7:]
		if a.startswith('--label='): labels = a[8:].split(',')
		if a.startswith('--crowd='): side = a[8:]
	if not args: sys.exit(__doc__ + CROWD_DOC)
	labels = labels or [os.path.basename(os.path.normpath(a)) for a in args]
	if side:
		crowd(args, labels, side)
		return
	sums = []
	for d, label in zip(args, labels):
		runs = load(d)
		if not runs: sys.exit('no rN_summary.json in ' + d)
		text, s = table(label, runs, hero)
		print(text + '\n')
		sums.append(s)
	if len(sums) > 1:
		print('## Side by side (mean +- sd over the runs)\n')
		print('| measure | ' + ' | '.join(labels) + ' |')
		print('|---|' + '---|' * len(labels))
		for key, name in STATS:
			print('| %s | ' % name + ' | '.join('-' if s[key] is None else '%.1f +- %.1f' % s[key][:2] for s in sums) + ' |')


if __name__ == '__main__':
	main()
