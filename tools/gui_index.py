#!/usr/bin/env python3
"""Index the original game's UI art (MyGUI data under data/gui) for grep, and cut each skin out of its texture.

Every piece of the original UI is named in text: a .layout widget names a skin (or a template, which names skins),
a ResourceSkin names a texture and one pixel rect per state for each of its sub-skins, an ImageSet names a texture
and frame points. This walks all of it, writes a one-line-per-thing index and renders every skin state at its
nominal size (sub-skins pasted at their offsets, as MyGUI draws it unstretched) plus every imageset frame.

Output (game-derived: never in this repo, see CLAUDE.md), default ~/git/kenshi/kenshi-inspect/findings/gui_index/:
	index.txt   grep it: `skin NAME`, `template NAME`, `image SET/GROUP/INDEX`, `pointer NAME`, `widget LAYOUT PATH`
	index.json  the same, structured
	crops/      skin/NAME.STATE.png, image/SET.GROUP.INDEX.png

Skins a layout names that are not in the data (PanelEmpty, ImageBox, ...) are MyGUI's built-in core skins:
listed as `builtin`. Skins the exe assigns in code are not visible here.

Usage: gui_index.py [--data GAME_DATA_DIR] [--out DIR] [--no-crops]
"""
import argparse, json, os, sys
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kenshi_install

DEFAULT_OUT = os.path.expanduser('~/git/kenshi/kenshi-inspect/findings/gui_index')

def ints(s):
	return [int(float(v)) for v in s.split()] if s else []

def parse(path):
	with open(path, 'rb') as f:
		return ET.fromstring(f.read().lstrip(b'\xef\xbb\xbf'))

class Index:
	def __init__(s, gui):
		s.gui = gui
		s.skins = {}		# name -> {texture, size, file, subs: [{type, offset, align, states: {state: rect}}]}
		s.templates = {}	# name -> {file, skins: [...]}
		s.images = {}		# "set/group/index" -> {texture, size, point, file}
		s.pointers = {}		# name -> {point, size, resource}
		s.widgets = []		# {layout, path, type, skin, name, caption, image, font}
		s.textures = {}		# lower-case basename -> path
		for root, _, files in os.walk(gui):
			for f in files:
				s.textures.setdefault(f.lower(), os.path.join(root, f))

	def load(s):
		for root, _, files in sorted(os.walk(s.gui)):
			for f in sorted(files):
				path = os.path.join(root, f)
				rel = os.path.relpath(path, s.gui)
				if f.endswith('.xml'):
					s.resources(parse(path), rel)
				elif f.endswith('.layout'):
					for w in parse(path).findall('Widget'):
						s.widget(w, rel, '')

	def resources(s, xml, rel):
		for r in xml.iter('Resource'):
			kind, name = r.get('type'), r.get('name')
			if kind == 'ResourceSkin':
				subs = []
				for b in r.findall('BasisSkin'):
					states = {st.get('name'): ints(st.get('offset')) for st in b.findall('State') if st.get('offset')}
					subs.append({'type': b.get('type'), 'offset': ints(b.get('offset')), 'align': b.get('align', ''), 'states': states})
				s.skins[name] = {'texture': r.get('texture', ''), 'size': ints(r.get('size')), 'file': rel, 'subs': subs}
			elif kind == 'ResourceLayout':
				s.templates[name] = {'file': rel, 'skins': [w.get('skin') for w in r.iter('Widget') if w.get('skin')]}
			elif kind == 'ResourceImageSet':
				for g in r.findall('Group'):
					for i in g.findall('Index'):
						frame = i.find('Frame')
						s.images[f"{name}/{g.get('name')}/{i.get('name')}"] = {'texture': g.get('texture', ''), 'size': ints(g.get('size')),
							'point': ints(frame.get('point')) if frame is not None else [0, 0], 'file': rel}
			elif kind == 'ResourceImageSetPointer':
				props = {p.get('key'): p.get('value') for p in r.findall('Property')}
				s.pointers[name] = {'point': props.get('Point', ''), 'size': props.get('Size', ''), 'resource': props.get('Resource', '')}

	def widget(s, w, layout, parent):
		props = {p.get('key'): p.get('value') for p in w.findall('Property')}
		label = w.get('name') or w.get('type')
		path = f'{parent}/{label}' if parent else label
		image = '/'.join(props[k] for k in ('ImageResource', 'ImageGroup', 'ImageName') if k in props)
		s.widgets.append({'layout': layout, 'path': path, 'type': w.get('type'), 'skin': w.get('skin', ''), 'name': w.get('name', ''),
			'caption': props.get('Caption', ''), 'image': image or props.get('ImageTexture', ''), 'font': props.get('FontName', '')})
		for c in w.findall('Widget'):
			s.widget(c, layout, path)

	def skinsOf(s, name):
		"""The data skins a widget's `skin` resolves to: itself, or a template's (one level; templates name skins)."""
		if name in s.skins:
			return [name]
		return [k for k in s.templates.get(name, {}).get('skins', []) if k in s.skins]

	def crops(s, out):
		from PIL import Image
		cache = {}
		def tex(name):
			if name.lower() not in cache:
				path = s.textures.get(name.lower())
				cache[name.lower()] = Image.open(path).convert('RGBA') if path else None
			return cache[name.lower()]
		made = {}
		os.makedirs(os.path.join(out, 'crops', 'skin'), exist_ok=True)
		os.makedirs(os.path.join(out, 'crops', 'image'), exist_ok=True)
		for name, sk in s.skins.items():
			atlas = tex(sk['texture']) if sk['texture'] else None
			if atlas is None or len(sk['size']) != 2 or min(sk['size']) <= 0:
				continue
			for state in sorted({st for sub in sk['subs'] for st in sub['states']}):
				img = Image.new('RGBA', tuple(sk['size']), (0, 0, 0, 0))
				for sub in sk['subs']:
					rect = sub['states'].get(state) or sub['states'].get('normal')
					if not rect or len(rect) != 4 or rect[2] <= 0 or rect[3] <= 0 or len(sub['offset']) != 4:
						continue	# text sub-skins carry a colour, not a rect
					piece = atlas.crop((rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]))
					dx, dy, dw, dh = sub['offset']
					if (dw, dh) != (rect[2], rect[3]) and dw > 0 and dh > 0:
						piece = piece.resize((dw, dh))
					img.alpha_composite(piece, (dx, dy))
				rel = f'crops/skin/{name}.{state}.png'
				img.save(os.path.join(out, rel))
				made.setdefault(('skin', name), []).append(rel)
		for key, im in s.images.items():
			atlas = tex(im['texture']) if im['texture'] else None
			if atlas is None or len(im['size']) != 2:
				continue
			x, y = (im['point'] + [0, 0])[:2]
			rel = 'crops/image/' + key.replace('/', '.') + '.png'
			atlas.crop((x, y, x + im['size'][0], y + im['size'][1])).save(os.path.join(out, rel))
			made[('image', key)] = [rel]
		return made

	def write(s, out, made):
		used = {}	# data skin or template -> ["layout:path", ...]
		for w in s.widgets:
			if w['skin']:
				used.setdefault(w['skin'], []).append(f"{w['layout']}:{w['path']}")
				for k in s.skinsOf(w['skin']):
					if k != w['skin']:
						used.setdefault(k, []).append(f"{w['layout']}:{w['path']} (via {w['skin']})")
		for t, tp in s.templates.items():
			for k in tp['skins']:
				used.setdefault(k, []).append(f'template {t}')
		def users(name):
			"""Distinct layouts (with widget counts) and templates: short enough to grep; the `widget` lines have the paths."""
			counts = {}
			for u in used.get(name, []):
				key = u.split(':')[0].replace('layout/', '')
				counts[key] = counts.get(key, 0) + 1
			return ', '.join(f'{k} x{n}' if n > 1 else k for k, n in counts.items()) or '-'
		def fmt(r):
			return 'x'.join(map(str, r[2:])) + '@' + ','.join(map(str, r[:2])) if len(r) == 4 else ' '.join(map(str, r))
		lines = []
		for name, sk in sorted(s.skins.items()):
			states = sorted({st for sub in sk['subs'] for st in sub['states']})
			rects = ' '.join(f"{sub['align'].replace(' ', '_') or 'sub'}:{fmt(sub['states'].get('normal', []))}" for sub in sk['subs'] if sub['states'].get('normal'))
			lines.append(f"skin {name} tex={sk['texture']} size={'x'.join(map(str, sk['size']))} states={','.join(states)} normal=[{rects}]"
				f" crops={','.join(made.get(('skin', name), []))} used_by={users(name)} file={sk['file']}")
		for name, tp in sorted(s.templates.items()):
			lines.append(f"template {name} skins={','.join(tp['skins'])} used_by={users(name)} file={tp['file']}")
		for key, im in sorted(s.images.items()):
			lines.append(f"image {key} tex={im['texture']} rect={'x'.join(map(str, im['size']))}@{','.join(map(str, im['point']))}"
				f" crop={','.join(made.get(('image', key), []))} file={im['file']}")
		for name, p in sorted(s.pointers.items()):
			lines.append(f"pointer {name} resource={p['resource']} size={p['size']} hotspot={p['point']}")
		builtin = sorted({w['skin'] for w in s.widgets if w['skin'] and w['skin'] not in s.skins and w['skin'] not in s.templates})
		lines.append('builtin ' + ' '.join(builtin))
		for w in s.widgets:
			extra = ''.join(f' {k}="{w[k]}"' for k in ('caption', 'image', 'font') if w[k])
			lines.append(f"widget {w['layout']} {w['path']} type={w['type']} skin={w['skin'] or '-'}"
				f" resolves={','.join(s.skinsOf(w['skin'])) or '-'}{extra}")
		with open(os.path.join(out, 'index.txt'), 'w') as f:
			f.write('\n'.join(lines) + '\n')
		with open(os.path.join(out, 'index.json'), 'w') as f:
			json.dump({'skins': s.skins, 'templates': s.templates, 'images': s.images, 'pointers': s.pointers, 'widgets': s.widgets,
				'builtin': builtin, 'used_by': used, 'crops': {f'{k[0]} {k[1]}': v for k, v in made.items()}}, f, indent=1)
		return len(lines)

def main():
	ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
	ap.add_argument('--data', default=None, help='game data dir (default: kenshi_install.py finds it)')
	ap.add_argument('--out', default=DEFAULT_OUT)
	ap.add_argument('--no-crops', action='store_true')
	a = ap.parse_args()
	a.data = kenshi_install.data_dir(a.data)
	gui = os.path.join(a.data, 'gui')
	if not os.path.isdir(gui):
		sys.exit(f'no gui dir under {a.data}')
	repo = os.path.realpath(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
	if os.path.commonpath([repo, os.path.realpath(a.out)]) == repo:
		sys.exit('refusing to write game-derived output inside the repo')
	idx = Index(gui)
	idx.load()
	os.makedirs(a.out, exist_ok=True)
	made = {} if a.no_crops else idx.crops(a.out)
	n = idx.write(a.out, made)
	print(f'{len(idx.skins)} skins, {len(idx.templates)} templates, {len(idx.images)} image frames, {len(idx.pointers)} pointers, '
		f'{len(idx.widgets)} widgets; {n} index lines, {sum(len(v) for v in made.values())} crops -> {a.out}')

if __name__ == '__main__':
	main()
