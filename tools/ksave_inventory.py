#!/usr/bin/env python3
"""Record-type inventory of a Kenshi save: `ksave_inventory.py <savedir> [--fields]`.

Counts every record type in quick.save, platoon/*.platoon and zone/*.zone (through kmod), and with --fields lists each type's field
groups (field names with digit runs collapsed to `#`, so `relationSID17` and `relation17` read `relationSID#` / `relation#`) with
the number of records that carry the group. Used by docs/reference/SAVE_IMPORT.md.
"""
import sys, os, glob, re, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod

KINDS = ('bool', 'float', 'int', 'vec3', 'vec4', 'string', 'file')


def group(name):
	return re.sub(r'\d+', '#', name)


HANDLE = ('C', 'CS', 'I', 'S', 'TYPE')


def fold_handles(groups):
	"""{(kind, group): n} -> the same with each complete five-int handle (<p>C CS I S TYPE) folded into one `<p>*` entry."""
	ints = {g for (k, g) in groups if k == 'int'}
	folded = set()
	for g in ints:
		if g.endswith('TYPE'):
			p = g[:-4]
			if all(p + x in ints for x in HANDLE): folded.add(p)
	out = collections.OrderedDict()
	for (k, g), w in sorted(groups.items()):
		p = next((p for p in folded if k == 'int' and g in [p + x for x in HANDLE]), None)
		if p is None: out[(k, g)] = w
		elif g.endswith('TYPE') and g == p + 'TYPE' : out[('handle', p + '*')] = w
	return out


def main(argv):
	args = [a for a in argv[1:] if not a.startswith('--')]
	if len(args) != 1: sys.exit(__doc__)
	d = args[0]
	where = {'quick': [os.path.join(d, 'quick.save')],
		'platoon': sorted(glob.glob(os.path.join(d, 'platoon', '*.platoon'))),
		'zone': sorted(glob.glob(os.path.join(d, 'zone', '*.zone')))}
	count = collections.defaultdict(collections.Counter)      # type -> {file kind: records}
	fields = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))   # type -> (kind, group) -> {where: records}
	refs = collections.defaultdict(collections.Counter)
	inst = collections.defaultdict(collections.Counter)
	failed = []
	for kind, paths in where.items():
		for p in paths:
			try:
				f, _ = kmod.parse(p)
			except Exception as e:
				failed.append((p, repr(e))); continue
			for r in f['records']:
				t = kmod.ITEMTYPES[r['type']]
				count[t][kind] += 1
				seen = set()
				for k in KINDS:
					for n, _v in r[k]:
						seen.add((k, group(n)))
				for key in seen: fields[t][key][kind] += 1
				for cat, lst in r['refs']:
					refs[t][group(cat)] += 1
				if r['inst']: inst[t][kind] += len(r['inst'])
	print('files: %s' % ', '.join('%s %d' % (k, len(v)) for k, v in where.items()))
	if failed:
		print('FAILED %d files' % len(failed))
		for p, e in failed: print('  ', p, e)
	print('%d record types' % len(count))
	for t in sorted(count, key=lambda t: -sum(count[t].values())):
		c = count[t]
		print('%-28s quick %6d  platoon %7d  zone %7d  (instances: %s)' % (t, c['quick'], c['platoon'], c['zone'],
			', '.join('%s %d' % kv for kv in sorted(inst[t].items())) or '-'))
		if '--fields' in argv:
			for (k, g), w in fold_handles(fields[t]).items():
				print('    %-6s %-40s %s' % (k, g, ' '.join('%s=%d' % kv for kv in sorted(w.items()))))
			for g, n in sorted(refs[t].items()): print('    refs   %-40s %d' % (g, n))


if __name__ == '__main__':
	main(sys.argv)
