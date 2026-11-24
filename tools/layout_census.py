#!/usr/bin/env python3
"""Layout census for the layout-keyed navmesh cache (kenshi-k16.7.148; NAVMESH_GENERATOR.md section 0).

Usage: layout_census.py DATA_DIR [--top N] [--mask] [--listing FILE]
	Walks every 0-buildinglist placement of the merged world layout (zone layers overlaid per cell by instance id, as
	building_geometry.load_placements) and counts how many share a shell key (type + destroyed) or a layout key (type +
	destroyed + the children of its interior layout: type and building-local transform, 0.01 u / 0.01 deg, gather order).
	Counts only the placements whose building has an `interior mask` (group 13) or path mode 3 (walkable).
	Children: a layout item (interiors.level) whose record is a BUILDING and not `is node` (NAVHASH.md child set). The
	"geometry key" also drops path mode 0 children (they cut and add nothing). interiors.level stores the transform in the
	building's local frame, so no re-derivation from world coordinates is needed (section 5 of the report).
	--mask counts only buildings with an `interior mask` (shops, houses; leaves out walls and bridges).
	--listing FILE writes the per-key town listing (game-derived: keep it out of the repo).
Standard library only; needs kmod.py and building_geometry.py.
"""
import sys, os, re, math, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod
from building_geometry import load_records, field, refs, ZONE_DIRS, LAYERS

def euler(q):
	w, x, y, z = q
	n = math.sqrt(w * w + x * x + y * y + z * z) or 1.0
	w, x, y, z = w / n, x / n, y / n, z / n
	if w < 0:
		w, x, y, z = -w, -x, -y, -z
	return (math.degrees(math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y))),
		math.degrees(math.asin(max(-1.0, min(1.0, 2 * (w * y - z * x))))),
		math.degrees(math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))))

def rnd(v, step):
	return round(v / step) + 0	# + 0: no -0

def child_key(item, step_u, step_d, exact):
	tid, p, q = item
	if exact:
		return (tid, p, q)
	return (tid, tuple(rnd(c, step_u) for c in p), tuple(rnd(c, step_d) for c in euler(q)))

def load_layouts(data):
	"""{layout sid: (building, [(target, (x, y, z), (qw, qx, qy, qz))])}, later layers edit earlier collections."""
	merged = {}
	for layer in ZONE_DIRS:
		path = os.path.join(data, 'newland', 'leveldata', layer, 'interiors.level')
		if not os.path.exists(path):
			continue
		for r in kmod.parse(path)[0]['records']:
			if r['type'] != kmod.ITEMTYPES.index('INSTANCE_COLLECTION'):
				continue
			b = field(r, 'string', 'associated building')
			if not b:
				continue
			cur = merged.setdefault(r['sid'], [b, {}])
			cur[0] = b
			for i in r['inst']:
				if not i['ref']:
					cur[1].pop(i['id'], None)
					continue
				v = struct.unpack('<7f', i['xform'])
				cur[1][i['id']] = (i['ref'], v[:3], v[3:])
	return {sid: (b, list(items.values())) for sid, (b, items) in merged.items()}

def load_world(data):
	"""-> placements [(cell, iid, target, destroyed, layout name)], town names per cell."""
	cells = collections.defaultdict(dict)
	root = os.path.join(data, 'newland', 'leveldata')
	GSB = kmod.ITEMTYPES.index('GAMESTATE_BUILDING')
	for layer in ZONE_DIRS:
		d = os.path.join(root, layer)
		if not os.path.isdir(d):
			continue
		for f in sorted(os.listdir(d)):
			m = re.match(r'zone\.(\d+)\.(\d+)\.zone$', f)
			if not m:
				continue
			cell = (int(m.group(1)), int(m.group(2)))
			recs = kmod.parse(os.path.join(d, f))[0]['records']
			st = {r['sid']: r for r in recs if r['type'] == GSB}
			for r in recs:
				if r['sid'] != '0-buildinglist':
					continue
				for inst in r['inst']:
					ref = inst['ref']
					if ref and ref.rsplit('.', 1)[-1].lower() in ('base', 'mod'):
						destroyed, lay = False, ''
						for s in inst['states']:
							g = st.get(s)
							if g:
								destroyed = bool(field(g, 'bool', 'destroyed', 0))
								lay = field(g, 'string', 'interior layout name', '')
						cells[cell][inst['id']] = (ref, destroyed, lay)
					elif not ref:
						cells[cell].pop(inst['id'], None)
	return [(c, i, *v) for c, insts in sorted(cells.items()) for i, v in insts.items()]

def load_towns(data, recs):
	"""{cell: [town name]} from the townlists (GAMESTATE_TOWN zone_x_k / zone_y_k)."""
	out = collections.defaultdict(set)
	for layer in ZONE_DIRS:
		path = os.path.join(data, 'newland', 'leveldata', layer, 'leveldata.level')
		if not os.path.exists(path):
			continue
		rs = kmod.parse(path)[0]['records']
		GT = kmod.ITEMTYPES.index('GAMESTATE_TOWN')
		byinst = {}
		for r in rs:
			if r['type'] == GT and field(r, 'string', 'instance'):
				byinst[field(r, 'string', 'instance')] = r
		for r in rs:
			if r['sid'] != '0-townlist':
				continue
			for i in r['inst']:
				g = byinst.get(i['id'])
				t = recs.get(i['ref'])
				if not g or not t:
					continue
				name = t['name']
				ints = dict(g['int'])
				for k in range(ints.get('zones_count', 0)):
					out[(ints.get('zone_x_%d' % k), ints.get('zone_y_%d' % k))].add(name)
	return out

def main(argv):
	if not argv:
		print(__doc__)
		return 1
	data = argv[0]
	top = int(argv[argv.index('--top') + 1]) if '--top' in argv else 25
	listing = argv[argv.index('--listing') + 1] if '--listing' in argv else None
	recs = load_records(data)
	layouts = load_layouts(data)
	placements = load_world(data)
	towns = load_towns(data, recs)
	BUILDING = kmod.ITEMTYPES.index('BUILDING')

	def isb(sid):
		r = recs.get(sid)
		return r is not None and r['type'] == BUILDING

	def flag(sid, name):
		return bool(field(recs[sid], 'bool', name, 0))

	def kids(lay, geometry):
		b = layouts.get(lay)
		out = []
		for tid, p, q in (b[1] if b else []):
			if not isb(tid) or flag(tid, 'is node'):
				continue
			if geometry and field(recs[tid], 'int', 'path mode', 2) == 0:
				continue
			out.append((tid, p, q))
		return out

	total = len(placements)
	missing = sum(not isb(p[2]) for p in placements)
	sel = []
	for p in placements:
		cell, iid, tid, destroyed, lay = p
		if not isb(tid):
			continue
		mask = bool([1 for c, l in recs[tid].get('refs', []) if c == 'interior mask' for e in l if e[1] != 0x7fffffff])
		walk = field(recs[tid], 'int', 'path mode', 2) == 3
		if (mask or walk) and (mask or '--mask' not in argv):
			sel.append((p, mask, walk))
	print('placements: %d total, %d with a BUILDING record missing' % (total, missing))
	print('with an interior mask or walkable path mode: %d (mask %d, walkable-only %d)' % (
		len(sel), sum(m for _, m, _ in sel), sum(w and not m for _, m, w in sel)))
	nlay = sum(1 for p, _, _ in sel if p[4])
	print('  of those naming an interior layout: %d; whose layout name does not join interiors.level: %d' % (
		nlay, sum(1 for p, _, _ in sel if p[4] and (p[2] + '-' + p[4]) not in layouts)))
	per = collections.Counter(p[2] for p, _, _ in sel)
	print('per building type (top 15 of %d types):' % len(per))
	for t, n in per.most_common(15):
		print('  %5d  %s (%s)' % (n, recs[t]['name'], t))

	it = [(p, m, w) for p, m, w in sel]
	terr = [x for x in it if flag(x[0][2], 'interior terrain')]
	print('interior terrain 1 (key includes the placement): %d placements, %d types' % (
		len(terr), len({x[0][2] for x in terr})))
	cand = [x for x in it if not flag(x[0][2], 'interior terrain')]
	print('cacheable (interior terrain 0): %d placements' % len(cand))

	def stats(label, keyf):
		groups = collections.defaultdict(list)
		for p, m, w in cand:
			groups[keyf(p)].append(p)
		shared = sum(len(v) for v in groups.values() if len(v) > 1)
		print('%s: %d distinct keys; %d placements share a key with at least one other (%.1f%%); largest %d' % (
			label, len(groups), shared, 100.0 * shared / max(1, len(cand)), max(map(len, groups.values()))))
		return groups

	shell = stats('shell key (type + destroyed)', lambda p: (p[2], p[3]))
	def lk(geometry, su, sd, exact=False):
		return lambda p: (p[2], p[3], tuple(child_key(c, su, sd, exact) for c in kids(p[2] + "-" + p[4], geometry))
			if p[4] else ())
	exact = stats('layout key, exact floats', lk(False, 0, 0, True))
	g = stats('layout key (0.01 u / 0.01 deg, all BUILDING children)', lk(False, 0.01, 0.01))
	gg = stats('layout key, geometry children only (path mode != 0)', lk(True, 0.01, 0.01))
	stats('  same at 0.1 u / 0.1 deg (float-noise indicator)', lk(False, 0.1, 0.1))
	stats('  type + destroyed + child type multiset (transform ignored)', lambda p: (p[2], p[3], tuple(sorted(c[0] for c in kids(p[2] + '-' + p[4], False))) if p[4] else ()))

	def tn(p):
		return ','.join(sorted(towns.get(p[0], ()))) or 'zone %d,%d' % p[0]
	out = []
	print('top %d layout keys (geometry children): count, type, destroyed, children, #towns' % top)
	for k, v in sorted(gg.items(), key=lambda kv: -len(kv[1]))[:top]:
		names = collections.Counter(tn(p) for p in v)
		print('  %4d  %s  destroyed=%d  children=%d  towns=%d' % (len(v), recs[k[0]]['name'], k[1], len(k[2]), len(names)))
		out.append((len(v), recs[k[0]]['name'], names))
	if listing:
		with open(listing, 'w') as f:
			for n, name, names in out:
				f.write('%d\t%s\t%s\n' % (n, name, '; '.join('%s x%d' % kv for kv in sorted(names.items()))))
	# distinct layout names per layout key: do copies share a layout name?
	return 0

if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
