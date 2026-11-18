#!/usr/bin/env python3
"""Parse/validate Kenshi terrain files in data/newland/land (see docs/formats/TERRAIN_FORMAT.md).

Usage: terrain.py PATH/TO/data/newland/land
Checks fullmap.tif layout + height fit against navtiles/seeds.def, features.dat,
fogfeatures.dat, blendinfo.dat, and the blendmap/blendinfo/biomemap identity.
Standard library only (includes a minimal non-interlaced 8-bit PNG decoder).
"""
import struct, sys, math, zlib, collections

ORIGIN = 147456.0
ZONE = 4608.0
HEIGHT_SCALE = 9800.0 / 65535.0

def zone_of(x, z):
	return math.floor((x + ORIGIN) / ZONE), math.floor((z + ORIGIN) / ZONE)

class Heightmap:
	"""fullmap.tif: classic little-endian TIFF, one uncompressed strip of u16."""
	def __init__(s, path):
		s.f = open(path, 'rb')
		h = s.f.read(8)
		if h[:4] != b'II*\0':
			raise ValueError('not a little-endian TIFF')
		s.f.seek(struct.unpack('<I', h[4:])[0])
		n, = struct.unpack('<H', s.f.read(2))
		s.tags = {}
		for _ in range(n):
			t, ty, c, v = struct.unpack('<HHII', s.f.read(12))
			s.tags[t] = v & 0xFFFF if ty == 3 else v
		s.w, s.h, s.off = s.tags[256], s.tags[257], s.tags[273]
		if s.tags[258] != 16 or s.tags[259] != 1 or s.tags[277] != 1 or s.tags[279] != s.w * s.h * 2:
			raise ValueError('unexpected TIFF layout')
	def raw(s, col, row):
		s.f.seek(s.off + 2 * (row * s.w + col))
		return struct.unpack('<H', s.f.read(2))[0]
	def height(s, x, z):
		"""Nearest sample; sample i sits at -147456 + 18*i (vertex vs. centre is unresolved, +-9 units)."""
		return s.raw(round((x + ORIGIN) / 18), round((z + ORIGIN) / 18)) * HEIGHT_SCALE

def read_seeds(path):
	b = open(path, 'rb').read()
	return [struct.unpack_from('<3f', b, i) for i in range(0, len(b), 12)]

def read_features(path):
	"""features.dat (MF01) -> {zone_index: [(name, pos, scale, rot)]}"""
	b = open(path, 'rb').read()
	if b[:4] != b'MF01':
		raise ValueError('bad magic')
	n, = struct.unpack_from('<i', b, 4)
	out = {}
	expect = 8 + 8 * n
	for i in range(n):
		off, size = struct.unpack_from('<ii', b, 8 + 8 * i)
		if not size:
			continue
		if off != expect:
			raise ValueError(f'zone {i}: blob at {off}, expected {expect}')
		cnt, = struct.unpack_from('<i', b, off)
		p = off + 4
		lst = []
		for _ in range(cnt):
			ln = b[p]
			name = b[p + 1:p + 1 + ln].decode()
			p += 1 + ln
			v = struct.unpack_from('<10f', b, p)
			p += 40
			lst.append((name, v[0:3], v[3:6], v[6:10]))
		if p != off + size:
			raise ValueError(f'zone {i}: blob size mismatch')
		out[i] = lst
		expect = off + size
	if expect != len(b):
		raise ValueError('trailing bytes')
	return out

def read_fog(path):
	"""fogfeatures.dat (FF02) -> [(name, type, rgba, radius, extra_floats)]"""
	b = open(path, 'rb').read()
	if b[:4] != b'FF02':
		raise ValueError('bad magic')
	n, = struct.unpack_from('<i', b, 4)
	p = 8
	out = []
	extra = {2: 4, 3: 8, 4: 29}
	for _ in range(n):
		ln = b[p]
		name = b[p + 1:p + 1 + ln].decode()
		p += 1 + ln
		t = b[p]
		rgba = struct.unpack_from('<4f', b, p + 1)
		radius, = struct.unpack_from('<f', b, p + 17)
		p += 21
		ext = struct.unpack_from('<%df' % extra[t], b, p)
		p += 4 * extra[t]
		out.append((name, t, rgba, radius, ext))
	if p != len(b):
		raise ValueError('trailing bytes')
	return out

def read_blendinfo(path):
	"""blendinfo.dat (KBI1) -> (dim, palettes[dim*dim][5], payloads[dim*dim])"""
	b = open(path, 'rb').read()
	if b[:4] != b'KBI1':
		raise ValueError('bad magic')
	w, h = struct.unpack_from('<ii', b, 4)
	pal = [struct.unpack_from('<5I', b, 12 + 20 * i) for i in range(w * h)]
	p = 12 + 20 * w * h
	blob, = struct.unpack_from('<i', b, p)
	p += 4
	pay = []
	for _ in range(w * h):
		if b[p] & 0xE0 == 0x20:
			pay.append(b[p:p + 1])
			p += 1
		else:
			pay.append(b[p:p + blob])
			p += blob
	if p != len(b):
		raise ValueError('trailing bytes')
	return w, pal, pay

def read_png(path):
	"""Minimal PNG decoder: 8-bit, non-interlaced, gray/RGB/RGBA. Returns (w, h, channels, bytes)."""
	d = open(path, 'rb').read()
	p = 8
	idat = b''
	while p < len(d):
		ln, = struct.unpack('>I', d[p:p + 4])
		t = d[p + 4:p + 8]
		c = d[p + 8:p + 8 + ln]
		p += 12 + ln
		if t == b'IHDR':
			w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', c)
		elif t == b'IDAT':
			idat += c
	if depth != 8 or inter:
		raise ValueError('unsupported PNG')
	bpp = {0: 1, 2: 3, 6: 4}[ctype]
	raw = zlib.decompress(idat)
	stride = w * bpp
	out = bytearray()
	prev = bytearray(stride)
	q = 0
	for _ in range(h):
		ft = raw[q]
		line = bytearray(raw[q + 1:q + 1 + stride])
		q += 1 + stride
		for x in range(stride):
			a = line[x - bpp] if x >= bpp else 0
			up = prev[x]
			c = prev[x - bpp] if x >= bpp else 0
			if ft == 1:
				line[x] = (line[x] + a) & 255
			elif ft == 2:
				line[x] = (line[x] + up) & 255
			elif ft == 3:
				line[x] = (line[x] + (a + up) // 2) & 255
			elif ft == 4:
				pp = a + up - c
				pa, pb, pc = abs(pp - a), abs(pp - up), abs(pp - c)
				line[x] = (line[x] + (a if pa <= pb and pa <= pc else up if pb <= pc else c)) & 255
		out += line
		prev = line
	return w, h, bpp, bytes(out)

BLEND_SLOT = {(255, 0, 0, 0): 0, (0, 255, 0, 0): 1, (0, 0, 255, 0): 2, (0, 0, 0, 255): 3, (0, 0, 0, 0): 4}

def main():
	d = sys.argv[1]
	hm = Heightmap(d + '/fullmap.tif')
	seeds = read_seeds(d + '/navtiles/seeds.def')
	pairs = [(hm.height(x, z), y) for x, y, z in seeds]
	n = len(pairs)
	ma = sum(a for a, _ in pairs) / n
	mb = sum(b for _, b in pairs) / n
	sab = sum((a - ma) * (b - mb) for a, b in pairs)
	saa = sum((a - ma) ** 2 for a, _ in pairs)
	sbb = sum((b - mb) ** 2 for _, b in pairs)
	rmse = math.sqrt(sum((a - b) ** 2 for a, b in pairs) / n)
	print(f'fullmap.tif {hm.w}x{hm.h} u16; seeds.def {n} points: r={sab / math.sqrt(saa * sbb):.4f} rmse={rmse:.1f}')

	feats = read_features(d + '/features.dat')
	tot = sum(len(v) for v in feats.values())
	inside = sum(1 for i, v in feats.items() for f in v if (lambda c: c[0] + 64 * c[1])(zone_of(f[1][0], f[1][2])) == i)
	print(f'features.dat: {len(feats)} zones, {tot} features, {inside} inside their zone')

	fog = read_fog(d + '/fogfeatures.dat')
	print(f'fogfeatures.dat: {len(fog)} volumes, types {dict(collections.Counter(f[1] for f in fog))}')

	dim, pal, pay = read_blendinfo(d + '/blendinfo.dat')
	print(f'blendinfo.dat: {dim}x{dim} cells, {sum(len(p) == 1 for p in pay)} uniform')

	w, h, c1, bm = read_png(d + '/blendmap.png')
	_, _, c2, bio = read_png(d + '/biomemap.png')
	ok = 0
	for y in range(h):
		for x in range(w):
			o1 = (y * w + x) * c1
			o2 = (y * w + x) * c2
			key = pal[(x // 32) + dim * (y // 32)][BLEND_SLOT[tuple(bm[o1:o1 + 4])]]
			ok += key == (bio[o2] << 16 | bio[o2 + 1] << 8 | bio[o2 + 2])
	print(f'biomemap == palette[blendmap]: {ok}/{w * h}')

if __name__ == '__main__':
	main()
