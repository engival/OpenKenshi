#!/usr/bin/env python3
"""Kenshi PhysX 2.8 collision files (NxuStream2 *.xml, cached NxuStream *.bin). See docs/formats/COLLISION_FORMAT.md.

Usage: collision.py [--world] [--tris] FILE.xml|FILE.bin ...
	prints actors/shapes/bounds; --world converts to Ogre y-up (part-local, before placement); --tris dumps triangles.

API:
	c = parse_xml(path)                 # Collection: c.actors[i].shapes[j], c.meshes
	tris = c.triangles()                # [(v0, v1, v2)] PhysX z-up, actor * shape pose applied
	tris = c.triangles(ogre=True)       # same, converted to Ogre/world axes (x, z, -y)
	placed = place(tris, pos, quat_wxyz, scale)   # world coordinates of one placed instance
	cooked = scan_bin(path)             # cooked mesh streams embedded in a binary .bin (geometry only)
	cache_name(xml_path, scale)         # the .bin name the game loads for a given scale

Standard library only.
"""
import sys, re, struct, math
import xml.etree.ElementTree as ET

IDENTITY = ((1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0))

# ---------------------------------------------------------------- cooked streams

def decode_triangle_mesh(d):
	"""'NXS\\x01MESH' cooked NxTriangleMesh. Header u32s at 8..35: version, flags, f32 0.001, 255, 0, Nv, Nt.
	Then Nv * 3 f32 vertices, Nt * 3 indices (u8 unless flags & 0x10 -> u16), then u32 Nt-1, remap, OPC tree (ignored)."""
	if d[:8] != b'NXS\x01MESH':
		raise ValueError('not a cooked triangle mesh')
	flags, = struct.unpack_from('<I', d, 12)
	nv, nt = struct.unpack_from('<2I', d, 28)
	vs = list(struct.iter_unpack('<3f', d[36:36 + 12 * nv]))
	o = 36 + 12 * nv
	if flags & 0x10:
		flat = struct.unpack_from('<%dH' % (3 * nt), d, o)
	else:
		flat = d[o:o + 3 * nt]
	tris = [tuple(flat[3 * i:3 * i + 3]) for i in range(nt)]
	if len(vs) != nv or len(tris) != nt or (tris and max(max(t) for t in tris) >= nv):
		raise ValueError('bad cooked triangle mesh')
	return {'kind': 'triangle_mesh', 'vertices': vs, 'triangles': tris}

def decode_convex_mesh(d):
	"""'NXS\\x01CVXM' cooked NxConvexMesh. Geometry is in the 'ICE\\x01CVHL' chunk: 7 u32 (5, Nv, Nt, ...),
	Nv * 3 f32 vertices, u32 Nv-1, Nt * 3 u8 indices (all game hulls have <= 256 vertices)."""
	if d[:8] != b'NXS\x01CVXM':
		raise ValueError('not a cooked convex mesh')
	tag = d.find(b'ICE\x01CVHL', 8)
	if tag < 0:
		raise ValueError('no CVHL chunk')
	h = struct.unpack_from('<7I', d, tag + 8)
	nv, nt = h[1], h[2]
	o = tag + 36
	vs = list(struct.iter_unpack('<3f', d[o:o + 12 * nv]))
	o += 12 * nv + 4
	flat = d[o:o + 3 * nt]
	tris = [tuple(flat[3 * i:3 * i + 3]) for i in range(nt)]
	if len(vs) != nv or len(tris) != nt or not tris or max(max(t) for t in tris) >= nv:
		raise ValueError('bad cooked convex mesh')
	return {'kind': 'convex_mesh', 'vertices': vs, 'triangles': tris}

def decode_cooked(d):
	if d[:8] == b'NXS\x01MESH':
		return decode_triangle_mesh(d)
	if d[:8] == b'NXS\x01CVXM':
		return decode_convex_mesh(d)
	raise ValueError('unknown cooked stream %r' % d[:8])

def scan_bin(path):
	"""Cooked meshes found in a binary NxuStream file, in file order (geometry only, no poses).
	Some .bin files store meshes uncooked (raw f32 points + u32 triangles); those are not found here."""
	d = open(path, 'rb').read()
	out = []
	pos = 0
	while True:
		hits = [i for i in (d.find(b'NXS\x01MESH', pos), d.find(b'NXS\x01CVXM', pos)) if i >= 0]
		if not hits:
			return out
		i = min(hits)
		try:
			m = decode_cooked(d[i:])
			m['offset'] = i
			out.append(m)
		except (ValueError, struct.error):
			pass
		pos = i + 8

# ---------------------------------------------------------------- pose math

def parse_pose(text):
	"""NxMat34 text: 9 rotation values row-major, then translation. p' = M p + t."""
	v = [float(x) for x in text.split()]
	if len(v) != 12:
		raise ValueError('pose with %d values' % len(v))
	return (tuple(v[:9]), tuple(v[9:]))

def compose(a, b):
	"""a * b: apply b first."""
	A, at = a
	B, bt = b
	M = tuple(sum(A[3 * i + k] * B[3 * k + j] for k in range(3)) for i in range(3) for j in range(3))
	t = tuple(sum(A[3 * i + k] * bt[k] for k in range(3)) + at[i] for i in range(3))
	return (M, t)

def apply_pose(pose, p):
	M, t = pose
	return tuple(M[3 * i] * p[0] + M[3 * i + 1] * p[1] + M[3 * i + 2] * p[2] + t[i] for i in range(3))

def physx_to_ogre(p):
	"""Collision files are z-up; Kenshi/Ogre is y-up. The game folds this into the instance matrix."""
	return (p[0], p[2], -p[1])

def quat_rotate(q, v):
	"""Rotate v by unit quaternion q = (w, x, y, z) (Ogre convention, as stored in zone instances)."""
	w, x, y, z = q
	tx = 2 * (y * v[2] - z * v[1])
	ty = 2 * (z * v[0] - x * v[2])
	tz = 2 * (x * v[1] - y * v[0])
	return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx))

def place(tris, pos, quat, scale=1.0):
	"""Ogre-axis part-local triangles -> world: pos + R(quat) * (scale * v)."""
	return [tuple(tuple(a + b for a, b in zip(quat_rotate(quat, (v[0] * scale, v[1] * scale, v[2] * scale)), pos)) for v in tri) for tri in tris]

def cache_name(xml_path, scale=1.0):
	"""The binary the game loads: '<stem>.bin', or '<stem>_<scale>.bin' when scale != 1 (Ogre toString, 6 digits)."""
	stem = re.sub(r'\.[^./\\]*$', '', xml_path)
	if scale != 1.0:
		stem += '_' + ('%.6g' % scale)
	return stem + '.bin'

# ---------------------------------------------------------------- primitive tessellation

def box_triangles(h):
	hx, hy, hz = h
	v = [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, hy, -hz), (-hx, hy, -hz),
		(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]
	f = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
		(1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
	return [(v[a], v[b], v[c]) for a, b, c in f]

def capsule_triangles(radius, height, seg=12, rings=3):
	"""Capsule along local Y; 'height' = distance between the hemisphere centres."""
	angles = [math.pi / 2 * i / rings for i in range(rings + 1)]
	rows = [(-height / 2 - radius * math.sin(a), radius * math.cos(a)) for a in reversed(angles)]
	rows += [(height / 2 + radius * math.sin(a), radius * math.cos(a)) for a in angles]
	rings_pts = [[(r * math.cos(2 * math.pi * k / seg), y, r * math.sin(2 * math.pi * k / seg)) for k in range(seg)] for y, r in rows]
	tris = []
	for a, b in zip(rings_pts, rings_pts[1:]):
		for k in range(seg):
			k2 = (k + 1) % seg
			tris.append((a[k], b[k], b[k2]))
			tris.append((a[k], b[k2], a[k2]))
	return [t for t in tris if len({t[0], t[1], t[2]}) == 3]

def sphere_triangles(radius):
	return capsule_triangles(radius, 0.0)

# ---------------------------------------------------------------- XML

SHAPE_KINDS = {'NxConvexShapeDesc': 'convex', 'NxTriangleMeshShapeDesc': 'tri_mesh', 'NxBoxShapeDesc': 'box',
	'NxCapsuleShapeDesc': 'capsule', 'NxSphereShapeDesc': 'sphere', 'NxPlaneShapeDesc': 'plane'}

class Shape:
	def __init__(s, kind):
		s.kind = kind
		s.name = ''
		s.local_pose = IDENTITY
		s.mesh = None			# decoded cooked mesh (convex / tri_mesh)
		s.params = {}			# box: dimensions (half extents); capsule: radius, height; sphere: radius; plane: normal, d

	def local_triangles(s):
		"""Triangles in the shape's own frame (no pose applied). Planes return []."""
		if s.mesh:
			vs = s.mesh['vertices']
			return [(vs[a], vs[b], vs[c]) for a, b, c in s.mesh['triangles']]
		if s.kind == 'box':
			return box_triangles(s.params['dimensions'])
		if s.kind == 'capsule':
			return capsule_triangles(s.params['radius'], s.params['height'])
		if s.kind == 'sphere':
			return sphere_triangles(s.params['radius'])
		return []

class Actor:
	def __init__(s):
		s.name = ''
		s.global_pose = IDENTITY
		s.shapes = []

class Collection:
	def __init__(s):
		s.sdk_version = None
		s.meshes = {}
		s.actors = []

	def triangles(s, ogre=False):
		"""All triangles with actor.globalPose * shape.localPose applied; ogre=True converts z-up -> y-up."""
		out = []
		for a in s.actors:
			for sh in a.shapes:
				pose = compose(a.global_pose, sh.local_pose)
				for tri in sh.local_triangles():
					t = tuple(apply_pose(pose, p) for p in tri)
					out.append(tuple(physx_to_ogre(p) for p in t) if ogre else t)
		return out

_BARE_AMP = re.compile(rb'&(?!(?:amp|lt|gt|quot|apos|#\d+|#x[0-9a-fA-F]+);)')

def _load(path):
	raw = open(path, 'rb').read()
	try:
		return ET.fromstring(raw)
	except ET.ParseError:
		return ET.fromstring(_BARE_AMP.sub(b'&amp;', raw))	# some ids contain a raw '&'

def _floats(text):
	return tuple(float(x) for x in text.split())

def parse_xml(path):
	root = _load(path)
	coll = root if root.tag == 'NxuPhysicsCollection' else root.find('.//NxuPhysicsCollection')
	if coll is None:
		raise ValueError('no NxuPhysicsCollection')
	c = Collection()
	if coll.get('sdkVersion'):
		c.sdk_version = int(coll.get('sdkVersion'))
	for tag in ('NxConvexMeshDesc', 'NxTriangleMeshDesc'):
		for m in coll.iter(tag):
			cd = m.find('cookedData')
			if cd is not None and (cd.text or '').strip():
				c.meshes[m.get('id', '')] = decode_cooked(bytes.fromhex(re.sub(r'\s+', '', cd.text)))
	for ad in coll.iter('NxActorDesc'):
		a = Actor()
		a.name = ad.get('name', '')
		gp = ad.find('globalPose')
		if gp is not None and (gp.text or '').strip():
			a.global_pose = parse_pose(gp.text)
		for sd in ad:
			kind = SHAPE_KINDS.get(sd.tag)
			if not kind:
				continue
			sh = Shape(kind)
			base = sd.find('NxShapeDesc')
			if base is not None:
				sh.name = base.get('name', '')
				lp = base.find('localPose')
				if lp is not None and (lp.text or '').strip():
					sh.local_pose = parse_pose(lp.text)
			if kind == 'box':
				sh.params['dimensions'] = _floats(sd.get('dimensions'))
			elif kind == 'capsule':
				sh.params = {'radius': float(sd.get('radius')), 'height': float(sd.get('height'))}
			elif kind == 'sphere':
				sh.params = {'radius': float(sd.get('radius'))}
			elif kind == 'plane':
				sh.params = {'normal': _floats(sd.get('normal', '0 0 1')), 'd': float(sd.get('d', 0))}
			else:
				sh.mesh = c.meshes.get(sd.get('meshData'))
			a.shapes.append(sh)
		c.actors.append(a)
	return c

# ---------------------------------------------------------------- CLI

def bounds(points):
	points = list(points)
	if not points:
		return None
	return tuple(min(p[i] for p in points) for i in range(3)), tuple(max(p[i] for p in points) for i in range(3))

def _fmt(b):
	return 'none' if b is None else '[%s] .. [%s]' % (', '.join('%.1f' % x for x in b[0]), ', '.join('%.1f' % x for x in b[1]))

def main(argv):
	ogre = '--world' in argv
	dump = '--tris' in argv
	paths = [a for a in argv if not a.startswith('--')]
	if not paths:
		print(__doc__)
		return 2
	for path in paths:
		if path.lower().endswith('.bin'):
			ms = scan_bin(path)
			print('%s: %d cooked stream(s)' % (path, len(ms)))
			for m in ms:
				print('  @%#x %s nv=%d nt=%d raw %s' % (m['offset'], m['kind'], len(m['vertices']), len(m['triangles']), _fmt(bounds(m['vertices']))))
			continue
		c = parse_xml(path)
		tris = c.triangles(ogre=ogre)
		print('%s: sdk=%s actors=%d tris=%d bounds %s' % (path, c.sdk_version, len(c.actors), len(tris), _fmt(bounds(p for t in tris for p in t))))
		for a in c.actors:
			print('  actor %r pose t=%s' % (a.name, a.global_pose[1]))
			for sh in a.shapes:
				extra = '%d tris' % len(sh.mesh['triangles']) if sh.mesh else sh.params
				print('    %-8s %r %s' % (sh.kind, sh.name, extra))
		if dump:
			for t in tris:
				print(' '.join('%.4f %.4f %.4f' % p for p in t))
	return 0

if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
