#!/usr/bin/env python3
"""lightning (kenshi-k16.2.177.1): the bolt's flash timeline from one-frame-a-shot PNGs.

The camera is static and the bolt never moves, so the per-pixel minimum over the series is the background (rain
streaks and the moving water vary, the terrain, HUD and debug text do not). The flash mask is the pixels whose
max-over-frames difference beats 40, kept to the bolt's column band (+-80 px around its densest column) and under
the debug text block. The mask is split into the column (the two bolt techniques) and the foot (the lowest 25 % of
its rows: the Flames fireball and the SPARKS at the base), so the fireball-only phase before the first bolt particle
and the end of the flash are both readable.

    measure.py OUT/mode [OUT/mode]      # shots fNNN.png; prints the two series and first/last lit frames
"""
import glob
import os
import sys

import numpy as np
from PIL import Image


def series(d):
    shots = sorted(glob.glob(os.path.join(d, "f*.png")))
    assert shots, "no shots in " + d
    names = [os.path.basename(p)[:4] for p in shots]
    vol = np.stack([np.asarray(Image.open(p).convert("L"), dtype=np.float32) for p in shots])
    H, W = vol.shape[1:]
    bg = vol.min(axis=0)
    diff = vol - bg
    mask = diff.max(axis=0) > 40
    mask[:70, :] = False               # the debug text rows change with the frame number
    mask[H - 220:, :] = False          # the HUD
    ys, xs = np.nonzero(mask)
    if not len(xs):
        return names, np.zeros(len(shots)), np.zeros(len(shots)), None
    hist = np.bincount(xs, minlength=W)
    x0 = int(np.argmax(np.convolve(hist, np.ones(121), "same")))
    keep = (xs >= x0 - 80) & (xs <= x0 + 80)
    m2 = np.zeros_like(mask)
    m2[ys[keep], xs[keep]] = True
    mask = m2
    ys, xs = np.nonzero(mask)
    if not len(ys):
        return names, np.zeros(len(shots)), np.zeros(len(shots)), None
    y0, y1 = int(ys.min()), int(ys.max())
    foot = np.zeros_like(mask)
    yfoot = y1 - int(0.25 * (y1 - y0))
    foot[yfoot:, :] = mask[yfoot:, :]
    col = mask & ~foot
    pos = np.clip(diff, 0, None)
    tot = np.array([float(p[col].sum()) for p in pos])
    fot = np.array([float(p[foot].sum()) for p in pos])
    return names, tot, fot, (x0, y0, y1, yfoot)


def report(d):
    names, tot, fot, box = series(d)
    if box is None:
        print("%s: nothing moves: no flash" % d)
        return names, tot, fot
    x0, y0, y1, yfoot = box
    # the pre-flash level: the first 20 shots (the run starts before the handler spawns)
    n0 = min(20, len(tot))
    cbase, fbase = float(np.median(tot[:n0])), float(np.median(fot[:n0]))
    print("%s: bolt band x %d, mask rows %d..%d (foot from %d); pre-flash column %.0f foot %.0f"
          % (d, x0, y0, y1, yfoot, cbase, fbase))
    lit = tot > 3.0 * cbase	# the column is ~10-20x the pre-flash level; 2x still catches the wet-ground afterglow
    flit = fot > max(2.0 * fbase, 1.0)
    first = lambda m: names[np.argmax(m)] if m.any() else "-"
    last = lambda m: names[len(m) - 1 - int(np.argmax(m[::-1]))] if m.any() else "-"
    print("  foot lit %s..%s  (%d frames);  column lit %s..%s  (%d frames)"
          % (first(flit), last(flit), int(flit.sum()), first(lit), last(lit), int(lit.sum())))
    peak = max(tot.max(), 1.0)
    for i, n in enumerate(names):
        print("  %s  column %9.0f  foot %9.0f  %s%s" % (n, tot[i], fot[i], "#" * int(60.0 * tot[i] / peak),
                                                       " ." * 0))
    return names, tot, fot


if __name__ == "__main__":
    for d in (sys.argv[1:] or ["."]):
        report(d)
