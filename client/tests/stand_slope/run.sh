#!/bin/bash
# The standing ray on an open slope (kenshi-k16.2.382.1, parent .382): bar_lure_hostile, the slope outside the Hub bar (--pos -50592,-1211). The
# wire y outdoors is the original's coarse navmesh, up to ~6 u under the curved terrain; the stand ray's window [-4.5, +1.5] then missed and the body
# was drawn at the wire y, feet off the ground. Reads KYLOG's per character per frame line (YLOG id tick x z sim terrain drawn ray-result) over the
# last third of the run and asserts: every drawn character with a ray result and no shape over it (ray result 1 from the heightmap, or the open-ground
# result) stands within MAXOFF (0.3 u) of the terrain, and the open-ground "no collision" misses (drawn == wire y, wire != terrain) are ~0.
# Usage: run.sh OUT   (OUT: output dir). PORT, BUILD as floor_threshold/run.sh. Prints the miss distribution too (the report's numbers).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../need_data.sh"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tree=$(cd "$here/../../.." && pwd)
. "$tree/client/tools/server_bin.sh"
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
. "$tree/client/tools/ports.sh"
build=${BUILD:-$tree/client/build}
pick_server "$tree" "$build/cpp/OpenKenshi-server"
server=$SERVER
frames=${FRAMES:-1500}
printf '10 set animate 1\n%s quit\n' "$frames" > "$out/script.txt"
start_server_on_free_port "$out/server.log" "$server" ${SRV_ARGS[@]+"${SRV_ARGS[@]}"} --listen 127.0.0.1:@PORT@ --scenario-file "$tree/scenarios/bar_lure_hostile.kscen" --debug-view
pid=$SERVER_PID; port=$SERVER_PORT
(cd "$tree" && KYLOG=1 timeout 580 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --pos -50592,-1211 --script "$out/script.txt" > "$out/client.log" 2> "$out/client.err")
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
grep -h "^YLOG" "$out/client.err" "$out/client.log" > "$out/ylog.txt"
python3 -I - "$out/ylog.txt" "${MAXOFF:-0.3}" <<'P'
import sys
maxoff=float(sys.argv[2])
rows=[]
for l in open(sys.argv[1]):
    r=l.split()
    if len(r)>=9: rows.append(dict(id=r[1],tick=float(r[2]),sim=float(r[5]),ter=float(r[6]),drawn=float(r[7]),res=int(r[8]),bil=float(r[9]) if len(r)>9 else 0.0))
if not rows: print("FAIL stand_slope: no YLOG lines (dev build with KCLIENT_DEV_TOOLS?)"); sys.exit(1)
ray=[r for r in rows if r['res']!=-9]
# miss = drawn at the wire y (no ray hit): res 0 no collision, -1 unloaded
miss=[r for r in ray if r['res']<=0]
print("rays %d, misses %d (res0 %d, unloaded %d)"%(len(ray),len(miss),sum(r['res']==0 for r in miss),sum(r['res']==-1 for r in miss)))
d=sorted(r['ter']-r['sim'] for r in miss)
if d: print("terrain-wire over misses: min %.2f median %.2f max %.2f; above +1.5: %d, below -4.5: %d"%(d[0],d[len(d)//2],d[-1],sum(x>1.5 for x in d),sum(x<-4.5 for x in d)))
# the check: open ground is drawn on the terrain, so no miss has the terrain above the wire y (the coarse navmesh under the curved ground, the 1888 of
# 1985 misses before the fix); the rest are a character over a building footprint without collision in the window (terrain far below) and stay under 1 %
over=[r for r in miss if r['ter']-r['sim']>1.5]
rate=100.0*len(miss)/max(1,len(ray))
# the drawn terrain triangle (ter) against the bilinear Heightmap::height (bil), and the root's drawn y against the triangle for every ray-hit frame
tb=sorted(r['ter']-r['bil'] for r in ray if r['bil'])
if tb: print("triangle - bilinear terrain: min %.2f median %.2f max %.2f"%(tb[0],tb[len(tb)//2],tb[-1]))
tw=sorted(r['ter']-r['sim'] for r in ray)
print("triangle - wire y (all ray frames): min %.2f median %.2f max %.2f"%(tw[0],tw[len(tw)//2],tw[-1]))
print("misses with the terrain above the wire y by >1.5: %d; miss rate %.2f %%"%(len(over),rate))
if over or rate>1.0: print("FAIL stand_slope, e.g. %s"%(over or miss)[0]); sys.exit(1)
print("PASS stand_slope")
P
