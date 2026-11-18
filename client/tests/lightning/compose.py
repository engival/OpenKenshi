#!/usr/bin/env python3
"""lightning (kenshi-k16.2.177.1): ours beside the original's live bolt, phase by phase.

The original capture (kenshi-rwh.120) is a ~29 fps sequence of one bolt: its frame k is timing.json's t_s[k], its
first bolt-column particle is frame 11 (0.44 s after the pause, i.e. after the spawn), and the light is gone at frame
44 (1.63 s). Our run shoots one frame a game-1/60 s, so a phase t maps to our frame
first_particle + round((t - 0.44) * 60).

    compose.py ORIG_DIR OUR_DIR OUT.png --first N [--phases 0,6,11,...] [--columns 6] [--width 420]
        ORIG_DIR = the capture's frames dir (seq/fNNN.jpg + timing.json), OUR_DIR = a run.sh mode dir (fNNN.png).
        --first  our frame of the first bolt-column particle (measure.py's "column lit" first frame).

Each cell is one phase: the original frame on top, the matching ours frame below, both cropped to the game view (our
debug text block and HUD are cut; the original has neither) and labelled with both frame numbers.
"""
import argparse
import json
import os

from PIL import Image, ImageDraw

ORIG_FPS = 1.0 / 0.0342      # the sequence's mean dt, only for a capture without timing.json
OUR_FPS = 60.0               # scripted runs step by kScriptDt = 1/60 s
ORIG_FIRST = 11              # the original's first bolt particle
ORIG_T_FIRST = 0.44          # ... at 0.44 s (weather_lightning_live.md answer 1)
CROP_TOP = 70                # our debug text block / the original's clean top: keep both from here
CROP_BOTTOM = 500            # our HUD starts here; the original's frame runs to 720


def times(orig_dir):
    p = os.path.join(orig_dir, "timing.json")
    if not os.path.exists(p):
        return None
    with open(p) as fh:
        return [f["t_s"] for f in json.load(fh)["frames"]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("orig")
    ap.add_argument("ours")
    ap.add_argument("out")
    ap.add_argument("--first", type=int, required=True, help="our frame of the first bolt particle")
    ap.add_argument("--orig-prefix", default="seq/f%03d.jpg")
    ap.add_argument("--ours-prefix", default="f%03d.png")
    ap.add_argument("--phases", default="0,6,11,14,20,25,28,33,38,41,44,50")
    ap.add_argument("--columns", type=int, default=6)
    ap.add_argument("--width", type=int, default=420, help="tile width (16:9 source, cropped to the game view)")
    args = ap.parse_args()

    phases = [int(p) for p in args.phases.split(",")]
    ts = times(args.orig)
    crop_h = CROP_BOTTOM - CROP_TOP
    tw = args.width
    th = tw * crop_h // 1280
    label = 15
    cols = max(1, args.columns)
    rows = (len(phases) + cols - 1) // cols
    cell_h = (th + label) * 2
    sheet = Image.new("RGB", (tw * cols, cell_h * rows), (12, 12, 12))
    dr = ImageDraw.Draw(sheet)
    for c, k in enumerate(phases):
        t = ts[k] if ts and k < len(ts) else k / ORIG_FPS
        ours_k = args.first + int(round((t - ORIG_T_FIRST) * OUR_FPS))
        x = (c % cols) * tw
        y = (c // cols) * cell_h
        for j, (d, name, colour, who) in enumerate((
            (args.orig, args.orig_prefix % k, (255, 240, 140), "orig f%03d %.2fs" % (k, t)),
            (args.ours, args.ours_prefix % ours_k, (140, 220, 255), "ours f%03d (phase %.2fs)" % (ours_k, t)),
        )):
            p = os.path.join(d, name)
            if not os.path.exists(p):
                dr.text((x + 3, y + j * (th + label) + 2), who + " MISSING", fill=(255, 80, 80))
                continue
            im = Image.open(p).convert("RGB").crop((0, CROP_TOP, 1280, CROP_BOTTOM)).resize((tw, th))
            sheet.paste(im, (x, y + j * (th + label) + label))
            dr.text((x + 3, y + j * (th + label) + 2), who, fill=colour)
    sheet.save(args.out)
    print("%s: %d phases in %dx%d cells, tile %dx%d, sheet %s" % (args.out, len(phases), cols, rows, tw, th, sheet.size))


if __name__ == "__main__":
    main()
