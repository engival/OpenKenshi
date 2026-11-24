#!/bin/bash
# beams_air cone (kenshi-ac8f): cone.sh OUT. FAILS when a lamp's cone is missing from a camera, or does not start at the bulb. sunshafts_bar (the Hub bar, roof cut), night, render.lamp_beams 1 with
# render.sun_shafts_debug=7 (the beam term x8 alone, so a pixel reads the cone itself). Each of the bar's three shaded lamps (9391 / 9392 / 9393, lamp:<id>.l10) is soloed in turn (the light
# debugger: only that lamp's cone) from the ask's camera (kenshi-jj3.28.1.1.1, the far view) and from a camera inside the bar 43-63 u from the lamp. Two samples per shot, projected from the lamp's
# world position (the free camera, yaw 0 = -z, 90 = +x): BELOW, 15 u under the bulb end of the fixture (13 u under the light record's position): the cone must be there (mean >= MIN, default
# 20 of 255); ABOVE, in the cord 6 u under the record's position and 4 u aside: air the old cone filled from its apex at the record's position, a cone from the bulb does not reach (<= RATIO x
# BELOW, default 0.25). Measured (old: apex at the record's position / new: apex at the bulb) in the log of the task item. Needs the game data (SKIP otherwise). Port: PORT. Exit 1 on a failure.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$here/../need_data.sh"
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
min=${MIN:-20}; ratio=${RATIO:-0.25}
python3 -I - "$out/script.txt" <<'P'
import sys, math
lamps = {'9391': (-50616.1, 1135.3, -1217.9), '9392': (-50539.0, 1128.6, -1187.4), '9393': (-50549.3, 1129.2, -1228.2)}
s = """100 set animate 1
100 set timescale 0
150 camera char Wanderer
310 move -50544 -1231
850 camera char Wanderer
852 key F down
854 key F up
856 key F down
858 key F up
"""
f = 1262
shots = []
cams = []
for k, (x, y, z) in lamps.items():
    cams.append((k, 'far', -50698, -1255, 110.5, 0 if k == '9391' else -8.8, 34 if k == '9391' else 28))	# 9391 hangs above the top of the ask's frame: level pitch and a little higher
    cx, cz = {'9391': (-50610, -1262), '9392': (-50600, -1170), '9393': (-50575, -1262)}[k]	# inside the bar, 43-63 u from the lamp, facing it
    cams.append((k, 'near', cx, cz, round(math.degrees(math.atan2(x - cx, -(z - cz))), 1), {'9391': 5, '9392': -3, '9393': 9}[k], 43))	# the pitch frames the two samples (the terrain under the camera sets its eye height)
for k, n, x, z, yaw, pit, alt in cams:
    name = '%s_%s' % (k, n)
    s += "%d set pos %d,%d\n%d set yaw %s\n%d set pitch %s\n%d set alt %s\n" % (f, x, z, f + 1, yaw, f + 2, pit, f + 3, alt)
    s += "%d light lamp:%s.l10 solo\n%d light lamp_beams on\n%d light lamps off\n" % (f + 4, k, f + 5, f + 6)
    s += "%d set time 2.82\n%d screenshot OUTDIR/%s.png\n" % (f + 40, f + 42, name)
    f += 60
s += "%d quit\n" % f
open(sys.argv[1], 'w').write(s)
P
mkdir -p "$out/b1"
SCEN=sunshafts_bar MODES=b1 XSET='--set render.sun_shafts_debug=7' SCRIPT="$out/script.txt" bash "$here/../lampbeams/run.sh" "$out" || exit 1
python3 -I - "$out/b1" "$min" "$ratio" <<'P'
import sys, math, re
from PIL import Image
import numpy as np
d, mn, ratio = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
lamps = {'9391': (-50616.1, 1135.3, -1217.9), '9392': (-50539.0, 1128.6, -1187.4), '9393': (-50549.3, 1129.2, -1228.2)}
def proj(cam, yaw, pitch, p, W=1280, H=720, fovy=45.0):
	y = math.radians(yaw); pt = math.radians(pitch)
	f = (math.sin(y) * math.cos(pt), math.sin(pt), -math.cos(y) * math.cos(pt))
	r = (math.cos(y), 0.0, math.sin(y))
	u = (r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0])
	if u[1] < 0: u = tuple(-a for a in u)
	v = [p[i] - cam[i] for i in range(3)]
	z = sum(v[i] * f[i] for i in range(3)); x = sum(v[i] * r[i] for i in range(3)); yy = sum(v[i] * u[i] for i in range(3))
	if z <= 0.1: return None
	t = math.tan(math.radians(fovy) / 2)
	return (W / 2 + x / z / (t * W / H) * W / 2, H / 2 - yy / z / t * H / 2)
def sample(a, pp):
	if pp is None: return None
	x, y = int(pp[0]), int(pp[1])
	if not (8 <= x < 1272 and 8 <= y < 500) or (x > 1090 and y < 240): return None
	return float(a[y - 3:y + 4, x - 3:x + 4].mean())
fail = 0
for k, p in lamps.items():
	for n in ('far', 'near'):
		name = '%s_%s' % (k, n)
		try:
			txt = open('%s/%s.txt' % (d, name)).read()
			m = re.search(r'^camera: free eye (\S+) (\S+) (\S+) heading \S+ ([\d.]+) pitch (\S+)', txt, re.M)
			cam = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
			yaw = float(m.group(4)); pit = float(m.group(5))
			a = np.asarray(Image.open('%s/%s.png' % (d, name)).convert('L')).astype(float)
		except Exception as e:
			print('beams_air cone %s: FAIL (no shot: %s)' % (name, e)); fail = 1; continue
		r = (math.cos(math.radians(yaw)), 0.0, math.sin(math.radians(yaw)))
		below = sample(a, proj(cam, yaw, pit, (p[0], p[1] - 13 - 15, p[2])))
		above = sample(a, proj(cam, yaw, pit, (p[0] + 4 * r[0], p[1] - 6, p[2] + 4 * r[2])))
		if below is None or above is None:
			print('beams_air cone %s: FAIL (the sample points are off screen: below %s above %s)' % (name, below, above)); fail = 1; continue
		ok = below >= mn and above <= ratio * below
		print('beams_air cone %s: %s below the bulb %.1f (>= %g), above %.1f (<= %g x below)' % (name, 'PASS' if ok else 'FAIL', below, mn, above, ratio))
		fail |= 0 if ok else 1
sys.exit(fail)
P
