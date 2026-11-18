#!/usr/bin/env python3
"""AI coverage facts: one row per AI taskType enum (RUNTIME_SYSTEMS "Coverage map").

Every AI goal is a `taskType` enum. This tool reports the facts side only; our side (handled, ported, partial,
checked) lives in the `kenshi_coverage` Dolt tables, written with `kcov set` / `kcov link` (kenshi-vyr.12.7):

  install data   gamedata.base + the 3 official mods (never mods.cfg): AI_PACKAGE goal rows (Leader / Squad / Squad2 /
                 Slave) resolved through `inherits from`, SQUAD_TEMPLATE `AI packages`, and the placement TSVs the world
                 model writes (town squads + active spawn sources) for the world weighting.
  kinspect       findings/ai_enum_map.tsv (one row per enum: status, resolver / task-class addresses, decoded, structs).

The helper's coverage_import runs this script and reads its Enums table (12 columns; the ours / handled / ported /
partial / checked columns stay empty) and the weighting paragraph. docs/reference/AI_COVERAGE.md is regenerated from
the tables by coverage_snapshot on master; never redirect this script into it.

Options: --kinspect PATH (default ~/git/kenshi/kenshi-inspect/findings/ai_enum_map.tsv), --world-data DIR (default the
committed world model TSVs). The install root is $KENSHI (default: the dev install).
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kmod
import kenshi_install

KENSHI = kenshi_install.install()
FILES = ['gamedata.base', 'Newwworld.mod', 'Dialogue.mod', 'rebirth.mod']
GOAL_KEYS = ['Leader AI Goals', 'Squad AI Goals', 'Squad2 AI Goals', 'Slave AI Goals']
DEFAULT_KINSPECT = os.path.expanduser('~/git/kenshi/kenshi-inspect/findings/ai_enum_map.tsv')
DEFAULT_WORLD = os.path.join(HERE, '..', 'docs', 'reference', 'world_model', 'data')


def parse_args(argv):
	kinspect = DEFAULT_KINSPECT
	world = DEFAULT_WORLD
	i = 0
	while i < len(argv):
		if argv[i] == '--kinspect' and i + 1 < len(argv):
			kinspect = argv[i + 1]
			i += 2
		elif argv[i] == '--world-data' and i + 1 < len(argv):
			world = argv[i + 1]
			i += 2
		else:
			sys.stderr.write(__doc__)
			raise SystemExit(2)
	return kinspect, world


def enum_names_from_def(path):
	"""taskType enum names from fcs_enums.def (the install carries it on the player's machine; the dev install does not)."""
	if not os.path.exists(path):
		return {}
	text = open(path, encoding='utf-8', errors='replace').read()
	start = text.index('enum taskType')
	body = text[text.index('{', start) + 1:text.index('}', start)]
	body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
	body = re.sub(r'//.*', '', body)
	names, index = {}, 0
	for part in body.split(','):
		part = part.strip()
		if not part:
			continue
		name = part.split('=')[0].strip()
		if '=' in part:
			index = int(part.split('=')[1].strip(), 0)
		names[index] = name
		index += 1
	return names


def read_kinspect(path):
	"""enum -> row, plus the machinery rows. Columns per the file's header."""
	rows, machinery = {}, []
	with open(path, encoding='utf-8') as f:
		header = f.readline().rstrip('\n').split('\t')
		for line in f:
			fields = line.rstrip('\n').split('\t')
			if len(fields) < len(header):
				fields += [''] * (len(header) - len(fields))
			row = dict(zip(header, fields))
			if row['enum'] == 'shared':
				machinery.append(row)
			else:
				rows[int(row['enum'])] = row
	return rows, machinery


class Game:
	"""The merged records of the 4 base files, limited to the types this report reads."""

	def __init__(self):
		self.items = {}
		for name in FILES:
			parsed, _ = kmod.parse(os.path.join(KENSHI, 'data', name))
			for record in parsed['records']:
				kind = kmod.ITEMTYPES[record['type']]
				if kind not in ('AI_TASK', 'AI_PACKAGE', 'SQUAD_TEMPLATE'):
					continue
				item = self.items.setdefault(record['sid'], {'type': kind, 'name': record['name'], 'int': {}, 'refs': {}})
				if record['flags'] & 2 and record['name']:
					item['name'] = record['name']
				for key, value in record['int']:
					item['int'][key] = value
				for category, refs in record['refs']:
					merged = item['refs'].setdefault(category, {})
					for entry in refs:
						if entry[1] == 0x7fffffff:
							merged.pop(entry[0], None)
						else:
							merged[entry[0]] = entry

	def of(self, kind):
		return {sid: item for sid, item in self.items.items() if item['type'] == kind}


def read_tsv(path):
	with open(path, encoding='utf-8') as f:
		header = f.readline().rstrip('\n').split('\t')
		return [dict(zip(header, line.rstrip('\n').split('\t'))) for line in f if line.strip()]


def main():
	kinspect_path, world_path = parse_args(sys.argv[1:])
	kinspect, machinery = read_kinspect(kinspect_path)
	enum_names = enum_names_from_def(os.path.join(KENSHI, 'fcs_enums.def'))
	game = Game()
	tasks = game.of('AI_TASK')
	packages = game.of('AI_PACKAGE')
	squads = game.of('SQUAD_TEMPLATE')

	# AI_TASK record -> taskType enum; several tasks share one enum.
	enum_of_task = {sid: item['int'].get('enum') for sid, item in tasks.items()}
	task_names = collections.defaultdict(list)
	for sid, enum in enum_of_task.items():
		if enum is not None:
			task_names[enum].append(tasks[sid]['name'])

	# Each package's resolved goal set: its own 4 lists plus every ancestor's (inherits from), deduplicated.
	direct = collections.Counter()   # packages whose own lists name the enum
	resolved = collections.Counter()  # packages whose own + inherited lists name the enum
	goal_sets = {}
	visiting = set()

	def resolved_goals(sid):
		if sid in goal_sets:
			return goal_sets[sid]
		if sid in visiting:
			return set()
		visiting.add(sid)
		own = set()
		for key in GOAL_KEYS:
			for task in packages[sid]['refs'].get(key, {}):
				enum = enum_of_task.get(task)
				if enum is not None:
					own.add(enum)
					direct[enum] += 1
		allgoals = set(own)
		for parent in packages[sid]['refs'].get('inherits from', {}):
			if parent in packages:
				allgoals |= resolved_goals(parent)
		visiting.discard(sid)
		goal_sets[sid] = allgoals
		return allgoals

	for sid in packages:
		for enum in resolved_goals(sid):
			resolved[enum] += 1

	# Squad templates running a package (all templates, whether placed or not).
	package_templates = collections.Counter()
	for item in squads.values():
		for package in item['refs'].get('AI packages', {}):
			package_templates[package] += 1
	template_use = collections.Counter()
	for package, count in package_templates.items():
		for enum in goal_sets.get(package, ()):
			template_use[enum] += count

	# World weighting: the squad slots the world actually places - one per town squad list entry, one per spawn source
	# active at start - each counted once per goal its packages' resolved lists contain (a goal twice in a squad is one).
	world_use = collections.Counter()
	slots = 0
	placed = []
	for name in ('town_squads.tsv', 'spawn_sources.tsv'):
		path = os.path.join(world_path, name)
		if os.path.exists(path):
			placed += read_tsv(path)
	for row in placed:
		if 'active_at_start' in row and row['active_at_start'] != 'yes':
			continue
		squad = row.get('squad_sid', '')
		if squad not in squads:
			continue
		goals = set()
		for package in squads[squad]['refs'].get('AI packages', {}):
			goals |= goal_sets.get(package, set())
		for enum in goals:
			world_use[enum] += 1
		slots += 1

	enums = sorted(set(kinspect) | set(resolved), key=lambda e: (
		kinspect.get(e, {}).get('status') == 'dead', -world_use[e], -resolved[e], e))
	live = [e for e in enums if kinspect.get(e, {}).get('status') != 'dead']
	dead = [e for e in enums if kinspect.get(e, {}).get('status') == 'dead']
	out = []
	def print_(line=''):
		out.append(line)

	print_('# AI coverage facts: taskType enums (install data + kinspect)\n')
	print_('Generated by `tools/ai_coverage.py` from the install data (`gamedata.base` + the 3 official mods) and kinspect')
	print_('`findings/ai_enum_map.tsv`. Facts only: our side lives in the `kenshi_coverage` tables (`kcov`), so the')
	print_('ours / handled / ported / partial / checked columns are empty. Input to the helper\'s coverage_import;')
	print_('docs/reference/AI_COVERAGE.md is coverage_snapshot\'s.\n')
	print_('Names are `$KENSHI/fcs_enums.def` `taskType`%s; the enum set and the exe columns come from the kinspect map.'
		% ('' if enum_names else ' (absent in this install, so kinspect\'s names stand in)'))
	print_('Order: world-weighted places first, then packages; dead enums last.\n')

	print_('## Totals\n')
	print_('| measure | count |')
	print_('|---|--:|')
	print_('| taskType enums | %d |' % len(enums))
	print_('| live | %d |' % len(live))
	print_('| dead (skipped) | %d |' % len(dead))
	print_('| live with data use (a package goal row) | %d |' % sum(1 for e in live if resolved[e]))
	print_('| live decoded (kinspect `decoded=y`) | %d |' % sum(1 for e in live if kinspect.get(e, {}).get('decoded') == 'y'))
	print_('')
	print_('World weighting: %d placed squad slots (one per town squad list entry, one per spawn source active at start; a'
		% slots)
	print_('spawn source is a placement site, not a live instance count), each counted once per goal its packages')
	print_('resolve to. Of %d (slot, goal) pairs (the world column sums to it).' % sum(world_use[e] for e in live))
	print_('By squad templates (%d distinct on a package, %d pairs; the squads column sums to it).'
		% (sum(1 for s in squads.values() if s['refs'].get('AI packages')), sum(template_use[e] for e in live)))
	print_('')

	print_('## Enums\n')
	print_('| enum | name | status | pkgs (resolved / rows) | squads | world | exe | ours | handled | ported | partial | checked |')
	print_('|--:|---|---|--:|--:|--:|---|---|---|---|---|---|')
	for enum in enums:
		row = kinspect.get(enum, {})
		names = enum_names.get(enum) or row.get('name') or '; '.join(task_names.get(enum, []))
		exe = []
		if row.get('resolver'):
			exe.append('R' + row['resolver'])
		if row.get('task_class'):
			exe.append(row['task_class'])
		if row.get('reg_pre'):
			exe.append('registry')
		# 12 columns: coverage_import reads them by position; ours .. checked are the tables' now.
		print_('| %d | %s | %s | %d / %d | %d | %d | %s |  |  |  |  |  |' % (
			enum, names, row.get('status', '?'), resolved[enum], direct[enum], template_use[enum], world_use[enum],
			' '.join(exe)))
	print_('')

	print_('## Shared machinery (not per enum)\n')
	print_('| layer | exe | decoded | structs |')
	print_('|---|---|---|---|')
	for row in machinery:
		print_('| %s | %s | %s | %s |' % (row['name'], row['resolver'] or '-', row['decoded'], row['structs'] or '-'))
	print_('')

	sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
	main()
