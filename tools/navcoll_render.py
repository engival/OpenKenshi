#!/usr/bin/env python3
"""Dev-only: render a ksim_navcoll_export case dir (navmesh faces + collision shapes) to PNGs, from a few angles.

    navcoll_render.py CASEDIR OUTDIR [--view NAME:az:el:ymin:ymax:cx:cz:half ...]

Orthographic. az = degrees clockwise seen from above (0: looking toward -z... see below), el = degrees above the horizon.
Without --view the default views round the building position of CASEDIR/region.txt apply. Reads nav_faces.tsv, nav_face_data.tsv, col_shapes.tsv
and the col_*.obj sidecars. The input is game-derived geometry; the script holds none.
No PIL/matplotlib needed (they do not load on the dev VM): numpy + zlib only. Each view writes <NAME>_nav.png, <NAME>_col.png, <NAME>_both.png.
"""
import math
import os
import sys

import numpy as np
import struct
import zlib

SIZE = 1400

# Default views, relative to the building position P of CASEDIR/region.txt: (name, az, el, y band below/above P.y, half-width of the xz crop)
# name:az:el:ymin:ymax:cx:cz:half is the --view form (cx, cz, half: crop square in world xz; y band to keep, by vertex).
DEFAULT_VIEWS = [
	("plan_all", 0, 90, -1e9, 1e9, 170),
	("floor0", 0, 90, -30, 25, 110),
	("floor1", 0, 90, 30, 70, 110),
	("oblique_all", 35, 40, -1e9, 1e9, 150),
	("oblique_in", 35, 40, -30, 110, 100),
	("oblique_in2", 215, 40, -30, 110, 100),
]


def default_views(case):
	pos = None
	with open(os.path.join(case, "region.txt")) as f:
		for line in f:
			if line.startswith("position "):
				pos = [float(v) for v in line.split()[1:4]]
	if pos is None:
		return []
	return ["%s:%g:%g:%g:%g:%g:%g:%g" % (n, az, el, pos[1] + lo if lo > -1e8 else lo, pos[1] + hi if hi < 1e8 else hi, pos[0], pos[2], half)
		for n, az, el, lo, hi, half in DEFAULT_VIEWS]


AREA_COLOURS = {}


def read_faces(case):
	faces = {}
	with open(os.path.join(case, "nav_faces.tsv")) as f:
		for line in f:
			if line.startswith("#"):
				continue
			t = line.split("\t")
			n = int(t[1])
			faces[int(t[0])] = np.array([float(x) for x in t[2:2 + 3 * n]]).reshape(n, 3)
	area = {}
	p = os.path.join(case, "nav_face_data.tsv")
	if os.path.exists(p):
		with open(p) as f:
			for line in f:
				t = line.rstrip("\n").split("\t")
				if len(t) == 3 and t[1] == "area":
					area[int(t[0])] = int(t[2])
	return faces, area


def read_shapes(case):
	shapes = []
	with open(os.path.join(case, "col_shapes.tsv")) as f:
		for line in f:
			if line.startswith("#"):
				continue
			t = line.rstrip("\n").split("\t")
			sid = int(t[0])
			verts, tris = [], []
			with open(os.path.join(case, "col_%d.obj" % sid)) as o:
				for l in o:
					if l.startswith("v "):
						verts.append([float(x) for x in l.split()[1:4]])
					elif l.startswith("f "):
						tris.append([int(x) - 1 for x in l.split()[1:4]])
			shapes.append((sid, t[2], t[3], np.array(verts), np.array(tris)))
	return shapes


class Cam:
	def __init__(self, az, el, cx, cy, cz, half):
		a, e = math.radians(az), math.radians(el)
		# view direction from the camera toward the scene; the camera sits at azimuth az (0 = south of the scene, at +z, looking to -z)
		self.fwd = np.array([-math.sin(a) * math.cos(e), -math.sin(e), -math.cos(a) * math.cos(e)])
		up0 = np.array([0.0, 1.0, 0.0]) if el < 89.5 else np.array([0.0, 0.0, -1.0])
		self.right = np.cross(self.fwd, up0)
		self.right /= np.linalg.norm(self.right)
		self.up = np.cross(self.right, self.fwd)
		self.c = np.array([cx, cy, cz])
		self.scale = SIZE / (2.0 * half * 1.2)

	def project(self, p):
		d = p - self.c
		return np.stack([SIZE / 2 + (d @ self.right) * self.scale, SIZE / 2 - (d @ self.up) * self.scale], axis=-1), d @ self.fwd


def colour_for(area):
	return {1: (110, 170, 120), 2: (200, 180, 90), 3: (90, 140, 200), 4: (150, 110, 200), 5: (210, 110, 110)}.get(area, (130, 160, 140))


def render(case, out, spec, faces, area, shapes):
	name, az, el, ymin, ymax, cx, cz, half = spec.split(":")
	az, el, ymin, ymax, cx, cz, half = float(az), float(el), float(ymin), float(ymax), float(cx), float(cz), float(half)
	band = [(fid, v) for fid, v in faces.items() if ymin <= v[:, 1].mean() <= ymax and abs(v[:, 0].mean() - cx) < half * 1.5 and abs(v[:, 2].mean() - cz) < half * 1.5]
	cy = np.mean([v[:, 1].mean() for _, v in band]) if band else 0.0
	cam = Cam(az, el, cx, cy, cz, half)
	items = []  # (depth, kind, points, colour)
	for fid, v in band:
		pts, depth = cam.project(v)
		items.append((depth.mean(), "nav", pts, colour_for(area.get(fid, 0)), fid))
	for sid, kind, group, verts, tris in shapes:
		if len(verts) == 0 or not (ymin <= verts[:, 1].mean() <= ymax or ymin <= verts[:, 1].max() and verts[:, 1].min() <= ymax):
			continue
		if abs(verts[:, 0].mean() - cx) > half * 1.5 or abs(verts[:, 2].mean() - cz) > half * 1.5:
			continue
		pts, depth = cam.project(verts)
		for t in tris:
			items.append((depth[t].mean(), "col", pts[t], (60, 60, 200) if "Walkable" in group else (200, 90, 40) if "Obstacle" in group else (150, 150, 150), sid))
	for what in ("nav", "col", "both"):
		fb = np.zeros((SIZE, SIZE, 3), dtype=np.float32)
		fb[:] = (24, 24, 28)
		for depth, kind, pts, colour, _ in sorted(items, key=lambda i: -i[0]):
			if what != "both" and kind != what:
				continue
			if kind == "nav":
				fill(fb, pts, colour, 0.6 if what == "both" else 0.85)
				outline(fb, pts, (235, 235, 235), 0.9)
			else:
				fill(fb, pts, colour, 0.12)
				outline(fb, pts, colour, 0.7)
		write_png(os.path.join(out, "%s_%s.png" % (name, what)), fb.astype(np.uint8))


def fill(fb, pts, colour, alpha):
	"""Alpha-fill a convex polygon (nav faces are convex; collision triangles are triangles)."""
	x0, y0 = np.floor(pts.min(axis=0)).astype(int)
	x1, y1 = np.ceil(pts.max(axis=0)).astype(int)
	x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, SIZE - 1), min(y1, SIZE - 1)
	if x1 < x0 or y1 < y0:
		return
	xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
	inside = np.ones(xs.shape, dtype=bool)
	n = len(pts)
	area = sum(pts[i][0] * pts[(i + 1) % n][1] - pts[(i + 1) % n][0] * pts[i][1] for i in range(n))
	if abs(area) < 1e-6:
		return
	for i in range(n):
		a, b = pts[i], pts[(i + 1) % n]
		cross = (b[0] - a[0]) * (ys - a[1]) - (b[1] - a[1]) * (xs - a[0])
		inside &= (cross * area >= 0)
	region = fb[y0:y1 + 1, x0:x1 + 1]
	region[inside] = region[inside] * (1 - alpha) + np.array(colour, dtype=np.float32) * alpha


def outline(fb, pts, colour, alpha):
	n = len(pts)
	for i in range(n):
		a, b = pts[i], pts[(i + 1) % n]
		steps = int(max(abs(b[0] - a[0]), abs(b[1] - a[1]))) + 1
		t = np.linspace(0, 1, steps + 1)
		x = np.round(a[0] + (b[0] - a[0]) * t).astype(int)
		y = np.round(a[1] + (b[1] - a[1]) * t).astype(int)
		ok = (x >= 0) & (x < SIZE) & (y >= 0) & (y < SIZE)
		fb[y[ok], x[ok]] = fb[y[ok], x[ok]] * (1 - alpha) + np.array(colour, dtype=np.float32) * alpha


def write_png(path, rgb):
	h, w, _ = rgb.shape
	raw = b"".join(b"\x00" + rgb[r].tobytes() for r in range(h))
	def chunk(tag, data):
		c = struct.pack(">I", len(data)) + tag + data
		return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
	with open(path, "wb") as f:
		f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
	args = [a for a in sys.argv[1:] if not a.startswith("--view")]
	views = [sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--view"]
	args = [a for a in args if a not in views]
	if len(args) != 2:
		print(__doc__)
		return 2
	case, out = args
	os.makedirs(out, exist_ok=True)
	key = os.path.basename(case.rstrip("/"))
	faces, area = read_faces(case)
	shapes = read_shapes(case)
	for spec in views or default_views(case):
		render(case, out, spec, faces, area, shapes)
	return 0


if __name__ == "__main__":
	sys.exit(main())
