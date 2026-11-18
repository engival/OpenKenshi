#!/usr/bin/env python3
"""Cross-check the C++ package sets and static pass against tools/dialogue.py.

Usage: dialogue_compare.py <ksim_dialogue_coverage> <KENSHI_DATA>

For a set of packages (the slice-1 ones of docs/DIALOGUE.md 8: Blister Hill "shoo civ", "Bandit thug shoo",
"Law spotting a generic crime", "starving bandits" 3781, every package rooting an EV_I_SEE_ENEMY_PLAYER
dialogue, plus Manhunters) the tool's `--package` dump must equal an independent walk of the merged records
here: the roots in registration order, each with its events, its condition count (HAS_TAG / PERSONALITY_TAG
folded out) and the four masks. The root set must also equal tools/dialogue.py's own package_dialogs().
Then the static pass (`--facts`) is compared for a few speakers.

The walk is the exe's (DIALOGUE_SELECTION_STATIC Q5): parents first, own dialogs after, a dialogue once per
event, "Unfinished" packages skipped, a package visited once.
"""
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', '..', '..', 'tools'))
exe, data_dir = sys.argv[1], sys.argv[2]
os.environ['KENSHI'] = os.path.dirname(os.path.normpath(data_dir))
import dialogue as dlg

D = dlg.Dialogue()
PACKAGES = D.by_type['DIALOGUE_PACKAGE']
DIALOGUES = D.by_type['DIALOGUE']
HAS_TAG, PERSONALITY_TAG = 23, 26
failures = []


def fail(message):
	failures.append(message)


def walk(package, visited, registry, skip=True):
	if (skip and 'Unfinished' in package.name) or package.sid in visited:
		return
	visited.add(package.sid)
	for parent in package.refs.get('inheritsFrom', {}):
		if parent in PACKAGES:
			walk(PACKAGES[parent], visited, registry, skip)
	for target, (v0, v1, v2) in package.refs.get('dialogs', {}).items():
		if target not in DIALOGUES:
			continue
		for event in (v0, v1, v2):
			if event in (0, 100) or not 0 < event < 76:
				continue
			registry.setdefault(target, [])
			if event not in registry[target]:
				registry[target].append(event)


def root_info(sid):
	"""(conds, reqPT, forbPT, reqLT, forbLT) of a DIALOGUE root, the way the compile folds them."""
	conds, masks = 0, [0, 0, 0, 0]
	for action, (v0, v1, v2) in DIALOGUES[sid].refs.get('conditions', {}).items():
		rec = D.recs.get(action)
		if rec is None or rec.type != 'DIALOG_ACTION':
			continue
		kind = rec.f.get('condition name', 0)
		if kind in (HAS_TAG, PERSONALITY_TAG):
			tag = rec.f.get('tag', -1)
			if not 0 <= tag <= 31:
				continue
			index = (0 if v0 == 1 else 1) if kind == PERSONALITY_TAG else (2 if v0 == 1 else 3)
			masks[index] |= 1 << tag
		else:
			conds += 1
	return (conds, *masks)


def expected(package, skip=True):
	visited, registry = set(), {}
	walk(package, visited, registry, skip)
	rows = []
	for sid, events in registry.items():
		conds, rp, fp, rl, fl = root_info(sid)
		rows.append('root %s events=%s conds=%d reqPT=%d forbPT=%d reqLT=%d forbLT=%d' % (
			sid, ','.join(str(e) for e in sorted(events)), conds, rp, fp, rl, fl))
	return visited, registry, rows


def static_reason(sid, facts, visited):
	"""The first failing static check of a root, in the order of DialogueBook::staticExclusion; '' = passes."""
	root = DIALOGUES[sid]
	_, rp, fp, _, _ = root_info(sid)
	if 0 <= facts['personality'] < 32:
		bit = 1 << facts['personality']
		if rp and not rp & bit:
			return 'personality not required'
		if fp & bit:
			return 'personality forbidden'

	def missing(category, value):
		listed = root.refs.get(category, {})
		return bool(listed) and bool(value) and value not in listed

	if missing('my faction', facts['faction']): return 'my faction'
	if missing('my race', facts['race']): return 'my race'
	if missing('my subrace', ''): return 'my subrace'
	if missing('is character', facts['character']): return 'is character'
	wanted = root.refs.get('has package', {})
	if wanted and not any(p in visited for p in wanted):
		return 'has package'
	for action, (v0, v1, v2) in root.refs.get('conditions', {}).items():
		rec = D.recs.get(action)
		if rec is None or rec.type != 'DIALOG_ACTION':
			continue
		kind = rec.f.get('condition name', 0)
		who = rec.f.get('who', 0)
		if who != 0:
			continue
		actual = {45: facts['female'], 79: facts['robot'], 9: facts['trader'], 19: facts['leader']}.get(kind)
		if actual is not None and actual != (v0 > 0):
			return dlg.CONDITIONS[kind]
	return ''


def run(args):
	out = subprocess.run([exe, '--data', data_dir] + args, capture_output=True, text=True)
	if out.returncode != 0:
		fail('tool failed: %s %s' % (args, out.stderr.strip()))
		return []
	return out.stdout.splitlines()


def split(lines):
	"""tool dump -> {package sid: [lines]}"""
	result, current = {}, None
	for line in lines:
		if line.startswith('package '):
			current = result.setdefault(line[len('package '):], [])
		elif current is not None:
			current.append(line)
	return result


def packages_with(*dialogue_names):
	found = []
	for name in dialogue_names:
		for sid, rec in DIALOGUES.items():
			if rec.name == name or sid == name:
				for package, _ in dlg.roots_of(D, rec):
					found.append(package.sid)
	return found


chosen = []
chosen += packages_with('3124-gamedata.base', '13509-Escapes.mod', '13643-Escapes.mod')
chosen += [sid for sid, rec in PACKAGES.items() if rec.name in ('starving bandits', 'Manhunters')]
for sid, rec in PACKAGES.items():
	if any(66 in v for v in rec.refs.get('dialogs', {}).values()):
		chosen.append(sid)
chosen = list(dict.fromkeys(chosen))
if '3781-gamedata.base' not in chosen:
	fail('3781 starving bandits is not among the chosen packages')

dumps = split(run(sum((['--package', sid] for sid in chosen), [])))
for sid in chosen:
	visited, registry, rows = expected(PACKAGES[sid])
	got = dumps.get(sid)
	if got is None:
		fail('%s missing from the tool dump' % sid)
		continue
	roots = [line for line in got if line.startswith('root ')]
	if roots != rows:
		extra = [r for r in roots if r not in rows][:3]
		lack = [r for r in rows if r not in roots][:3]
		fail('%s roots differ: tool has %d, expected %d; only in tool %s; only expected %s' % (sid, len(roots), len(rows), extra, lack))
	# package_dialogs also lists the entries with no event (the 14 of DIALOGUE_DATA 2), which the exe never registers
	own = {t for t, (evs, _) in dlg.package_dialogs(D, PACKAGES[sid]).items() if any(e.startswith('EV_') for e in evs)}
	if own != set(expected(PACKAGES[sid], False)[1]):
		fail('%s: root set differs from tools/dialogue.py package_dialogs: %d vs %d' % (sid, len(registry), len(own)))
	if 'roots %d' % len(registry) not in got:
		fail('%s: roots count line %s, expected %d' % (sid, [l for l in got if l.startswith('roots')], len(registry)))

# Manhunters: 63 effective dialogues (DIALOGUE_DATA 6)
manhunters = [sid for sid, rec in PACKAGES.items() if rec.name == 'Manhunters']
if not manhunters or len(expected(PACKAGES[manhunters[0]])[1]) != 63:
	fail('Manhunters effective count is not 63')

# Static pass: facts from the data itself so that some roots pass and some fail.
races = [next(iter(r.refs['my race'])) for r in DIALOGUES.values() if r.refs.get('my race')]
chars = [next(iter(r.refs['is character'])) for r in DIALOGUES.values() if r.refs.get('is character')]
factions = [next(iter(r.refs['in town of'])) for r in DIALOGUES.values() if r.refs.get('in town of')]
variants = [
	dict(personality=9, leader=0, female=1, robot=0, trader=0, race='', faction='', character=''),
	dict(personality=2, leader=1, female=0, robot=1, trader=1, race=races[0] if races else '', faction='', character=''),
	dict(personality=13, leader=1, female=0, robot=0, trader=0, race='', faction=factions[0] if factions else '',
		character=chars[0] if chars else ''),
	dict(personality=0, leader=1, female=0, robot=0, trader=0, race='', faction='', character=''),
]
reasons_seen = set()
for facts in variants:
	spec = ','.join('%s=%s' % (k, v) for k, v in facts.items())
	dumps = split(run(sum((['--package', sid] for sid in chosen), []) + ['--facts', spec]))
	for sid in chosen:
		visited, registry, _ = expected(PACKAGES[sid])
		got = {line[len('root '):].split(' events=')[0]: line.split('static=', 1)[1] for line in dumps.get(sid, []) if line.startswith('root ')}
		for root in registry:
			want = static_reason(root, dict(facts, personality=facts['personality'],
				female=bool(facts['female']), robot=bool(facts['robot']), trader=bool(facts['trader']), leader=bool(facts['leader'])),
				visited) or 'pass'
			reasons_seen.add(want)
			if got.get(root) != want:
				fail('static %s root %s facts %s: tool %r expected %r' % (sid, root, spec, got.get(root), want))
print('packages compared: %d; static outcomes seen: %s' % (len(chosen), sorted(reasons_seen)))
if failures:
	for message in failures[:20]:
		print('FAIL', message)
	sys.exit(1)
