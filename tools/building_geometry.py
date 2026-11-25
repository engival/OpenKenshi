#!/usr/bin/env python3
"""Placed buildings -> world-space collision (or render) triangles. See docs/formats/COLLISION_FORMAT.md.

Usage: building_geometry.py DATA_DIR [--coverage] [--heights] [--zone X Y] [--obj OUT.obj]
	--coverage  collision coverage of BUILDING records and of placements
	--heights   check world y = terrain(x, z) + instance y against GAMESTATE_BUILDING 'world Y pos'
	--zone X Y  list placements in one cell; with --obj, write their world triangles as Wavefront OBJ

Chain: zone '0-buildinglist' instance -> BUILDING (target sid) -> refs 'parts' -> BUILDING_PART
	file 'xml collision' (collision) / 'phs or mesh' (render .mesh). World vertex =
	(x, terrain(x, z) + y, z) + R(qw, qx, qy, qz) * (scale * v_ogre), scale = BUILDING float 'scale'.
Needs kmod.py, terrain.py, collision.py, ogre_mesh.py next to it. Standard library only.
"""
import sys, os, re, struct, collections, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod, terrain, collision, ogre_mesh

LAYERS = ['gamedata.base', 'Newwworld.mod', 'Dialogue.mod', 'rebirth.mod']	# precedence assumed, later wins
ZONE_DIRS = ['', 'Newwworld', 'Dialogue', 'rebirth']
FIELD_KINDS = ('bool', 'float', 'int', 'vec3', 'vec4', 'string', 'file')
REMOVED = 0x7fffffff

def load_records(data):
	"""{sid: record} with mod layers overlaid field by field (refs merged per category, REMOVED drops)."""
	recs = {}
	for fn in LAYERS:
		path = os.path.join(data, fn)
		if not os.path.exists(path):
			continue
		for r in kmod.parse(path)[0]['records']:
			dst = recs.get(r['sid'])
			if dst is None:
				recs[r['sid']] = dict(r)
				continue
			for kind in FIELD_KINDS:
				m = dict(dst.get(kind, []))
				m.update(r.get(kind, []))
				dst[kind] = list(m.items())
			cats = collections.OrderedDict((c, {e[0]: e for e in l}) for c, l in dst.get('refs', []))
			for c, l in r.get('refs', []):
				d = cats.setdefault(c, {})
				for e in l:
					if e[1] == REMOVED:
						d.pop(e[0], None)
					else:
						d[e[0]] = e
			dst['refs'] = [(c, list(d.values())) for c, d in cats.items()]
			dst['name'], dst['type'] = r['name'], r['type']
	return recs

def field(rec, kind, name, default=None):
	for k, v in rec.get(kind, []):
		if k == name:
			return v
	return default

def f32(raw, default=1.0):
	return struct.unpack('<f', raw)[0] if raw else default

def refs(rec, cat):
	return [e[0] for c, l in rec.get('refs', []) if c == cat for e in l if e[1] != REMOVED]

class FileIndex:
	"""Case-insensitive lookup of record paths like '.\\data\\buildings\\x.xml'."""
	def __init__(s, data):
		s.data = data
		s.idx = {}
		for dp, dn, fn in os.walk(data):
			for f in fn:
				full = os.path.join(dp, f)
				s.idx.setdefault(os.path.relpath(full, data).replace('\\', '/').lower(), full)

	def find(s, recpath):
		p = (recpath or '').strip().replace('\\', '/').lower()
		p = re.sub(r'^(\./)?(data/)?', '', p)
		return s.idx.get(p) if p else None

def load_placements(data):
	"""[(cell, instance id, target sid, (x, y, z), (qw, qx, qy, qz))], layers overlaid per cell by instance id."""
	cells = collections.defaultdict(dict)
	root = os.path.join(data, 'newland', 'leveldata')
	for layer in ZONE_DIRS:
		d = os.path.join(root, layer)
		if not os.path.isdir(d):
			continue
		for f in sorted(os.listdir(d)):
			m = re.match(r'zone\.(\d+)\.(\d+)\.zone$', f)
			if not m:
				continue
			cell = (int(m.group(1)), int(m.group(2)))
			for r in kmod.parse(os.path.join(d, f))[0]['records']:
				if r['sid'] != '0-buildinglist':
					continue
				for inst in r['inst']:
					ref = inst['ref']
					if ref and ref.rsplit('.', 1)[-1].lower() in ('base', 'mod'):
						v = struct.unpack('<7f', inst['xform'])
						cells[cell][inst['id']] = (ref, v[:3], v[3:])
					elif not ref:
						cells[cell].pop(inst['id'], None)	# tombstone
	return [(c, i, ref, p, q) for c, insts in sorted(cells.items()) for i, (ref, p, q) in insts.items()]

def roll(*key):
	"""Deterministic stable value in [0, 1) from the key parts (no global RNG)."""
	h = hashlib.blake2b('\0'.join(map(str, key)).encode(), digest_size=8).digest()
	return int.from_bytes(h, 'little') / 2.0 ** 64

def select_parts(rec, iid, category='parts'):
	"""Part sids chosen for one placed instance (fcs.def, docs/reference/BUILDING_MATERIALS.md).
	parts ref v0 = group, v1 = chance. Group 0: each part rolled alone, v1 an absolute %.
	Group N > 0: exactly one part, weighted by v1 normalised within the group.
	RETIRED (kenshi-k16.6.583): this is not the original's rule any more. The original seeds MSVCR100 rand()
	from the placement x/z (kworld::selectBuildingParts, cpp/include/kworld/buildingparts.hpp, rwh.206);
	this stable hash of the instance id stands in only for the legacy Python tools.
	An `interior` entry with chance 0 is always placed, as the mask is (k16.6.260: s-IV's
	ramp 42261, the way up to floor 2); same rule as kworld selectParts, collision.cpp."""
	interior = category == 'interior'
	groups = collections.OrderedDict()
	for c, l in rec.get('refs', []):
		if c != category:
			continue
		for e in l:
			if e[1] != REMOVED:
				groups.setdefault(e[1], []).append((e[0], 100 if e[2] is None else e[2]))
	chosen = []
	for g, members in groups.items():
		if g == 0:
			chosen += [p for p, w in members if (interior and w == 0) or roll(iid, rec['sid'], g, p) * 100 < w]
			continue
		total = sum(max(w, 0) for _, w in members)
		if total <= 0:
			continue
		t = roll(iid, rec['sid'], g) * total
		for p, w in members:
			t -= max(w, 0)
			if t < 0:
				chosen.append(p)
				break
	return chosen

class Geometry:
	"""Per-BUILDING part-local triangles (Ogre axes, unscaled), cached per part."""
	def __init__(s, data, recs=None):
		s.recs = recs or load_records(data)
		s.files = FileIndex(data)
		s.cache = {}

	def part(s, psid):
		"""-> (triangles, kind) with kind in collision / render_only / none."""
		if psid not in s.cache:
			p = s.recs.get(psid)
			col = s.files.find(field(p, 'file', 'xml collision')) if p else None
			mesh = s.files.find(field(p, 'file', 'phs or mesh')) if p else None
			if col:
				s.cache[psid] = (collision.parse_xml(col).triangles(ogre=True), 'collision')
			elif mesh and mesh.lower().endswith('.mesh'):
				s.cache[psid] = (ogre_mesh.triangles(ogre_mesh.parse(mesh)), 'render_only')
			else:
				s.cache[psid] = ([], 'none')
		return s.cache[psid]

	def building(s, sid, iid=''):
		"""-> (triangles, {'parts': n, 'collision': n, 'render_only': n, 'none': n}, scale) for the parts
		select_parts() picks for instance iid."""
		r = s.recs.get(sid)
		tris = []
		st = collections.Counter()
		for psid in select_parts(r, iid) if r else []:
			t, kind = s.part(psid)
			st['parts'] += 1
			st[kind] += 1
			tris += t
		return tris, st, f32(field(r, 'float', 'scale')) if r else 1.0

def world_triangles(geo, hm, ref, pos, quat, iid=''):
	tris, st, scale = geo.building(ref, iid)
	origin = (pos[0], hm.height_bilinear(pos[0], pos[2]) + pos[1], pos[2])
	return collision.place(tris, origin, quat, scale)

def add_bilinear(hm):
	def height_bilinear(x, z):
		fx = (x + terrain.ORIGIN) / 18
		fz = (z + terrain.ORIGIN) / 18
		i, j = int(fx), int(fz)
		a, b = fx - i, fz - j
		h = lambda c, r: hm.raw(c, r) * terrain.HEIGHT_SCALE
		return (h(i, j) * (1 - a) + h(i + 1, j) * a) * (1 - b) + (h(i, j + 1) * (1 - a) + h(i + 1, j + 1) * a) * b
	hm.height_bilinear = height_bilinear
	return hm

def coverage(geo, placements):
	kinds = collections.Counter()
	for sid, r in geo.recs.items():
		if kmod.ITEMTYPES[r['type']] == 'BUILDING':
			kinds[classify(geo, sid)] += 1
	print('BUILDING records by geometry source:', dict(kinds))
	per = collections.Counter(classify(geo, ref) for _, _, ref, _, _ in placements)
	types = collections.defaultdict(set)
	for _, _, ref, _, _ in placements:
		types[classify(geo, ref)].add(ref)
	print('placements: %d, distinct targets: %d' % (len(placements), len({p[2] for p in placements})))
	for k, v in per.most_common():
		print('  %-28s placements %6d  building types %4d' % (k, v, len(types[k])))

def classify(geo, sid):
	r = geo.recs.get(sid)
	if r is None or kmod.ITEMTYPES[r['type']] != 'BUILDING':
		return 'missing target'
	st = collections.Counter()
	for psid in refs(r, 'parts'):
		p = geo.recs.get(psid)
		col = geo.files.find(field(p, 'file', 'xml collision')) if p else None
		mesh = geo.files.find(field(p, 'file', 'phs or mesh')) if p else None
		st['col' if col else ('mesh' if mesh and mesh.lower().endswith('.mesh') else 'none')] += 1
	if not st:
		return 'no parts'
	if st['col'] and not st['mesh'] and not st['none']:
		return 'all parts collision'
	if st['col']:
		return 'mixed collision + render'
	if st['mesh']:
		return 'render mesh only'
	return 'no geometry'

def heights(data, hm):
	"""Instance y is relative to the terrain: compare terrain + y with the saved 'world Y pos'."""
	errs = []
	root = os.path.join(data, 'newland', 'leveldata')
	for layer in ZONE_DIRS:
		d = os.path.join(root, layer)
		for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
			if not f.endswith('.zone'):
				continue
			recs = kmod.parse(os.path.join(d, f))[0]['records']
			states = {r['sid']: r for r in recs}
			for r in recs:
				if r['sid'] != '0-buildinglist':
					continue
				for inst in r['inst']:
					x, y, z = struct.unpack('<3f', inst['xform'][:12])
					for s in inst.get('states', []):
						wy = field(states.get(s, {}), 'float', 'world Y pos')
						if wy:
							errs.append(f32(wy) - (hm.height_bilinear(x, z) + y))
	errs.sort()
	n = len(errs)
	print('world Y pos - (terrain + y): n=%d p5 %.1f p25 %.1f p50 %.1f p75 %.1f p95 %.1f; |e|<20: %d' % ((n,) +
		tuple(errs[int(n * k)] for k in (.05, .25, .5, .75, .95)) + (sum(1 for e in errs if abs(e) < 20),)))

def main(argv):
	if not argv:
		print(__doc__)
		return 2
	data = argv[0]
	hm = add_bilinear(terrain.Heightmap(os.path.join(data, 'newland', 'land', 'fullmap.tif')))
	if '--heights' in argv:
		heights(data, hm)
	if '--coverage' in argv or '--zone' in argv:
		geo = Geometry(data)
		placements = load_placements(data)
		if '--coverage' in argv:
			coverage(geo, placements)
		if '--zone' in argv:
			i = argv.index('--zone')
			cell = (int(argv[i + 1]), int(argv[i + 2]))
			out = open(argv[argv.index('--obj') + 1], 'w') if '--obj' in argv else None
			nv = 0
			for c, iid, ref, pos, quat in placements:
				if c != cell:
					continue
				r = geo.recs.get(ref)
				print('%-28s %-24s %-30s %s pos=(%.1f, %.1f, %.1f)' % (iid, ref, r['name'] if r else '?', classify(geo, ref), *pos))
				if out:
					tris = world_triangles(geo, hm, ref, pos, quat, iid)
					out.write('o %s\n' % iid)
					for t in tris:
						for v in t:
							out.write('v %.3f %.3f %.3f\n' % v)
						out.write('f %d %d %d\n' % (nv + 1, nv + 2, nv + 3))
						nv += 3
	return 0

if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
