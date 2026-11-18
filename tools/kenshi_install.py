"""Find the Kenshi install for the tools in this directory.

Order: (a) the tool's own --game / --data argument (passed to install()/data_dir() as `arg`),
(b) env KENSHI (install dir) or KENSHI_DATA (its data/ dir), (c) every "path" in Steam's
libraryfolders.vdf, (d) the usual GOG places. An explicit argument or env value that is not an
install is an error, never a silent fall-through. No match: exit with a message naming KENSHI.
"""
import os, re, sys

STEAM_ROOTS = ('~/.steam/steam', '~/.local/share/Steam')
GOG_PLACES = ('~/GOG Games/Kenshi/game', '~/GOG Games/Kenshi', '~/Games/kenshi/drive_c/GOG Games/Kenshi')


def _fail(msg):
	sys.exit('kenshi_install: %s\nSet KENSHI to the Kenshi install directory (the one holding data/), or KENSHI_DATA to its data/ directory.' % msg)


def _is_install(d):
	return os.path.isfile(os.path.join(d, 'data', 'gamedata.base'))


def steam_libraries():
	libs = []
	for root in STEAM_ROOTS:
		steamapps = os.path.join(os.path.expanduser(root), 'steamapps')
		libs.append(os.path.join(steamapps, 'common', 'Kenshi'))
		try:
			with open(os.path.join(steamapps, 'libraryfolders.vdf'), errors='replace') as f: text = f.read()
		except OSError:
			continue
		for path in re.findall(r'"path"\s+"([^"]*)"', text):
			libs.append(os.path.join(path.replace('\\\\', '\\'), 'steamapps', 'common', 'Kenshi'))
	return libs


def install(arg=None):
	"""The install directory. `arg` is the tool's own --game value: an install dir, or its data/ dir."""
	if arg:
		d = arg.rstrip('/')
		if os.path.basename(d) == 'data': d = os.path.dirname(d)
		if not _is_install(d): _fail('%s is not a Kenshi install (no data/gamedata.base)' % arg)
		return d
	if os.environ.get('KENSHI'):
		d = os.environ['KENSHI']
		if not _is_install(d): _fail('KENSHI=%s is not a Kenshi install (no data/gamedata.base)' % d)
		return d
	if os.environ.get('KENSHI_DATA'):
		d = os.path.dirname(os.environ['KENSHI_DATA'].rstrip('/'))
		if not _is_install(d): _fail('KENSHI_DATA=%s is not a Kenshi data directory (no gamedata.base)' % os.environ['KENSHI_DATA'])
		return d
	for d in steam_libraries() + [os.path.expanduser(p) for p in GOG_PLACES]:
		if _is_install(d): return d
	_fail('no Kenshi install found (looked in the Steam libraries and the usual GOG places)')


def data_dir(arg=None):
	"""install()'s data/ directory (no trailing slash)."""
	return os.path.join(install(arg), 'data')
