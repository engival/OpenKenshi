#!/usr/bin/env python3
"""client.log of a pass-1 run (records_drift_move1.txt, KCLIENT_DRIFT_LOG=1) -> a TRACK file for fake_agent.py: `dt x y z`, dt = game seconds since the quick-save
instant, as the client's drift view counts them (the drawn tick's sim time minus the align offset); y is the fixture's constant.
usage: drift_track.py CLIENT_LOG ID [Y] > track.txt   (kenshi-cym.40.2.6)"""
import re, sys
y = sys.argv[3] if len(sys.argv) > 3 else "142.790"
last = None
print("# dt x y z (game s since the quick-save instant)")
for ln in open(sys.argv[1], errors="replace"):
    m = re.match(r"drift-trace: id %s t (\S+) x (\S+) z (\S+)" % sys.argv[2], ln)
    if not m: continue
    t, x, z = float(m.group(1)), float(m.group(2)), float(m.group(3))
    if last is not None and t <= last[0]: continue
    if last is not None and t - last[0] < 1.0: continue   # about one sample per game second: the agent's orig rate in game time is much higher than the real 1 Hz, interpolation error stays small
    print("%.4f %.3f %s %.3f" % (t, x, y, z)); last = (t, x, z)
