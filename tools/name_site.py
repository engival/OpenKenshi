#!/usr/bin/env python3
"""name_site.py: give a world position a working name (region, map area, nearest towns).

	tools/name_site.py [--data DIR] [--towns N] X Z [X Z ...]   (install found by kenshi_install.py: --data, KENSHI / KENSHI_DATA, Steam, GOG)
	tools/name_site.py 94177 94434        # knav_ground's "zone 52.52 patch"

Region: the BIOME_GROUP whose `index` colour the areas map (data/newland/land/areasmap.tga, TERRAIN_FORMAT.md) has at the point.
Towns: the townlist instances of the leveldata layers (WORLD_LAYOUT.md "Towns"), named by their TOWN record; nearest first.
Directions are map directions: +x east, +z south (the areas map row 0 is z minimum, at the top of the in-game map).
Records are merged base -> Newwworld -> Dialogue -> rebirth, later wins (precedence not verified, WORLD_LAYOUT.md).
"""
import argparse, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod, kenshi_install

WORLD_MIN, WORLD_SIZE, ZONE = -147456.0, 294912.0, 4608.0
LAYERS = ['gamedata.base', 'Newwworld.mod', 'Dialogue.mod', 'rebirth.mod']
LEVELS = ['newland/leveldata/Newwworld/leveldata.level', 'newland/leveldata/Dialogue/leveldata.level', 'newland/leveldata/rebirth/leveldata.level']

def records(data):
	merged = {}
	for name in LAYERS:
		path = os.path.join(data, name)
		if os.path.exists(path):
			for r in kmod.parse(path)[0]['records']:
				# a later layer's record of the same sid changes the fields it carries, the rest stay (MOD_FORMAT.md)
				old = merged.get(r['sid'])
				if old is None:
					merged[r['sid']] = {'type': r['type'], 'name': r['name'], 'int': dict(r['int'])}
				else:
					old['name'] = r['name'] or old['name']
					old['int'].update(r['int'])
	return merged

def areas_map(path):
	b = open(path, 'rb').read()
	if b[2] != 2 or b[16] != 24:
		raise SystemExit(f'{path}: not an uncompressed 24-bit TGA')
	w, h = struct.unpack_from('<HH', b, 12)
	top_first = bool(b[17] & 0x20)
	first = 18 + b[0]
	def at(col, row):
		p = first + ((row if top_first else h - 1 - row) * w + col) * 3
		return (b[p + 2] << 16) | (b[p + 1] << 8) | b[p]
	return w, h, at

def towns(data, recs):
	seen = {}
	for name in LEVELS:
		path = os.path.join(data, name)
		if not os.path.exists(path):
			continue
		for r in kmod.parse(path)[0]['records']:
			for inst in r['inst']:
				town = recs.get(inst['ref'])
				if town is None or town['type'] != kmod.ITEMTYPES.index('TOWN'):
					continue
				x, y, z = struct.unpack('<3f', inst['xform'][:12])
				seen[(inst['ref'], round(x), round(z))] = (town['name'], x, z)
	return list(seen.values())

def compass(dx, dz):
	# +z is south: a bearing clockwise from north
	deg = math.degrees(math.atan2(dx, -dz)) % 360
	return ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'][int((deg + 22.5) // 45) % 8]

def map_area(x, z):
	fx, fz = (x - WORLD_MIN) / WORLD_SIZE, (z - WORLD_MIN) / WORLD_SIZE
	ns = 'north' if fz < 1 / 3 else 'south' if fz > 2 / 3 else 'middle'
	ew = 'west' if fx < 1 / 3 else 'east' if fx > 2 / 3 else 'middle'
	if ns == ew == 'middle':
		return 'centre of the map'
	if ns == 'middle':
		return f'{ew} middle of the map'
	if ew == 'middle':
		return f'{ns} middle of the map'
	return f'{ns}-{ew} of the map'

def main():
	ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
	ap.add_argument('--data', default=None, help='the install data/ dir (default: kenshi_install.py finds it)')
	ap.add_argument('--towns', type=int, default=3, help='nearest towns to list')
	ap.add_argument('coords', nargs='+', type=float, help='X Z pairs (world units)')
	a = ap.parse_args()
	if len(a.coords) % 2:
		ap.error('coords come in X Z pairs')
	a.data = kenshi_install.data_dir(a.data)
	recs = records(a.data)
	regions = {}
	for r in recs.values():
		if r['type'] == kmod.ITEMTYPES.index('BIOME_GROUP'):
			index = r['int'].get('index')
			if index is not None:
				regions[index] = r['name']
	w, h, at = areas_map(os.path.join(a.data, 'newland/land/areasmap.tga'))
	places = towns(a.data, recs)
	for k in range(0, len(a.coords), 2):
		x, z = a.coords[k], a.coords[k + 1]
		col = min(w - 1, max(0, int((x - WORLD_MIN) / WORLD_SIZE * w)))
		row = min(h - 1, max(0, int((z - WORLD_MIN) / WORLD_SIZE * h)))
		colour = at(col, row)
		region = regions.get(colour, f'unknown region #{colour:06x}')
		near = sorted(places, key=lambda t: math.hypot(t[1] - x, t[2] - z))[:a.towns]
		zone = f'{int((x - WORLD_MIN) // ZONE)},{int((z - WORLD_MIN) // ZONE)}'
		head = near[0]
		d = math.hypot(head[1] - x, head[2] - z)
		where = f'at {head[0]}' if d < 500 else f'{d / 1000:.1f} k {compass(x - head[1], z - head[2])} of {head[0]}'
		print(f'{x:.0f}, {z:.0f}: "{region}, {where}"')
		print(f'  zone {zone}; {map_area(x, z)}; region {region}')
		for name, tx, tz in near:
			print(f'  {math.hypot(tx - x, tz - z):8.0f} u  {compass(tx - x, tz - z):2s}  {name}  ({tx:.0f}, {tz:.0f})')

if __name__ == '__main__':
	main()
