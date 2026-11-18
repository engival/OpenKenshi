#!/usr/bin/env python3
# peaks.py WAV LOG: per world post in LOG ("audio: object N event ... voices" + "world sound ... at T s"), the L/R peak of the KAUDIO_WAV render
# over the window [T, T + 1.2 s] of each voice (48 kHz stereo float32). Prints one line per voice and the max per distance.
import re, struct, sys, array, collections

wav, log = sys.argv[1], sys.argv[2]
d = open(wav, 'rb').read()
pos = 12
while pos + 8 <= len(d):
    cid, size = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
    if cid == b'data':
        body = d[pos + 8:] if size == 0xffffffff else d[pos + 8:pos + 8 + size]
        break
    pos += 8 + size
a = array.array('f'); a.frombytes(body[:len(body) // 4 * 4])
L, R = a[0::2], a[1::2]
rate = 48000
per = collections.defaultdict(list)
for line in open(log):
    m = re.search(r'world sound (\d+): voice (\d+) at ([\d.]+) s, ([-\d.]+) dB, pan ([-+\d.]+), rate [\d.]+, ([\d.]+) u', line)
    if not m: continue
    t = float(m.group(3)); i0, i1 = int(t * rate), int((t + 1.2) * rate)
    pl, pr = max(map(abs, L[i0:i1]), default=0), max(map(abs, R[i0:i1]), default=0)
    dist = float(m.group(6))
    per[round(dist)].append((pl, pr))
    print(f'dist {dist:6.1f} u  at {t:6.2f} s  logged {m.group(4):>6} dB pan {m.group(5)}  peak L {pl:.4f} R {pr:.4f}')
for dist, v in sorted(per.items()):
    print(f'== {dist} u: {len(v)} voices, max peak L {max(x[0] for x in v):.4f} R {max(x[1] for x in v):.4f}, mean max(L,R) {sum(max(x) for x in v) / len(v):.4f}')
