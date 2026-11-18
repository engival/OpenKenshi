#!/usr/bin/env python3
"""Throwaway parser/serializer for Kenshi .mod/.base/.save per MOD_FORMAT.md."""
import struct, sys, collections, os

ITEMTYPES = """BUILDING CHARACTER WEAPON ARMOUR ITEM ANIMAL_ANIMATION ATTACHMENT RACE LOCATION WAR_SAVESTATE FACTION
NULL_ITEM ZONE_MAP TOWN WORLDMAP_CHARACTER CHARACTER_APPEARANCE_OLD LOCATIONAL_DAMAGE COMBAT_TECHNIQUE DIALOGUE DIALOGUE_LINE
TECHTREE RESEARCH AI_TASK AI_STATE ANIMATION STATS PERSONALITY CONSTANTS BIOMES BUILDING_PART INSTANCE_COLLECTION DIALOG_ACTION
TEMPORARY_INFO MOD_FILENAME PLATOON GAMESTATE_BUILDING GAMESTATE_CHARACTER GAMESTATE_FACTION GAMESTATE_TOWN_INSTANCE_LIST STATE
SAVED_STATE INVENTORY_STATE INVENTORY_ITEM_STATE REPEATABLE_BUILDING_PART_SLOT MATERIAL_SPEC MATERIAL_SPECS_COLLECTION CONTAINER
MATERIAL_SPECS_CLOTHING GAMESTATE_BUILDING_INTERIOR VENDOR_LIST MATERIAL_SPECS_WEAPON WEAPON_MANUFACTURER SQUAD_TEMPLATE ROAD
LOCATION_NODE COLOR_DATA CAMERA MEDICAL_STATE MEDICAL_PART_STATE FOLIAGE_LAYER FOLIAGE_MESH GRASS BUILDING_FUNCTIONALITY
DAY_SCHEDULE NEW_GAME_STARTOFF GAMESTATE_CRAFTING CHARACTER_APPEARANCE GAMESTATE_AI WILDLIFE_BIRDS MAP_FEATURES DIPLOMATIC_ASSAULTS
SINGLE_DIPLOMATIC_ASSAULT AI_PACKAGE DIALOGUE_PACKAGE GUN_DATA HUMAN_CHARACTER ANIMAL_CHARACTER UNIQUE_SQUAD_TEMPLATE
FACTION_TEMPLATE AI_SCHEDULE WEATHER SEASON EFFECT ITEM_PLACEMENT_GROUP WORD_SWAPS NEST NEST_ITEM CHARACTER_PHYSICS_ATTACHMENT
LIGHT HEAD BLUEPRINT SHOP_TRADER_CLASS FOLIAGE_BUILDING FACTION_CAMPAIGN GAMESTATE_TOWN BIOME_GROUP EFFECT_FOG_VOLUME FARM_DATA
FARM_PART ENVIRONMENT_RESOURCES RACE_GROUP ARTIFACTS MAP_ITEM BUILDINGS_SWAP ITEMS_CULTURE ANIMATION_EVENT TUTORIAL CROSSBOW
TERRAIN_DECALS AMBIENT_SOUND WORLD_EVENT_STATE LIMB_REPLACEMENT ANIMATION_FILE ____XXX___ OBJECT_TYPE_MAX""".split()

class R:
	def __init__(s, b): s.b = b; s.p = 0
	def take(s, n):
		if s.p + n > len(s.b): raise EOFError(f"read {n} at {s.p} past end {len(s.b)}")
		v = s.b[s.p:s.p+n]; s.p += n; return v
	def i32(s): return struct.unpack('<i', s.take(4))[0]
	def u32(s): return struct.unpack('<I', s.take(4))[0]
	def u8(s): return s.take(1)[0]
	def f32raw(s): return s.take(4)          # keep raw bytes: NaN payloads / -0 survive
	def boolean(s):
		v = s.u8()
		if v > 1: raise ValueError(f"bool byte {v} at {s.p-1}")
		return v
	def str(s):
		n = s.i32()
		return s.take(n).decode('utf-8', 'surrogateescape') if n > 0 else ''   # a TERRAIN_DECALS instance id is a raw i32 (128 is not UTF-8)

class W:
	def __init__(s): s.a = bytearray()
	def i32(s, v): s.a += struct.pack('<i', v)
	def u32(s, v): s.a += struct.pack('<I', v)
	def u8(s, v): s.a.append(v)
	def raw(s, b): s.a += b
	def str(s, v):
		e = v.encode('utf-8', 'surrogateescape'); s.i32(len(e)); s.a += e

def read_header(r, ver):
	h = {}
	end = 0
	if ver >= 17:
		h['size'] = r.u32(); end = h['size'] + r.p
	h['version'] = r.i32()
	h['author'] = r.str(); h['description'] = r.str()
	h['dependencies'] = r.str(); h['referenced'] = r.str()
	if r.p < end:
		h['saveCounter'] = r.u32(); h['lastMergeResolve'] = r.u32()
		h['merged'] = [(r.str(), r.u32(), r.u32()) for _ in range(r.u8())]
	if r.p < end:
		h['deletes'] = []
		for _ in range(r.u8()):
			h['deletes'].append((r.str(), r.u32(), r.str()))
	if end:
		if r.p != end: raise ValueError(f"header end mismatch {r.p} != {end}")
	return h

def read_record(r, ver, fname):
	rec = {}
	rec['size'] = r.i32()
	rec['type'] = r.i32(); rec['id'] = r.i32()
	rec['name'] = r.str()
	rec['sid'] = r.str() if ver >= 7 else f"{rec['id']}-{fname}"
	if ver >= 15:
		rec['flags'] = r.u32()
	elif ver >= 11:
		rec['tags'] = [(r.str(), r.boolean()) for _ in range(r.i32())]
	rec['bool'] = [(r.str(), r.boolean()) for _ in range(r.i32())]
	rec['float'] = [(r.str(), r.f32raw()) for _ in range(r.i32())]
	rec['int'] = [(r.str(), r.i32()) for _ in range(r.i32())]
	if ver > 8:
		rec['vec3'] = [(r.str(), r.take(12)) for _ in range(r.i32())]
		rec['vec4'] = [(r.str(), r.take(16)) for _ in range(r.i32())]
	rec['string'] = [(r.str(), r.str()) for _ in range(r.i32())]
	rec['file'] = [(r.str(), r.str()) for _ in range(r.i32())]
	rec['refs'] = []
	for _ in range(r.i32()):
		cat = r.str(); lst = []
		for _ in range(r.i32()):
			if ver < 8: lst.append(r.take(8)); continue
			sid = r.str(); v0 = r.i32()
			v1 = v2 = None
			if ver >= 10: v1 = r.i32(); v2 = r.i32()
			lst.append((sid, v0, v1, v2))
		rec['refs'].append((cat, lst))
	rec['inst'] = []
	for _ in range(r.i32()):
		inst = {}
		inst['id'] = r.str() if ver >= 15 else r.i32()
		inst['ref'] = r.str() if ver >= 8 else ''
		inst['xform'] = r.take(28)     # x y z qw qx qy qz
		inst['states'] = []
		if ver > 6:
			inst['states'] = [r.str() if ver >= 15 else r.i32() for _ in range(r.i32())]
		rec['inst'].append(inst)
	return rec

def parse(path):
	b = open(path, 'rb').read()
	r = R(b); fname = os.path.basename(path)
	ver = r.i32()
	if not 1 <= ver <= 17: raise ValueError(f"bad version {ver}")
	f = {'ver': ver, 'header': read_header(r, ver) if ver >= 16 else None}
	f['lastID'] = r.i32()
	f['records'] = []
	for _ in range(r.i32()):
		start = r.p
		rec = read_record(r, ver, fname)
		rec['_len'] = r.p - start
		f['records'].append(rec)
	f['leftover'] = len(b) - r.p
	return f, b

def write(f):
	w = W(); ver = f['ver']; w.i32(ver)
	h = f['header']
	if h is not None:
		if ver >= 17:
			pos = len(w.a); w.u32(0)
		w.i32(h['version']); w.str(h['author']); w.str(h['description'])
		w.str(h['dependencies']); w.str(h['referenced'])
		if 'saveCounter' in h:
			w.u32(h['saveCounter']); w.u32(h['lastMergeResolve'])
			w.u8(len(h['merged']))
			for k, a, b in h['merged']: w.str(k); w.u32(a); w.u32(b)
		if 'deletes' in h:
			w.u8(len(h['deletes']))
			for k, v, items in h['deletes']: w.str(k); w.u32(v); w.str(items)
		if ver >= 17:
			struct.pack_into('<I', w.a, pos, len(w.a) - pos - 4)
	w.i32(f['lastID']); w.i32(len(f['records']))
	for rec in f['records']:
		w.i32(rec['size']); w.i32(rec['type']); w.i32(rec['id']); w.str(rec['name'])
		if ver >= 7: w.str(rec['sid'])
		if ver >= 15: w.u32(rec['flags'])
		elif ver >= 11:
			w.i32(len(rec['tags']))
			for k, v in rec['tags']: w.str(k); w.u8(v)
		w.i32(len(rec['bool']))
		for k, v in rec['bool']: w.str(k); w.u8(v)
		for kind in ('float', 'int', 'vec3', 'vec4', 'string', 'file'):
			if kind in ('vec3', 'vec4') and ver <= 8: continue
			w.i32(len(rec[kind]))
			for k, v in rec[kind]:
				w.str(k)
				if kind == 'int': w.i32(v)
				elif kind in ('string', 'file'): w.str(v)
				else: w.raw(v)
		w.i32(len(rec['refs']))
		for cat, lst in rec['refs']:
			w.str(cat); w.i32(len(lst))
			for e in lst:
				if ver < 8: w.raw(e); continue
				sid, v0, v1, v2 = e; w.str(sid); w.i32(v0)
				if ver >= 10: w.i32(v1); w.i32(v2)
		w.i32(len(rec['inst']))
		for inst in rec['inst']:
			if ver >= 15: w.str(inst['id'])
			else: w.i32(inst['id'])
			if ver >= 8: w.str(inst['ref'])
			w.raw(inst['xform'])
			if ver > 6:
				w.i32(len(inst['states']))
				for s in inst['states']:
					if ver >= 15: w.str(s)
					else: w.i32(s)
	return bytes(w.a)

if __name__ == '__main__':
	for path in sys.argv[1:]:
		f, b = parse(path)
		print(f"== {os.path.basename(path)}  size={len(b)} ver={f['ver']} lastID={f['lastID']} records={len(f['records'])} leftover={f['leftover']}")
		if f['header']: print("   header:", {k: v for k, v in f['header'].items()})
		print("   roundtrip byte-exact:", write(f) == b)
		c = collections.Counter(ITEMTYPES[r['type']] if 0 <= r['type'] < len(ITEMTYPES) else r['type'] for r in f['records'])
		print("   types:", dict(sorted(c.items(), key=lambda kv: -kv[1])))
		recs = f['records']
		fl = collections.Counter(hex(r.get('flags', 0)) for r in recs)
		print("   flags:", dict(fl.most_common(12)))
		sz = collections.Counter((r['size'] == r['_len']) if r['size'] else 'zero' for r in recs)
		print("   size field:", dict(sz), "distinct sizes sample:", sorted(set(r['size'] for r in recs))[:5])
		print("   fields: ", {k: sum(len(r[k]) for r in recs) for k in ('bool','float','int','vec3','vec4','string','file')},
			"refs", sum(len(l) for r in recs for _, l in r['refs']), "inst", sum(len(r['inst']) for r in recs),
			"states", sum(len(i['states']) for r in recs for i in r['inst']))
		rv = collections.Counter()
		for r in recs:
			for _, l in r['refs']:
				for e in l:
					rv['removed(MAX)' if e[1] == 0x7fffffff else 'normal'] += 1
		print("   refs:", dict(rv))
		uns = 0; tot = 0
		for r in recs:
			for k in ('bool','float','int','vec3','vec4','string','file'):
				keys = [x[0] for x in r[k]]
				tot += 1
				if keys != sorted(keys): uns += 1
		rc = sum(1 for r in recs if [c for c, _ in r['refs']] != sorted(c for c, _ in r['refs']))
		print(f"   field lists not in ordinal order: {uns}/{tot}; ref-category lists unsorted: {rc}")
		ninst_empty_ref = sum(1 for r in recs for i in r['inst'] if i['ref'] == '')
		print("   instances with empty ref (deleted):", ninst_empty_ref,
			" records with REMOVED bool:", sum(1 for r in recs if any(k == 'REMOVED' for k, _ in r['bool'])))
