#!/usr/bin/env python3
"""zone_span_audit.py: how many 4608 u zone cells does each placed object's footprint cover?

	tools/zone_span_audit.py [--data DIR] [--list N]

Objects: every building placement (zone files, building_geometry.load_placements) and every terrain feature
(features.dat, MAP_FEATURES records). Footprint: the xz bounding box of the placed collision triangles (render mesh when a
part has no collision), as building_geometry / collision place them. Prints a histogram of spans (cells in x by cells in z)
and the N widest objects, with tools/name_site.py-style names for where they sit.

The span depends on where the box falls on the grid: a 5000 u object covers 2 or 3 cells along an axis. So the widest
extents are listed as well as the spans.
"""
import argparse, collections, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod, collision, ogre_mesh, terrain, kenshi_install
import building_geometry as bg

WORLD_MIN, ZONE = -147456.0, 4608.0

def cell(v):
	return int((v - WORLD_MIN) // ZONE)

def box(points):
	points = [p for p in points if math.isfinite(p[0]) and math.isfinite(p[2])]
	if not points:
		return None
	xs = [p[0] for p in points]
	zs = [p[2] for p in points]
	return min(xs), min(zs), max(xs), max(zs)

def local_points(tris):
	return list({v for t in tris for v in t})

def main():
	ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
	ap.add_argument('--data', default=None, help='the install data/ dir (default: kenshi_install.py finds it)')
	ap.add_argument('--list', type=int, default=15, help='widest objects to list')
	a = ap.parse_args()
	data = kenshi_install.data_dir(a.data)
	geo = bg.Geometry(data)
	objs = []	# (kind, name, sid, (x, z), (x0, z0, x1, z1), source)
	empty = collections.Counter()
	local = {}
	for c, iid, ref, pos, quat in bg.load_placements(data):
		tris, st, scale = geo.building(ref, iid)
		key = (ref, tuple(bg.select_parts(geo.recs.get(ref), iid)) if geo.recs.get(ref) else ())
		if key not in local:
			local[key] = local_points(tris)
		pts = local[key]
		if not pts:
			empty['building'] += 1
			continue
		placed = [tuple(a + b for a, b in zip(collision.quat_rotate(quat, (v[0] * scale, v[1] * scale, v[2] * scale)), (pos[0], 0, pos[2]))) for v in pts]
		r = geo.recs.get(ref)
		src = 'collision' if st['collision'] else 'render'
		objs.append(('building', r['name'] if r else '?', ref, (pos[0], pos[2]), box(placed), src))
	feats = terrain.read_features(os.path.join(data, 'newland', 'land', 'features.dat'))
	fcache = {}
	for z, lst in feats.items():
		for name, pos, scl, rot in lst:
			r = geo.recs.get(name)
			if name not in fcache:
				col = geo.files.find(bg.field(r, 'file', 'collision')) if r else None
				mesh = geo.files.find(bg.field(r, 'file', 'mesh')) if r else None
				if col:
					fcache[name] = (local_points(collision.parse_xml(col).triangles(ogre=True)), 'collision')
				elif mesh and mesh.lower().endswith('.mesh'):
					fcache[name] = (local_points(ogre_mesh.triangles(ogre_mesh.parse(mesh))), 'render')
				else:
					fcache[name] = ([], 'none')
			pts, src = fcache[name]
			if not pts:
				empty['feature'] += 1
				continue
			placed = [tuple(a + b for a, b in zip(collision.quat_rotate(rot, (v[0] * scl[0], v[1] * scl[1], v[2] * scl[2])), (pos[0], 0, pos[2]))) for v in pts]
			objs.append(('feature', r['name'] if r else name, name, (pos[0], pos[2]), box(placed), src))
	nan = collections.Counter(o[0] for o in objs if o[4] is None)
	objs = [o for o in objs if o[4] is not None]
	spans = collections.Counter()
	by_kind = collections.defaultdict(collections.Counter)
	for kind, name, sid, p, (x0, z0, x1, z1), src in objs:
		s = (cell(x1) - cell(x0) + 1, cell(z1) - cell(z0) + 1)
		s = (max(s), min(s))
		spans[s] += 1
		by_kind[kind][s] += 1
	print(f'{len(objs)} objects with geometry ({sum(1 for o in objs if o[0] == "building")} building placements, '
		f'{sum(1 for o in objs if o[0] == "feature")} features); without geometry: {dict(empty)}; only non-finite vertices: {dict(nan)}')
	print('zone cells covered (long side x short side): count  [buildings / features]')
	for s in sorted(spans):
		print(f'  {s[0]}x{s[1]}: {spans[s]:6d}  [{by_kind["building"][s]} / {by_kind["feature"][s]}]')
	print(f'widest {a.list} by the longer xz extent (zone = {ZONE:.0f} u):')
	widest = sorted(objs, key=lambda o: -max(o[4][2] - o[4][0], o[4][3] - o[4][1]))[:a.list]
	for kind, name, sid, (x, z), (x0, z0, x1, z1), src in widest:
		w, d = x1 - x0, z1 - z0
		cx, cz = cell(x1) - cell(x0) + 1, cell(z1) - cell(z0) + 1
		print(f'  {max(w, d):7.0f} u ({w:.0f} x {d:.0f})  {cx}x{cz} cells  {kind:8s} {name}  [{sid}]  at ({x:.0f}, {z:.0f}) zone {cell(x)},{cell(z)}  {src}')

if __name__ == '__main__':
	main()
