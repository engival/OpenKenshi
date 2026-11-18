#!/usr/bin/env python3
"""Parse and validate Kenshi data/globalPathing.path (see docs/formats/GLOBAL_PATHING_FORMAT.md).

Usage: global_pathing.py PATH/TO/globalPathing.path
Exits non-zero if any structural invariant fails.
"""
import struct, sys, math, collections

ORIGIN = -147456.0
BLOCK = 18432.0		# 4 zones of 4608

def parse(b):
	ver, n, n2 = struct.unpack_from('<3I', b, 0)
	p = 12
	nodes = []
	for _ in range(n):
		nid, a, bb, na, nb = struct.unpack_from('<5I', b, p)
		x0, z0, x1, z1 = struct.unpack_from('<4f', b, p + 20)
		e, = struct.unpack_from('<I', b, p + 36)
		p += 40
		edges = [struct.unpack_from('<IfI', b, p + 12 * k) for k in range(e)]
		p += 12 * e
		nodes.append({'id': nid, 'cell': (a, bb), 'nbr': (na, nb), 'rect': (x0, z0, x1, z1), 'edges': edges})
	trailer = struct.unpack_from('<4I', b, p)
	return {'version': ver, 'count': n, 'count2': n2, 'nodes': nodes, 'trailer': trailer, 'end': p + 16}

def check(g, size):
	errs = []
	def need(cond, msg):
		if not cond: errs.append(msg)
	need(g['end'] == size, f"consumed {g['end']} of {size} bytes")
	need([n['id'] for n in g['nodes']] == list(range(1, g['count'] + 1)), 'ids not 1..N in order')
	orient = collections.Counter()
	for n in g['nodes']:
		a, bb = n['cell']
		x0, z0, x1, z1 = n['rect']
		if z1 - z0 == 1.0:
			o = 'H'
			ok = n['nbr'] == (a, bb + 1) and z0 == ORIGIN + (bb + 1) * BLOCK \
				and ORIGIN + a * BLOCK <= x0 < x1 <= ORIGIN + (a + 1) * BLOCK and x0 % 72 == 0 and x1 % 72 == 0
		elif x1 - x0 == 1.0:
			o = 'V'
			ok = n['nbr'] == (a + 1, bb) and x0 == ORIGIN + (a + 1) * BLOCK \
				and ORIGIN + bb * BLOCK <= z0 < z1 <= ORIGIN + (bb + 1) * BLOCK and z0 % 72 == 0 and z1 % 72 == 0
		else:
			o, ok = '?', False
		orient[o] += 1
		need(ok, f"node {n['id']} geometry")
	edges = {(n['id'], t): (c, k) for n in g['nodes'] for t, c, k in n['edges']}
	need(len(edges) == sum(len(n['edges']) for n in g['nodes']), 'duplicate edge targets')
	need(all(edges.get((t, s)) == v for (s, t), v in edges.items()), 'edges not symmetric')
	byid = {n['id']: n for n in g['nodes']}
	cells = lambda n: {n['cell'], n['nbr']}
	need(all(len(cells(byid[s]) & cells(byid[t])) == 1 for s, t in edges), 'edge endpoints do not share exactly one cell')
	return errs, orient, edges, byid

def main():
	b = open(sys.argv[1], 'rb').read()
	g = parse(b)
	errs, orient, edges, byid = check(g, len(b))
	print(f"size {len(b)}  header ({g['version']}, {g['count']}, {g['count2']})  trailer {g['trailer']}")
	print(f"nodes {g['count']}  orientation {dict(orient)}  directed edges {len(edges)}")
	mid = lambda n: ((n['rect'][0] + n['rect'][2]) / 2, (n['rect'][1] + n['rect'][3]) / 2)
	ratio = collections.defaultdict(list)
	for (s, t), (c, k) in edges.items():
		d = math.dist(mid(byid[s]), mid(byid[t]))
		if d:
			ratio[k].append(c / d)
	for k in sorted(ratio):
		v = sorted(ratio[k])
		print(f"class {k}: {len(v)} edges, cost/midpoint-distance median {v[len(v) // 2]:.4f}")
	for e in errs[:20]:
		print('FAIL', e)
	print('PASS' if not errs else f'FAIL ({len(errs)} errors)')
	sys.exit(1 if errs else 0)

if __name__ == '__main__':
	main()
