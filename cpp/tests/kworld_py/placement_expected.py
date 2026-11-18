#!/usr/bin/env python3
"""Placement totals and the COLLISION_FORMAT.md section 4 example, from kmod.py.

Usage: placement_expected.py <kmod.py> <KENSHI_DATA_ROOT>
The scale map overlays the four shipped files in documented load order.
"""
import glob
import importlib.util
import os
import struct
import sys

FILES = ['gamedata.base', 'Newwworld.mod', 'Dialogue.mod', 'rebirth.mod']


def load(name, path):
	spec = importlib.util.spec_from_file_location(name, path)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def main():
	kmod = load('kworld_ref_kmod', sys.argv[1])
	root = sys.argv[2]

	scales = {}
	for name in FILES:
		file, _ = kmod.parse(os.path.join(root, name))
		for record in file['records']:
			if kmod.ITEMTYPES[record['type']] != 'BUILDING':
				continue
			for key, raw in record['float']:
				if key == 'scale':
					scales[record['sid']] = struct.unpack('<f', raw)[0]

	placements = resolved = 0
	missing = set()
	example = None
	leveldata = os.path.join(root, 'newland', 'leveldata')
	for path in sorted(glob.glob(leveldata + '/**/*.zone', recursive=True)):
		file, _ = kmod.parse(path)
		for record in file['records']:
			for instance in record['inst']:
				x, y, z = struct.unpack('<3f', instance['xform'][:12])
				if instance['ref'] == '' or (x, y, z) == (0, 0, 0):
					continue
				placements += 1
				if instance['ref'] in scales:
					resolved += 1
				else:
					missing.add(instance['ref'])
				if instance['id'] == '2-rebirth-INGAME' and path.endswith('/Newwworld/zone.5.44.zone'):
					example = (instance['ref'], x, y, z, scales.get(instance['ref']))

	print('placements', placements)
	print('resolved', resolved)
	print('missing_placements', placements - resolved)
	print('missing_targets', len(missing))
	if example is not None:
		building, x, y, z, scale = example
		print('example_building', building)
		print('example_x', repr(x))
		print('example_y', repr(y))
		print('example_z', repr(z))
		print('example_scale', repr(scale))


if __name__ == '__main__':
	main()
