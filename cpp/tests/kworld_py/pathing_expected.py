#!/usr/bin/env python3
"""Global-pathing counts from the reference global_pathing.py.

Usage: pathing_expected.py <global_pathing.py> <globalPathing.path>
Prints key/value lines consumed by tests/kworld_pathing.cpp.
"""
import collections
import importlib.util
import sys


def load(name, path):
	spec = importlib.util.spec_from_file_location(name, path)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def main():
	reference = load('kworld_ref_pathing', sys.argv[1])
	data = open(sys.argv[2], 'rb').read()
	graph = reference.parse(data)
	errors, orientation, edges, _ = reference.check(graph, len(data))
	classes = collections.Counter(value[1] for value in edges.values())
	print('size', len(data))
	print('version', graph['version'])
	print('count', graph['count'])
	print('count2', graph['count2'])
	print('trailer', *graph['trailer'])
	print('nodes', graph['count'])
	print('H', orientation['H'])
	print('V', orientation['V'])
	print('edges', len(edges))
	print('class20', classes.get(20, 0))
	print('class27', classes.get(27, 0))
	print('errors', len(errors))


if __name__ == '__main__':
	main()
