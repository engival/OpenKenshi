#!/usr/bin/env python3
"""Catalogue of every BUILDING record: meshes, parts, materials, placed-instance counts, shell openness.

Usage: building_catalogue.py DATA_DIR OUT_PREFIX [--name SUBSTR]
	writes OUT_PREFIX.csv and OUT_PREFIX.md (one row per BUILDING, sorted by category then most-placed first);
	--name prints the full part/mesh listing of the BUILDING records whose name contains SUBSTR (case-insensitive).

Records are merged as the game does (building_geometry.load_records: a mod record for an existing sid is a field diff
on the base record, 'refs' merged per category, REMOVED drops). Placed count = 0-buildinglist instances over the
world zone files, per target sid (building_geometry.load_placements; the zone layers overlay by instance id).

Columns: sid, name, category, group, scale, parts, exterior meshes (the 'parts' refs' render meshes), interior meshes
('interior' refs), mask meshes, materials (the building's 'material' refs, then each part's), placed, open_edges /
open_loops (boundary edges of the exterior part meshes, vertices welded by position; a rough shell-openness number,
the part seams count too, so read it next to the mesh names, not alone), window_parts (parts whose name or mesh
has 'window'). Names and numbers only. Standard library only; needs building_geometry.py, kmod.py, ogre_mesh.py.
"""
import sys, os, csv, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod, ogre_mesh
from building_geometry import load_records, load_placements, FileIndex, field, f32, refs

def mesh_name(files, rec):
	p = field(rec, 'file', 'phs or mesh')
	return os.path.basename(p.replace('\\', '/')) if p else ''

def boundary(path, cache={}):
	"""(triangles, boundary edges, open loops) of one mesh, vertices welded by position (1/1000 unit)."""
	if path in cache:
		return cache[path]
	try:
		m = ogre_mesh.parse(path)
	except Exception:
		cache[path] = (0, 0, 0)
		return cache[path]
	ids = {}
	edges = collections.Counter()
	ntri = 0
	for a, b, c in ogre_mesh.triangles(m):
		v = [ids.setdefault(tuple(round(x * 1000) for x in p), len(ids)) for p in (a, b, c)]
		if len({*v}) < 3:
			continue
		ntri += 1
		for i in range(3):
			e = (v[i], v[(i + 1) % 3])
			edges[(min(e), max(e))] += 1
	bnd = [e for e, n in edges.items() if n == 1]
	adj = collections.defaultdict(list)
	for a, b in bnd:
		adj[a].append(b)
		adj[b].append(a)
	seen, loops = set(), 0
	for s in adj:
		if s in seen:
			continue
		loops += 1
		st = [s]
		while st:
			x = st.pop()
			if x not in seen:
				seen.add(x)
				st += adj[x]
	cache[path] = (ntri, len(bnd), loops)
	return cache[path]

def materials(recs, rec):
	out = [recs[s]['name'] for s in refs(rec, 'material') if s in recs]
	for ps in refs(rec, 'parts'):
		p = recs.get(ps)
		out += [recs[s]['name'] for s in refs(p, 'material') if s in recs] if p else []
	return list(dict.fromkeys(out))

def rows(data):
	recs = load_records(data)
	files = FileIndex(data)
	placed = collections.Counter(ref for _, _, ref, _, _ in load_placements(data))
	out = []
	for sid, r in recs.items():
		if kmod.ITEMTYPES[r['type']] != 'BUILDING':
			continue
		ext, win, op_e, op_l = [], [], 0, 0
		for ps in refs(r, 'parts'):
			p = recs.get(ps)
			if not p:
				continue
			mn = mesh_name(files, p)
			ext.append(mn)
			if 'window' in (p['name'] + mn).lower():
				win.append(p['name'])
			path = files.find(field(p, 'file', 'phs or mesh'))
			if path and path.lower().endswith('.mesh'):
				_, e, l = boundary(path)
				op_e += e
				op_l += l
		inter = [mesh_name(files, recs[s]) for s in refs(r, 'interior') if s in recs]
		mask = [mesh_name(files, recs[s]) for s in refs(r, 'interior mask') if s in recs]
		out.append({'sid': sid, 'name': r['name'], 'category': field(r, 'string', 'building category', ''),
			'group': field(r, 'string', 'building group', ''), 'scale': round(f32(field(r, 'float', 'scale')), 3),
			'parts': len(ext), 'exterior_meshes': '; '.join(dict.fromkeys(m for m in ext if m)),
			'interior_meshes': '; '.join(dict.fromkeys(m for m in inter if m)),
			'mask_meshes': '; '.join(dict.fromkeys(m for m in mask if m)),
			'materials': '; '.join(materials(recs, r)), 'placed': placed[sid],
			'open_edges': op_e, 'open_loops': op_l, 'window_parts': '; '.join(win)})
	out.sort(key=lambda x: (x['category'], -x['placed'], x['name']))
	return out

def main(argv):
	if len(argv) < 2:
		print(__doc__)
		return 2
	data, prefix = argv[0], argv[1]
	rs = rows(data)
	if '--name' in argv:
		s = argv[argv.index('--name') + 1].lower()
		rs = [r for r in rs if s in r['name'].lower()]
		for r in rs:
			print(r['sid'], r['name'])
			for k in ('exterior_meshes', 'interior_meshes', 'mask_meshes', 'materials', 'window_parts'):
				print('  %-16s %s' % (k, r[k]))
			print('  placed %d open_edges %d open_loops %d' % (r['placed'], r['open_edges'], r['open_loops']))
		return 0
	cols = list(rs[0])
	with open(prefix + '.csv', 'w', newline='') as f:
		w = csv.DictWriter(f, cols)
		w.writeheader()
		w.writerows(rs)
	with open(prefix + '.md', 'w') as f:
		tot = sum(r['placed'] for r in rs)
		f.write('# BUILDING catalogue\n\n%d BUILDING records, %d placed instances. Sorted by category, then most-placed first.\n'
			% (len(rs), tot))
		f.write('Columns as in building_catalogue.py; meshes are .mesh basenames under data/buildings. open = boundary edges / open loops of the exterior parts.\n')
		cat = None
		for r in rs:
			if r['category'] != cat:
				cat = r['category']
				n = sum(1 for x in rs if x['category'] == cat)
				f.write('\n## %s (%d)\n\n| sid | name | placed | parts | exterior meshes | interior meshes | materials | open e/l | window parts |\n|---|---|---:|---:|---|---|---|---|---|\n' % (cat or '(none)', n))
			f.write('| %s | %s | %d | %d | %s | %s | %s | %d/%d | %s |\n' % (r['sid'], r['name'].replace('|', '/'), r['placed'], r['parts'],
				r['exterior_meshes'], r['interior_meshes'], r['materials'], r['open_edges'], r['open_loops'], r['window_parts']))
	print('%d buildings -> %s.csv, %s.md' % (len(rs), prefix, prefix))
	return 0

if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
