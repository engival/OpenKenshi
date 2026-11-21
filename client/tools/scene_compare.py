# Compares scene-test output with the baseline and builds contact sheets. Called by scene_test.sh:
#   scene_compare.py OUTDIR BASEDIR BLESS SCENES_FILE [KEEP_ALL]
# Images are NAME.webp (lossless, the Linux default) or NAME.png (screenshot.format=png); baselines may be either. A scene that
# passes keeps no image in OUTDIR (kenshi-k16.2.470.1): its row says "unchanged: NAME = <baseline file>" and summary.txt lists it;
# only changed / new / FAIL scenes keep image, sheet and the overview. KEEP_ALL=1 (scene_test.sh --keep-all) keeps everything.
import json, math, os, shutil, sys
from PIL import Image, ImageChops, ImageDraw

THRESHOLD = 16		# per-channel difference that counts as a changed pixel
PASS_PERCENT = 0.5	# changed pixels (% of the non-HUD area) up to which a scene passes
CELL = (640, 360)

out, base, bless, scenes_file = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4]
keep_all = len(sys.argv) > 5 and sys.argv[5] == "1"
summary = []

def say(line):
	print(line)
	summary.append(line)

def find(d, n):
	for ext in (".webp", ".png"):
		if os.path.exists(f"{d}/{n}{ext}"):
			return f"{d}/{n}{ext}"
	return None

def save(img, path):
	if path.endswith(".webp"):
		img.save(path, lossless=True, exact=True)
	else:
		img.save(path)

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
say(f"{'scene':18} {'status':8} {'rmse':>6} {'chg%':>6} {'bld':>5} {'fol':>6} {'ter':>4} {'cpu ms':>7} {'wait ms':>8} {'sg':>3}")
ext = ".png"
for n in names:
	st = json.load(open(f"{out}/{n}.json"))
	curpath = find(out, n)
	if curpath is None:
		say(f"{n:18} FAIL     no image was written")
		failed = True
		continue
	ext = os.path.splitext(curpath)[1]
	cur = Image.open(curpath)
	cur.load()
	bpath = find(base, n)
	rmse = pct = None
	if bpath:
		oldimg = Image.open(bpath)
		rmse, pct = compare(cur, oldimg, st["hud_box"])
		status = "pass" if pct <= PASS_PERCENT else "changed"
	else:
		status = "new"
	if st["sg_errors"] or st["load_failed"]:
		status, failed = "FAIL", True
	unchanged = status == "pass" and not keep_all
	ref = bpath
	if bless:
		for e in (".webp", ".png"):
			if os.path.exists(f"{base}/{n}{e}"):
				os.remove(f"{base}/{n}{e}")
		bpath = f"{base}/{n}{ext}"
		shutil.copy(curpath, bpath)
		shutil.copy(f"{out}/{n}.json", f"{base}/{n}.json")
		status += "+blessed"
	cmp = f"{rmse:6.2f} {pct:6.2f}" if rmse is not None else f"{'-':>6} {'-':>6}"
	say(f"{n:18} {status:8} {cmp} {st['buildings_drawn']:5} {st['foliage_drawn']:6} {st['terrain_chunks_drawn']:4} {st['cpu_ms']:7.2f} {st['load_wait_ms']:8.0f} {st['sg_errors']:3}")
	if unchanged:
		say(f"unchanged: {n} = {ref}")
		os.remove(curpath)
		continue
	rows.append((n, status, cur))
	sheet = Image.new("RGB", (CELL[0] * 3, CELL[1]), (0, 0, 0))
	for i, (path, label) in enumerate(((refs.get(n), "reference"), (ref if not bless else None, "baseline"), (curpath, "current"))):
		sheet.paste(tile(path, f"{n}: {label}"), (CELL[0] * i, 0))
	save(sheet, f"{out}/{n}.sheet{ext}")
if not names:	# no scene rendered (kclient could not read scenes.txt: it is found beside the binary's parent dir): an empty overview image made PIL raise "tile cannot extend outside image"
	print("scene_compare: no scene was rendered; nothing to compare", file=sys.stderr)
	sys.exit(1)
if rows:	# the overview holds the scenes that kept an image (all of them with --keep-all)
	cols = 3
	over = Image.new("RGB", (CELL[0] * cols, CELL[1] * ((len(rows) + cols - 1) // cols)), (0, 0, 0))
	for i, (n, status, cur) in enumerate(rows):
		t = Image.new("RGB", CELL, (40, 40, 40))
		t.paste(cur.convert("RGB").resize(CELL, Image.LANCZOS))
		ImageDraw.Draw(t).text((6, CELL[1] - 14), f"{n} [{status}]", fill=(255, 255, 0))
		over.paste(t, (CELL[0] * (i % cols), CELL[1] * (i // cols)))
	save(over, f"{out}/overview{ext}")
else:
	say("every scene unchanged: no scene image kept (--keep-all keeps them)")
open(f"{out}/summary.txt", "w").write("\n".join(summary) + "\n")
sys.exit(1 if failed else 0)
