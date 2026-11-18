# Compares scene-test output with the baseline and builds contact sheets. Called by scene_test.sh:
#   scene_compare.py OUTDIR BASEDIR BLESS SCENES_FILE
import json, math, os, shutil, sys
from PIL import Image, ImageChops, ImageDraw

THRESHOLD = 16		# per-channel difference that counts as a changed pixel
PASS_PERCENT = 0.5	# changed pixels (% of the non-HUD area) up to which a scene passes
CELL = (640, 360)

out, base, bless, scenes_file = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4]

def scene_refs():
	refs = {}
	for line in open(scenes_file):
		fields = line.split("#")[0].strip().split("\t")
		for f in fields[1:]:
			if f.startswith("ref="):
				r = f[4:].strip()
				if "/" not in r:
					r = os.path.join(os.environ.get("KCLIENT_SCENE_REFS", ""), r) if os.environ.get("KCLIENT_SCENE_REFS") else ""
				if r and os.path.exists(r):
					refs[fields[0]] = r
				else:
					print(f"scene {fields[0]}: no reference ({f[4:].strip()})")
	return refs

def compare(cur, old, box):
	diff = ImageChops.difference(cur.convert("RGB"), old.convert("RGB"))
	x, y, w, h = box
	diff.paste((0, 0, 0), (x, y, x + w, y + h))
	r, g, b = diff.split()
	hist = ImageChops.lighter(ImageChops.lighter(r, g), b).histogram()
	n = diff.width * diff.height - w * h
	sq = sum(sum(c.histogram()[i] * i * i for i in range(256)) for c in (r, g, b))
	changed = sum(hist[THRESHOLD + 1:])
	return math.sqrt(sq / (3 * n)), 100.0 * changed / n

def tile(path, label):
	img = Image.new("RGB", CELL, (40, 40, 40))
	if path and os.path.exists(path):
		img.paste(Image.open(path).convert("RGB").resize(CELL, Image.LANCZOS))
	ImageDraw.Draw(img).text((6, CELL[1] - 14), label, fill=(255, 255, 0))
	return img

refs = scene_refs()
names = [n for n in refs.keys() | {os.path.splitext(f)[0] for f in os.listdir(out) if f.endswith(".json")} if os.path.exists(f"{out}/{n}.json")]
order = [l.split("\t")[0] for l in open(scenes_file) if l.strip() and not l.startswith("#")]
names.sort(key=lambda n: order.index(n) if n in order else 999)
rows = []
failed = False
print(f"{'scene':18} {'status':8} {'rmse':>6} {'chg%':>6} {'bld':>5} {'fol':>6} {'ter':>4} {'cpu ms':>7} {'wait ms':>8} {'sg':>3}")
for n in names:
	st = json.load(open(f"{out}/{n}.json"))
	cur = Image.open(f"{out}/{n}.png")
	bpng = f"{base}/{n}.png"
	rmse = pct = None
	if os.path.exists(bpng):
		rmse, pct = compare(cur, Image.open(bpng), st["hud_box"])
		status = "pass" if pct <= PASS_PERCENT else "changed"
	else:
		status = "new"
	if st["sg_errors"] or st["load_failed"]:
		status, failed = "FAIL", True
	if bless:
		shutil.copy(f"{out}/{n}.png", bpng)
		shutil.copy(f"{out}/{n}.json", f"{base}/{n}.json")
		status += "+blessed"
	cmp = f"{rmse:6.2f} {pct:6.2f}" if rmse is not None else f"{'-':>6} {'-':>6}"
	print(f"{n:18} {status:8} {cmp} {st['buildings_drawn']:5} {st['foliage_drawn']:6} {st['terrain_chunks_drawn']:4} {st['cpu_ms']:7.2f} {st['load_wait_ms']:8.0f} {st['sg_errors']:3}")
	rows.append((n, status, cur))
	sheet = Image.new("RGB", (CELL[0] * 3, CELL[1]), (0, 0, 0))
	for i, (path, label) in enumerate(((refs.get(n), "reference"), (bpng if not bless else None, "baseline"), (f"{out}/{n}.png", "current"))):
		sheet.paste(tile(path, f"{n}: {label}"), (CELL[0] * i, 0))
	sheet.save(f"{out}/{n}.sheet.png")
cols = 3
over = Image.new("RGB", (CELL[0] * cols, CELL[1] * ((len(rows) + cols - 1) // cols)), (0, 0, 0))
for i, (n, status, cur) in enumerate(rows):
	t = tile(f"{out}/{n}.png", f"{n} [{status}]")
	over.paste(t, (CELL[0] * (i % cols), CELL[1] * (i // cols)))
over.save(f"{out}/overview.png")
sys.exit(1 if failed else 0)
