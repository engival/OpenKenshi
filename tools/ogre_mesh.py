#!/usr/bin/env python3
"""Minimal Ogre binary .mesh reader: positions + triangle indices (MeshSerializer v1.41, v1.8, v1.100). See docs/formats/COLLISION_FORMAT.md.

Usage: ogre_mesh.py FILE.mesh ...      (prints version, submeshes, vertex/triangle counts, vertex bounds)

API:
	m = parse(path)          # m.version, m.submeshes (dicts), m.shared_geometry, m.bounds_chunk
	triangles(m)             # [(v0, v1, v2)] in mesh-local Ogre axes (y up), all submeshes
	vertex_bounds(m)         # ((minx, miny, minz), (maxx, maxy, maxz)) from the vertices

Format notes (verified on all 3,396 .mesh files in the game):
- chunk = u16 id, u32 length (length includes the 6-byte header); the 0x1000 header has no length, just a '\\n'-terminated version string.
- strings are '\\n'-terminated. Parents' lengths are not trusted (v1.8/v1.41 M_MESH/M_SUBMESH lengths do not cover
  their children); parsing walks child chunks by id instead.
Standard library only.
"""
import sys, struct, re

M_HEADER = 0x1000
M_MESH = 0x3000
M_SUBMESH = 0x4000
M_SUBMESH_OPERATION = 0x4010
M_SUBMESH_BONE_ASSIGNMENT = 0x4100
M_SUBMESH_TEXTURE_ALIAS = 0x4200
M_GEOMETRY = 0x5000
M_GEOMETRY_VERTEX_DECLARATION = 0x5100
M_GEOMETRY_VERTEX_ELEMENT = 0x5110
M_GEOMETRY_VERTEX_BUFFER = 0x5200
M_GEOMETRY_VERTEX_BUFFER_DATA = 0x5210
M_MESH_BOUNDS = 0x9000

SUBMESH_CHILDREN = {M_GEOMETRY, M_SUBMESH_OPERATION, M_SUBMESH_BONE_ASSIGNMENT, M_SUBMESH_TEXTURE_ALIAS}
GEOMETRY_CHILDREN = {M_GEOMETRY_VERTEX_DECLARATION, M_GEOMETRY_VERTEX_BUFFER}
VES_POSITION = 1
OT_TRIANGLE_LIST = 4

def read_line(b, o):
	e = b.index(b'\n', o)
	return b[o:e].decode('latin-1'), e + 1

def chunk(b, o):
	if o + 6 > len(b):
		return None, 0
	cid, size = struct.unpack_from('<HI', b, o)
	if size < 6 or o + size > len(b):
		raise ValueError('bad chunk %#06x size %d at %d' % (cid, size, o))
	return cid, size

def read_geometry(b, o):
	"""M_GEOMETRY at o -> (geometry dict, offset after its last child)."""
	vcount, = struct.unpack_from('<I', b, o + 6)
	g = {'vertex_count': vcount, 'decl': [], 'buffers': {}, 'positions': []}
	p = o + 10
	while p + 6 <= len(b) and struct.unpack_from('<H', b, p)[0] in GEOMETRY_CHILDREN:
		cid, size = chunk(b, p)
		end = p + size
		if cid == M_GEOMETRY_VERTEX_DECLARATION:
			q = p + 6
			while q + 6 <= end and struct.unpack_from('<H', b, q)[0] == M_GEOMETRY_VERTEX_ELEMENT:
				_, s2 = chunk(b, q)
				g['decl'].append(struct.unpack_from('<5H', b, q + 6))	# source, type, semantic, offset, index
				q += s2
		else:
			bind, vsize = struct.unpack_from('<2H', b, p + 6)
			q = p + 10
			if q + 6 <= end and struct.unpack_from('<H', b, q)[0] == M_GEOMETRY_VERTEX_BUFFER_DATA:
				_, s2 = chunk(b, q)
				g['buffers'][bind] = (vsize, b[q + 6:q + s2])
		p = end
	for src, typ, sem, off, idx in g['decl']:
		if sem == VES_POSITION and idx == 0 and src in g['buffers']:
			vsize, raw = g['buffers'][src]
			g['positions'] = [struct.unpack_from('<3f', raw, i * vsize + off) for i in range(vcount)]
			break
	return g, p

def read_submesh(b, o):
	p = o + 6
	material, p = read_line(b, p)
	shared = b[p]
	icount, = struct.unpack_from('<I', b, p + 1)
	wide = b[p + 5]
	p += 6
	fmt = '<%d%s' % (icount, 'I' if wide else 'H')
	sm = {'material': material, 'shared': bool(shared), 'indices': list(struct.unpack_from(fmt, b, p)),
		'geometry': None, 'operation': OT_TRIANGLE_LIST}
	p += icount * (4 if wide else 2)
	while p + 6 <= len(b) and struct.unpack_from('<H', b, p)[0] in SUBMESH_CHILDREN:
		cid, size = chunk(b, p)
		if cid == M_GEOMETRY:
			sm['geometry'], p = read_geometry(b, p)
			continue
		if cid == M_SUBMESH_OPERATION:
			sm['operation'], = struct.unpack_from('<H', b, p + 6)
		p += size
	return sm, p

class Mesh:
	def __init__(s):
		s.version = None
		s.submeshes = []
		s.shared_geometry = None
		s.bounds_chunk = None		# exporter AABB + radius; may be padded, prefer vertex_bounds()

def parse(path):
	b = open(path, 'rb').read()
	if struct.unpack_from('<H', b, 0)[0] != M_HEADER:
		raise ValueError('not an Ogre mesh')
	version, p = read_line(b, 2)
	m = Mesh()
	vm = re.search(r'v([0-9.]+)\]', version)
	m.version = vm.group(1) if vm else version
	while p + 6 <= len(b) and struct.unpack_from('<H', b, p)[0] != M_MESH:
		p += chunk(b, p)[1]
	p += 7				# M_MESH header + bool skeletallyAnimated
	while p + 6 <= len(b):
		cid, size = struct.unpack_from('<HI', b, p)
		if cid == M_SUBMESH:
			sm, p = read_submesh(b, p)
			m.submeshes.append(sm)
		elif cid == M_GEOMETRY:
			m.shared_geometry, p = read_geometry(b, p)
		else:
			if size < 6 or p + size > len(b):
				break
			if cid == M_MESH_BOUNDS:
				m.bounds_chunk = struct.unpack_from('<7f', b, p + 6)
			p += size
	return m

def submesh_positions(m, sm):
	g = m.shared_geometry if sm['shared'] else sm['geometry']
	return g['positions'] if g else []

def triangles(m):
	out = []
	for sm in m.submeshes:
		if sm['operation'] != OT_TRIANGLE_LIST:
			continue
		vs = submesh_positions(m, sm)
		ix = sm['indices']
		out += [(vs[ix[i]], vs[ix[i + 1]], vs[ix[i + 2]]) for i in range(0, len(ix) - 2, 3)]
	return out

def vertex_bounds(m):
	pts = [v for sm in m.submeshes for v in submesh_positions(m, sm)]
	if not pts:
		return None
	return tuple(min(p[i] for p in pts) for i in range(3)), tuple(max(p[i] for p in pts) for i in range(3))

def main(argv):
	if not argv:
		print(__doc__)
		return 2
	for path in argv:
		m = parse(path)
		b = vertex_bounds(m)
		nt = sum(len(sm['indices']) // 3 for sm in m.submeshes)
		nv = sum(len(submesh_positions(m, sm)) for sm in m.submeshes if not sm['shared']) + len(m.shared_geometry['positions'] if m.shared_geometry else [])
		print('%s: v%s submeshes=%d verts=%d tris=%d bounds %s' % (path, m.version, len(m.submeshes), nv, nt,
			b and '[%s] .. [%s]' % (', '.join('%.1f' % x for x in b[0]), ', '.join('%.1f' % x for x in b[1]))))
	return 0

if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
