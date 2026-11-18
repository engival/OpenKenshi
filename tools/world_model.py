#!/usr/bin/env python3
"""Build the reference model of Kenshi's DEFAULT world from shipped game data.

Standard library only.  Writes one TSV per section into ./data/ (rewritten
byte-identically on every run).  Every number in WORLD_MODEL.md comes from
this script's output.

Inputs (all under DATA = $KENSHI/data/ unless noted; KENSHI defaults to the
Steam install; nothing else
is read, in particular never mods.cfg or __mods.list):
  gamedata.base, Newwworld.mod, Dialogue.mod, rebirth.mod   merged in this order
  newland/leveldata/{Newwworld,Dialogue,rebirth}/leveldata.level
                                  town placements (merged by instance id)
  newland/leveldata/*.zone and newland/leveldata/<layer>/*.zone
                                  building placements (overlay, see zone_placements)
  leveldata.level                 base-level town placements; read only to write
                                  town_placements_base_level.tsv, not used otherwise
  $KENSHI/fcs_enums.def           taskType enum
  tools/kmod.py                   the binary record parser

Usage:  nice -n 10 python3 tools/world_model.py [outdir]
        (default outdir: docs/reference/world_model/data)
"""
import sys, os, glob, struct, collections, re

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kmod
import kenshi_install

KENSHI = kenshi_install.install()
DATA = KENSHI + '/data/'
FILES = ['gamedata.base', 'Newwworld.mod', 'Dialogue.mod', 'rebirth.mod']
ZONE_LAYERS = [('.', DATA + 'newland/leveldata/*.zone')] + [
    (l, DATA + 'newland/leveldata/%s/*.zone' % l)
    for l in ['Newwworld', 'Dialogue', 'rebirth']]
LD = DATA + 'newland/leveldata/'
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'docs', 'reference', 'world_model', 'data')
os.makedirs(OUT, exist_ok=True)

# ---------------------------------------------------------------- enums (data)
def parse_enum(name, path=KENSHI + '/fcs_enums.def'):
    text = open(path, encoding='utf-8', errors='replace').read()
    i = text.index('enum ' + name)
    j = text.index('{', i)
    k = text.index('}', j)
    body = re.sub(r'/\*.*?\*/', '', text[j + 1:k], flags=re.S)
    body = re.sub(r'//.*', '', body)
    out, idx = {}, 0
    for part in body.split(','):
        part = part.strip()
        if not part:
            continue
        if '=' in part:
            nmv, vv = part.split('=', 1)
            try:
                idx = int(vv.strip(), 0)
            except ValueError:
                nmv = part
        else:
            nmv = part
        out[idx] = nmv.split('=')[0].strip()
        idx += 1
    return out

TASKTYPE = parse_enum('taskType')
TOWNTYPE = {0: 'NEST', 1: 'OUTPOST', 2: 'TOWN', 3: 'VILLAGE', 4: 'RUINS',
            5: 'SLAVE_CAMP', 6: 'MILITARY', 7: 'PRISON', 8: 'NEST_MARKER',
            9: 'POI', 10: 'NULL'}
WARCAMPAIGN = {0: 'ASSAULT_TOWN', 1: 'CONQUER_TOWN', 2: 'DEFEND_TOWN',
               3: 'TRADER_VISIT'}
BUILDINGFUNCTION = {
    0: 'BF_ANY', 1: 'BF_MINE', 2: 'BF_RESOURCE_STORAGE', 3: 'BF_RESEARCH',
    4: 'BF_REFINERY', 5: 'BF_GENERATOR', 6: 'BF_BED', 7: 'BF_TRAINING',
    8: 'BF_CAGE', 9: 'BF_SHOP', 10: 'BF_CRAFTING', 11: 'BF_ITEM_FURNACE',
    12: 'BF_LIGHT', 13: 'BF_TURRET', 14: 'BF_ITEM_FURNACE2',
    15: 'BF_WIND_GENERATOR', 16: 'BF_FARM', 17: 'BF_STORAGE',
    18: 'BF_CRAFTING2', 19: 'BF_HEALING', 20: 'BF_BUILDING_MATERIALS',
    21: 'BF_RESEARCH2', 22: 'BF_FOOD', 23: 'BF_WATER', 24: 'BF_POWER',
    25: 'BF_TECH', 26: 'BF_RAIN_COLLECTOR', 27: 'BF_MINE_NATURAL',
    28: 'BF_STEERING', 29: 'BF_ENGINE', 30: 'BF_LIQUID_TANK'}
MININGRESOURCE = {0: 'NONE', 1: 'IRON', 2: 'STONE', 3: 'COPPER', 4: 'CARBON',
                  5: 'WATER', 6: 'GROUND'}

# ---------------------------------------------------------------- loader
class DB:
    def __init__(self):
        self.items = {}
        self.record_counts = collections.Counter()
        self.merged_after = collections.Counter()
        self.headers = {}
        self.removed_names = {}

    def load(self, fn):
        f, b = kmod.parse(DATA + fn)
        self.headers[fn] = f.get('header', {})
        self.record_counts[fn] = len(f['records'])
        for r in f['records']:
            t = kmod.ITEMTYPES[r['type']]
            sid = r['sid']
            it = self.items.get(sid)
            if it is None:
                it = {'type': t, 'name': r['name'], 'bool': {}, 'float': {},
                      'int': {}, 'string': {}, 'file': {}, 'refs': {}, 'inst': {}}
                self.items[sid] = it
            elif r['flags'] & 2 and r['name']:
                it['name'] = r['name']
            for k, v in r['bool']:
                it['bool'][k] = bool(v)
            for k, v in r['float']:
                it['float'][k] = struct.unpack('<f', v)[0]
            for k, v in r['int']:
                it['int'][k] = v
            for k, v in r['string']:
                it['string'][k] = v
            for k, v in r['file']:
                it['file'][k] = v
            for cat, lst in r['refs']:
                d = it['refs'].setdefault(cat, {})
                for e in lst:
                    if e[1] == 0x7fffffff:
                        d.pop(e[0], None)
                    else:
                        d[e[0]] = (e[1], e[2], e[3])
            for inst in r['inst']:
                if inst['ref'] == '':
                    it['inst'].pop(inst['id'], None)
                else:
                    it['inst'][inst['id']] = {
                        'ref': inst['ref'], 'states': inst['states'],
                        'x': struct.unpack('<7f', inst['xform'])}
        for sid in [s for s, i in self.items.items() if i['bool'].get('REMOVED')]:
            self.removed_names[sid] = (self.items[sid]['name'], fn)
            del self.items[sid]
        self.merged_after[fn] = len(self.items)

    def of(self, t):
        return {s: i for s, i in self.items.items() if i['type'] == t}

DB_ = DB()
for _fn in FILES:
    DB_.load(_fn)
db = DB_
def nm(s):
    return db.items[s]['name'] if s in db.items else s

def N(s):
    return nm(s)

def top_n(counter, n=None):
    """Counter items by (-count, key): a stable tie order, unlike most_common."""
    items = sorted(counter.items(), key=lambda kv: (-kv[1], str(kv[0])))
    return items if n is None else items[:n]

def write_tsv(name, header, rows):
    path = os.path.join(OUT, name)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write('\t'.join(header) + '\n')
        for r in rows:
            f.write('\t'.join('' if v is None else str(v) for v in r) + '\n')
    print('  wrote %-32s %6d rows' % (name, len(rows)))

def refname(d, k, idx=0):
    v = d.get(k)
    if not v:
        return ''
    return refname_of(list(v)[idx])

def refname_of(sid):
    return N(sid)

# =====================================================================
# 0. LOAD ORDER
# =====================================================================
def section_load_order():
    print('[0] load order')
    rows = []
    for i, fn in enumerate(FILES):
        h = db.headers.get(fn, {})
        rows.append((i + 1, fn, db.record_counts[fn], h.get('version', ''),
                     h.get('dependencies', ''), h.get('referenced', ''),
                     db.merged_after[fn]))
    write_tsv('load_order.tsv',
              ['order', 'file', 'records_in_file', 'header_version',
               'header_dependencies', 'header_referenced', 'merged_items_after'],
              rows)
    print('  merged items', len(db.items))

# =====================================================================
# a. DEFAULT WORLD STATE
# =====================================================================
WES = db.of('WORLD_EVENT_STATE')

def eval_state(sid, mode='chosen'):
    """Evaluate WORLD_EVENT_STATE at the initial new-game snapshot.

    AND over every condition.  Each ref carries a required value in v0.
    All UNIQUE_NPCs are assumed alive (state 1) and the player has no
    allies/enemies.  mode='chosen' ignores `player involvement` (initial
    snapshot has not involved the player); mode='strict' requires it false.
    """
    i = WES.get(sid)
    if not i:
        return None
    ok = True
    for t, e in i['refs'].get('NPC is', {}).items():
        ok = ok and (1 == e[0])
    for t, e in i['refs'].get('NPC is NOT', {}).items():
        ok = ok and (1 != e[0])
    for t, e in i['refs'].get('player ally', {}).items():
        if e[0] == 1:
            ok = False
    for t, e in i['refs'].get('player enemy', {}).items():
        if e[0] == 1:
            ok = False
    if mode == 'strict' and i['bool'].get('player involvement'):
        ok = False
    return ok

def fmt_cond(cat, d):
    return '; '.join('%s=%s' % (N(t), e[0]) for t, e in d.items())

def section_world_state():
    print('[a] world state')
    rows = []
    for sid, i in WES.items():
        rows.append((i['name'], sid, i['bool'].get('player involvement'),
                     eval_state(sid, 'chosen'), eval_state(sid, 'strict'),
                     fmt_cond('NPC is', i['refs'].get('NPC is', {})),
                     fmt_cond('NPC is NOT', i['refs'].get('NPC is NOT', {})),
                     fmt_cond('player ally', i['refs'].get('player ally', {})),
                     fmt_cond('player enemy', i['refs'].get('player enemy', {}))))
    write_tsv('world_state.tsv',
              ['name', 'sid', 'player_involvement', 'default_true_chosen',
               'default_true_strict', 'NPC_is', 'NPC_is_NOT', 'player_ally',
               'player_enemy'], rows)
    print('  states', len(WES), 'true chosen', sum(1 for r in rows if r[3]),
          'true strict', sum(1 for r in rows if r[4]))
    return rows

# town positions ------------------------------------------------------
def town_entries():
    entries = {}
    for layer in ['Newwworld', 'Dialogue', 'rebirth']:
        f, b = kmod.parse(LD + layer + '/leveldata.level')
        for r in f['records']:
            if kmod.ITEMTYPES[r['type']] == 'GAMESTATE_TOWN_INSTANCE_LIST':
                for i in r['inst']:
                    x, y, z, qw, qx, qy, qz = struct.unpack('<7f', i['xform'])
                    entries[i['id']] = (i['ref'], x, z, layer,
                                        i['states'][0] if i['states'] else None,
                                        i['id'], y)
    return list(entries.values())

def town_cells():
    gt = {}
    for layer in ['Newwworld', 'Dialogue', 'rebirth']:
        f, b = kmod.parse(LD + layer + '/leveldata.level')
        for r in f['records']:
            if kmod.ITEMTYPES[r['type']] == 'GAMESTATE_TOWN':
                gt[r['sid']] = {'int': dict(r['int']), 'string': dict(r['string'])}
    out = {}
    for ref, x, z, layer, state, iid, y in town_entries():
        g = gt.get(state)
        cells = []
        if g:
            n = g['int'].get('zones_count', 0)
            for k in range(n):
                cx = g['int'].get('zone_x_' + str(k))
                cy = g['int'].get('zone_y_' + str(k))
                if cx is not None and cy is not None:
                    cells.append((cx, cy))
        out[(ref, iid)] = cells
    return out

def base_level_towns():
    """Town placements in the base data/leveldata.level (not used by the model)."""
    f, b = kmod.parse(DATA + 'leveldata.level')
    out = []
    for r in f['records']:
        if kmod.ITEMTYPES[r['type']] == 'GAMESTATE_TOWN_INSTANCE_LIST':
            for i in r['inst']:
                x, y, z, qw, qx, qy, qz = struct.unpack('<7f', i['xform'])
                out.append((i['id'], i['ref'], x, y, z))
    return out

T = db.of('TOWN')
BASE_LEVEL_TOWNS = base_level_towns()
TOWN_ENTRIES = town_entries()
TOWN_CELLS = town_cells()
TOWN_POS_ALL = collections.defaultdict(list)  # root sid -> [(x,z,layer,iid)]
TOWN_POS = {}          # root sid -> primary placement (x,z,layer,iid)
TOWN_DECL = {}         # root sid -> set(cells), union over all placements
for ref, x, z, layer, state, iid, y in TOWN_ENTRIES:
    TOWN_POS_ALL[ref].append((x, z, layer, iid))
    TOWN_DECL.setdefault(ref, set()).update(TOWN_CELLS.get((ref, iid), []))
for ref, plist in TOWN_POS_ALL.items():
    TOWN_POS[ref] = plist[0]

def section_base_level():
    """Document the base data/leveldata.level placements and why they are skipped."""
    print('[d] base level town placements (not used)')
    rows = []
    for iid, ref, x, y, z in sorted(BASE_LEVEL_TOWNS, key=lambda e: (e[1], e[0])):
        live = ref in db.items
        new = TOWN_POS_ALL.get(ref, [])
        if live:
            status = 'live'
        else:
            status = ('REMOVED in ' + db.removed_names[ref][1]
                      if ref in db.removed_names else 'absent')
        rows.append((iid, N(ref) if live else db.removed_names.get(ref, (ref,))[0], ref, status, '%.0f' % x, '%.0f' % y, '%.0f' % z,
                     len(new),
                     '; '.join('%s(%.0f,%.0f)' % (l, nx, nz) for nx, nz, l, _ in new),
                     'placed in newland' if new else 'NOT placed in newland'))
    write_tsv('town_placements_base_level.tsv',
              ['entry_id', 'town', 'town_sid', 'town_status', 'base_x', 'base_y',
               'base_z', 'n_newland_placements', 'newland_placements', 'note'], rows)
    n_live_unplaced = sum(1 for r in rows if r[3] == 'live' and not r[7])
    print('  base-level placements', len(rows), 'with a newland placement',
          sum(1 for r in rows if r[7]), 'live and unplaced in newland', n_live_unplaced)

OVERRIDE_TARGETS = collections.Counter()
for s, i in T.items():
    for v in i['refs'].get('override town', {}):
        OVERRIDE_TARGETS[v] += 1
TOWN_ROOTS = [s for s in T if OVERRIDE_TARGETS[s] == 0]
TOWN_ROOT_OF = {}
for r in TOWN_ROOTS:
    TOWN_ROOT_OF[r] = r

def resolve_town(root, mode='chosen'):
    """Follow `override town` in stored order; first active variant wins."""
    cur = root
    seen = set()
    chain = [root]
    while cur not in seen:
        seen.add(cur)
        nxt = None
        for v in T[cur]['refs'].get('override town', {}):
            ws = T[v]['refs'].get('world state', {})
            if not ws or all(eval_state(w, mode) == bool(e[0])
                             for w, e in ws.items()):
                nxt = v
                break
        if nxt is None:
            return cur, chain
        chain.append(nxt)
        cur = nxt
    return cur, chain

def town_faction(sid):
    d = T[sid]['refs'].get('faction', {})
    return list(d)[0] if d else ''

ACTIVE = {}   # root -> active town sid
CHAINS = {}
for r in TOWN_ROOTS:
    a, ch = resolve_town(r, 'chosen')
    ACTIVE[r] = a
    CHAINS[r] = ch
    TOWN_ROOT_OF[a] = r

def town_display(root):
    a = ACTIVE[root]
    return N(a), N(root)

def zone_of(x, z):
    if x is None or z is None:
        return '', ''
    return int((x + 147456) // 4608), int((z + 147456) // 4608)

def town_ref_list(sid, key):
    return [(s, T[sid]['refs'][key][s][0]) for s in T[sid]['refs'].get(key, {})]

def all_variants(root):
    """All TOWN records reachable through `override town`, stored order, BFS."""
    out, seen, queue = [], set(), [root]
    while queue:
        cur = queue.pop(0)
        for v in T[cur]['refs'].get('override town', {}):
            if v not in seen:
                seen.add(v)
                out.append(v)
                queue.append(v)
    return out

def section_towns():
    print('[d] towns')
    rows, vrows, sqrows = [], [], []
    changed = 0
    var_roots = collections.OrderedDict()
    for root in TOWN_ROOTS:
        a = ACTIVE[root]
        pos = TOWN_POS.get(root)
        x, z = (pos[0], pos[1]) if pos else (None, None)
        zx, zy = zone_of(x, z)
        fac = town_faction(a) or town_faction(root)
        variants = all_variants(root)
        for vi, v in enumerate(variants):
            var_roots.setdefault(v, []).append((root, vi))
        nchanged = 1 if a != root else 0
        changed += nchanged
        plist = TOWN_POS_ALL.get(root, [])
        rows.append((root, N(root), a, N(a), T[a]['int'].get('type', ''),
                     TOWNTYPE.get(T[a]['int'].get('type'), ''),
                     N(fac), fac, x, z, zx, zy,
                     pos[2] if pos else '', pos[3] if pos else '',
                     len(plist),
                     '; '.join('%s@%.0f,%.0f' % (p[2], p[0], p[1]) for p in plist),
                     len(TOWN_DECL.get(root, [])),
                     ';'.join('%d,%d' % c for c in sorted(TOWN_DECL.get(root, []))),
                     len(variants), nchanged,
                     len(T[a]['refs'].get('residents', {})),
                     len(T[a]['refs'].get('bar squads', {})),
                     len(T[a]['refs'].get('default resident', {})),
                     len(T[a]['refs'].get('roaming squads', {})),
                     N(next(iter(T[a]['refs'].get('trade culture', {}))))
                     if T[a]['refs'].get('trade culture') else '',
                     len(T[a]['refs'].get('trade prices', {}))))
        car = T[a]
        for key, label in [('residents', 'residents'),
                           ('bar squads', 'bar squads'),
                           ('default resident', 'default resident'),
                           ('roaming squads', 'roaming squads'),
                           ('debris', 'debris'),
                           ('loot spawn', 'loot spawn')]:
            for s, e in car['refs'].get(key, {}).items():
                sqrows.append((root, N(root), N(a), fac, label, s, N(s),
                               sq_faction(s) if 'SQUAD_TEMPLATE' ==
                               db.items.get(s, {}).get('type') else ''))
    for v, rlist in var_roots.items():
        ws = T[v]['refs'].get('world state', {})
        cond = '; '.join('%s=%s' % (N(w), e[0]) for w, e in ws.items())
        active = any(ACTIVE[r] == v for r, _ in rlist)
        condtrue = (all(eval_state(w, 'chosen') == bool(e[0])
                        for w, e in ws.items()) if ws else 'unconditional')
        vrows.append(('; '.join(N(r) for r, _ in rlist),
                      '; '.join(r for r, _ in rlist),
                      v, N(v), 'yes' if active else 'no',
                      '; '.join(str(i) for _, i in rlist),
                      T[v]['int'].get('type', ''),
                      TOWNTYPE.get(T[v]['int'].get('type'), ''),
                      N(town_faction(v)), town_faction(v),
                      len(ws), cond, condtrue))
    write_tsv('towns.tsv',
              ['root_sid', 'root_name', 'active_sid', 'active_name', 'type',
               'type_name', 'owner_faction', 'owner_sid', 'x', 'z',
               'zone_x', 'zone_y', 'layer', 'entry_id', 'n_placements',
               'placements', 'declared_cells',
               'declared_cell_list', 'n_variant_records', 'variants_active',
               'n_residents', 'n_bar_squads', 'n_default_resident',
               'n_roaming_squads', 'trade_culture', 'n_trade_prices'], rows)
    write_tsv('town_variants.tsv',
              ['root_names', 'root_sids', 'variant_sid', 'variant_name',
               'is_default_active', 'stored_indexes', 'type', 'type_name',
               'owner_faction', 'owner_sid', 'n_world_state', 'world_states',
               'condition_true_at_default'], vrows)
    write_tsv('town_squads.tsv',
              ['root_sid', 'root_name', 'town_name', 'town_faction', 'category',
               'squad_sid', 'squad_name', 'squad_faction'], sqrows)
    print('  TOWN records', len(T), 'roots/default towns', len(TOWN_ROOTS),
          'variant records', len(OVERRIDE_TARGETS),
          'placed roots', sum(1 for r in TOWN_ROOTS if r in TOWN_POS),
          'roots changed at default', changed)
    print('  town squad refs', len(sqrows))

def sq_faction_sid(sid):
    i = db.items.get(sid)
    if not i:
        return ''
    d = i['refs'].get('faction', {})
    return list(d)[0] if d else ''

def sq_faction(sid):
    return N(sq_faction_sid(sid)) if sq_faction_sid(sid) else ''

# =====================================================================
# b. FACTIONS
# =====================================================================
F = db.of('FACTION')
S = db.of('SQUAD_TEMPLATE')
BG = db.of('BIOME_GROUP')
BIO = db.of('BIOMES')

def is_animal_squad(sid):
    i = S.get(sid)
    if not i:
        return False
    r = i['refs']
    return bool(r.get('animals') or r.get('animals2')) and not (
        r.get('squad') or r.get('leader'))

def faction_squad_refs(fsid):
    out = []
    i = F[fsid]
    for key in ['squads', 'special squads', 'residents', 'bar squads',
                'default resident', 'roaming squads']:
        out += list(i['refs'].get(key, {}))
    return out

# spawn tables --------------------------------------------------------
def build_spawns():
    homeless = []   # (record_type, record_name, squad_sid)
    nest = []       # (record_type, record_name, town_sid)
    for recs, label in [(BG, 'BIOME_GROUP'), (BIO, 'BIOMES')]:
        for s, i in recs.items():
            for x in i['refs'].get('homeless spawns', {}):
                homeless.append((label, i['name'], x))
            for x in i['refs'].get('nests', {}):
                nest.append((label, i['name'], x))
    return homeless, nest

HOMELESS, NESTS = build_spawns()
HOMELESS_FAC = collections.defaultdict(list)   # faction -> (biome, squad)
for label, bname, sq in HOMELESS:
    f = sq_faction_sid(sq)
    if f:
        HOMELESS_FAC[f].append((bname, sq))
NEST_FAC = collections.defaultdict(list)
for label, bname, t in NESTS:
    i = T.get(t)
    if i:
        fs = i['refs'].get('faction', {})
        if fs:
            NEST_FAC[list(fs)[0]].append((bname, t))
# factions appearing in placed-town squads
TOWN_SQUAD_FACS = collections.Counter()
for r in TOWN_ROOTS:
    for key in ['residents', 'bar squads', 'default resident', 'roaming squads']:
        for sq in T[ACTIVE[r]]['refs'].get(key, {}):
            f = sq_faction_sid(sq)
            if f:
                TOWN_SQUAD_FACS[f] += 1
# campaign owners
CAMPAIGN_FACS = collections.Counter()
for s, i in db.of('FACTION_CAMPAIGN').items():
    fs = i['refs'].get('faction', {})
    if fs:
        CAMPAIGN_FACS[list(fs)[0]] += 1

PLACED_OWNERS = collections.Counter()
PLACED_TOWNS_OF = collections.defaultdict(list)
for r in TOWN_ROOTS:
    if r in TOWN_POS:
        f = town_faction(ACTIVE[r]) or town_faction(r)
        if f:
            PLACED_OWNERS[f] += 1
            PLACED_TOWNS_OF[f].append(N(ACTIVE[r]))

SQUADS_OF_FACTION = collections.defaultdict(list)
for _s, _i in S.items():
    for _x in _i['refs'].get('faction', {}):
        SQUADS_OF_FACTION[_x].append(_s)
for _s, _i in F.items():
    for _x in faction_squad_refs(_s):
        if _x in S and _x not in SQUADS_OF_FACTION[_s]:
            SQUADS_OF_FACTION[_s].append(_x)

def faction_kind(fsid):
    if fsid == 'nofac':
        return 'player-related'
    sqs = [s for s in SQUADS_OF_FACTION[fsid] if s in S]
    if sqs and all(is_animal_squad(s) for s in sqs):
        return 'animal'
    if PLACED_OWNERS[fsid] > 0:
        return 'major'
    if (TOWN_SQUAD_FACS[fsid] or F[fsid]['refs'].get('residents') or
            F[fsid]['refs'].get('bar squads') or
            F[fsid]['refs'].get('roaming squads')):
        return 'minor'
    if HOMELESS_FAC[fsid] or NEST_FAC[fsid] or CAMPAIGN_FACS[fsid]:
        return 'roaming'
    return 'unused'

def section_factions():
    print('[b] factions')
    rows, brows, rrows = [], [], []
    kinds = collections.Counter()
    for sid, i in F.items():
        kind = faction_kind(sid)
        kinds[kind] += 1
        declared = [N(x) for x in i['refs'].get('biomes', {})]
        spawn_biomes = sorted(set(b for b, _ in HOMELESS_FAC[sid]) |
                              set(b for b, _ in NEST_FAC[sid]))
        rows.append((i['name'], sid, kind, PLACED_OWNERS[sid],
                     '; '.join(sorted(PLACED_TOWNS_OF[sid])),
                     '; '.join(declared), '; '.join(spawn_biomes),
                     len(i['refs'].get('relations', {})),
                     i['int'].get('default relation'),
                     i['int'].get('fundamental type'),
                     i['bool'].get('not real'),
                     i['bool'].get('anti slavery'),
                     i['bool'].get('heals strangers'),
                     i['bool'].get('allow slaves weapons'),
                     i['bool'].get('offers bounties'),
                     i['int'].get('roaming population'),
                     len(i['refs'].get('campaigns', {}))))
        for b in declared:
            brows.append((i['name'], sid, 'declared FACTION biomes', b, ''))
        for b, sq in HOMELESS_FAC[sid]:
            brows.append((i['name'], sid, 'biome homeless spawn', b, N(sq)))
        for b, t in NEST_FAC[sid]:
            brows.append((i['name'], sid, 'biome nest town faction', b, N(t)))
    for sid, i in F.items():
        for tgt, e in i['refs'].get('relations', {}).items():
            rrows.append((i['name'], sid, N(tgt), tgt, e[0],
                          'listed'))
    write_tsv('factions.tsv',
              ['name', 'sid', 'kind', 'towns_owned_default',
               'towns_owned_names', 'declared_biomes', 'spawn_biomes',
               'n_relation_entries', 'default_relation', 'fundamental_type',
               'not_real', 'anti_slavery', 'heals_strangers',
               'allow_slaves_weapons', 'offers_bounties',
               'roaming_population', 'n_campaigns'], rows)
    write_tsv('faction_biomes.tsv',
              ['faction', 'faction_sid', 'source', 'biome', 'detail'], brows)
    write_tsv('faction_relations.tsv',
              ['faction', 'faction_sid', 'target', 'target_sid', 'relation',
               'listed'], rrows)
    print('  factions', len(F), dict(kinds))
    print('  relation entries', len(rrows), 'biome rows', len(brows))
    return kinds

def faction_relations_matrix(factions):
    """Square matrix using listed relations, else row default relation."""
    out = []
    for a in factions:
        row = [N(a)]
        for b in factions:
            if a == b:
                row.append('')
                continue
            e = F[a]['refs'].get('relations', {}).get(b)
            row.append(e[0] if e else F[a]['int'].get('default relation'))
        out.append(row)
    return out

# =====================================================================
# c. FACTION CAMPAIGNS AND DIPLOMATIC ASSAULTS
# =====================================================================
FC = db.of('FACTION_CAMPAIGN')
CAMPAIGN_OWNERS = collections.defaultdict(list)   # campaign sid -> faction sids
for fsid, fi in F.items():
    for c in fi['refs'].get('campaigns', {}):
        CAMPAIGN_OWNERS[c].append(fsid)

def campaign_ancestors(sid):
    out, stack = [], [sid]
    while stack:
        x = stack.pop()
        for a in FC.get(x, {}).get('refs', {}).get('inherits from', {}):
            if a in FC and a not in out and a != sid:
                out.append(a)
                stack.append(a)
    return out

def inherited_ref(sid, key):
    if FC[sid]['refs'].get(key):
        return list(FC[sid]['refs'][key])
    for a in campaign_ancestors(sid):
        if FC[a]['refs'].get(key):
            return list(FC[a]['refs'][key])
    return []

def cs_eligible(sid, mode='chosen'):
    ws = FC[sid]['refs'].get('world state', {})
    return all(eval_state(w, mode) == bool(e[0]) for w, e in ws.items()), len(ws)

def campaign_trigger_mode(sid):
    i = FC[sid]
    bits = []
    if i['bool'].get('territorial triggers'):
        bits.append('territorial')
    if i['refs'].get('dialog announcement'):
        bits.append('dialogue-announcement')
    if i['refs'].get('loss trigger') or i['refs'].get('victory trigger'):
        bits.append('campaign-followup')
    return '+'.join(bits) if bits else 'faction-list-only'

def section_campaigns():
    print('[c] campaigns')
    rows = []
    modes = collections.Counter()
    eligible_n = 0
    def trigfmt(d):
        return '; '.join('%s(min=%s max=%s chance=%s%%)' % (N(x), e[0], e[1], e[2])
                         for x, e in d.items())
    for sid, i in FC.items():
        elig, nws = cs_eligible(sid)
        eligible_n += 1 if elig else 0
        mode = campaign_trigger_mode(sid)
        modes[mode] += 1
        owners = CAMPAIGN_OWNERS.get(sid, [])
        phases = {}
        for ph in ['pt1 AI leader', 'pt1 AI others', 'pt2 AI leader',
                   'pt2 AI others', 'retreat AI leader', 'retreat AI others',
                   'victory AI leader', 'victory AI others']:
            phases[ph] = '; '.join(N(x) for x in inherited_ref(sid, ph))
        rows.append((i['name'], sid, '; '.join(N(f) for f in owners),
                     '; '.join(owners), i['int'].get('key'),
                     WARCAMPAIGN.get(i['int'].get('key'), ''),
                     i['string'].get('display name', ''),
                     i['bool'].get('is hostile'),
                     i['bool'].get('territorial triggers'),
                     i['bool'].get('target characters'),
                     i['int'].get('target population min'),
                     i['int'].get('target tech level >='),
                     i['int'].get('num forces'),
                     i['int'].get('travel speed loaded'),
                     i['int'].get('travel speed unloaded'),
                     i['int'].get('repetition limit'),
                     i['float'].get('chance per day'),
                     i['float'].get('range near'), i['float'].get('range far'),
                     i['float'].get('repeat limit'),
                     elig, nws,
                     '; '.join('%s=%s' % (N(w), e[0])
                               for w, e in i['refs'].get('world state', {}).items()),
                     '; '.join(N(x) for x in i['refs'].get('inherits from', {})),
                     len(i['refs'].get('loss trigger', {})),
                     len(i['refs'].get('victory trigger', {})),
                     trigfmt(i['refs'].get('loss trigger', {})),
                     trigfmt(i['refs'].get('victory trigger', {})),
                     '; '.join(N(x) for x in i['refs'].get('dialog announcement', {})),
                     '; '.join(N(x) for x in i['refs'].get('squads to use', {})),
                     '; '.join(N(x) for x in i['refs'].get('trigger player ally', {})),
                     '; '.join(N(x) for x in i['refs'].get('faction override', {})),
                     mode,
                     phases['pt1 AI leader'], phases['pt1 AI others'],
                     phases['pt2 AI leader'], phases['pt2 AI others'],
                     phases['retreat AI leader'], phases['retreat AI others'],
                     phases['victory AI leader'], phases['victory AI others']))
    write_tsv('campaigns.tsv',
              ['name', 'sid', 'owner_factions', 'owner_sids', 'key', 'key_name',
               'display_name', 'is_hostile', 'territorial_triggers',
               'target_characters', 'target_population_min',
               'target_tech_level_min', 'num_forces', 'travel_speed_loaded',
               'travel_speed_unloaded', 'repetition_limit', 'chance_per_day',
               'range_near', 'range_far', 'repeat_limit_days',
               'world_state_eligible', 'n_world_state', 'world_states',
               'inherits_from', 'n_loss_trigger', 'n_victory_trigger',
               'loss_trigger_timing', 'victory_trigger_timing',
               'dialog_announcement', 'squads_to_use', 'trigger_player_ally',
               'faction_override', 'trigger_mode', 'pt1_AI_leader',
               'pt1_AI_others', 'pt2_AI_leader', 'pt2_AI_others',
               'retreat_AI_leader', 'retreat_AI_others', 'victory_AI_leader',
               'victory_AI_others'], rows)
    print('  campaigns', len(FC), 'world-state eligible', eligible_n)
    print('  trigger modes', dict(modes))
    print('  owning factions', len(CAMPAIGN_OWNERS), 'with owner', len(CAMPAIGN_OWNERS))
    return rows, modes, eligible_n

DA = db.of('DIPLOMATIC_ASSAULTS')
SA = db.of('SINGLE_DIPLOMATIC_ASSAULT')

def section_assaults():
    print('[c] diplomatic assaults')
    drow, srow = [], []
    for sid, i in DA.items():
        drow.append((i['name'], sid,
                     len(i['refs'].get('assaults', {})),
                     '; '.join(N(x) for x in i['refs'].get('assaults', {}))))
    for sid, i in SA.items():
        cond = '; '.join(N(x) for x in i['refs'].get('conditions', {}))
        dlg = '; '.join(
            '%s: %s' % (k, N(x))
            for k in ['dialogue', 'dialogue announce', 'dialogue squad']
            for x in i['refs'].get(k, {}))
        srow.append((i['name'], sid, i['bool'].get('is aggressive'),
                     i['int'].get('repeat timer hours min'),
                     i['int'].get('repeat timer hours max'),
                     '; '.join(N(x) for x in i['refs'].get('main squad', {})),
                     '; '.join(N(x) for x in i['refs'].get('AI packages', {})),
                     '; '.join(N(x) for x in i['refs'].get('delivery AI package', {})),
                     '; '.join(N(x) for x in i['refs'].get('fallback AI package', {})),
                     cond, dlg))
    write_tsv('diplomatic_assaults.tsv',
              ['container_name', 'container_sid', 'n_assaults', 'assaults'],
              drow)
    write_tsv('diplomatic_assault_singles.tsv',
              ['name', 'sid', 'is_aggressive', 'repeat_timer_hours_min',
               'repeat_timer_hours_max', 'main_squad', 'AI_packages',
               'delivery_AI_package', 'fallback_AI_package', 'conditions',
               'dialogues'], srow)
    print('  containers', len(DA), 'single assaults', len(SA))

# =====================================================================
# d/e/f/h. ZONE PLACEMENTS (buildings, shops, production, roaming)
# =====================================================================
_BUILDINGS = db.of('BUILDING')

def zone_placements():
    """BUILDING placements in the zone files after the layer overlay.

    Same rule as the C++ kworld loader: layers in load order
    ZONE_LAYERS ('.' = newland/leveldata/*.zone, then Newwworld, Dialogue,
    rebirth); only the INSTANCE_COLLECTION '0-buildinglist'; key =
    (zone file cell, instance id); a later entry replaces the earlier one,
    an empty target deletes the key, a later non-empty entry revives it.
    Survivors are classed by target: BUILDING (returned), level-local record
    (an INVENTORY_STATE defined in the zone files, not returned) or missing
    (target in no file, kept in ZONE_STATS).  GAMESTATE_BUILDING records
    are keyed by sid, later layers replacing earlier ones.
    """
    if hasattr(zone_placements, '_cache'):
        return zone_placements._cache
    gsb = {}
    local = set()
    layer_of = {}
    cur = collections.OrderedDict()
    st = collections.Counter()
    for l, pat in ZONE_LAYERS:
        for p in sorted(glob.glob(pat)):
            parts = os.path.basename(p).split('.')
            cx, cy = int(parts[1]), int(parts[2])
            f, b = kmod.parse(p)
            for r in f['records']:
                t = kmod.ITEMTYPES[r['type']]
                if t == 'GAMESTATE_BUILDING':
                    gsb[r['sid']] = {'str': dict(r['string']),
                                     'int': dict(r['int']),
                                     'bool': dict(r['bool']),
                                     'float': dict(r['float'])}
                elif t in ('INVENTORY_STATE', 'INVENTORY_ITEM_STATE'):
                    local.add(r['sid'])
                elif t == 'INSTANCE_COLLECTION' and r['sid'] == '0-buildinglist':
                    for i in r['inst']:
                        key = (cx, cy, i['id'])
                        st['entries.' + l] += 1
                        if i['ref'] == '':
                            st['deletes.' + l] += 1
                            if cur.pop(key, None) is None:
                                st['deletes_of_absent'] += 1
                            continue
                        if key in layer_of and key not in cur:
                            st['revived'] += 1
                        x, y, z, qw, qx, qy, qz = struct.unpack('<7f', i['xform'])
                        cur[key] = {'ref': i['ref'], 'x': x, 'y': y, 'z': z,
                                    'cx': cx, 'cy': cy, 'layer': l,
                                    'id': i['id'],
                                    'state': i['states'][0] if i['states'] else None,
                                    'g': None}
                        layer_of[key] = l
    pl = []
    for key, p in cur.items():
        if p['ref'] in _BUILDINGS:
            pl.append(p)
        elif p['ref'] in local:
            st['survivors_level_local'] += 1
        elif p['ref'] in db.items:
            st['survivors_other_type'] += 1
        else:
            st['survivors_missing_target'] += 1
            st['missing.' + p['ref']] += 1
    for p in pl:
        p['g'] = gsb.get(p['state'])
    # base-game zone files (data/leveldata/*.zone): counted, not used
    bcells = set()
    for p in sorted(glob.glob(DATA + 'leveldata/*.zone')):
        parts = os.path.basename(p).split('.')
        f, b = kmod.parse(p)
        st['unused_base_zone_files'] += 1
        for r in f['records']:
            if (kmod.ITEMTYPES[r['type']] == 'INSTANCE_COLLECTION'
                    and r['sid'] == '0-buildinglist'):
                for i in r['inst']:
                    key = (int(parts[1]), int(parts[2]), i['id'])
                    bcells.add(key[:2])
                    st['unused_base_zone_entries'] += 1
                    if i['ref'] in _BUILDINGS:
                        st['unused_base_zone_building_entries'] += 1
                    if key in layer_of:
                        st['unused_base_zone_keys_also_in_newland'] += 1
    st['unused_base_zone_keys_also_in_newland'] += 0
    st['unused_base_zone_cells'] = len(bcells)
    st['unused_base_zone_cells_also_in_newland'] = len(
        bcells & set(k[:2] for k in layer_of))
    st['survivors_building'] = len(pl)
    st['survivors_total'] = len(cur)
    st['placements_total'] = len(pl) + st['survivors_missing_target']
    zone_placements._cache = pl
    zone_placements.stats = st
    return pl

ZONE_STATS = (zone_placements(), zone_placements.stats)[1]
PLACEMENTS = None
CELL_TOWNS = collections.defaultdict(list)
for _ref, _x, _z, _layer, _state, _iid, _y in TOWN_ENTRIES:
    for _c in TOWN_CELLS.get((_ref, _iid), []):
        CELL_TOWNS[_c].append((_ref, _x, _z))

def place_town(p):
    """Attribute a placement to nearest town whose declared -S5e cell matches."""
    cand = CELL_TOWNS.get((p['cx'], p['cy']))
    if not cand:
        return None, ''
    best = min(cand, key=lambda t: (t[1] - p['x']) ** 2 + (t[2] - p['z']) ** 2)
    return best[0], 'declared cell, nearest town'

def gstr(p, key):
    g = p.get('g')
    return g['str'].get(key, '') if g else ''

def gint(p, key, default=None):
    g = p.get('g')
    return g['int'].get(key, default) if g else default

SHOP_LABEL = re.compile(
    r'shop|\bbar\b|saloon|bakery|armory|market|trader|hospital|thieves|'
    r'smuggler|merchant', re.I)

def section_zone_overlay():
    print('[d/e/h] zone placement overlay')
    st = ZONE_STATS
    pl = zone_placements()
    n_attr = sum(1 for p in pl if place_town(p)[0])
    rows = [('building_placements_attributed_to_town', n_attr),
            ('building_placements_unattributed', len(pl) - n_attr),
            ('building_placements_without_GSB_state',
             sum(1 for p in pl if p['g'] is None))]
    rows += [(k, st[k]) for k in sorted(st) if not k.startswith('missing.')]
    rows += [('missing_target.' + k[len('missing.'):], st[k])
             for k in sorted(st) if k.startswith('missing.')]
    write_tsv('zone_overlay.tsv', ['metric', 'value'], rows)
    print('  placements', st['placements_total'], '= building targets',
          st['survivors_building'], '+ missing targets',
          st['survivors_missing_target'])

def section_shops():
    print('[e] shops and vendors')
    global PLACEMENTS
    PLACEMENTS = zone_placements()
    labels = collections.Counter()
    for p in PLACEMENTS:
        n = gstr(p, 'name')
        if n:
            labels[n] += 1
    shoprows, vlistrows, vitemrows = [], [], []
    labelled = vendor_shops = 0
    shop_by_label = collections.Counter()
    # vendor lists and entries
    seen_v = set()
    for sid, i in db.of('VENDOR_LIST').items():
        cats = {k: len(v) for k, v in i['refs'].items()}
        vlistrows.append((i['name'], sid, i['int'].get('items count'),
                          i['int'].get('money item min'),
                          i['int'].get('money item max'),
                          '; '.join('%s=%d' % kv for kv in sorted(cats.items()))))
        seen_v.add(sid)
        for cat, d in i['refs'].items():
            for x, e in d.items():
                vitemrows.append((i['name'], sid, cat, N(x), x,
                                  db.items.get(x, {}).get('type', ''), e[0]))
    for p in PLACEMENTS:
        lab = gstr(p, 'name')
        if not lab or not SHOP_LABEL.search(lab):
            continue
        labelled += 1
        shop_by_label[lab] += 1
        owner = gstr(p, 'owner faction ID')
        sq = gstr(p, 'squad template')
        sqi = S.get(sq) if sq else None
        vls = list(sqi['refs'].get('vendors', {})) if sqi else []
        if vls:
            vendor_shops += 1
        root, attr = place_town(p)
        if root:
            town_n, town_owner = N(ACTIVE[root]), town_faction(ACTIVE[root])
        else:
            town_n, town_owner = '', ''
        cats = collections.Counter()
        for v in vls:
            vi = db.items.get(v)
            if vi:
                for k in vi['refs']:
                    cats[k] += 1
        shoprows.append((lab, p['ref'], N(p['ref']),
                         town_n, root or '', town_owner,
                         N(owner), owner, N(sq) if sq else '',
                         len(vls), '; '.join(N(v) for v in vls),
                         '; '.join('%s=%d' % kv for kv in sorted(cats.items())),
                         p['cx'], p['cy'], p['x'], p['z'], attr))
    write_tsv('shops.tsv',
              ['building_label', 'building_sid', 'building_name', 'town',
               'town_root_sid', 'town_faction', 'owner_faction',
               'owner_sid', 'vendor_squad', 'n_vendor_lists',
               'vendor_lists', 'vendor_categories', 'cell_x', 'cell_y',
               'x', 'z', 'attribution'], shoprows)
    write_tsv('vendor_lists.tsv',
              ['name', 'sid', 'items_count', 'money_item_min', 'money_item_max',
               'category_entry_counts'], vlistrows)
    write_tsv('vendor_items.tsv',
              ['vendor_list', 'vendor_list_sid', 'category', 'item', 'item_sid',
               'item_type', 'relative_chance_v0'], vitemrows)
    # per-town and per-label aggregates, derived from the rows above
    tagg = collections.defaultdict(lambda: [0, 0, 0, collections.Counter()])
    for r in shoprows:
        k = (r[3] or '(unattributed)', r[5])
        tagg[k][0] += 1
        tagg[k][1] += 1 if r[9] else 0
        tagg[k][2] += r[9]
        tagg[k][3][r[0]] += 1
    write_tsv('town_shops.tsv',
              ['town', 'town_faction', 'shop_labelled_placements',
               'vendor_capable_placements', 'vendor_list_refs', 'shop_labels'],
              [(t, f, v[0], v[1], v[2],
                '; '.join('%s=%d' % kv for kv in top_n(v[3])))
               for (t, f), v in sorted(tagg.items(),
                                       key=lambda kv: (-kv[1][0], kv[0]))])
    tvl = collections.Counter()
    for r in shoprows:
        for vn in (r[10].split('; ') if r[10] else []):
            if vn:
                tvl[(r[3] or '(unattributed)', r[5], vn)] += 1
    write_tsv('town_vendor_lists.tsv',
              ['town', 'town_faction', 'vendor_list', 'shop_placements'],
              [(t, f, v, n) for (t, f, v), n in
               sorted(tvl.items(), key=lambda kv: (-kv[1], kv[0]))])
    lagg = collections.defaultdict(lambda: [0, 0, collections.Counter()])
    for r in shoprows:
        lagg[r[0]][0] += 1
        lagg[r[0]][1] += 1 if r[9] else 0
        for kv in (r[11].split('; ') if r[11] else []):
            if '=' in kv:
                cat, n = kv.rsplit('=', 1)
                lagg[r[0]][2][cat] += int(n)
    write_tsv('shop_types.tsv',
              ['shop_label', 'placements', 'vendor_capable',
               'vendor_list_category_refs'],
              [(lab, v[0], v[1],
                '; '.join('%s=%d' % kv for kv in top_n(v[2])))
               for lab, v in sorted(lagg.items(),
                                    key=lambda kv: (-kv[1][0], kv[0]))])
    print('  placements', ZONE_STATS['placements_total'], '(building targets',
          len(PLACEMENTS), '+ missing targets',
          ZONE_STATS['survivors_missing_target'], ') shop-labelled', labelled,
          'with resolving vendor squad', vendor_shops, 'vendor lists', len(vlistrows),
          'vendor entries', len(vitemrows))
    print('  top labels', top_n(shop_by_label, 12))
    return shoprows

# production ----------------------------------------------------------
def building_prod_info(bsid):
    """Return (is_prod, functions, resources, outputs) for a BUILDING sid."""
    funcs, res, out = [], [], []
    isprod = False
    for fid in _BUILDINGS[bsid]['refs'].get('functionality', {}):
        f = db.items.get(fid)
        if not f:
            continue
        fun = f['int'].get('function')
        if f['bool'].get('is production building'):
            isprod = True
        if fun in (1, 4, 5, 10, 14, 26, 27):
            isprod = True
        if f['refs'].get('produces') or f['refs'].get('consumes') or \
                f['refs'].get('item crafts') or f['refs'].get('armour crafts') or \
                f['refs'].get('weapon crafts') or f['refs'].get('crossbow crafts') or \
                f['refs'].get('robotics crafts'):
            isprod = True
        funcs.append('%s(%s)' % (BUILDINGFUNCTION.get(fun, fun), fun))
        r = f['int'].get('world resource mining')
        if r:
            res.append('%s(%s)' % (MININGRESOURCE.get(r, r), r))
        for x in f['refs'].get('produces', {}):
            out.append(N(x))
    return isprod, funcs, res, out

def building_prod_mult(bsid):
    """Sorted distinct BUILDING_FUNCTIONALITY 'production mult' floats (as text)."""
    vals = set()
    for fid in _BUILDINGS[bsid]['refs'].get('functionality', {}):
        f = db.items.get(fid)
        if f and 'production mult' in f['float']:
            vals.add('%.6g' % f['float']['production mult'])
    return ';'.join(sorted(vals, key=float))

def section_production_rates():
    """Rate/time fields: BUILDING_FUNCTIONALITY.production mult, ITEM.production time."""
    fun_rows = []
    for sid, f in sorted(db.of('BUILDING_FUNCTIONALITY').items()):
        if 'production mult' not in f['float']:
            continue
        fn = f['int'].get('function')
        fun_rows.append((f['name'], sid, '%s(%s)' % (BUILDINGFUNCTION.get(fn, fn), fn),
                         '%.6g' % f['float']['production mult'],
                         f['bool'].get('is production building'),
                         f['bool'].get('output per day'),
                         f['int'].get('max operators'),
                         f['int'].get('tech level'),
                         len(f['refs'].get('produces', {})),
                         len(f['refs'].get('consumes', {}))))
    write_tsv('production_functionality_rates.tsv',
              ['functionality', 'sid', 'function', 'production_mult',
               'is_production_building', 'output_per_day_flag', 'max_operators',
               'tech_level', 'n_produces', 'n_consumes'], fun_rows)
    item_rows = [(i['name'], sid, i['int']['production time'])
                 for sid, i in sorted(db.of('ITEM').items())
                 if 'production time' in i['int']]
    write_tsv('item_production_time.tsv',
              ['item', 'sid', 'production_time_hours'], item_rows)
    print('  production mult records', len(fun_rows), 'items with production time',
          len(item_rows))

def section_production():
    print('[h] production')
    global PLACEMENTS
    if PLACEMENTS is None:
        PLACEMENTS = zone_placements()
    rows = []
    by_town = collections.Counter()
    by_fac = collections.Counter()
    prod_buildings = set()
    unattributed = 0
    missing_gsb = 0
    for p in PLACEMENTS:
        isprod, funcs, res, out = building_prod_info(p['ref'])
        if not isprod:
            continue
        prod_buildings.add(p['ref'])
        root, attr = place_town(p)
        if p.get('g') is None:
            missing_gsb += 1
        owner = gstr(p, 'owner faction ID')
        if root:
            town_n, town_fac = N(ACTIVE[root]), town_faction(ACTIVE[root])
        else:
            town_n, town_fac = '', ''
            unattributed += 1
        rows.append((p['ref'], N(p['ref']), town_n, root or '', town_fac,
                     N(owner), owner, ';'.join(sorted(set(funcs))),
                     ';'.join(sorted(set(res))), ';'.join(sorted(set(out))),
                     building_prod_mult(p['ref']),
                     p['cx'], p['cy'], p['x'], p['z'], attr))
        by_town[(town_n or '(unattributed)', N(owner))] += 1
        by_fac[N(owner)] += 1
    write_tsv('production.tsv',
              ['building_sid', 'building_name', 'town', 'town_root_sid',
               'town_faction', 'owner_faction', 'owner_sid', 'functions',
               'mining_resources', 'produces', 'production_mult', 'cell_x', 'cell_y', 'x', 'z',
               'attribution'], rows)
    srows = [(t, f, c) for (t, f), c in sorted(by_town.items(), key=lambda kv: (-kv[1], kv[0]))]
    write_tsv('production_by_town.tsv', ['town', 'owner_faction', 'placements'], srows)
    frows = [(f, c) for f, c in sorted(by_fac.items(), key=lambda kv: (-kv[1], kv[0]))]
    write_tsv('production_by_faction.tsv', ['owner_faction', 'placements'], frows)
    print('  production BUILDING records', len(prod_buildings),
          'placements', len(rows), 'unattributed', unattributed,
          'missing GSB state', missing_gsb)
    print('  top owners', frows[:10])
    return rows

# =====================================================================
# f. ROAMING SQUADS / SPAWN SOURCES
# =====================================================================
def squad_eligible(sid):
    i = S.get(sid)
    if not i:
        return True, 0
    ws = i['refs'].get('world state', {})
    return all(eval_state(w) == bool(e[0]) for w, e in ws.items()), len(ws)

def squad_ai(sid):
    i = S.get(sid)
    if not i:
        return []
    return [(p, e[0]) for p, e in i['refs'].get('AI packages', {}).items()]

def build_spawn_sources():
    rows = []   # squad, source_type, source_name, biome, detail, active_at_start
    # town roaming squads
    for r in TOWN_ROOTS:
        if r not in TOWN_POS:
            continue
        for sq in T[ACTIVE[r]]['refs'].get('roaming squads', {}):
            rows.append((sq, 'town roaming squad', N(ACTIVE[r]), '',
                         'town root ' + N(r), 'yes'))
    # biome homeless
    for label, bname, sq in HOMELESS:
        rows.append((sq, 'biome homeless spawn', bname, bname, label, 'yes'))
    # biome nests: resident squads of the nest TOWN
    for label, bname, t in NESTS:
        ti = T.get(t)
        if not ti:
            continue
        for key in ['residents', 'default resident', 'bar squads', 'roaming squads']:
            for sq in ti['refs'].get(key, {}):
                rows.append((sq, 'biome nest town', bname, bname,
                             'nest ' + N(t) + ' [' + key + ']', 'yes'))
    # campaigns: potential, not spawned at start
    for csid, ci in FC.items():
        elig, _ = cs_eligible(csid)
        for sq in inherited_ref(csid, 'squads to use'):
            rows.append((sq, 'campaign', N(csid), '',
                         'campaign owner ' + '; '.join(
                             N(f) for f in CAMPAIGN_OWNERS.get(csid, [])),
                         'no' if elig else 'no'))
    # faction roaming squads
    for fsid, fi in F.items():
        for sq in fi['refs'].get('roaming squads', {}):
            rows.append((sq, 'faction roaming squad', N(fsid), '', 'FACTION', 'yes'))
    # dedupe
    out, seen = [], set()
    for r in rows:
        if r[0] not in S:
            continue
        key = r[:5]
        if key in seen:
            continue
        seen.add(key)
        out.append(r)
    return out

SPAWN_SOURCES = build_spawn_sources()
DEFAULT_SQUAD_SIDS = set(
    r[0] for r in SPAWN_SOURCES
    if r[5] == 'yes' and squad_eligible(r[0])[0])

def section_roaming():
    print('[f] roaming squads')
    rows, srows = [], []
    bysq = collections.defaultdict(list)
    for sq, st, sn, biome, detail, active in SPAWN_SOURCES:
        bysq[sq].append((st, sn, biome, detail, active))
        srows.append((sq, N(sq), sq_faction(sq), st, sn, biome, detail, active,
                      squad_eligible(sq)[0]))
    for sq, srcs in bysq.items():
        i = S[sq]
        elig, nws = squad_eligible(sq)
        rows.append((N(sq), sq, sq_faction(sq), len(srcs),
                     '; '.join(sorted(set(x[0] for x in srcs))),
                     '; '.join(sorted(set(x[1] for x in srcs))),
                     i['bool'].get('is trader'),
                     len(i['refs'].get('vendors', {})),
                     len(i['refs'].get('squad', {})),
                     len(i['refs'].get('leader', {})),
                     len(i['refs'].get('animals', {})),
                     '; '.join('%s[%s]' % (N(p), pr) for p, pr in squad_ai(sq)),
                     elig, nws,
                     'yes' if (sq in DEFAULT_SQUAD_SIDS) else 'no'))
    write_tsv('roaming_squads.tsv',
              ['squad_name', 'squad_sid', 'faction', 'n_sources',
               'source_types', 'source_names', 'is_trader', 'n_vendor_lists',
               'n_squad_members', 'n_leader_members', 'n_animal_members',
               'AI_packages', 'world_state_eligible', 'n_world_state',
               'default_active'], rows)
    write_tsv('spawn_sources.tsv',
              ['squad_sid', 'squad_name', 'faction', 'source_type',
               'source_name', 'biome', 'detail', 'active_at_start',
               'squad_world_state_eligible'], srows)
    print('  source rows', len(SPAWN_SOURCES), 'distinct squads', len(bysq),
          'default-active squads', len(DEFAULT_SQUAD_SIDS),
          'active-source but state-ineligible', len(
              set(r[0] for r in SPAWN_SOURCES if r[5] == 'yes') - DEFAULT_SQUAD_SIDS),
          'inactive/non-start sources', sum(1 for r in SPAWN_SOURCES if r[5] != 'yes'))
    print('  source types', collections.Counter(r[1] for r in SPAWN_SOURCES))
    return rows

# =====================================================================
# g. AI PACKAGE FAMILIES
# =====================================================================
P = db.of('AI_PACKAGE')
AITASK = db.of('AI_TASK')
DL = db.of('DIALOGUE_LINE')
UST = db.of('UNIQUE_SQUAD_TEMPLATE')
SA = db.of('SINGLE_DIPLOMATIC_ASSAULT')

def ai_ancestors(sid):
    out, stack = [], [sid]
    while stack:
        x = stack.pop()
        for a in P.get(x, {}).get('refs', {}).get('inherits from', {}):
            if a in P and a not in out and a != sid:
                out.append(a)
                stack.append(a)
    return out

def ai_goals(sid):
    out = []
    for x in [sid] + ai_ancestors(sid):
        for key in ['Leader AI Goals', 'Squad AI Goals', 'Squad2 AI Goals',
                    'Slave AI Goals']:
            for t in P[x]['refs'].get(key, {}):
                if t not in out:
                    out.append(t)
    return out

FAMILY_RULES = [
    ('Escaping slave', r'escaping slave|escape prison'),
    ('Bodyguard', r'bodyguard'),
    ('Animal', r'\banimal|\bcreature|\bbeast|\bwolf|blood hunter|megabeast|'
               r'\bnest\b|goat|herbivor|raptor|gorillo|skimmer|land bat|'
               r'\bcrab\b|fish resident'),
    ('Cannibal', r'cannibal'),
    ('Fog', r'\bfog\b'),
    ('Slaver', r'slav|manhunt'),
    ('Diplomat', r'diplomat'),
    ('Robot', r'robot'),
    ('Guard', r'guard|\bgate\b|\bwall\b|police|cage warden|throne|'
              r'\bking\b|noble'),
    ('Patrol', r'patrol'),
    ('Trade & shops', r'shop|\bbar\b|saloon|trader|trade|vendor|market|'
                      r'merchant|hospital'),
    ('Raid & assault', r'raid|assault|occup|invas|campaign|military|attack|'
                       r'combat|retreat|loot|kidnap|demand|\bwar\b|flee|siege'),
    ('Wander & travel', r'wander|travel|passing|vagrant|exile|refug|nomad|'
                        r'roam|disband|go home|camping'),
    ('Resident & work', r'resident|civilian|worker|\bwork\b|farm|\bmine|'
                        r'squatter|\bhome\b|village|herder|undertaker|fish|'
                        r'lab\b|storage|craft|smith'),
]

def family_by_name(name):
    ln = name.lower()
    if ln.startswith('addon'):
        return 'Addon (mixins)'
    for fam, pat in FAMILY_RULES:
        if re.search(pat, ln):
            return fam
    return None

AI_FAMILY = {}
AI_FAMILY_RULE = {}
for p in P:
    f = family_by_name(P[p]['name'])
    how = 'name keyword'
    if f is None:
        for a in ai_ancestors(p):
            if P[a]['name'].lower().startswith('addon'):
                continue
            f = family_by_name(P[a]['name'])
            if f:
                how = 'nearest non-addon ancestor'
                break
    if f is None:
        f = 'Other / unclassified'
        how = 'none'
    AI_FAMILY[p] = f
    AI_FAMILY_RULE[p] = how

# default-world squads = town residents/bar/etc in placed active towns + spawns
DEFAULT_WORLD_SQUADS = set(s for s in DEFAULT_SQUAD_SIDS)
for _r in TOWN_ROOTS:
    if _r not in TOWN_POS:
        continue
    for key in ['residents', 'bar squads', 'default resident', 'roaming squads']:
        for _sq in T[ACTIVE[_r]]['refs'].get(key, {}):
            if _sq in S:
                DEFAULT_WORLD_SQUADS.add(_sq)

def section_ai():
    print('[g] AI packages')
    all_squad_refs = collections.Counter()
    def_squad_refs = collections.Counter()
    def_squads_of_pkg = collections.defaultdict(set)
    prio = collections.defaultdict(list)
    for sq, si in S.items():
        for p, e in si['refs'].get('AI packages', {}).items():
            all_squad_refs[p] += 1
            prio[p].append(e[0])
            if sq in DEFAULT_WORLD_SQUADS:
                def_squad_refs[p] += 1
                def_squads_of_pkg[p].add(sq)
    swaps = collections.Counter()
    contracts = collections.Counter()
    sets = collections.Counter()
    for s, i in DL.items():
        for p in i['refs'].get('change AI', {}):
            swaps[p] += 1
        for p in i['refs'].get('AI contract', {}):
            contracts[p] += 1
        for p in i['refs'].get('set AI package', {}):
            sets[p] += 1
    phases = collections.Counter()
    for csid, ci in FC.items():
        for ph in ['pt1 AI leader', 'pt1 AI others', 'pt2 AI leader',
                   'pt2 AI others', 'retreat AI leader', 'retreat AI others',
                   'victory AI leader', 'victory AI others']:
            for p in inherited_ref(csid, ph):
                phases[p] += 1
    assault_refs = collections.Counter()
    for s, i in SA.items():
        for k in ['AI packages', 'delivery AI package', 'fallback AI package']:
            for p in i['refs'].get(k, {}):
                assault_refs[p] += 1
    fallback_refs = collections.Counter()
    for s, i in UST.items():
        for p in i['refs'].get('fallback AI package', {}):
            fallback_refs[p] += 1
    lead_refs = collections.Counter()
    for s, i in db.items.items():
        for p in i['refs'].get('Leads to', {}):
            lead_refs[p] += 1
    faction_fallback = collections.Counter()
    for s, i in F.items():
        for p in i['refs'].get('AI fallback', {}):
            faction_fallback[p] += 1

    rows, arows = [], []
    famagg = collections.defaultdict(lambda: {
        'n': 0, 'all': 0, 'def': 0, 'swaps': 0, 'contracts': 0, 'sets': 0,
        'phases': 0, 'assault': 0, 'fallback': 0, 'tasks': collections.Counter(),
        'squads': set(), 'pkgs': []})
    for p, i in P.items():
        gl = ai_goals(p)
        own_gl = sum(len(i['refs'].get(k, {})) for k in
                     ['Leader AI Goals', 'Squad AI Goals', 'Squad2 AI Goals',
                      'Slave AI Goals'])
        tasks = []
        for t in gl:
            ti = AITASK.get(t, {}).get('int', {}).get('enum')
            tasks.append(TASKTYPE.get(ti, str(ti)))
        top = top_n(collections.Counter(tasks), 6)
        fam = AI_FAMILY[p]
        rows.append((i['name'], p, fam, AI_FAMILY_RULE[p],
                     '; '.join(N(a) for a in ai_ancestors(p)),
                     own_gl, len(gl), '; '.join('%s=%d' % kv for kv in top),
                     all_squad_refs[p], len(def_squads_of_pkg[p]),
                     (min(prio[p]) if prio[p] else ''),
                     (max(prio[p]) if prio[p] else ''),
                     swaps[p], contracts[p], sets[p], phases[p],
                     assault_refs[p], fallback_refs[p], lead_refs[p],
                     faction_fallback[p]))
        a = famagg[fam]
        a['n'] += 1
        a['all'] += all_squad_refs[p]
        a['def'] += len(def_squads_of_pkg[p])
        a['swaps'] += swaps[p]
        a['contracts'] += contracts[p]
        a['sets'] += sets[p]
        a['phases'] += phases[p]
        a['assault'] += assault_refs[p]
        a['fallback'] += fallback_refs[p]
        a['squads'] |= def_squads_of_pkg[p]
        a['pkgs'].append(i['name'])
        for t in sorted(set(tasks)):
            a['tasks'][t] += 1
    for fam, a in famagg.items():
        arows.append((fam, a['n'], len(a['squads']), a['all'], a['swaps'],
                      a['contracts'], a['sets'], a['phases'], a['assault'],
                      a['fallback'],
                      '; '.join('%s=%d' % kv for kv in top_n(a['tasks'], 6)),
                      '; '.join(sorted(a['pkgs'])[:12])))
    arows.sort(key=lambda r: (-r[2], -r[1], r[0]))
    write_tsv('ai_packages.tsv',
              ['name', 'sid', 'family', 'family_rule', 'inherits_from',
               'own_goal_count', 'resolved_goal_count', 'top_goal_tasks',
               'squad_refs_all', 'default_world_squads_using',
               'priority_min', 'priority_max', 'dialogue_change_AI',
               'dialogue_AI_contract', 'dialogue_set_AI_package',
               'campaign_phase_uses', 'diplomatic_assault_uses',
               'unique_squad_fallback', 'leads_to', 'faction_AI_fallback'], rows)
    write_tsv('ai_families.tsv',
              ['family', 'n_packages', 'default_world_squads_using',
               'squad_refs_all', 'dialogue_change_AI', 'dialogue_AI_contract',
               'dialogue_set_AI_package', 'campaign_phase_uses',
               'diplomatic_assault_uses', 'unique_squad_fallback',
               'top_goal_tasks', 'example_packages'], arows)
    print('  AI_PACKAGE records', len(P), 'families', len(arows))
    for r in arows:
        print('   %-20s packages=%2d default_squads=%3d all_refs=%3d swaps=%3d contracts=%3d phases=%3d'
              % (r[0], r[1], r[2], r[3], r[4], r[5], r[7]))
    return arows

# =====================================================================
# e. PRICE DATA
# =====================================================================
def section_prices():
    print('[e] prices')
    rows = []
    for t in ['ITEM', 'WEAPON', 'ARMOUR', 'CONTAINER', 'LIMB', 'MAP_ITEM',
              'WEAPON_MANUFACTURER']:
        for s, i in db.of(t).items():
            man = ''
            if t == 'WEAPON':
                mf = i['refs'].get('manufacturer', {})
                if mf:
                    mi = db.items.get(list(mf)[0], {})
                    man = '%s(price_mod=%s)' % (N(list(mf)[0]),
                                                mi.get('float', {}).get('price mod', ''))
            rows.append((t, i['name'], s, i['int'].get('value'),
                         i['float'].get('profitability', ''),
                         i['float'].get('relative price mult', ''),
                         i['float'].get('price mod', ''), man))
    write_tsv('item_values.tsv',
              ['record_type', 'name', 'sid', 'value', 'profitability',
               'relative_price_mult', 'price_mod', 'weapon_manufacturer'], rows)
    # culture assignments
    crows, cirows = [], []
    for s, i in db.of('ITEMS_CULTURE').items():
        for x, e in i['refs'].get('trade prices', {}).items():
            cirows.append((i['name'], s, N(x), x,
                           db.items.get(x, {}).get('type', ''), e[0]))
    active_towns = set(ACTIVE.values())
    for s, i in F.items():
        for x in i['refs'].get('trade culture', {}):
            crows.append(('FACTION', i['name'], s, N(x), x, 'yes'))
    for s, i in T.items():
        for x in i['refs'].get('trade culture', {}):
            crows.append(('TOWN', i['name'], s, N(x), x,
                          'yes' if s in active_towns else 'no'))
    write_tsv('trade_cultures.tsv',
              ['owner_type', 'owner_name', 'owner_sid', 'culture_name',
               'culture_sid', 'active_at_default'], crows)
    write_tsv('trade_culture_items.tsv',
              ['culture_name', 'culture_sid', 'item', 'item_sid', 'item_type',
               'multiplier_v0'], cirows)
    srows = []
    for s, i in S.items():
        if ('buy mult' in i['float'] or 'sell mult' in i['float'] or
                i['bool'].get('is trader') or i['refs'].get('vendors')):
            srows.append((i['name'], s, i['float'].get('buy mult'),
                          i['float'].get('sell mult'),
                          i['int'].get('vendor money'),
                          i['int'].get('vendors fill total amount'),
                          i['float'].get('vendors refresh time'),
                          i['float'].get('vendors fill rate'),
                          i['bool'].get('is trader'),
                          len(i['refs'].get('vendors', {}))))
    write_tsv('squad_trade.tsv',
              ['squad_name', 'squad_sid', 'buy_mult', 'sell_mult',
               'vendor_money', 'vendors_fill_total_amount',
               'vendors_refresh_time', 'vendors_fill_rate', 'is_trader',
               'n_vendor_lists'], srows)
    vals = [r[3] for r in rows if isinstance(r[3], int)]
    print('  price-bearing records', len(rows), 'value min/max', min(vals), max(vals))
    print('  cultures', len(db.of('ITEMS_CULTURE')), 'assignments', len(crows),
          'culture item multipliers', len(cirows), 'squads with trade data', len(srows))
    print('  towns with trade culture', sum(1 for r in crows if r[0] == 'TOWN'),
          'factions with trade culture', sum(1 for r in crows if r[0] == 'FACTION'))
    return rows

# =====================================================================
# a. NEW GAME STARTS AND WORLD-STATE DRIVERS
# =====================================================================
def section_starts():
    print('[a] new game starts')
    rows = []
    for s, i in db.of('NEW_GAME_STARTOFF').items():
        rows.append((i['name'], s, i['string'].get('difficulty'),
                     i['string'].get('style'), i['int'].get('money'),
                     i['int'].get('start pos X'), i['int'].get('start pos Z'),
                     i['bool'].get('force start pos'),
                     '; '.join(N(x) for x in i['refs'].get('squad', {})),
                     '; '.join(N(x) for x in i['refs'].get('town', {})),
                     '; '.join(N(x) for x in i['refs'].get('faction relations', {})),
                     '; '.join(N(x) for x in i['refs'].get('force race', {})),
                     '; '.join(N(x) for x in i['refs'].get('research', {}))))
    write_tsv('new_game_starts.tsv',
              ['name', 'sid', 'difficulty', 'style', 'money', 'start_pos_X',
               'start_pos_Z', 'force_start_pos', 'starting_squads',
               'starting_towns', 'faction_relations', 'force_race',
               'starting_research'], rows)
    print('  starts', len(rows), 'with any world-state field',
          sum(1 for s, i in db.of('NEW_GAME_STARTOFF').items()
              if i['refs'].get('world state') or 'world state' in i['bool']))
    return rows

def section_world_drivers():
    print('[a] world-state drivers')
    # town variants
    vorows = collections.defaultdict(list)
    for v, vi in T.items():
        for w, e in vi['refs'].get('world state', {}).items():
            # find roots whose variant list contains v
            roots = [r for r in TOWN_ROOTS
                     if v in T[r]['refs'].get('override town', {})]
            vorows[w].append('%s -> %s [need %s]%s' % (
                '; '.join(N(r) for r in roots) or '(unreachable)',
                N(v), e[0], ' ACTIVE' if any(ACTIVE.get(r) == v for r in roots) else ''))
    crows = collections.defaultdict(list)
    for c, ci in FC.items():
        for w, e in ci['refs'].get('world state', {}).items():
            crows[w].append('%s [need %s]' % (N(c), e[0]))
    srows = collections.defaultdict(list)
    for sq, si in S.items():
        for w, e in si['refs'].get('world state', {}).items():
            srows[w].append('%s [need %s]' % (N(sq), e[0]))
    rows = []
    for w, wi in WES.items():
        rows.append((wi['name'], w, eval_state(w), len(vorows[w]),
                     ' | '.join(sorted(vorows[w])),
                     len(crows[w]), ' | '.join(sorted(crows[w])),
                     len(srows[w]), ' | '.join(sorted(srows[w]))))
    write_tsv('world_state_drivers.tsv',
              ['state_name', 'state_sid', 'true_at_default', 'n_town_variants',
               'town_variants', 'n_campaigns', 'campaigns', 'n_squads', 'squads'],
              rows)
    print('  states used by town variants', sum(1 for w in WES if vorows[w]),
          'by campaigns', sum(1 for w in WES if crows[w]),
          'by squads', sum(1 for w in WES if srows[w]))
    return rows

# =====================================================================
# b. MOST IMPORTANT FACTIONS / RELATION MATRIX
# =====================================================================
def faction_importance():
    return sorted(F, key=lambda f: (-PLACED_OWNERS[f],
                                    -(len(F[f]['refs'].get('relations', {})) +
                                      len(F[f]['refs'].get('coexistence', {}))),
                                    -len(F[f]['refs'].get('campaigns', {})),
                                    F[f]['name']))

def section_relation_matrix():
    print('[b] relation matrix')
    top = faction_importance()[:15]
    rows = []
    for a in top:
        row = [N(a)]
        for b in top:
            if a == b:
                row.append('')
            else:
                e = F[a]['refs'].get('relations', {}).get(b)
                row.append(e[0] if e else F[a]['int'].get('default relation'))
        rows.append(row)
    write_tsv('faction_relations_top.tsv',
              ['faction'] + [N(b) for b in top], rows)
    write_tsv('faction_importance.tsv',
              ['rank', 'faction', 'sid', 'kind', 'towns_owned',
               'relation_entries', 'campaigns'],
              [(i + 1, N(f), f, faction_kind(f), PLACED_OWNERS[f],
                len(F[f]['refs'].get('relations', {})),
                len(F[f]['refs'].get('campaigns', {})))
               for i, f in enumerate(faction_importance())])
    print('  top 15', [N(x) for x in top])
    return top

# =====================================================================
# e. ARBITRAGE-RELEVANT MULTIPLIERS
# =====================================================================
def section_arbitrage():
    print('[e] arbitrage multipliers')
    owners = collections.defaultdict(list)
    for typ, name, sid, cname, csid in [
            (r[0], r[1], r[2], r[3], r[4])
            for r in []]:
        pass
    # culture -> owners
    active_towns = set(ACTIVE.values())
    cult_owners = collections.defaultdict(list)
    for s, i in F.items():
        for c in i['refs'].get('trade culture', {}):
            cult_owners[c].append('FACTION:' + N(s))
    for s, i in T.items():
        if s not in active_towns:
            continue
        for c in i['refs'].get('trade culture', {}):
            cult_owners[c].append('TOWN:' + N(s))
    byitem = collections.defaultdict(list)
    for s, i in db.of('ITEMS_CULTURE').items():
        for x, e in i['refs'].get('trade prices', {}).items():
            byitem[x].append((i['name'], e[0]))
    for s, i in T.items():
        if s not in active_towns:
            continue
        for x, e in i['refs'].get('trade prices', {}).items():
            byitem[x].append(('TOWN:' + N(s), e[0]))
    rows = []
    for x, vals in byitem.items():
        mults = [v for _, v in vals if isinstance(v, (int, float))]
        if not mults:
            continue
        rows.append((N(x), x, db.items.get(x, {}).get('type', ''),
                     db.items.get(x, {}).get('int', {}).get('value', ''),
                     len(vals), '; '.join('%s=%s' % v for v in vals),
                     min(mults), max(mults),
                     (max(mults) / min(mults)) if min(mults) else '',
                     len(set(v[0] for v in vals))))
    rows.sort(key=lambda r: (-(r[7] if isinstance(r[7], (int, float)) else 0), r[0]))
    write_tsv('arbitrage.tsv',
              ['item', 'item_sid', 'item_type', 'base_value', 'n_multipliers',
               'multipliers', 'min_multiplier', 'max_multiplier', 'spread_ratio',
               'n_multiplier_sources'], rows)
    print('  items with culture/town multipliers', len(rows),
          'assignments', sum(r[4] for r in rows))
    return rows

def section_default_sensitivity():
    print('[a] default sensitivity (player involvement ignored vs enforced)')
    rows = []
    changed = 0
    for r in TOWN_ROOTS:
        a_ch, _ = resolve_town(r, 'chosen')
        a_st, _ = resolve_town(r, 'strict')
        changed += 1 if a_ch != a_st else 0
        if a_ch != a_st:
            rows.append((N(r), r, N(a_ch), a_ch, N(a_st), a_st))
    write_tsv('default_sensitivity.tsv',
              ['root_name', 'root_sid', 'chosen_active', 'chosen_sid',
               'strict_active', 'strict_sid'], rows)
    print('  town roots differing', changed)
    return rows

# =====================================================================
# SUMMARY
# =====================================================================
def section_summary():
    print('[summary]')
    kinds = collections.Counter(faction_kind(f) for f in F)
    pl = PLACEMENTS or zone_placements()
    shop_labelled = vendor_capable = 0
    for p in pl:
        lab = gstr(p, 'name')
        if lab and SHOP_LABEL.search(lab):
            shop_labelled += 1
            sq = gstr(p, 'squad template')
            si = S.get(sq) if sq else None
            if si and si['refs'].get('vendors'):
                vendor_capable += 1
    prod_all = sum(1 for b in _BUILDINGS if building_prod_info(b)[0])
    prod_placed = len(set(p['ref'] for p in pl if building_prod_info(p['ref'])[0]))
    rows = [
        ('merged_items', len(db.items)),
        ('records_parsed', sum(db.record_counts.values())),
        ('factions_total', len(F)),
        ('factions_major', kinds['major']),
        ('factions_minor', kinds['minor']),
        ('factions_roaming', kinds['roaming']),
        ('factions_animal', kinds['animal']),
        ('factions_player_related', kinds['player-related']),
        ('factions_unused', kinds['unused']),
        ('town_records', len(T)),
        ('default_town_roots', len(TOWN_ROOTS)),
        ('placed_town_roots', sum(1 for r in TOWN_ROOTS if r in TOWN_POS)),
        ('town_placements', len(TOWN_ENTRIES)),
        ('base_level_town_placements_not_used', len(BASE_LEVEL_TOWNS)),
        ('variant_records', len(OVERRIDE_TARGETS)),
        ('towns_changed_at_default', sum(1 for r in TOWN_ROOTS if ACTIVE[r] != r)),
        ('world_states', len(WES)),
        ('world_states_true_chosen', sum(1 for s in WES if eval_state(s))),
        ('world_states_true_strict', sum(1 for s in WES if eval_state(s, 'strict'))),
        ('campaigns', len(FC)),
        ('campaigns_eligible_default', sum(1 for s in FC if cs_eligible(s)[0])),
        ('campaign_owning_factions', len(set(f for v in CAMPAIGN_OWNERS.values() for f in v))),
        ('diplomatic_assault_containers', len(DA)),
        ('diplomatic_assault_singles', len(SA)),
        ('squad_templates', len(S)),
        ('vendor_lists', len(db.of('VENDOR_LIST'))),
        ('shop_trader_classes', len(db.of('SHOP_TRADER_CLASS'))),
        ('shop_labelled_placements', shop_labelled),
        ('vendor_capable_shop_placements', vendor_capable),
        ('placements_total', ZONE_STATS['placements_total']),
        ('building_placements', len(pl)),
        ('placements_missing_target', ZONE_STATS['survivors_missing_target']),
        ('placements_level_local_target', ZONE_STATS['survivors_level_local']),
        ('production_building_records', prod_all),
        ('production_building_records_placed', prod_placed),
        ('production_placements', sum(1 for p in pl if building_prod_info(p['ref'])[0])),
        ('roaming_source_rows', len(SPAWN_SOURCES)),
        ('roaming_distinct_templates', len(set(r[0] for r in SPAWN_SOURCES))),
        ('roaming_default_active', len(DEFAULT_SQUAD_SIDS)),
        ('default_world_squads', len(DEFAULT_WORLD_SQUADS)),
        ('ai_packages', len(P)),
        ('ai_families', len(set(AI_FAMILY.values()))),
        ('new_game_starts', len(db.of('NEW_GAME_STARTOFF'))),
    ]
    write_tsv('summary.tsv', ['metric', 'value'], rows)
    for k, v in rows:
        print('  %-36s %s' % (k, v))
    return rows

def main():
    section_load_order()
    section_starts()
    section_world_state()
    section_towns()
    section_base_level()
    section_zone_overlay()
    section_factions()
    section_relation_matrix()
    section_campaigns()
    section_assaults()
    section_shops()
    section_production()
    section_production_rates()
    section_roaming()
    section_ai()
    section_prices()
    section_world_drivers()
    section_default_sensitivity()
    section_arbitrage()
    section_summary()
    print('done: %s' % OUT)

if __name__ == '__main__':
    main()
