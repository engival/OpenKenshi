#!/usr/bin/env python3
"""Foot log analysis (client `log feet`, docs/CLIENT_GOALS.md "Locomotion fidelity" step 1).

Usage: footlog.py LOG [--id N] [--tol 0.5] [--series T0 T1] [--csv OUT] [--csvdir DIR]

Per foot and frame the planted point is the lower of heel / ball. A foot is in contact while that point is within --tol of the
ground under it. Slide = the horizontal travel of a point that is in contact on both frames (the ball when both are).

"Matched body speed" (the combat strafe measurement): a contact only counts as a slide when the body is moving (drawn speed
above MIN_BODY) and the contact point travels at a fraction of that speed within MATCH_LO..MATCH_HI (the foot dragged along
with the body). A swing foot brushing the ground passes at two to three times the body speed and is dropped. This is the slide
that reads as the body floating; it is reported per clip and per character (MINE: the ratio window and the body speed).

With --id: each stance (start, length, contact points, slide, worst slide speed, mean gap to the ground), then totals.
Without --id: one summary row per character (the whole brawl), then the per-clip slide table: frames, foot contacts, matched
contacts, matched slide distance, mean matched slide / body speed, slide / body over every contact (a held foot counts as 0, so the
foot lock's target is measurable), slide / body over the contact-window frames only (the log's lock state says the window is down;
"-" when the log has no lock, i.e. the lock was off), and the worst matched frame; then, when the log carries the
foot lock (anim.foot_lock), the contacts split by that foot's lock state: held at its plant (with how fast the held point still
moves), released past the leg's reach, or free (the lock's window closed / it is off).
--series prints the frames of a time window; --csv writes every frame of one id; --csvdir writes one CSV per character of
every frame (time, foot height over the ground, foot horizontal speed, body speed, the lower clip and its weight).
"""
import argparse
import math
import os
import re
import sys
from collections import OrderedDict

# footlog T DT id ID tick K wire FWD LAT legs MOVE STRAFE phase P flags 0xX stop S lie L rise R warp W steer T lock L R pose P | L ... | R ... | clips [| actroot ...]
# lock: the foot lock per foot, 1 held at its plant, 2 released past the leg's reach (window still down), 0 none
# pose: the pose's largest local joint rotation step, deg/frame (the blend's peak angular velocity, on or off anim.inertialize)
# actroot (anim.action_root, on a travelling action): the clip root's replay point, the wire, the drawn body and the eased correction, x/z
LINE = re.compile(r'^footlog (?P<t>\S+) (?P<dt>\S+) id (?P<id>\d+)(?: tick (?P<tick>\d+))? wire (?P<wire0>\S+) (?P<wire1>\S+)(?: wv (?P<wv>\S+))?'
                  r' legs (?P<legs0>\S+) (?P<legs1>\S+) phase (?P<phase>\S+) flags (?P<flags>\S+)(?: act "(?P<act>[^"]*)")?(?: ca (?P<ca>-?\d+) cd (?P<cd>\d))? stop (?P<stop>\d) lie (?P<lie>-?\d+)'
                  r' rise (?P<rise>\d) warp (?P<warp>\d) steer (?P<steer>\d)(?: drop (?P<drop>\S+))?(?: lock (?P<lock0>\d) (?P<lock1>\d))?(?: pose (?P<pose>\S+))? \| L (?P<L>.*?) \| R (?P<R>.*?) \| (?P<clips>.*?)(?: \| actroot (?P<actroot>.*))?$')
# before the tick and state fields (logs kept in a findings directory): same line without tick..steer
LINE_OLD = re.compile(r'^footlog (?P<t>\S+) (?P<dt>\S+) id (?P<id>\d+) wire (?P<wire0>\S+) (?P<wire1>\S+) legs (?P<legs0>\S+) (?P<legs1>\S+)'
                      r' phase (?P<phase>\S+) \| L (?P<L>.*?) \| R (?P<R>.*?) \| (?P<clips>.*)$')
PT = re.compile(r'(heel|ball) (\S+) (\S+) (\S+) g (\S+)')
WEIGHT = re.compile(r'^\d+\.\d+$')
AIR_SPEED = 2.0   # u/s of the drawn speed below which both feet up is not "airborne with the body moving" (MINE)
MIN_BODY = 2.0    # u/s of the drawn body speed below which a foot contact carries no slide reading (MINE)
MATCH_LO = 0.3    # a dragged contact travels within this fraction of the body speed (MINE)
MATCH_HI = 1.6    # above this it is a swing foot brushing the ground, not a slide (MINE)
# the lower moving clips' names (dump contacts: each character's code 0, non-idle records, by clip name). The heaviest one in
# a frame's clip list is the lower layer's clip for the per-clip table.
LOWER = {'strafe lower', 'stepback', 'stepback short', 'jog lower', 'run lower', 'walk lower', 'limp R lower', 'limp L lower',
         'limprunR', 'limprunL', 'ninjarun', 'crouch walk'}


def parse(path, want):
    rows = []
    for line in open(path, errors='replace'):
        text = line.strip()
        old = False
        m = LINE.match(text)
        if not m:
            m = LINE_OLD.match(text)
            old = True
        if not m:
            continue
        cid = int(m.group('id'))
        if want and cid != want:
            continue
        feet_txt = (m.group('L'), m.group('R'))
        tail = dict(tick=0, wv=0.0, flags=0, act='', ca=-1, cd=0, stop=0, lie=-1, rise=0, warp=0, steer=0, drop=0.0, lock=(0, 0), pose=0.0) if old else dict(
            tick=int(m.group('tick') or 0), wv=float(m.group('wv') or 0), flags=int(m.group('flags'), 0), act=m.group('act') or '',
            ca=int(m.group('ca') or -1), cd=int(m.group('cd') or 0), stop=int(m.group('stop')), lie=int(m.group('lie')),
            rise=int(m.group('rise')), warp=int(m.group('warp')), steer=int(m.group('steer')), drop=float(m.group('drop') or 0),
            lock=(int(m.group('lock0') or 0), int(m.group('lock1') or 0)), pose=float(m.group('pose') or 0))
        feet = []
        for side in feet_txt:
            pts = {}
            for p in PT.finditer(side):
                pts[p.group(1)] = tuple(float(p.group(k)) for k in (2, 3, 4, 5))
            feet.append(pts)
        r = dict(t=float(m.group('t')), dt=float(m.group('dt')), id=cid, wire=(float(m.group('wire0')), float(m.group('wire1'))),
                 legs=(float(m.group('legs0')), float(m.group('legs1'))), phase=float(m.group('phase')), feet=feet,
                 clips=m.group('clips').strip(), **tail)
        r['pairs'] = clip_pairs(r['clips'])
        r['lower'], r['lower_w'] = lower_clip(r['pairs'])
        r['body'] = math.hypot(*r['legs'])
        r['wire_sp'] = math.hypot(*r['wire'])
        rows.append(r)
    return rows


def clip_pairs(clips):
    """('strafe lower', 0.81), ...: the clip names carry spaces, the layer weights are the bare float tokens."""
    out, name = [], []
    for tok in clips.split():
        if WEIGHT.match(tok):
            out.append((' '.join(name), float(tok)))
            name = []
        else:
            name.append(tok)
    return out


def lower_clip(pairs):
    best, bw = '', 0.0
    for n, w in pairs:
        if n in LOWER and w > bw:
            best, bw = n, w
    return best, bw


def gap(p):
    return p[1] - p[3]


def lowest(pts):
    return min(pts, key=lambda k: gap(pts[k]))


def speed(a, b, dt):
    return math.hypot(a[0] - b[0], a[2] - b[2]) / dt if dt > 0 else 0.0


def contact_frame(rows, i, tol):
    """Per foot: the point in contact now and a frame ago, its slide travel, its speed, and whether it matches the body speed."""
    r = rows[i]
    slides = [0.0, 0.0]
    matched = [False, False]
    slide_gap = [0.0, 0.0]
    low_sp = [0.0, 0.0]
    pt_sp = [dict(heel=0.0, ball=0.0), dict(heel=0.0, ball=0.0)]
    up = [False, False]
    prev = rows[i - 1] if i else None
    for f in range(2):
        pts = r['feet'][f]
        low = lowest(pts)
        up[f] = gap(pts[low]) >= tol
        if prev is None:
            continue
        pp = prev['feet'][f]
        low_sp[f] = speed(pts[low], pp[low], r['dt'])	# the foot's representative point, contact or not
        for w in ('heel', 'ball'):
            pt_sp[f][w] = speed(pts[w], pp[w], r['dt'])
        if up[f]:
            continue
        both = [k for k in ('ball', 'heel') if gap(pts[k]) < tol and gap(pp[k]) < tol]
        if not both:
            continue
        k = both[0]
        slides[f] = math.hypot(pts[k][0] - pp[k][0], pts[k][2] - pp[k][2])
        slide_gap[f] = gap(pts[k])
        if r['dt'] > 0 and r['body'] >= MIN_BODY:
            ratio = slides[f] / r['dt'] / r['body']
            matched[f] = MATCH_LO <= ratio <= MATCH_HI
    r['slides'] = slides
    r['matched'] = matched
    r['slide_gap'] = slide_gap
    r['low_sp'] = low_sp
    r['pt_sp'] = pt_sp
    r['up'] = up
    return slides, matched


def analyse(rows, tol):
    """Stances and totals for one character's rows (one id, frames in order)."""
    stances = [[], []]
    open_st = [None, None]
    tot = [0.0, 0.0]
    mtot = [0.0, 0.0]
    mframes = [0, 0]
    contact = [0, 0]
    worst = 0.0
    worst_clips = ''
    gap_sum, gap_n = 0.0, 0
    air = 0
    for i, r in enumerate(rows):
        slides, matched = contact_frame(rows, i, tol)
        for f in range(2):
            pts = r['feet'][f]
            low = lowest(pts)
            gap_sum += gap(pts[low])
            gap_n += 1
            if r['up'][f]:
                if open_st[f] is not None:
                    stances[f].append(open_st[f])
                    open_st[f] = None
                continue
            contact[f] += 1
            if open_st[f] is None:
                open_st[f] = dict(t0=r['t'], t1=r['t'], slide=0.0, worst=0.0, gaps=[], pts=set(), legs0=r['legs'][0], clip=r['lower'])
            st = open_st[f]
            st['t1'] = r['t']
            st['slide'] += slides[f]
            if r['dt'] > 0:
                st['worst'] = max(st['worst'], slides[f] / r['dt'])
            st['gaps'].append(gap(pts[low]))
            st['pts'].add(low)
            st['legs1'] = r['legs'][0]
            tot[f] += slides[f]
            if matched[f]:
                mtot[f] += slides[f]
                mframes[f] += 1
        if all(r['up']) and math.hypot(*r['legs']) > AIR_SPEED:
            air += 1
        for f in range(2):
            if matched[f] and r['dt'] > 0 and slides[f] / r['dt'] > worst:
                worst = slides[f] / r['dt']
                worst_clips = r['clips']
    for f in range(2):
        if open_st[f] is not None:
            stances[f].append(open_st[f])
    return dict(stances=stances, tot=tot, mtot=mtot, mframes=mframes, contact=contact, worst=worst, worst_clips=worst_clips,
                gap=gap_sum / gap_n if gap_n else 0.0, air=air)


def clip_table(rows, tol, stats=None):
    """Per lower clip over every character: contacts, matched contacts, slide distance and the slide as a fraction of the body speed.
    stats accumulates across characters (the caller passes one dict for the whole log)."""
    if stats is None:
        stats = OrderedDict()
    for i in range(1, len(rows)):
        r = rows[i]
        contact_frame(rows, i, tol)
        clip = r['lower'] or '(no moving lower clip)'
        s = stats.setdefault(clip, dict(frames=0, contact=0, matched=0, slide=0.0, dtsum=0.0, body=0.0, ratio=0.0, gap=0.0, worst=(0.0, 0.0, 0.0, 0),
                                       held=0, held_sp=[], blocked=0, free=0, free_ratio=0.0, free_n=0, all_ratio=0.0, all_n=0, win_ratio=0.0, win_n=0))
        s['frames'] += 1
        for f in range(2):
            if r['up'][f]:
                continue
            s['contact'] += 1
            # the lock holds one sole point; the other swings with the foot's own roll, so the foot's speed is the slower of the two
            pa, pts = rows[i - 1]['feet'][f], r['feet'][f]
            v = min(math.hypot(pts[w][0] - pa[w][0], pts[w][2] - pa[w][2]) / r['dt'] for w in ('heel', 'ball')) if r['dt'] > 0 else 0.0
            # slide/body over every contact (a held foot is 0, a dragging foot ~1): the foot lock's target is measurable here, unlike the
            # banded ratio above, which drops a still foot out of both its numerator and its denominator
            if r['body'] >= MIN_BODY and r['dt'] > 0:
                s['all_ratio'] += v / r['body']
                s['all_n'] += 1
            # the same ratio over the contact-window frames only: the lock's state is nonzero exactly while the animation's window says
            # the point is down (held at its plant, or released past the reach and still down); held-still frames are included
            if r['lock'][f] and r['body'] >= MIN_BODY and r['dt'] > 0:
                s['win_ratio'] += v / r['body']
                s['win_n'] += 1
            # the foot lock's state on this contact: held at its plant, released past the leg's reach, or none
            if r['lock'][f] == 1:
                s['held'] += 1
                s['held_sp'].append(v)
            elif r['lock'][f] == 2:
                s['blocked'] += 1
            else:
                s['free'] += 1
                if r['body'] >= MIN_BODY:
                    s['free_ratio'] += v / r['body']
                    s['free_n'] += 1
            if not r['matched'][f]:
                continue
            sp = r['slides'][f] / r['dt'] if r['dt'] > 0 else 0.0
            s['matched'] += 1
            s['slide'] += r['slides'][f]
            s['dtsum'] += r['dt']
            s['body'] += r['body']
            s['gap'] += r['slide_gap'][f]
            s['ratio'] += sp / r['body'] if r['body'] > 0 else 0.0
            if sp > s['worst'][0]:
                s['worst'] = (sp, r['t'], r['body'], r['id'])
    return stats


def summary(rows, tol):
    groups = OrderedDict()
    for r in rows:
        groups.setdefault(r['id'], []).append(r)
    out = []
    for cid, rs in groups.items():
        a = analyse(rs, tol)
        out.append((sum(a['mtot']), cid, rs, a))
    out.sort(key=lambda x: (-x[0], x[1]))
    print(f'{len(rows)} frames, {len(groups)} characters, contact tolerance {tol}, matched body speed {MIN_BODY}+ u/s, '
          f'slide/body {MATCH_LO}..{MATCH_HI}')
    print(f'{"id":>8} {"frames":>7} {"stances L/R":>11} {"slide u":>8} {"dragged u":>9} {"drag frames":>11} {"worst u/s":>10} {"gap u":>7} {"air":>5}')
    for ms, cid, rs, a in out:
        s = a['stances']
        print(f'{cid:>8} {len(rs):>7} {len(s[0]):>5}/{len(s[1]):<5} {sum(a["tot"]):>8.2f} {ms:>9.2f} {sum(a["mframes"]):>5}/{sum(a["contact"]):<5}'
              f' {a["worst"]:>10.1f} {a["gap"]:>+7.3f} {a["air"]:>5}')
    stats = OrderedDict()
    for cid, rs in groups.items():
        clip_table(rs, tol, stats)
    print(f'\nper lower clip (all {len(groups)} characters):')
    print(f'{"clip":<22} {"frames":>7} {"contacts":>8} {"dragged":>8} {"drag u":>8} {"mean slide":>10} {"body u/s":>8} {"slide/body":>10} {"all/body":>8} {"win/body":>8} {"gap u":>7}  worst dragged frame')
    for clip, s in stats.items():
        m = s['matched']
        mean_sp = s['slide'] / s['dtsum'] if s['dtsum'] > 0 else 0.0
        mean_body = s['body'] / m if m else 0.0
        ratio = s['ratio'] / m if m else 0.0
        allratio = s['all_ratio'] / s['all_n'] if s['all_n'] else 0.0
        winratio = s['win_ratio'] / s['win_n'] if s['win_n'] else None	# None: the log has no lock state (the lock was off)
        mgap = s['gap'] / m if m else 0.0
        w = s['worst']
        wintxt = f'{winratio:8.2f}' if winratio is not None else f'{"-":>8}'
        print(f'{clip:<22} {s["frames"]:>7} {s["contact"]:>8} {m:>8} {s["slide"]:>8.2f} {mean_sp:>10.2f} {mean_body:>8.1f} {ratio:>10.2f} {allratio:>8.2f} {wintxt} {mgap:>+7.3f}'
              f'  {w[0]:6.1f} u/s body {w[2]:5.1f} t {w[1]:6.2f} id {w[3]}')
    if any(s['held'] or s['blocked'] for s in stats.values()):
        # the foot lock: on every contact, that foot's lock state. held still = the share whose slower sole point moves < 1 u/s (what
        # holding the foot means); free/body the mean slide of the contacts the lock does not cover (its window closed, its reach spent,
        # or the lock off). A mean held speed would be dragged up by the frames the character teleports or is corrected (MINE)
        print('\nper lower clip, foot lock (contacts split by that foot\'s lock):')
        print(f'{"clip":<22} {"contacts":>8} {"held":>6} {"held still":>10} {"held med u/s":>12} {"blocked":>7} {"free":>6} {"free/body":>9}')
        for clip, s in stats.items():
            sp = sorted(s['held_sp'])
            still = 100.0 * sum(1 for v in sp if v < 1.0) / len(sp) if sp else 0.0
            med = sp[len(sp) // 2] if sp else 0.0
            fr = s['free_ratio'] / s['free_n'] if s['free_n'] else 0.0
            print(f'{clip:<22} {s["contact"]:>8} {s["held"]:>6} {still:>9.1f}% {med:>12.2f} {s["blocked"]:>7} {s["free"]:>6} {fr:>9.2f}')
    return stats


HEADER = ('t,id,tick,wire_fwd,wire_lat,wv,legs_fwd,legs_lat,body_speed,phase,flags,act,lower,lower_w,'
          + ','.join(f'{s}_{w}_{c}' for s in 'LR' for w in ('heel', 'ball') for c in ('x', 'y', 'z', 'gap', 'speed'))
          + ',L_foot_speed,R_foot_speed,L_slide,L_slide_matched,R_slide,R_slide_matched,clips\n')


def csv_row(r):
    vals = [r['t'], r['id'], r['tick'], *r['wire'], r['wv'], *r['legs'], r['body'], r['phase'], r['flags']]
    out = ','.join(f'{v:.4f}' if isinstance(v, float) else str(v) for v in vals)
    out += ',"' + r['act'] + '","' + r['lower'] + '",' + f'{r["lower_w"]:.2f}'
    pt_sp = r.get('pt_sp', [dict(heel=0.0, ball=0.0), dict(heel=0.0, ball=0.0)])
    for f in range(2):
        for w in ('heel', 'ball'):
            p = r['feet'][f][w]
            out += f',{p[0]:.4f},{p[1]:.4f},{p[2]:.4f},{gap(p):+.4f},{pt_sp[f][w]:.2f}'
    slides = r.get('slides', [0.0, 0.0])
    matched = r.get('matched', [False, False])
    sp = r.get('low_sp', [0.0, 0.0])
    out += f',{sp[0]:.2f},{sp[1]:.2f},{slides[0]:.4f},{int(matched[0])},{slides[1]:.4f},{int(matched[1])},"{r["clips"]}"'
    return out + '\n'


def series(rows, tol, window):
    groups = OrderedDict()	# contact_frame compares consecutive frames: a window interleaves several characters, so per id
    for r in rows:
        groups.setdefault(r['id'], []).append(r)
    for rs in groups.values():
        analyse(rs, tol)
    t0, t1 = window
    print('\n     t      id    tick   wireF    wv  legsF  legsL  body  phase flags      act        lower        L gap/speed  slide | R gap/speed  slide | clips')
    for r in rows:
        if not t0 <= r['t'] <= t1:
            continue
        out = (f'{r["t"]:7.3f} {r["id"]:>7} {r["tick"]:>7} {r["wire"][0]:6.1f} {r["wv"]:6.1f} {r["legs"][0]:6.1f} {r["legs"][1]:6.1f} {r["body"]:5.1f} {r["phase"]:6.3f}'
               f' 0x{r["flags"]:<4x} {r["act"][:10]:>10} {r["lower"][:12]:>12}')
        for f in range(2):
            pts = r['feet'][f]
            sp = r['low_sp'][f]
            m = '*' if r['matched'][f] else ' '
            out += f' | {gap(pts[lowest(pts)]):+6.2f} {sp:6.1f}{m} {r["slides"][f]:5.2f}'
        print(out + ' | ' + r['clips'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--id', type=int, default=0)
    ap.add_argument('--tol', type=float, default=0.5)
    ap.add_argument('--series', type=float, nargs=2)
    ap.add_argument('--csv')
    ap.add_argument('--csvdir', help='write one per-frame CSV per character (time, foot height, foot speed, body speed, the lower clip)')
    a = ap.parse_args()
    rows = parse(a.log, a.id)
    if not rows:
        sys.exit('no footlog lines')
    if a.id:
        analyse(rows, a.tol)
        if a.series:
            series(rows, a.tol, a.series)
        if a.csv:
            with open(a.csv, 'w') as f:
                f.write(HEADER)
                for r in rows:
                    f.write(csv_row(r))
    else:
        summary(rows, a.tol)
        if a.series:
            series(rows, a.tol, a.series)
    if a.csvdir:
        os.makedirs(a.csvdir, exist_ok=True)
        groups = OrderedDict()
        for r in rows:
            groups.setdefault(r['id'], []).append(r)
        for cid, rs in groups.items():
            analyse(rs, a.tol)
            with open(os.path.join(a.csvdir, f'{cid}.csv'), 'w') as f:
                f.write(HEADER)
                for r in rs:
                    f.write(csv_row(r))
        print(f'wrote {len(groups)} CSVs to {a.csvdir}')


if __name__ == '__main__':
    main()
