#!/usr/bin/env python3
"""Kenshi dialogue data: load the merged records, resolve a DIALOGUE tree, print it, dump stats.

Usage: dialogue.py tree SID_OR_NAME [--lines-only]   print one DIALOGUE tree (sid, or an exact / substring name)
       dialogue.py package NAME_OR_SID               one DIALOGUE_PACKAGE: inherits, event slots, root dialogues with conditions
       dialogue.py stats                             census counts (record types, fields, conditions, actions, events)
       dialogue.py fields TYPE                       per field: kind, count, value range (TYPE = DIALOGUE, DIALOGUE_LINE, ...)
       dialogue.py refs TYPE                         per reference list: count, target types, v0/v1/v2 ranges
       dialogue.py events                            events -> dialogues rooted per EV_* (via DIALOGUE_PACKAGE `dialogs`)
       dialogue.py find TEXT                         DIALOGUE / LINE records whose name or text contains TEXT
Env: KENSHI (install dir). Standard library only. See docs/reference/DIALOGUE_DATA.md.

Merge: gamedata.base -> Newwworld.mod -> Dialogue.mod -> rebirth.mod. Later fields override per key, reference lists
merge per (category, target sid), v0=v1=v2=0x7fffffff removes an entry, a record with bool REMOVED is dropped.
Enum tables below are copied from the FCS decompile (reference/fcs/forgotten_construction_set/*Enum.cs), whose
numbering is the one the data stores (EV_NONE = 0 first).
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod
import kenshi_install

KENSHI = kenshi_install.install()
DATA = KENSHI + '/data/'
FILES = ['gamedata.base', 'Newwworld.mod', 'Dialogue.mod', 'rebirth.mod']
GONE = 0x7fffffff

EVENTS = """EV_NONE EV_PLAYER_TALK_TO_ME EV_ANNOUNCEMENT EV_I_SEE_NEUTRAL_SQUAD EV_I_SEE_RAGDOLL EV_______ EV_SOUND_THE_ALARM EV_I_________
EV_THIEF_CAUGHT_STEALING_FROM_ME EV_SHOO_FROM_MY_BUILDING EV_MARKED_FOR_DEATH EV_SCREAMING_TORTURE EV_BAR_TALK EV_UNLOCK_MY_CAGE_OR_SHACKLES
EV_UNLOCK_MY_CAGE_ATTEMPT EV_I_DEFEATED_SQUAD EV_LAUNCH_ATTACK EV_INTRUDER_FOUND EV_HEALING_OTHER_START EV_BEING_HEALED_START
EV_HEALING_OTHER_FINISHED EV_BEING_HEALED_FINISHED EV_FIRSTAID_KIT_EMPTY EV_GET_UP_PEACE EV_GET_UP_FIGHT EV_GET_UP_UNNECCESSARY_FIGHT
EV_HARRASSMENT_SHOUTS EV_I_SEE_ANIMAL_SQUAD EV_SPEECH_INTERRUPTED_ATTACKED_BY_TARGET EV_SPEECH_INTERRUPTED_ATTACKED_BY_STRANGERS
EV_CONTRACT_JOB_ENDED EV_BETRAYAL EV_LOOTING_WEAPON_ONLY EV_LOOTING_EVERYTHING EV_I_SEE_UNIFORM_IMPOSTER EV_INTRODUCING_NEW_SLAVE
EV_ESCAPING_SLAVE_SPOTTED EV_RECAPTURED_A_SLAVE EV_SHOUT_AT_SLAVE_WORKER EV_SLAVE_DELIVERY EV_ESCAPED_EX_SLAVE_SPOTTED
EV_WITNESS_GENERIC_ASSAULT EV_WITNESS_LOOTING_ALLY EV_WITNESS_THIEF_OR_LOCKPICK EV_BOUNTY_SPOTTED EV_ESCAPED_PRISONER_SPOTTED
EV_PRISONER_FREE_TO_GO EV_ALMOST_WOKE_UP EV_ENTER_BIOME EV_ENTER_TOWN EV_SQUAD_BROKEN EV_BOUGHT_ME_FROM_SLAVERY
EV_EATING_SOMETHING_SOUNDS EV_WORSHIPING_SOMETHING EV_SLAVE_ESCAPE_OPPORTUNITY_SAVIOR EV_SLAVE_ESCAPE_OPPORTUNITY_ALONE
EV_ASSASSINATION_FAILED EV_EATING_MY_CROPS EV_KIDNAPPING_MY_ALLY EV_USING_MY_TRAINING_EQUIPMENT EV_GIVE_UP_CHASE EV_ACID_FEET
EV_ACID_RAIN EV_ACID_WATER EV_WINDY EV_POISON_GAS EV_I_SEE_ENEMY_PLAYER EV_I_SEE_ALLY_PLAYER EV_I_SEE_ILLEGAL_PLAYER_BUILDING
EV_BURNING EV_LOST_LEG EV_LOST_ARM EV_I_SEE_PLAYER_NICE_BUILDING EV_TAKEN_OVER_PLAYER_TOWN EV_CROWD_TRIGGERED EV_MAX""".split()

CONDITIONS = """DC_NONE DC_RELATIONS DC_PLAYERMONEY DC_REPUTATION DC_CARRYING_BOUNTY_ALIVE DC_CARRYING_BOUNTY_DEAD DC_FACTION_VARIABLE
DC_IMPRISONED_BY_TARGET DC_IMPRISONED_BY_OTHER DC_IS_A_TRADER DC_FACTION_RANK DC_BUILDING_IS_CLOSED_AND_SECURED DC_PLAYER_TECH_LEVEL
DC_NUM_DIALOG_EVENT_REPEATS DC_IS_IMPRISONED DC_IMPRISONMENT_IS_DEATHROW DC_TARGET_IN_TALKING_RANGE DC_IN_MY_BUILDING
DC_TARGET_LAST_SEEN_X_HOURS_AGO DC_IS_LEADER DC_MET_TARGET_BEFORE DC_WEAKER_THAN_ME DC_STRONGER_THAN_ME DC_HAS_TAG DC_IS_ALLY DC_IS_ENEMY
DC_PERSONALITY_TAG DC_BROKEN_LEG DC_BROKEN_ARM DC_DAMAGED_HEAD DC_NEARLY_KO DC_IN_A_NON_PLAYER_TOWN DC_IS_RUNNING DC_COPS_AROUND
NULL_NULL_____DC_TARGET_SQUAD_SIZE DC_SQUAD_SIZE DC_IS_PLAYER DC_NUM_BACKPACKS DC_SQUAD_ONLY_ANIMALS DC_IS_OUTNUMBERED
DC_BOUNTY_AMOUNT_PERCEIVED DC_IS_KO DC_IS_NEARLY_KO DC_SQUAD_IS_DOWN DC_IS_DEAD DC_IS_FEMALE DC_CARRYING_SOMEONE_TO_ENSLAVE
DC_BOUNTY_AMOUNT_ACTUAL DC_IM_UNARMED DC_TOWN_HAS_FORTIFICATIONS_WALLS DC_TARGET_IS_MY_MISSION_TARGET DC_MY_MISSION_IS_FRIENDLY
DC_I_LOVE_THIS_GUY DC_I_HATE_THIS_GUY DC_I_SHOULD_SCREW_THIS_GUY_OVER DC_I_SHOULD_HELP_THIS_GUY DC_IN_COMBAT DC_WITHIN_TOWN_WALLS
DC_TOWN_WALLS_LOCKED_UP DC_IS_SLAVE DC_HAS_A_BASE_NEARBY DC_TARGET_IS_SLAVE_OF_MY_FACTION DC_IS_ESCAPED_SLAVE DC_IS_IN_LOCKED_CAGE
DC_WEARING_LOCKED_SHACKLES DC_IS_SAME_RACE_AS_ME DC_CAN_AFFORD_BOUNTY DC_IS_SNEAKING DC_IS_INDOORS DC_HAS_ILLEGAL_ITEM
DC_USING_MY_TRAINING_EQUIPMENT DC_STARVING DC_MIXED_GENDER_GROUP DC_TOWN_LEVEL_CURRENT_LOCATION DC_PLAYERS_BEST_TOWN_LEVEL
DC_IN_A_PLAYER_TOWN DC_TARGET_CHARACTER_EXISTS DC_IS_RECRUITABLE DC_HAS_AI_CONTRACT DC_HAS_ROBOT_LIMBS DC_END""".split()

ACTIONS = """DA_NONE DA_TRADE DA_TALK_TO_LEADER DA_JOIN_SQUAD_WITH_EDIT DA_AFFECT_RELATIONS DA_AFFECT_REPUTATION DA_ATTACK_CHASE_FOREVER
DA_GO_HOME DA_TAKE_MONEY DA_GIVE_MONEY DA_PAY_BOUNTY DA_CHARACTER_EDITOR DA_FORCE_SPEECH_TIMER DA_DECLARE_WAR DA_END_WAR DA_CLEAR_AI
DA_FOLLOW_WHILE_TALKING DA_THUG_HUNTER DA_JOIN_SQUAD_FAST DA_REMEMBER_CHARACTER DA_FLAG_TEMP_ALLY DA_FLAG_TEMP_ENEMY DA_MATES_KILL_ME
DA_MAKE_TARGET_RUN_FASTER DA_GIVE_TARGET_MY_SLAVES DA_TAG_ESCAPED_SLAVE DA_FREE_TARGET_SLAVE DA_MERGE_WITH_SIMILAR_SQUADS
DA_SEPARATE_TO_MY_OWN_SQUAD DA_ARREST_TARGET DA_ARREST_TARGETS_CARRIED_PERSON DA_ATTACK_TOWN DA_ASSIGN_BOUNTY DA_CRIME_ALARM DA_RUN_AWAY
DA_INCREASE_FACTION_RANK DA_LOCK_THIS_DIALOG DA_ASSAULT_PHASE DA_RETREAT_PHASE DA_VICTORY_PHASE DA_ENSLAVE_TARGETS_CARRIED_PERSON
CHOOSE_SLAVES_SELLING CHOOSE_SLAVES_BUYING CHOOSE_PRISONER_BAIL CHOOSE_CONSCRIPTION CHOOSE_RECRUITING CHOOSE_HIRING_CONTRACT
SURRENDER_NON_HUMANS CHOOSE_ANIMALS_BUYING DA_CLEAR_BOUNTY DA_PLAYER_SELL_PRISONERS DA_PLAYER_SURRENDER_MEMBER_DIFFERENT_RACE
DA_SUMMON_MY_SQUAD DA_REMOVE_SLAVE_STATUS DA_OPEN_NEAREST_GATE DA_ATTACK_STAY_NEAR_HOME DA_MASSIVE_ALARM DA_ATTACK_IF_NO_COEXIST
DA_KNOCKOUT DA_END""".split()

TALKERS = "T_ME T_TARGET T_TARGET_IF_PLAYER T_INTERJECTOR1 T_INTERJECTOR2 T_INTERJECTOR3 T_WHOLE_SQUAD T_TARGET_WITH_RACE".split()
COMPARE = ['==', '<', '>']	# CE_EQUALS CE_LESS_THAN CE_MORE_THAN; ConditionControl.methodToInt: "<" 1, ">" 2
REPEAT = "DR_NO_LIMIT DR_SHORT_2 DR_MEDIUM_6 DR_LONG_48 DR_LONG_1WEEK DR_ONCE_ONLY DR_VSHORT_020".split()
LT_TAGS = "LT_NONE LT_MY_INTRUDER LT_MY_LIFESAVER LT_FREED_ME LT_STOLE_FROM_ME LT_MY_CAPTOR LT_FRIENDLY_AQUAINTANCE LT_DEFEATED_MY_SQUAD_ONCE LT_SQUAD_LOST_TO_ME_ONCE LT_KILLED_MY_FRIEND LT_I_SCREWED_THIS_GUY LT_MAX".split()
PT_TAGS = "PT_NONE PT_HONORABLE PT_TRAITOROUS PT___ PT__ PT_SMART PT_DUMB PT____ PT_____ PT_BRAVE PT_FEARFUL PT_WARM_KIND PT_COLD_CRUEL PT_NORMAL PT_MANIACAL PT_END".split()
OBJ_TYPES = "OT_NONE OT_LAW_ENFORCEMENT OT_MILITARY OT_TRADER OT_CIVILIAN OT_DIPLOMAT OT_SLAVE OT_SLAVER OT_BANDIT OT_ADVENTURER OT_END".split()

def ename(table, v):
	return table[v] if isinstance(v, int) and 0 <= v < len(table) else str(v)

# ---------------------------------------------------------------- loader
class Rec:
	"""One merged record. f = {key: value} for bool/float/int/vec3/vec4/string/file (kinds kept in k), refs = {cat: {sid: (v0,v1,v2)}}."""
	__slots__ = ('sid', 'type', 'name', 'f', 'k', 'refs', 'files', 'order')

def load(files=FILES, data=DATA):
	"""-> (recs {sid: Rec}, removed {sid: (name, file)}). Insertion order of `recs` is first-definition order."""
	recs, removed = {}, {}
	for fn in files:
		path = data + fn
		if not os.path.exists(path):
			continue
		for r in kmod.parse(path)[0]['records']:
			sid = r['sid']
			x = recs.get(sid)
			if x is None:
				x = recs[sid] = Rec()
				x.sid = sid; x.f = {}; x.k = {}; x.refs = {}; x.files = []
				x.order = len(recs)
			x.type = kmod.ITEMTYPES[r['type']]
			if r['name']: x.name = r['name']
			elif not hasattr(x, 'name'): x.name = ''
			x.files.append(fn)
			for kind in ('bool', 'float', 'int', 'vec3', 'vec4', 'string', 'file'):
				for key, v in r[kind]:
					if kind == 'float': v = struct.unpack('<f', v)[0]
					elif kind in ('vec3', 'vec4'): v = struct.unpack('<%df' % (len(v) // 4), v)
					elif kind == 'bool': v = bool(v)
					x.f[key] = v; x.k[key] = kind
			for cat, lst in r['refs']:
				d = x.refs.setdefault(cat, {})
				for e in lst:
					if e[1] == GONE: d.pop(e[0], None)
					else: d[e[0]] = (e[1], e[2], e[3])
			if x.f.get('REMOVED'):
				removed[sid] = (x.name, fn, x.type)
				del recs[sid]
	return recs, removed

class Dialogue:
	"""Merged data plus the dialogue-specific indexes."""
	def __init__(s):
		s.recs, s.removed = load()
		s.by_type = collections.defaultdict(dict)
		for sid, r in s.recs.items():
			s.by_type[r.type][sid] = r
		s.used_by = collections.defaultdict(list)	# target sid -> [(source sid, category)]
		for sid, r in s.recs.items():
			for cat, d in r.refs.items():
				for t in d:
					s.used_by[t].append((sid, cat))

	def find(s, key, types=('DIALOGUE',)):
		"""sid, exact name, or unique-ish substring of the name -> Rec (or None)."""
		if key in s.recs: return s.recs[key]
		pool = [r for t in types for r in s.by_type[t].values()]
		for test in (lambda r: r.name == key, lambda r: r.name.lower() == key.lower(), lambda r: key.lower() in r.name.lower()):
			hit = [r for r in pool if test(r)]
			if hit: return hit
		return None

	def target(s, sid):
		return s.recs.get(sid)

	def lines_of(s, rec):
		"""Child lines in file order -> [Rec]; dangling ids are returned as the bare sid string."""
		return [s.recs.get(t, t) for t in rec.refs.get('lines', {})]

	def condition(s, sid, v0):
		"""A `conditions` entry -> dict(cond, who, cmp, value, tag, extra) or None when the target is not a DIALOG_ACTION."""
		c = s.recs.get(sid)
		if c is None or c.type != 'DIALOG_ACTION':
			return None
		cmp = c.f.get('compare by', 0)
		if isinstance(cmp, str): cmp = {'<': 1, '>': 2}.get(cmp, 0)
		return dict(cond=ename(CONDITIONS, c.f.get('condition name')), who=ename(TALKERS, c.f.get('who', 0)),
			cmp=ename(COMPARE, cmp), value=v0, tag=c.f.get('tag'), stringvar=c.f.get('stringvar'))

	def effect(s, sid, v0):
		a = s.recs.get(sid)
		if a is None or a.type != 'DIALOG_ACTION':
			return None
		return dict(action=ename(ACTIONS, a.f.get('action name')), value=v0, who=ename(TALKERS, a.f.get('who', 0)))

	def events_of(s, v):
		return [ename(EVENTS, e) for e in v if e]

# ---------------------------------------------------------------- tree printing
LINE_FLAGS = ('interjection', 'fail')
LINE_REFS_ACTION = ('AI contract', 'change AI', 'interrupt', 'trigger campaign', 'lock campaign', 'unlock campaign', 'change relations',
	'crowd trigger', 'give item', 'locks', 'unlocks', 'unlock but keep me')
LINE_REFS_COND = ('has package', 'in town of', 'is character', 'my faction', 'my race', 'my subrace', 'no target race',
	'target carrying character', 'target faction', 'target has item', 'target has item type', 'target race', 'world state')

def score(r):
	"""FCS conversation.calculateScore, minus the unused singular `condition` block (no data carries it): the number after '+' in tree labels."""
	n = r.f.get('score bonus', 0)
	if r.f.get('locked'): n += 2
	if r.f.get('chance permanent', 100) < 99: n += 1
	if r.f.get('interjection'): n += 2
	sp = r.f.get('speaker', 0)
	if sp != 0: n += 1
	if 3 <= sp <= 5: n += 1
	return n

def label(r, root=False):
	"""Editor-style node label: [C][E][ntexts]+score. C = has `conditions`, E = has `effects` or any action-type list, ntexts = non-empty text variants when > 1."""
	c = len(r.refs.get('conditions', {})) > 0
	if root:
		t = 'C' if c else ''
	else:
		c = c or any(r.refs.get(k) for k in LINE_REFS_COND)
		e = bool(r.refs.get('effects')) or any(r.refs.get(k) for k in LINE_REFS_ACTION)
		t = ('C' if c else '') + ('E' if e else '')
		nt = sum(1 for k, v in r.f.items() if k.startswith('text') and k[4:].isdigit() and v)
		if nt > 1: t += (' ' if t else '') + str(nt)
	sc = score(r)
	if sc: t += '+%d' % sc
	return '[%s]' % t if t else ''

def nm(d, sid):
	r = d.recs.get(sid)
	return '%s "%s"' % (sid, r.name) if r else '%s (MISSING)' % sid

def describe_node(d, r, ind, out, seen):
	"""One DIALOGUE or DIALOGUE_LINE node and (recursively) its `lines`."""
	pad = '\t' * ind
	head = []
	for key in ('speaker', ):
		if key in r.f: head.append(ename(TALKERS, r.f[key]))
	for key in LINE_FLAGS:
		if r.f.get(key): head.append(key.upper())
	for key, v in sorted(r.f.items()):
		if key in ('speaker', 'REMOVED') or key in LINE_FLAGS or key.startswith('text'): continue
		if key == 'repetition limit': v = ename(REPEAT, v)
		elif key == 'target is type': v = ename(OBJ_TYPES, v)
		head.append('%s=%s' % (key, round(v, 3) if isinstance(v, float) else v))
	out.append('%s%s %s %s%s' % (pad, label(r, ind == 0), r.sid, ' "%s"' % r.name if r.name else '', ('  ' + ' '.join(head) if head else '')))
	texts = [r.f[k] for k in sorted(r.f) if k.startswith('text') and r.f[k]]
	for t in texts:
		out.append('%s\t"%s"' % (pad, t.replace('\n', ' / ')))
	for sid, (v0, v1, v2) in r.refs.get('conditions', {}).items():
		c = d.condition(sid, v0)
		if c:
			extra = ''
			if c['tag'] not in (None, 0, -1):
				extra = ' tag=%s' % (ename(LT_TAGS, c['tag']) if c['cond'] == 'DC_HAS_TAG' else ename(PT_TAGS, c['tag']) if c['cond'] == 'DC_PERSONALITY_TAG' else c['tag'])
			if c['stringvar']: extra += ' stringvar=%r' % c['stringvar']
			out.append('%s\tIF %s %s %s %s%s' % (pad, c['who'], c['cond'], c['cmp'], c['value'], extra))
		else:
			out.append('%s\tIF (not a DIALOG_ACTION) %s v=(%d,%d,%d)' % (pad, nm(d, sid), v0, v1, v2))
	for cat in LINE_REFS_COND:
		for sid, v in r.refs.get(cat, {}).items():
			out.append('%s\tIF %s %s v=%s' % (pad, cat, nm(d, sid), v))
	for sid, (v0, v1, v2) in r.refs.get('effects', {}).items():
		e = d.effect(sid, v0)
		out.append('%s\tDO %s' % (pad, '%s %s' % (e['action'], v0) if e else '(not a DIALOG_ACTION) ' + nm(d, sid)))
	for cat in LINE_REFS_ACTION:
		for sid, v in r.refs.get(cat, {}).items():
			out.append('%s\tDO %s %s v=%s' % (pad, cat, nm(d, sid), v))
	known = set(('lines', 'conditions', 'effects') + LINE_REFS_COND + LINE_REFS_ACTION)
	for cat, dd in r.refs.items():
		if cat not in known:
			for sid, v in dd.items():
				out.append('%s\t?%s %s v=%s' % (pad, cat, nm(d, sid), v))
	for ch in d.lines_of(r):
		if isinstance(ch, str):
			out.append('%s\t-> MISSING %s' % (pad, ch))
		elif ch.sid in seen:
			out.append('%s\t-> (again) %s' % (pad, ch.sid))
		else:
			seen.add(ch.sid)
			describe_node(d, ch, ind + 1, out, seen)

def roots_of(d, r):
	"""Which packages root this dialogue under which events; who owns those packages."""
	res = []
	for src, cat in d.used_by.get(r.sid, []):
		s = d.recs[src]
		if s.type == 'DIALOGUE_PACKAGE' and cat == 'dialogs':
			res.append((s, d.events_of(s.refs['dialogs'][r.sid])))
	return res

def package_dialogs(d, p, seen=None):
	"""Effective (sid -> (events, owner package)) of a package including inheritsFrom (own entries win over inherited ones). Merge rule is an assumption."""
	seen = seen or set()
	out = {}
	if p.sid in seen: return out
	seen.add(p.sid)
	for t in p.refs.get('inheritsFrom', {}):
		q = d.recs.get(t)
		if q: out.update(package_dialogs(d, q, seen))
	for t, v in p.refs.get('dialogs', {}).items():
		out[t] = (d.events_of(v), p.sid)
	return out

def print_package(d, key):
	hit = d.find(key, ('DIALOGUE_PACKAGE', ))
	for p in (hit if isinstance(hit, list) else [hit] if hit else []):
		print('%s "%s" tag=%r files=%s' % (p.sid, p.name, p.f.get('tag', ''), p.files))
		for t in p.refs.get('inheritsFrom', {}): print('  inheritsFrom', nm(d, t))
		for t, v in p.refs.get('dialogs', {}).items():
			r = d.recs.get(t)
			print('  %s %-40s %s %s' % (v, ','.join(d.events_of(v)), label(r, True) if r else '', nm(d, t)))
			if r:
				for sid, (v0, v1, v2) in r.refs.get('conditions', {}).items():
					c = d.condition(sid, v0)
					if c: print('\t\t%s %s %s %s tag=%s' % (c['who'], c['cond'], c['cmp'], c['value'], c['tag']))
		eff = package_dialogs(d, p)
		print('  effective dialogs incl. inherited: %d (own %d)' % (len(eff), len(p.refs.get('dialogs', {}))))

def print_tree(d, key):
	hit = d.find(key, ('DIALOGUE', ))
	if hit is None:
		print('no DIALOGUE matches', key); return
	for r in (hit if isinstance(hit, list) else [hit]):
		out = []
		describe_node(d, r, 0, out, {r.sid})
		print('\n'.join(out))
		for p, evs in roots_of(d, r):
			print('  in package %s "%s" under %s' % (p.sid, p.name, evs))
		for src, cat in d.used_by.get(r.sid, []):
			if d.recs[src].type != 'DIALOGUE_PACKAGE':
				print('  also referenced by %s via %s' % (nm(d, src), cat))
		print()

# ---------------------------------------------------------------- stats
def num_range(vals):
	vals = [v for v in vals if isinstance(v, (int, float))]
	return (min(vals), max(vals)) if vals else None

def cmd_fields(d, T):
	by = collections.defaultdict(list)
	kind = {}
	for r in d.by_type[T].values():
		for k, v in r.f.items():
			by[k].append(v); kind[k] = r.k[k]
	print('%s: %d records' % (T, len(d.by_type[T])))
	for k in sorted(by):
		vs = by[k]
		c = collections.Counter(vs if kind[k] != 'string' else [])
		rng = num_range(vs) if kind[k] in ('int', 'float') else None
		extra = 'range %s' % (rng,) if rng else ''
		if kind[k] == 'bool': extra = 'true %d false %d' % (sum(vs), len(vs) - sum(vs))
		elif kind[k] == 'int' and len(c) <= 12: extra += ' values %s' % dict(sorted(c.items(), key=str))
		elif kind[k] == 'string': extra = 'distinct %d, e.g. %r' % (len(set(vs)), vs[0][:40])
		print('  %-28s %-6s n=%-6d %s' % (k, kind[k], len(vs), extra))

def cmd_refs(d, T):
	cats = collections.defaultdict(lambda: [0, collections.Counter(), [[], [], []], 0])
	for r in d.by_type[T].values():
		for cat, dd in r.refs.items():
			c = cats[cat]
			c[3] += 1
			for sid, v in dd.items():
				c[0] += 1
				t = d.recs.get(sid)
				c[1][t.type if t else 'MISSING'] += 1
				for i in range(3): c[2][i].append(v[i])
	print('%s: %d records' % (T, len(d.by_type[T])))
	for cat in sorted(cats):
		n, tt, vv, owners = cats[cat]
		print('  %-26s refs=%-6d on %d recs  targets %s  v0 %s v1 %s v2 %s' % (cat, n, owners, dict(tt), num_range(vv[0]), num_range(vv[1]), num_range(vv[2])))

def cmd_stats(d):
	print('merged records: %d, removed by a later file: %d' % (len(d.recs), len(d.removed)))
	for T in ('DIALOGUE', 'DIALOGUE_LINE', 'DIALOG_ACTION', 'DIALOGUE_PACKAGE'):
		print('%-18s live %d  removed by a later file %d' % (T, len(d.by_type[T]), sum(1 for v in d.removed.values() if v[2] == T)))
	conds, acts, ev_roots = collections.Counter(), collections.Counter(), collections.Counter()
	for r in d.by_type['DIALOGUE_LINE'].values():
		for cat, dd in r.refs.items():
			for sid, v in dd.items():
				a = d.recs.get(sid)
				if cat == 'conditions' and a and a.type == 'DIALOG_ACTION': conds[ename(CONDITIONS, a.f.get('condition name'))] += 1
				if cat == 'effects' and a and a.type == 'DIALOG_ACTION': acts[ename(ACTIONS, a.f.get('action name'))] += 1
	for r in d.by_type['DIALOGUE'].values():
		for sid in r.refs.get('conditions', {}):
			a = d.recs.get(sid)
			if a and a.type == 'DIALOG_ACTION': conds[ename(CONDITIONS, a.f.get('condition name'))] += 1
	print('condition kinds used:', len(conds), sum(conds.values()), dict(conds.most_common()))
	print('action kinds used:', len(acts), sum(acts.values()), dict(acts.most_common()))
	for r in d.by_type['DIALOGUE_PACKAGE'].values():
		for sid, v in r.refs.get('dialogs', {}).items():
			for e in v:
				if e: ev_roots[ename(EVENTS, e)] += 1
	print('package entries per event:', dict(ev_roots.most_common()))

def cmd_events(d):
	roots = collections.defaultdict(set)
	for p in d.by_type['DIALOGUE_PACKAGE'].values():
		for sid, v in p.refs.get('dialogs', {}).items():
			for e in v:
				if e: roots[ename(EVENTS, e)].add(sid)
	for ev in EVENTS:
		if ev in roots:
			print('%-48s %5d dialogues' % (ev, len(roots[ev])))

def cmd_find(d, text):
	t = text.lower()
	for T in ('DIALOGUE', 'DIALOGUE_LINE'):
		for r in d.by_type[T].values():
			if t in r.name.lower() or any(isinstance(v, str) and t in v.lower() for v in r.f.values()):
				print('%s %s "%s"' % (T, r.sid, r.name))

def main(argv):
	if len(argv) < 2:
		print(__doc__); return 1
	d = Dialogue()
	cmd = argv[1]
	if cmd == 'tree': print_tree(d, ' '.join(a for a in argv[2:] if not a.startswith('--')))
	elif cmd == 'package': print_package(d, ' '.join(argv[2:]))
	elif cmd == 'stats': cmd_stats(d)
	elif cmd == 'fields': cmd_fields(d, argv[2])
	elif cmd == 'refs': cmd_refs(d, argv[2])
	elif cmd == 'events': cmd_events(d)
	elif cmd == 'find': cmd_find(d, ' '.join(argv[2:]))
	else: print(__doc__); return 1
	return 0

if __name__ == '__main__':
	sys.exit(main(sys.argv))
