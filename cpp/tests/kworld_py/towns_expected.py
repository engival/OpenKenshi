#!/usr/bin/env python3
"""TOWN records and townlist placements from the reference kmod.py.

Usage: towns_expected.py <kmod.py> <KENSHI_DATA_ROOT>
Lines:
  towns TAB count
  town TAB stringID
  placement TAB layer TAB id TAB target TAB state TAB x TAB y TAB z TAB
            zones_count TAB cellX TAB cellZ TAB declared("x,y;..." or "-")
TOWN records apply REMOVED the way the merge does; the name is not emitted
because name-merge rules are version-dependent and not needed here.
"""
import glob
import importlib.util
import math
import os
import struct
import sys

ORIGIN = 147456.0
ZONE = 4608.0
FILES = ['gamedata.base', 'Newwworld.mod', 'Dialogue.mod', 'rebirth.mod']


def load(name, path):
	spec = importlib.util.spec_from_file_location(name, path)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def cell_of(x, z):
	return math.floor((x + ORIGIN) / ZONE), math.floor((z + ORIGIN) / ZONE)


def main():
	kmod = load('kworld_ref_kmod', sys.argv[1])
	root = sys.argv[2]

	towns = set()
	for name in FILES:
		file, _ = kmod.parse(os.path.join(root, name))
		for record in file['records']:
			if kmod.ITEMTYPES[record['type']] != 'TOWN':
				continue
			removed = any(key == 'REMOVED' and value for key, value in record['bool'])
			if removed:
				towns.discard(record['sid'])
			else:
				towns.add(record['sid'])
	print('towns', len(towns), sep='\t')
	for sid in sorted(towns):
		print('town', sid, sep='\t')

	leveldata = os.path.join(root, 'newland', 'leveldata')
	for path in sorted(glob.glob(leveldata + '/*/leveldata.level')):
		layer = os.path.basename(os.path.dirname(path))
		file, _ = kmod.parse(path)
		states = {}
		for record in file['records']:
			if kmod.ITEMTYPES[record['type']] == 'GAMESTATE_TOWN':
				states[record['sid']] = dict(record['int'])
		for record in file['records']:
			if record['sid'] != '0-townlist':
				continue
			for instance in record['inst']:
				x, y, z = struct.unpack('<3f', instance['xform'][:12])
				state = instance['states'][0] if instance['states'] else ''
				data = states.get(state, {})
				count = data.get('zones_count', 0)
				declared = []
				for k in range(max(count, 0)):
					cx = data.get(f'zone_x_{k}')
					cz = data.get(f'zone_y_{k}')
					if cx is not None and cz is not None:
						declared.append(f'{cx},{cz}')
				cx, cz = cell_of(x, z)
				print('placement', layer, instance['id'], instance['ref'], state,
					repr(x), repr(y), repr(z), count, cx, cz,
					';'.join(declared) if declared else '-', sep='\t')


if __name__ == '__main__':
	main()
