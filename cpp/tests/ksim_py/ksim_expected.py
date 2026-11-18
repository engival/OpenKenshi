#!/usr/bin/env python3
"""Merged-record spot checks for the ksim world model, via the reference kmod.py.

Usage: ksim_expected.py <kmod.py> <KENSHI_DATA_ROOT>

The four shipped files are overlaid in the same order as
kworld::kGameDataLoadOrder (gamedata.base, Newwworld.mod, Dialogue.mod,
rebirth.mod). REMOVED records and reference tombstones are applied the way
kdata::GameData::apply does.

Lines (tab-separated):
  count   TYPE NAME                    live records of one item type
  rel     NAME COUNT                  relationship pair entries
  townowner TOWN FACTION              TOWN.`faction`
  factionrel FROM TO VALUE            FACTION.`relations` v0
  squad   TEMPLATE CATEGORY TARGET V0 V1 V2
                                      SQUAD_TEMPLATE member/animals entries
"""
import importlib.util
import os
import sys

FILES = ['gamedata.base', 'Newwworld.mod', 'Dialogue.mod', 'rebirth.mod']
REMOVED = 2147483647

PREFAB_TYPES = [
	'FACTION', 'TOWN', 'SQUAD_TEMPLATE', 'CHARACTER', 'HUMAN_CHARACTER',
	'ANIMAL_CHARACTER', 'BUILDING', 'RACE', 'AI_PACKAGE',
]

# Relation name -> [(record type, reference category), ...].
RELATIONS = {
	'faction_relations': [('FACTION', 'relations')],
	'membership': [
		('SQUAD_TEMPLATE', 'faction'), ('CHARACTER', 'faction'),
		('HUMAN_CHARACTER', 'faction'), ('ANIMAL_CHARACTER', 'faction'),
	],
	'town_ownership': [('TOWN', 'faction')],
	'town_residents': [('TOWN', 'residents')],
	'town_bar_squads': [('TOWN', 'bar squads')],
	'town_roaming_squads': [('TOWN', 'roaming squads')],
	'town_default_resident': [('TOWN', 'default resident')],
	'faction_residents': [('FACTION', 'residents')],
	'faction_default_resident': [('FACTION', 'default resident')],
	'faction_bar_squads': [('FACTION', 'bar squads')],
	'faction_squads': [('FACTION', 'squads')],
	'faction_squad_default': [('FACTION', 'squad default')],
	'faction_roaming_squads': [('FACTION', 'roaming squads')],
	'squad_members': [('SQUAD_TEMPLATE', 'squad')],
	'squad_members2': [('SQUAD_TEMPLATE', 'squad2')],
	'squad_leaders': [('SQUAD_TEMPLATE', 'leader')],
	'squad_choosefrom': [('SQUAD_TEMPLATE', 'choosefrom list')],
	'squad_animals': [('SQUAD_TEMPLATE', 'animals')],
	'squad_animals2': [('SQUAD_TEMPLATE', 'animals2')],
	'race_options': [
		('CHARACTER', 'race'), ('HUMAN_CHARACTER', 'race'),
		('ANIMAL_CHARACTER', 'race'),
	],
	'ai_package_links': [('SQUAD_TEMPLATE', 'AI packages')],
	'ai_inherits': [('AI_PACKAGE', 'inherits from')],
	'ai_leads_to': [('AI_PACKAGE', 'Leads to')],
	'overrides_town': [('TOWN', 'override town')],
}

SQUAD_LISTS = ['squad', 'squad2', 'leader', 'choosefrom list', 'animals',
	'animals2']


def load(name, path):
	spec = importlib.util.spec_from_file_location(name, path)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def removed(record):
	return any(key == 'REMOVED' and value for key, value in record['bool'])


def reference_removed(entry):
	sid, v0, v1, v2 = entry
	if v0 != REMOVED:
		return False
	if v1 is None:
		return True
	return v1 == REMOVED and v2 == REMOVED


def merge(kmod, root):
	"""stringID -> {'type': name, 'refs': {category: {target: (v0,v1,v2)}}}."""
	records = {}
	for name in FILES:
		file, _ = kmod.parse(os.path.join(root, name))
		for record in file['records']:
			sid = record['sid']
			if removed(record):
				records.pop(sid, None)
				continue
			merged = records.get(sid)
			if merged is None:
				# kdata keeps the type of the first definition.
				merged = {'type': kmod.ITEMTYPES[record['type']], 'refs': {}}
				records[sid] = merged
			for category, entries in record['refs']:
				bucket = merged['refs'].setdefault(category, {})
				for entry in entries:
					target, v0, v1, v2 = entry
					if reference_removed(entry):
						bucket.pop(target, None)
					else:
						bucket[target] = (v0, v1, v2)
	return records


def category(records, type_name, category_name):
	"""Merged entries of one type/category whose target still exists."""
	out = []
	for sid, record in records.items():
		if record['type'] != type_name:
			continue
		for target, values in record['refs'].get(category_name, {}).items():
			if target in records:
				out.append((sid, target, values))
	return out


def main():
	kmod = load('ksim_ref_kmod', sys.argv[1])
	root = sys.argv[2]
	records = merge(kmod, root)

	type_counts = {}
	for record in records.values():
		type_counts[record['type']] = type_counts.get(record['type'], 0) + 1
	for type_name in PREFAB_TYPES:
		print('count', type_name, type_counts.get(type_name, 0), sep='\t')

	entries = {}
	for name, sources in RELATIONS.items():
		pairs = []
		for type_name, category_name in sources:
			pairs.extend(category(records, type_name, category_name))
		entries[name] = pairs
		print('rel', name, len(pairs), sep='\t')

	for town, faction, _ in entries['town_ownership']:
		print('townowner', town, faction, sep='\t')

	for source, target, values in entries['faction_relations']:
		print('factionrel', source, target, values[0], sep='\t')

	for source, target, values in entries['squad_members']:
		print('squad', source, 'squad', target, *values, sep='\t')
	for relation, name in [('squad2', 'squad_members2'),
			('leader', 'squad_leaders'), ('choosefrom list', 'squad_choosefrom'),
			('animals', 'squad_animals'), ('animals2', 'squad_animals2')]:
		for source, target, values in entries[name]:
			print('squad', source, relation, target, *values, sep='\t')


if __name__ == '__main__':
	main()
