#!/usr/bin/env python3
"""Survey Kenshi world-layout files (see docs/formats/WORLD_LAYOUT.md).

Usage: world_survey.py KENSHI_INSTALL_DIR
Parses every data/newland/leveldata/**/*.zone and leveldata.level with kmod.py,
checks the zone-cell coordinate mapping, trailing blocks and town cells.
"""
import sys, os, glob, math, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod

ORIGIN = 147456.0
ZONE = 4608.0
T = kmod.ITEMTYPES

def cell_of(x, z):
	return math.floor((x + ORIGIN) / ZONE), math.floor((z + ORIGIN) / ZONE)

def trailing_block(f, b):
	"""Zone/leveldata trailer after the records: i32 n + n ascending unique i32. None if absent."""
	lo = f['leftover']
	if not lo:
		return None
	n, = struct.unpack_from('<i', b, len(b) - lo)
	if lo != 4 + 4 * n:
		raise ValueError(f'trailer size {lo} != 4+4*{n}')
	vals = struct.unpack_from('<%di' % n, b, len(b) - lo + 4)
	if list(vals) != sorted(set(vals)):
		raise ValueError('trailer not ascending/unique')
	return vals

def main():
	root = sys.argv[1] + '/data/newland/leveldata'
	vers = collections.Counter()
	types = collections.defaultdict(collections.Counter)
	inst = collections.Counter()
	tails = collections.Counter()
	cells = set()
	inside = outside = 0
	for fp in sorted(glob.glob(root + '/**/*.zone', recursive=True)):
		f, b = kmod.parse(fp)
		layer = os.path.relpath(os.path.dirname(fp), root)
		vers[(layer, f['ver'])] += 1
		tails['present' if trailing_block(f, b) is not None else 'absent'] += 1
		X, Y = map(int, os.path.basename(fp).split('.')[1:3])
		cells.add((X, Y))
		for r in f['records']:
			types[layer][T[r['type']]] += 1
			for i in r['inst']:
				x, y, z = struct.unpack('<3f', i['xform'][:12])
				if i['ref'] == '':
					inst['deleted'] += 1
				elif (x, y, z) == (0, 0, 0):
					inst['inventory row'] += 1
				else:
					inst['placement'] += 1
					if cell_of(x, z) == (X, Y):
						inside += 1
					else:
						outside += 1
	print('zone files by (layer, version):', dict(vers))
	print('trailing blocks:', dict(tails))
	for k, v in types.items():
		print(f'records [{k}]:', dict(v))
	print('instances:', dict(inst))
	print(f'placements inside the cell named by their file: {inside}, outside: {outside}')
	print(f'{len(cells)} distinct cells, X {min(c[0] for c in cells)}..{max(c[0] for c in cells)},'
		f' Y {min(c[1] for c in cells)}..{max(c[1] for c in cells)}')

	for fp in sorted(glob.glob(root + '/*/leveldata.level')):
		f, b = kmod.parse(fp)
		gst = {r['sid']: r for r in f['records'] if T[r['type']] == 'GAMESTATE_TOWN'}
		ok = tot = 0
		for r in f['records']:
			if r['sid'] != '0-townlist':
				continue
			for i in r['inst']:
				g = gst.get(i['states'][0]) if i['states'] else None
				d = dict(g['int']) if g else {}
				n = d.get('zones_count', 0)
				if not n:
					continue
				x, _, z = struct.unpack('<3f', i['xform'][:12])
				tot += 1
				ok += cell_of(x, z) in {(d.get(f'zone_x_{k}'), d.get(f'zone_y_{k}')) for k in range(n)}
		roads = sum(1 for r in f['records'] if T[r['type']] == 'ROAD')
		print(f'{os.path.relpath(fp, root)}: {len(f["records"])} records, {len(gst)} GAMESTATE_TOWN,'
			f' {roads} ROAD, towns inside declared cells {ok}/{tot}')

if __name__ == '__main__':
	main()
