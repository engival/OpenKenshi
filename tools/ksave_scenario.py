#!/usr/bin/env python3
"""Original save -> ksim scenario file (a TEST FIXTURE, not save compatibility).

  ksave_scenario.py SAVEDIR OUT.scenario [--radius R] [--game DIR] [--name NAME]

Reads an original Kenshi save dir (quick.save, platoon/*.platoon; tools/ksave.py) and writes every character within
R units (default 2000) of the player leader into a KSCEN text file (docs/reference/SCENARIO_IMPORT.md) that
`ksim_server --scenario-file` and `ksim_fight` load.  Beside it, `OUT.kclient_args` gets one line of kclient args when
the save holds the camera.  Every sid the file mentions is checked against the merged game data (the vanilla load
order: gamedata.base, Newwworld, Dialogue, rebirth); a sid that does not resolve (e.g. a mod item) is listed and the
run FAILS.  Nothing is substituted.

Converted files are game-derived: write them under ~/git/kenshi/kenshi-inspect/findings/fight_fixtures/, never in the repo.
The save is only read.
"""
import sys, os, math, argparse, re
from urllib.parse import quote

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ksave
import kenshi_install

# STATS record name -> ksim FighterSkills field.  The file keeps every STATS value; the loader maps these.
SAFE = '.-_~+'


def enc(v):
	if isinstance(v, bool): v = int(v)
	if isinstance(v, float): v = repr(v)
	return quote(str(v), safe=SAFE)


def kv(pairs):
	return ' '.join('%s=%s' % (k, v) for k, v in pairs if v is not None)


def lst(items):
	return ','.join(items)


def rot_forward(q):
	"""View direction of the camera quaternion q = (w, x, y, z): the conjugate rotates (0, 0, -1).
	UNSURE: the saved rotation looks inverse of Ogre's camera orientation; with q itself the pitch of the sb1 save came out +26.6 deg (up) although
	the camera sits above its focus looking down, so the conjugate is used.  The yaw sign follows that guess."""
	w, x, y, z = q
	x, y, z = -x, -y, -z
	return -(2 * (x * z + w * y)), -(2 * (y * z - w * x)), -(1 - 2 * (x * x + y * y))


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument('savedir')
	ap.add_argument('out')
	ap.add_argument('--radius', type=float, default=2000.0)
	ap.add_argument('--centre', default=None, help='X,Z the radius is measured from (default: the player leader); the loader removes the default-world squads inside it')
	ap.add_argument('--attack', action='append', default=[], metavar='ATTACKERS_RE|TARGETS_RE|X,Z|CYCLE[|YMAX]',
		help='give `order` records (KSCEN 2): every character whose "Squad/Name" matches ATTACKERS_RE is ordered to attack one of the picked non-player characters '
		'matching TARGETS_RE that are standing (y below YMAX if given), ordered by distance to X,Z; the attackers, ordered by distance to X,Z, take the CYCLE nearest '
		'targets round robin (CYCLE 1: all attack the nearest). Repeatable; pass as --attack=SPEC. This is how kinspect ordered the crowd fixtures at load.')
	ap.add_argument('--game', default=None, help='game data dir (default: kenshi_install.py finds it)')
	ap.add_argument('--name', default=None)
	ap.add_argument('--weather-slot', type=int, default=None, help='BIOMES slot to take the weather from (the slot of the player biome group is ambiguous)')
	a = ap.parse_args()

	a.game = kenshi_install.data_dir(a.game)
	game = ksave.Game(a.game)
	if not game.recs: sys.exit('no game data under ' + a.game)
	S = ksave.Save(a.savedir, game)
	cam = S.camera
	pfaction = cam['pfaction name']
	chars = S.characters()

	# the player leader: a leader of a squad of the player's faction (CAMERA selected character is a handle, not resolvable here)
	def pf_squad(c): return S.platoons[c['platoon']]['faction name'] == pfaction
	leaders = [c for c in chars if pf_squad(c) and c['state']['GAMESTATE_CHARACTER'].get('is leader')]
	if not leaders: sys.exit('no player leader found (faction %r)' % pfaction)
	lead = leaders[0]
	cx, cz = lead['pos'][0], lead['pos'][2]
	if a.centre:
		cx, cz = (float(v) for v in a.centre.split(','))

	picked = [c for c in chars if math.hypot(c['pos'][0] - cx, c['pos'][2] - cz) <= a.radius]
	missing = {}   # sid -> what refers to it

	def need(sid, why):
		if sid and sid not in game.recs: missing.setdefault(sid, []).append(why)
		return sid

	lines = ['KSCEN 2' if a.attack else 'KSCEN 1']
	lines.append('source ' + kv([('save', enc(a.name or os.path.basename(os.path.normpath(a.savedir)))), ('game', enc(cam['version'])),
		('radius', enc(a.radius)), ('centre', '%s,%s' % (enc(cx), enc(cz))), ('leader', enc(lead['name']))]))
	d, h, m = S.clock()
	lines.append('clock ' + kv([('day', d), ('hour', h), ('minute', m)]))

	# weather: the slot whose SEASON belongs to the player's biome group (the slot->group map is not saved: ksave.py guess)
	grp = cam.get('biome_text_groud')
	wt = S.weathers()
	slots = sorted(i for i, v in wt.items() if v.get('s') in game.seasons.get(grp, ()))
	if a.weather_slot is not None: slots = [a.weather_slot]
	if grp and len(slots) == 1:
		w0 = wt[slots[0]]
		lines.append('weather ' + kv([('group', enc(need(grp, 'biome group'))), ('weather', enc(need(w0['w'], 'weather slot %d' % slots[0]))),
			('season', enc(w0.get('s'))), ('slot', slots[0])]))
	else:
		# several biome groups share a SEASON, so the season does not name the slot (ksave.py: the slot -> group map is not saved)
		print('warning: weather slot of biome group %r is %s; the file carries NO weather (pass --weather-slot N to choose; slots: %s)' % (
			grp, 'ambiguous (%s)' % slots if slots else 'unknown', {i: game.name(wt[i]['w']) for i in slots}), file=sys.stderr)

	# camera
	camargs = None
	try:
		pos, q, zoom = cam['pos'], cam['rot'], cam['zoom']
		f = rot_forward(q)
		# z mirrored against kclient's yaw (0 looks toward -z): checked live by the client on sb1 (2a9d76a; atan2(x, -z) gave -47.3 and faced the
		# canyon wall, atan2(x, z) = -132.7 frames Flashbox and the bandits; findings/fight_fixtures/sb1_flashbox/camera_yaw_grid.png)
		yaw = math.degrees(math.atan2(f[0], f[2]))
		pitch = math.degrees(math.asin(max(-1.0, min(1.0, f[1]))))
		lines.append('camera ' + kv([('pos', '%s,%s,%s' % tuple(enc(v) for v in pos)), ('yaw', enc(round(yaw, 3))), ('pitch', enc(round(pitch, 3))), ('dist', enc(round(-zoom, 3)))]))
		camargs = '--camera-pos %.3f %.3f %.3f --camera-yaw %.3f --camera-pitch %.3f --camera-dist %.3f' % (pos + (yaw, pitch, -zoom))
	except KeyError:
		pass

	# relation of each faction toward the player's faction (the rows of GAMESTATE_FACTION; a missing entry = 0)
	pf_rec = [r for r in S.factions if r.name == pfaction]
	pf_sid = pf_rec[0]['gamedata stringID'] if pf_rec else None
	toward_player = {}
	for r in S.factions:
		for k, v in r.d.items():
			if k.startswith('relationSID') and v == pf_sid:
				toward_player[r['gamedata stringID']] = r.get('relation' + k[len('relationSID'):], 0.0)

	# squads (those with a picked member), in file order of first appearance
	order = []
	for c in picked:
		if c['platoon'] not in order: order.append(c['platoon'])
	sq_id = {pn: i + 1 for i, pn in enumerate(order)}
	for pn in order:
		p = S.platoons[pn]
		lines.append('squad ' + kv([('id', sq_id[pn]), ('name', enc(pn)), ('faction', enc(need(p['faction stringID'], 'squad %s faction' % pn))),
			('template', enc(need(p['squad template'], 'squad %s template' % pn))), ('package', enc(need(p['currentPackage'], 'squad %s package' % pn))),
			('player', int(p['faction name'] == pfaction)), ('money', int(p['money'])),
			('hostile', int(p['faction name'] != pfaction and toward_player.get(p['faction stringID'], 0.0) < 0))]))

	# characters
	for n, c in enumerate(picked, 1):
		st = c['state']; ch = st['GAMESTATE_CHARACTER']; md = st['MEDICAL_STATE']; ap_ = st['CHARACTER_APPEARANCE']; sd = st['STATS']
		who = '%s/%s' % (c['platoon'], c['name'])
		x, y, z = c['pos']; w, qx, qy, qz = c['rot']
		yaw = math.atan2(2 * (w * qy + qx * qz), 1 - 2 * (qy * qy + qx * qx))   # rotation about +y (q is a pure y rotation in the saves seen)
		rr = ap_.refs.get('race')
		race = rr[0][0] if rr else ''
		lines.append('char ' + kv([('id', n), ('squad', sq_id[c['platoon']]), ('name', enc(c['name'])), ('template', enc(need(c['template'], who + ' template'))),
			('race', enc(need(race, who + ' race'))), ('x', enc(x)), ('y', enc(y)), ('z', enc(z)), ('yaw', enc(round(yaw, 6))),
			('leader', int(bool(ch.get('is leader')))), ('player', int(S.platoons[c['platoon']]['faction name'] == pfaction)),
			('faction', enc(need(ch.get('owner faction ID'), who + ' faction'))),
			('passive', int(ch.get('passive mode', 0))), ('defensive', int(ch.get('defensive mode', 0))), ('taunt', int(ch.get('taunt mode', 0))),
			('chase', int(ch.get('chase mode', 0))), ('ranged', int(ch.get('ranged mode', 0))), ('stealth', int(ch.get('stealth', 0))),
			('noncombat', int(ch.get('non combat mode', 0))), ('speed', int(ch.get('speed mode', 0))), ('age', enc(ch.get('age', 1.0))),
			('blood', enc(md['blood'])), ('hunger', enc(md['hung'])), ('fed', enc(md.get('fed', 0.0))), ('bleeding', enc(md['bleeding'])), ('ko', enc(md['KO'])),
			('unconscious', int(md['unconcious'])), ('coma', int(md['coma'])), ('dead', int(md['dead'])), ('incapacitated', int(md['incapacitated'])),
			('stats', lst('%s:%s' % (enc(k), enc(v)) for k, v in sorted(sd.d.items()) if isinstance(v, float)))]))
		for i in range(7):
			sid = md.get('sid%d' % i)
			if not sid: continue
			lines.append('limb ' + kv([('char', n), ('sid', enc(need(sid, who + ' limb %d' % i))), ('flesh', enc(md['flesh%d' % i])), ('hit', enc(md['hit%d' % i])),
				('hitmult', enc(md['hitmult%d' % i])), ('wear', enc(md['wear%d' % i])), ('bandage', enc(md['bandage%d' % i])), ('stun', enc(md['stun%d' % i])), ('rig', enc(md['rig%d' % i]))]))
		for it in S.inventory(c):
			lines.append('item ' + kv([('char', n), ('sid', enc(need(it['base data sid'], who + ' item'))), ('section', enc(it['section'])), ('qty', int(it['quantity'])),
				('level', int(it['level'])), ('quality', enc(it['quality'])), ('material', enc(need(it['material sid'], who + ' item material'))),
				('company', enc(need(it['company sid'], who + ' item company'))), ('color', enc(need(it['color sid'], who + ' item colour'))),
				('x', int(it['inventory x'])), ('y', int(it['inventory y'])), ('charges', enc(it['charges']))]))
		morphs = [(k, v) for k, v in sorted(ap_.d.items()) if isinstance(v, float)]
		skin = ap_.get('Skin Tone')
		lines.append('look ' + kv([('char', n), ('female', int(ap_['sex female'])), ('age', enc(ap_.get('Age', 0.0))),
			('head', enc(need(ap_.get('head'), who + ' head'))), ('hair', enc(need(ap_.get('hair style'), who + ' hair'))), ('beard', enc(need(ap_.get('beard'), who + ' beard'))),
			('stance', enc(ap_.get('idle stance', ''))), ('skin', lst(enc(v) for v in skin) if skin else None),
			('morphs', lst('%s:%s' % (enc(k), enc(v)) for k, v in morphs))]))

	# initial attack orders (KSCEN 2), after every char/limb/item/look line so the ids exist
	for spec in a.attack:
		parts = spec.split('|')
		att_re, tgt_re, near, cycle = parts[0], parts[1], parts[2], int(parts[3])
		ymax = float(parts[4]) if len(parts) > 4 else None
		ox, oz = (float(v) for v in near.split(','))
		def label(c): return '%s/%s' % (c['platoon'], c['name'])
		def dist(c): return math.hypot(c['pos'][0] - ox, c['pos'][2] - oz)
		def alive(c):
			md = c['state']['MEDICAL_STATE']
			return not (md['dead'] or md['unconcious'] or md['coma'] or md['incapacitated'])
		is_player = lambda c: S.platoons[c['platoon']]['faction name'] == pfaction
		numbered = [(n, c) for n, c in enumerate(picked, 1)]
		targets = sorted(((c, n) for n, c in numbered if re.search(tgt_re, label(c)) and not is_player(c) and alive(c) and (ymax is None or c['pos'][1] < ymax)),
			key=lambda t: dist(t[0]))[:max(1, cycle)]
		attackers = sorted(((c, n) for n, c in numbered if re.search(att_re, label(c)) and not is_player(c)), key=lambda t: dist(t[0]))
		if not targets:
			sys.exit('--attack %r: no standing target matches %r' % (att_re, tgt_re))
		for i, (c, n) in enumerate(attackers):
			lines.append('order ' + kv([('char', n), ('attack', targets[i % len(targets)][1])]))
		print('--attack %s: %d attackers over %d targets (%s)' % (att_re, len(attackers), len(targets), ', '.join(label(t[0]) for t in targets)), file=sys.stderr)

	if missing:
		print('FAILED: %d referenced record(s) are not in the merged game data (vanilla load order); nothing written, nothing substituted:' % len(missing), file=sys.stderr)
		for sid, why in sorted(missing.items()):
			print('  %s  <- %s%s' % (sid, '; '.join(why[:3]), ' (+%d more)' % (len(why) - 3) if len(why) > 3 else ''), file=sys.stderr)
		sys.exit(1)

	with open(a.out, 'w') as f: f.write('\n'.join(lines) + '\n')
	print('%s: %d characters in %d squads within %g of %s (%d player-controlled)' % (a.out, len(picked), len(order), a.radius, lead['name'],
		sum(1 for c in picked if S.platoons[c['platoon']]['faction name'] == pfaction)))
	side = os.path.splitext(a.out)[0] + '.kclient_args'
	if camargs:
		with open(side, 'w') as f: f.write(camargs + '\n')
		print('%s: %s' % (side, camargs))


if __name__ == '__main__':
	main()
