#!/usr/bin/env python3
"""bark_chase count (kenshi-jtnm): read client.log files from run.sh and count the speech bubbles per speaker squad.

usage: count.py [--csv] CLIENT_LOG...
Each log line is "<wall s> <client stdout line>". `speech: A to B: "text" (DIALOGUE / LINE, 3.0 s)` shows a bubble for its duration (a later speech of the
same speaker replaces it, `speech_end: A` cuts it short). Squads: the first `char ID "name"` dump of the run, a name -> squad map (Holy ... = patrol,
others = their own). Prints per squad: bubbles, the most bubbles on screen at one moment, the time spent with 2+ at once, a table of each bubble, and every
line repeated inside its record's repeat delay (the line's `repetition limit` from the game data: 1 = 2 h, 2 = 6 h, 3 = 48 h, 4 = 168 h, 5 = 5000 h, 6 = 0.33 h,
one game hour = 109.1 s at 1x; a line repeated in the log at all is listed with its limit, "repeat" when the gap is under the limit).
"""
import re, sys, os, collections
HOURS = {0: 0.0, 1: 2.0, 2: 6.0, 3: 48.0, 4: 168.0, 5: 5000.0, 6: 0.33}
TICK_S = 0.05	# one sim tick = 0.05 game s: the client's `tick N` stamp (kenshi-jtnm) replaces the wall time when present
GAME_HOUR_S = 2618.1819 / 24
def data_dir():
	"""KENSHI_DATA from the environment, else the KENSHI_DATA line of client/build/CMakeCache.txt; None when neither is a directory."""
	d = os.environ.get('KENSHI_DATA', '')
	if not d:
		root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../..')
		try:
			with open(os.path.join(root, 'client/build/CMakeCache.txt')) as f:
				for l in f:
					m = re.match(r'KENSHI_DATA:[A-Z]*=(.*)', l)
					if m: d = m.group(1).strip(); break
		except OSError:
			pass
	return os.path.join(d, '') if d and os.path.isdir(d) else None
_lim = {}
def limits(sids):
	"""line / dialogue sid -> repetition limit code from the game data (None when not found)."""
	todo = [s for s in sids if s not in _lim]
	if not todo: return
	here = os.path.dirname(os.path.abspath(__file__))
	sys.path.insert(0, os.path.join(here, '../../../tools'))
	try:
		import kmod
	except Exception:
		for s in todo: _lim[s] = None
		return
	DATA = data_dir()
	if not DATA:
		print('count.py: no KENSHI_DATA (env or client/build/CMakeCache.txt), repeat limits skipped', file=sys.stderr)
		for s in todo: _lim[s] = None
		return
	for fn in ['gamedata.base', 'rebirth.mod', 'Dialogue.mod', 'Newwworld.mod']:
		try: f, _ = kmod.parse(DATA + fn)
		except Exception: continue
		for r in f['records']:
			if r['sid'] in todo:
				_lim[r['sid']] = dict(r['int']).get('repetition limit', 0)
	for s in todo: _lim.setdefault(s, None)

SP = re.compile(r'^\s*([\d.]+) speech: (\d+) to (\d+): "(.*)" \((.*?) / (.*?), ([\d.]+) s(.*)\)(?: tick (\d+))?$')
SE = re.compile(r'^\s*([\d.]+) speech_end: (\d+)(?: tick (\d+))?')
CH = re.compile(r'^\s*[\d.]+ char (\d+) "([^"]*)"')
def squad_of(name):
	return 'Holy Nation patrol' if name.startswith('Holy') else name
def analyse(path, csv):
	names, ev = {}, []
	for l in open(path, errors='replace'):
		m = CH.match(l)
		if m: names.setdefault(int(m[1]), m[2])
		m = SP.match(l)
		if m: ev.append(('s', float(m[9]) * TICK_S if m[9] else float(m[1]), int(m[2]), m[4], m[5], m[6], float(m[7]), m[8]))
		m = SE.match(l)
		if m: ev.append(('e', float(m[3]) * TICK_S if m[3] else float(m[1]), int(m[2])))
	limits({e[5] for e in ev if e[0] == 's'} | {e[4] for e in ev if e[0] == 's'})
	bub = []	# (squad, speaker, t0, t1, text, dlg, line)
	live = {}
	for e in ev:
		if e[0] == 's':
			_, t, a, text, dlg, line, dur, fl = e
			if a in live: live[a][3] = min(live[a][3], t)
			b = [squad_of(names.get(a, f'#{a}')), a, t, t + dur, text, dlg, line, fl]
			bub.append(b); live[a] = b
		else:
			a = e[2]
			if a in live: live[a][3] = min(live[a][3], e[1])
	print(f'== {path}: {len(bub)} bubbles, speakers {sorted({(b[0], b[1]) for b in bub})}')
	by = collections.defaultdict(list)
	for b in bub: by[b[0]].append(b)
	for sq, bs in sorted(by.items()):
		pts = sorted([(b[2], 1) for b in bs] + [(b[3], -1) for b in bs], key=lambda p: (p[0], p[1]))
		cur = peak = 0; multi = 0.0; last = None
		hist = collections.Counter()
		for t, d in pts:
			if last is not None: hist[cur] += t - last
			cur += d; peak = max(peak, cur); last = t
		multi = sum(v for k, v in hist.items() if k >= 2)
		same_tick = 0
		for i, b in enumerate(bs):
			same_tick += sum(1 for c in bs[:i] if c[1] != b[1] and abs(c[2] - b[2]) < 0.35)
		print(f'-- {sq}: {len(bs)} bubbles by {len({b[1] for b in bs})} speakers; most on screen at once {peak}; seconds with 2+ on screen {multi:.1f} of {sum(hist.values()):.1f} with any; bubbles starting within 0.35 s of another speaker\'s {same_tick}')
		seen = {}
		rep = 0; reps = []
		for b in bs:
			key = b[6]
			if key in seen:
				gap = b[2] - seen[key][2]
				lim = _lim.get(key)
				code = lim if lim is not None else _lim.get(b[5])
				secs = HOURS.get(code, 0) * GAME_HOUR_S if code is not None else None
				within = secs is not None and gap < secs
				reps.append((b, gap, code, secs, within)); rep += within
			seen[key] = b
		print(f'   lines repeated at all: {len(reps)}; inside the record\'s repeat delay: {rep}')
		for b in bs:
			print(f'   {b[2]:7.2f}-{b[3]:7.2f}  #{b[1]:<4} {names.get(b[1], "?"):<16} {b[4]!r}  [{b[5]} / {b[6]}]')
		for b, gap, code, secs, within in reps:
			print(f'   REPEAT {"INSIDE delay" if within else "outside delay"}: {b[4]!r} line {b[6]} again after {gap:.1f} s (limit code {code}, {"%.0f s" % secs if secs is not None else "?"})')
if __name__ == '__main__':
	a = [x for x in sys.argv[1:] if not x.startswith('--')]
	if not a: print(__doc__); sys.exit(2)
	for p in a: analyse(p, '--csv' in sys.argv)
