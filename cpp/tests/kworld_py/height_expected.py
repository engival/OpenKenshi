#!/usr/bin/env python3
"""Bilinear heights from the reference terrain.py TIFF reader.

Usage: height_expected.py <terrain.py> <data/newland/land> <points.txt>
Points file: one "x z" pair per line. Prints "x<TAB>z<TAB>height".
"""
import importlib.util
import math
import sys


def load(name, path):
	spec = importlib.util.spec_from_file_location(name, path)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def main():
	terrain = load('kworld_ref_terrain', sys.argv[1])
	heightmap = terrain.Heightmap(sys.argv[2] + '/fullmap.tif')
	size = heightmap.w
	# TERRAIN_FORMAT.md: 16,385 samples span 16,384 * 18 units.
	spacing = 294912.0 / (size - 1)

	with open(sys.argv[3]) as points:
		for line in points:
			fields = line.split()
			if not fields:
				continue
			x, z = map(float, fields[:2])
			fx = min(max((x + terrain.ORIGIN) / spacing, 0.0), size - 1.0)
			fz = min(max((z + terrain.ORIGIN) / spacing, 0.0), size - 1.0)
			col0, row0 = math.floor(fx), math.floor(fz)
			col1, row1 = min(col0 + 1, size - 1), min(row0 + 1, size - 1)
			tx, tz = fx - col0, fz - row0
			raw = ((heightmap.raw(col0, row0) * (1 - tx) + heightmap.raw(col1, row0) * tx) * (1 - tz)
				+ (heightmap.raw(col0, row1) * (1 - tx) + heightmap.raw(col1, row1) * tx) * tz)
			print(f'{x!r}\t{z!r}\t{raw * terrain.HEIGHT_SCALE:.17g}')


if __name__ == '__main__':
	main()
