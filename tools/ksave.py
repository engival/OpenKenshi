#!/usr/bin/env python3
"""Kenshi save reader: `ksave.py dump <savedir>` and `ksave.py diff <savedirA> <savedirB>`.

A save dir holds quick.save, platoon/*.platoon, zone/zone.X.Y.zone.  All three are the
kmod record container (version 15, no header) followed by a trailing "handle pool" block.
Format notes: SAVE_FORMAT.md.  Game-data names (items, towns, packages) are resolved from
--game=DIR (default: kenshi_install.py finds it: $KENSHI / $KENSHI_DATA, Steam, GOG).
"""
import sys, os, re, glob, struct, collections, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod
import kenshi_install

TYPES = kmod.ITEMTYPES
GAME_FILES = ['gamedata.base', 'Newwworld.mod', 'Dialogue.mod', 'rebirth.mod']   # later overrides earlier


def fl(b):
	return struct.unpack('<%df' % (len(b) // 4), b)


def tail_pools(b, leftover):
	"""Trailing block: repeated (i32 n, i32[n]).  Returns list of int lists."""
	t = b[len(b) - leftover:]; p = 0; pools = []
	while p < len(t):
		n = struct.unpack_from('<i', t, p)[0]; p += 4
		if n < 0 or p + 4 * n > len(t): raise ValueError('bad tail pool at %d' % p)
		pools.append(list(struct.unpack_from('<%di' % n, t, p))); p += 4 * n
	return pools


class Rec:
	"""kmod record with by-name field access (floats/vec decoded)."""
	def __init__(s, r):
		s.r = r; s.type = TYPES[r['type']]; s.sid = r['sid']; s.name = r['name']
		s.d = {}
		for k in ('bool', 'int', 'string', 'file'):
			for n, v in r[k]: s.d[n] = v
		for k in ('float', 'vec3', 'vec4'):
			for n, v in r[k]:
				x = fl(v); s.d[n] = x[0] if k == 'float' else x
		s.refs = {c: l for c, l in r['refs']}
		s.inst = r['inst']
	def __getitem__(s, k): return s.d[k]
	def get(s, k, default=None): return s.d.get(k, default)
	def handle(s, pre):
		"""Handle stored as <pre>CS/S/C/I/TYPE ints -> (C, I, CS, S, TYPE) or None if null."""
		d = s.d
		if pre + 'C' not in d and pre + 'TYPE' not in d: return None
		h = (d.get(pre + 'C', 0), d.get(pre + 'I', 0), d.get(pre + 'CS', 0), d.get(pre + 'S', 0), d.get(pre + 'TYPE', 11))
		return None if h[4] == 11 and not any(h[:4]) else h


class File:
	def __init__(s, path):
		f, b = kmod.parse(path)
		s.path = path; s.ver = f['ver']; s.lastID = f['lastID']; s.size = len(b); s.leftover = f['leftover']
		s.recs = [Rec(r) for r in f['records']]
		s.by_sid = {r.sid: r for r in s.recs}
		s.pools = tail_pools(b, f['leftover'])
	def of(s, t): return [r for r in s.recs if r.type == t]


class Game:
	def __init__(s, d):
		s.recs = {}; s.seasons = collections.defaultdict(set)   # BIOME_GROUP sid -> SEASON sids
		for fn in GAME_FILES:
			p = os.path.join(d, fn)
			if not os.path.exists(p): continue
			f, _ = kmod.parse(p)
			for r in f['records']:
				s.recs[r['sid']] = (TYPES[r['type']], r['name'])
				if TYPES[r['type']] == 'BIOME_GROUP':
					for cat, lst in r['refs']:
						if cat == 'seasons':
							for sid, v0, v1, v2 in lst:
								(s.seasons[r['sid']].discard if v0 == 0x7fffffff else s.seasons[r['sid']].add)(sid)
	def name(s, sid):
		if not sid: return ''
		x = s.recs.get(sid)
		return '%s' % x[1] if x else sid


class NoGame:
	seasons = {}
	def name(s, sid): return sid


class Save:
	def __init__(s, d, game=None):
		s.dir = d; s.game = game or NoGame()
		s.q = File(os.path.join(d, 'quick.save'))
		s.plat = {os.path.basename(p)[:-8]: File(p) for p in sorted(glob.glob(os.path.join(d, 'platoon', '*.platoon')))}
		zs = sorted(glob.glob(os.path.join(d, 'zone', '*.zone')))
		s.zones = {os.path.basename(p): File(p) for p in zs}
		s.camera = s.q.of('CAMERA')[0]
		s.biomes = s.q.of('BIOMES')[0]
		s.towns = s.q.of('GAMESTATE_TOWN')
		s.town_by_c = {t['handC']: t for t in s.towns if t.get('handTYPE') == 13}
		s.platoons = {r.name: r for r in s.q.of('PLATOON')}
		s.factions = s.q.of('GAMESTATE_FACTION')

	# --- clock / weather / money -------------------------------------------------
	def clock(s):
		c = s.camera
		return c['time day'], c['time hour'], c['time minute']

	def money(s): return s.camera['player money']

	def weather_slot(s):
		"""BIOMES slot whose SEASON belongs to the player's biome group (CAMERA biome_text_groud).
		Slot -> group mapping is not stored in the save; found by matching the season (guess)."""
		g = s.camera.get('biome_text_groud')
		return g

	def weathers(s):
		b = s.biomes; out = {}
		for k, v in b.d.items():
			m = re.match(r'(\d+)_w_([ws])_id$', k)
			if m: out.setdefault(int(m.group(1)), {})[m.group(2)] = v
		return out

	# --- characters ----------------------------------------------------------------
	def characters(s):
		"""[(platoon name, idx, dict)] from the .platoon files."""
		out = []
		for pn, f in s.plat.items():
			ic = f.of('INSTANCE_COLLECTION')[0]
			for i, inst in enumerate(ic.inst):
				st = {f.by_sid[x].type: f.by_sid[x] for x in inst['states'] if x in f.by_sid}
				ch = st.get('GAMESTATE_CHARACTER')
				x, y, z, qw, qx, qy, qz = fl(inst['xform'])
				out.append(dict(platoon=pn, idx=i, inst=inst['id'], template=inst['ref'], pos=(x, y, z),
					rot=(qw, qx, qy, qz), name=ch['name'] if ch else '?', state=st, file=f))
		return out

	def inventory(s, c):
		f = c['file']; inv = c['state'].get('INVENTORY_STATE'); items = []
		if not inv: return items
		for inst in inv.inst:
			it = f.by_sid.get(inst['ref'])
			if it: items.append(it)
		return items

	def town_name(s, handC):
		t = s.town_by_c.get(handC)
		return t.name.replace('Town state ', '') if t else '-'

	def hunger(s, c):
		m = c['state'].get('MEDICAL_STATE')
		return m['hung'] * 100 if m else None   # UI "Hunger" = hung*100 (verified vs screenshot 283)


def hhmm(d, h, m): return 'Day %d %02d:%02d' % (d, h, m)


def fmt_pos(p): return '(%.1f, %.1f, %.1f)' % p


def dump(d, game, out=sys.stdout):
	S = Save(d, game)
	w = lambda *a: print(*a, file=out)
	q = S.q
	w('SAVE %s' % d)
	w('quick.save: v%d lastID=%d records=%d size=%d tail-pools=%s' % (q.ver, q.lastID, len(q.recs), q.size, [len(p) for p in q.pools]))
	w('game version string: %s   area: %s   player faction: %s' % (S.camera['version'], S.camera['area'], S.camera['pfaction name']))
	w('platoon files: %d   zone files: %s' % (len(S.plat), ', '.join('%s (%d recs)' % (n, len(z.recs)) for n, z in S.zones.items())))
	day, h, m = S.clock()
	w('CLOCK: %s   (sky update %.3f/%.3f)' % (hhmm(day, h, m), S.camera['sky update current time'], S.camera['sky update total time']))
	w('MONEY: %d' % S.money())
	grp = S.camera.get('biome_text_groud')
	w('BIOME GROUP at camera: %s (%s)' % (game.name(grp), grp))
	wt = S.weathers()
	w('WEATHER slots: %d (per-biome; slot->group map not saved). Distinct current weather:' % len(wt))
	cnt = collections.Counter(game.name(v.get('w')) for v in wt.values())
	w('   ' + ', '.join('%s x%d' % kv for kv in cnt.most_common()))
	mine = sorted(i for i, v in wt.items() if v.get('s') in game.seasons.get(grp, ()))
	w('   player biome slot(s) by season match (guess): %s -> %s' % (mine, [game.name(wt[i]['w']) for i in mine]))
	camp = S.camera
	w('CAMERA pos=%s rot=%s zoom=%.1f floor num=%s' % (fmt_pos(camp['pos']), tuple(round(x, 3) for x in camp['rot']), camp['zoom'], camp.get('floor num', 'absent')))
	w('RESEARCH finished: %s' % ([game.name(r.d[k]) for r in q.of('RESEARCH') for k in r.d if k.startswith('finished')]))
	# factions
	F = S.factions
	w('FACTIONS: %d' % len(F))
	pf = [r for r in F if r.name == S.camera['pfaction name']]
	rel = collections.defaultdict(dict)
	for r in F:
		for k, v in r.d.items():
			m_ = re.match(r'relationSID(\d+)$', k)
			if m_ and v == (pf[0]['gamedata stringID'] if pf else None):
				rel[r.name] = (r.get('relation' + m_.group(1)), r.get('trust' + m_.group(1)))
	w('  relations toward player faction (value; 0 = neutral default): %s' %
		', '.join('%s=%g' % (n, v[0]) for n, v in sorted(rel.items()) if v[0]))
	# towns
	nt = [t for t in S.towns if t.get('handTYPE') == 13]
	w('TOWN STATES: %d towns, %d nests/other' % (len(nt), len(S.towns) - len(nt)))
	cur = S.town_by_c.get(74)
	# squads
	chars = S.characters()
	byp = collections.defaultdict(list)
	for c in chars: byp[c['platoon']].append(c)
	w('SQUADS: %d platoons, %d characters' % (len(S.platoons), len(chars)))
	for pn in sorted(S.platoons):
		p = S.platoons[pn]
		home = p.handle('hometown'); cur_ = p.handle('currenttown')
		w('- %s  faction=%s [%s]  chars=%d  money=%d  pos=%s' % (pn, p['faction name'], p['faction stringID'], p['char count'], p['money'], fmt_pos(p['position'])))
		w('    home town: %s   basetown: %s   current town: %s   package: %s   squad template: %s%s' % (
			S.town_name(home[0]) if home else '-', game.name(p['basetown']) or '-', S.town_name(cur_[0]) if cur_ else '-',
			game.name(p['currentPackage']) or '-', game.name(p['squad template']),
			'   resident' if p['is resident'] else ''))
		for c in byp.get(pn, []):
			md = c['state'].get('MEDICAL_STATE'); ch = c['state'].get('GAMESTATE_CHARACTER')
			inv = S.inventory(c)
			flags = [k for k in ('is leader', 'chase mode', 'passive mode', 'defensive mode', 'stealth', 'ranged mode', 'taunt mode') if ch.get(k)]
			w('    * %s%s  [%s]  pos=%s  hunger=%.0f blood=%.1f%s  items=%d %s' % (
				c['name'], '' if not ch.get('is leader') else ' (leader)', game.name(c['template']), fmt_pos(c['pos']),
				S.hunger(c), md['blood'], '  DEAD' if md['dead'] else '', len(inv),
				('modes=' + ','.join(flags)) if flags else ''))
			if pn == 'Nameless_0':
				st = c['state']['STATS']
				w('        stats: ' + ', '.join('%s=%.2f' % (k, v) for k, v in sorted(st.d.items()) if isinstance(v, float) and v))
				w('        limbs: ' + ', '.join('%s flesh=%.0f hit=%.0f' % (game.name(md['sid%d' % i]), md['flesh%d' % i], md['hit%d' % i]) for i in range(7)))
				for it in inv:
					w('        item: %-10s %-28s x%d q=%.0f lvl=%d @(%d,%d)' % (it['section'], game.name(it['base data sid']), it['quantity'], it['quality'], it['level'], it['inventory x'], it['inventory y']))
	for zn, z in S.zones.items():
		bl = [r for r in z.of('INSTANCE_COLLECTION')]
		B = z.of('GAMESTATE_BUILDING')
		own = collections.Counter(r['owner faction ID'] for r in B)
		w('ZONE %s: %d GAMESTATE_BUILDING, %d placements, %d item states, %d placement groups; owners: %s' % (
			zn, len(B), sum(1 for i in bl[0].inst if i['ref']), len(z.of('INVENTORY_ITEM_STATE')), len(z.of('ITEM_PLACEMENT_GROUP')),
			', '.join('%s x%d' % (game.name(k), v) for k, v in own.most_common(6))))


# ----------------------------------------------------------------------------------------
# diff

ID_RE = re.compile(r'^\d+-[^ ]*INGAME')


def norm(v):
	if isinstance(v, tuple): return tuple(round(x, 3) for x in v)
	if isinstance(v, float): return round(v, 4)
	if isinstance(v, str) and ID_RE.match(v): return '<renumbered id>'
	return v


def char_sig(c):
	"""Identity of a character across saves: its STATS record (per-character random floats, constant over time;
	athletics/xp excluded because the player trains them).  Unique among 242 chars; stable while the char exists."""
	d = c['state']['STATS'].d
	return hashlib.md5(repr(sorted((k, round(v, 2)) for k, v in d.items() if k not in ('athletics', 'xp'))).encode()).hexdigest()[:8]


def char_labels(S, ref=None):
	"""(platoon, idx) -> stable label '<platoon>/<name>#<sig>'.  Order/platoon membership is not stable between saves."""
	chars = S.characters(); lab = {}
	known = {c['name'] + char_sig(c): ref[0][(c['platoon'], c['idx'])] for c in ref[1]} if ref else {}
	for c in chars:
		k = c['name'] + char_sig(c)
		lab[(c['platoon'], c['idx'])] = known.get(k, '%s/%s#%s' % (c['platoon'], c['name'], char_sig(c)))
	return lab, chars


def flatten(S, ref=None):
	"""-> {path: value} for every stable field of the save.  ref = char_labels() result of the other save."""
	o = {}
	def put(prefix, r, skip=()):
		for k, v in r.d.items():
			if k in skip: continue
			o[prefix + '/' + k] = norm(v)
	q = S.q
	put('camera', S.camera); put('biome', S.biomes)
	for r in q.of('RESEARCH'): put('research', r)
	for r in q.of('PLATOON'): put('platoon/' + r.name, r)
	for r in q.of('GAMESTATE_FACTION'): put('faction/%s/%s' % (r.name, r.get('gamedata stringID')), r)
	for i, r in enumerate(q.of('WAR_SAVESTATE')): put('war/%s' % (r.get('faction') or i), r)
	for r in S.towns: put('town/%s:%s' % (r.get('handC'), r.name), r)
	for i, r in enumerate(q.of('GAMESTATE_TOWN_INSTANCE_LIST')[0].inst):
		o['townlist/%d' % i] = (r['ref'], tuple(round(x, 2) for x in fl(r['xform'])[:3]))
	lab, chars = char_labels(S, ref)
	for c in chars:
		pre = 'char/' + lab[(c['platoon'], c['idx'])]
		o[pre + '/pos'] = tuple(round(x, 3) for x in c['pos']); o[pre + '/rot'] = tuple(round(x, 3) for x in c['rot'])
		for t, r in c['state'].items(): put(pre + '/' + t, r)
		for j, it in enumerate(S.inventory(c)): put(pre + '/item%d' % j, it)
	for zn, z in S.zones.items():
		bl = z.of('INSTANCE_COLLECTION')[0]; seen = collections.Counter()
		for inst in bl.inst:
			if not inst['ref'] and not inst['states']: continue
			x = fl(inst['xform'])
			key = inst['id'] if 'zone' not in inst['id'] else '%s@%.0f,%.0f,%.0f' % (inst['ref'], x[0], x[1], x[2])
			seen[key] += 1
			if seen[key] > 1: key += '~%d' % seen[key]
			pre = 'zone/%s/b/%s' % (zn, key)
			o[pre + '/pos'] = tuple(round(v, 2) for v in x); o[pre + '/target'] = inst['ref']
			for sid in inst['states']:
				r = z.by_sid.get(sid)
				if not r: continue
				put(pre + '/' + r.type, r)
				if r.type == 'INVENTORY_STATE':
					for j, it in enumerate(r.inst):
						ir = z.by_sid.get(it['ref'])
						if ir: put(pre + '/item%d' % j, ir)
	return o


def diff(da, db, game, out=sys.stdout):
	A = Save(da, game); B = Save(db, game)
	w = lambda *a: print(*a, file=out)
	fa = flatten(A); fb = flatten(B, char_labels(A))
	w('DIFF %s -> %s' % (da, db))
	ca, cb = A.clock(), B.clock()
	dm = ((cb[0] - ca[0]) * 24 + cb[1] - ca[1]) * 60 + cb[2] - ca[2]
	w('clock: %s -> %s  (+%d game minutes)' % (hhmm(*ca), hhmm(*cb), dm))
	w('money: %d -> %d' % (A.money(), B.money()))
	w('quick.save lastID %d -> %d; records %d -> %d' % (A.q.lastID, B.q.lastID, len(A.q.recs), len(B.q.recs)))
	keys = set(fa) | set(fb)
	groups = collections.defaultdict(lambda: [0, 0, 0, 0])   # total, changed, added, removed
	changed = []
	for k in sorted(keys):
		g = '/'.join(k.split('/')[:1 if k.startswith(('camera', 'biome', 'research')) else 2])
		if k.startswith('char/'): g = 'char'
		if k.startswith('zone/'): g = 'zone'
		if k.startswith('platoon/'): g = 'platoon'
		if k.startswith(('town/', 'townlist/')): g = k.split('/')[0]
		if k.startswith('faction/'): g = 'faction'
		if k.startswith('war/'): g = 'war'
		gg = groups[g]; gg[0] += 1
		if k not in fa: gg[2] += 1
		elif k not in fb: gg[3] += 1
		elif fa[k] != fb[k]: gg[1] += 1; changed.append((k, fa[k], fb[k]))
	w('per group: fields total / changed / only-in-B / only-in-A')
	for g, v in sorted(groups.items()): w('  %-12s %6d %6d %6d %6d' % ((g,) + tuple(v)))
	# changed fields summarised by kind (digits collapsed)
	kinds = collections.Counter(re.sub(r'\d+', '#', re.sub(r'^(char|platoon|faction|town|war|zone)/[^/]*(/[^/]*)?', r'\1', k)) for k, _, _ in changed)
	w('changed field kinds (top 40):')
	for k, n in kinds.most_common(40): w('  %5d  %s' % (n, k))
	w('player characters / camera / platoons (changed fields):')
	for k, a, b in changed:
		if k.startswith(('camera', 'char/Nameless_0', 'platoon/Nameless_0', 'research')):
			w('  %s: %s -> %s' % (k, a, b))
	# movement
	mv = []
	for k in fa:
		if k.endswith('/pos') and k.startswith('char/') and k in fb:
			a, b = fa[k], fb[k]; dist = sum((x - y) ** 2 for x, y in zip(a, b)) ** .5
			mv.append((dist, k))
	mv.sort(reverse=True)
	still = sum(1 for d, _ in mv if d < 0.01)
	w('character movement: %d chars, %d did not move (<0.01); farthest:' % (len(mv), still))
	for d, k in mv[:8]: w('  %8.1f  %s' % (d, k))
	# weather
	wa, wb = A.weathers(), B.weathers()
	ch = [i for i in wa if wa[i].get('w') != wb.get(i, {}).get('w')]
	w('weather slots whose WEATHER id changed: %d of %d' % (len(ch), len(wa)))
	# hunger
	ha = {(c['platoon'], c['idx']): S_h for c in A.characters() if (S_h := A.hunger(c)) is not None}
	hb = {(c['platoon'], c['idx']): S_h for c in B.characters() if (S_h := B.hunger(c)) is not None}
	dh = [hb[k] - ha[k] for k in ha if k in hb]
	if dh: w('hunger (hung*100) delta: min %.2f median %.2f max %.2f over %d chars' % (min(dh), sorted(dh)[len(dh) // 2], max(dh), len(dh)))
	w('full changed list size: %d (use --all to print)' % len(changed))
	if '--all' in sys.argv:
		for k, a, b in changed: w('  %s: %s -> %s' % (k, a, b))


def main(argv):
	args = [a for a in argv[1:] if not a.startswith('--')]
	gdir = None
	for a in argv[1:]:
		if a.startswith('--game='): gdir = a[7:]
	game = Game(kenshi_install.data_dir(gdir))
	if len(args) == 2 and args[0] == 'dump': dump(args[1], game)
	elif len(args) == 3 and args[0] == 'diff': diff(args[1], args[2], game)
	else: sys.exit(__doc__)


if __name__ == '__main__':
	main(sys.argv)
