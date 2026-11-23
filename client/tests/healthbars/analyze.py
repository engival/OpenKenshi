#!/usr/bin/env python3
"""kenshi-1fxq: per shot, the wire parts (dump medical) against the drawn bar layers (pixels). usage: analyze.py RUNDIR"""
import re, sys
from PIL import Image
d = sys.argv[1]
log = open(f"{d}/client.log").read().splitlines()
import glob, os
names = sorted(os.path.basename(f)[:-4] for f in glob.glob(f"{d}/r[0-9]_*.png") if not f.endswith("_panel.png") and "_panel" not in f)
shots, cur = [], None
for ln in log:
    if ln.startswith("clock:"):
        cur = {"clock": int(re.search(r"tick (\d+)", ln).group(1)), "parts": [], "mt": 0}
        shots.append(cur)
    elif ln.startswith("medical") and " part " in ln and cur is not None:
        m = re.search(r'name "([^"]*)" health (\S+) stun (\S+) max (\S+) bandage (\S+)', ln)
        cur["parts"].append((m.group(1), *(float(m.group(i)) for i in range(2, 6))))
    elif ln.startswith("medical") and "tick" in ln and cur is not None:
        cur["mt"] = int(re.search(r"tick (\d+)", ln).group(1))
for s, n in zip(shots, names): s["name"] = n
ROW = {"Head": 596.5, "Stomach": 612.5, "Chest": 628.5, "Left Arm": 644.5, "Right Arm": 660.5, "Left Leg": 676.5, "Right Leg": 692.5}
X0, X1 = 174, 333
def cls(p):
    r, g, b = p
    if g > r + 25 and g > b + 25: return "G"
    if r > 100 and g > 70 and b < 50 and r > g: return "Y"
    if abs(r - g) < 30 and b >= r - 12 and r > 85: return "S"   # grey
    if r > g + 25 and g < 60: return "R"
    return "?"
print("shot clock_tick wire_tick part health stun bandage | exp green yellow grey (px) | drawn green yellow grey red")
for s in shots:
    im = Image.open(f"{d}/{s['name']}.png").convert("RGB")
    for name, h, st, mx, bd in s["parts"]:
        if h >= 99.9 and st == 0 and bd == 0: continue
        y = int(ROW[name]) - 6
        c = [cls(im.getpixel((x, y))) for x in range(X0, X1 + 1)]
        W = X1 - X0 + 1
        eg = max(0, min(1, (h - st) / mx)) * W
        ey = max(0, min(1, (h + bd - st) / mx)) * W - eg
        es = st / mx * W
        print(f"{s['name']} {s['clock']} {s['mt']} {name} {h:.2f} {st:.2f} {bd:.2f} | {eg:.0f} {ey:.0f} {es:.0f} | {c.count('G')} {c.count('Y')} {c.count('S')} {c.count('R')}")
