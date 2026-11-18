#!/usr/bin/env python3
"""Per-zone instance counts from the reference kmod.py parser.

Usage: zone_expected.py <kmod.py> <KENSHI_DATA_ROOT>
Prints one line per zone file:
  <path relative to newland/leveldata> TAB records TAB version TAB placements
  TAB deletions TAB inventory_rows
"""
import glob
import importlib.util
import os
import struct
import sys


def load(name, path):
	spec = importlib.util.spec_from_file_location(name, path)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def main():
	kmod = load('kworld_ref_kmod', sys.argv[1])
	root = os.path.join(sys.argv[2], 'newland', 'leveldata')
	for path in sorted(glob.glob(root + '/**/*.zone', recursive=True)):
		file, _ = kmod.parse(path)
		placements = deletions = inventory = 0
		for record in file['records']:
			for instance in record['inst']:
				x, y, z = struct.unpack('<3f', instance['xform'][:12])
				if instance['ref'] == '':
					deletions += 1
				elif (x, y, z) == (0, 0, 0):
					inventory += 1
				else:
					placements += 1
		print(os.path.relpath(path, root), len(file['records']), file['ver'],
			placements, deletions, inventory, sep='\t')


if __name__ == '__main__':
	main()
