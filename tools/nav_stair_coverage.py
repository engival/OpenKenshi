#!/usr/bin/env python3
# Hub bar stair tread coverage (k16.7.139): share of the stair hull top face (col_46 of ksim_navcoll_export hubbar) that nav polygons cover.
# usage: nav_stair_coverage.py EXPORT_DIR [YTOL=8] [HULL_OBJ] [land]   (land = area 0 only; HULL_OBJ defaults to EXPORT_DIR/col_46.obj;
# for the original export pass our col_46.obj: same hull, the original names it col_58). Game-derived output: never commit it.
import sys, numpy as np
d = sys.argv[1]; hull = sys.argv[3] if len(sys.argv)>3 else d+'/col_46.obj'; ytol = float(sys.argv[2]) if len(sys.argv) > 2 else 8; only0 = len(sys.argv)>4 and sys.argv[4]=='land'
V=[]
for l in open(hull):
    if l.startswith('v '): V.append([float(x) for x in l.split()[1:]])
V=np.array(V)
top=V[[3,2,6,7]]            # v4 v3 v7 v8 (1-based 4,3,7,8)
o=top[0]; L=top[1]-top[0]; W=top[3]-top[0]   # long edge, width edge
# plane y = o.y + a*s + b*t in (s,t) of the parallelogram coords
M=np.array([[L[0],W[0]],[L[2],W[2]]]); Minv=np.linalg.inv(M)
def st(x,z): return Minv @ np.array([x-o[0],z-o[2]])
Lh=np.hypot(L[0],L[2]); Wh=np.hypot(W[0],W[2])
areas={}
try:
    for l in open(d+'/nav_face_data.tsv'):
        p=l.split()
        if l[0]!='#' and len(p)>=3 and p[1]=='area': areas[int(p[0])]=int(p[2])
except FileNotFoundError: pass
polys=[]; ids=[]
for l in open(d+'/nav_faces.tsv'):
    if l[0]=='#' or not l.split()[0].lstrip('-').isdigit(): continue
    p=l.split(); n=int(p[1]); v=np.array([float(x) for x in p[2:2+3*n]]).reshape(n,3)
    polys.append(v); ids.append(int(p[0]))
def inpoly(v,x,z):
    # convex xz test, either winding
    s=None
    for i in range(len(v)):
        a=v[i]; b=v[(i+1)%len(v)]
        c=(b[0]-a[0])*(z-a[2])-(b[2]-a[2])*(x-a[0])
        if abs(c)<1e-9: continue
        if s is None: s=c>0
        elif s!=(c>0): return False
    return True
def polyY(v,x,z):
    # plane through the polygon's centroid with the Newell normal (the first 3 vertices can be near-collinear: k16.7.152)
    a=v.mean(0); w=np.roll(v,-1,0)
    n=np.array([((v[:,1]-w[:,1])*(v[:,2]+w[:,2])).sum(),((v[:,2]-w[:,2])*(v[:,0]+w[:,0])).sum(),((v[:,0]-w[:,0])*(v[:,1]+w[:,1])).sum()])
    if abs(n[1])<1e-9: return a[1]
    return a[1]-(n[0]*(x-a[0])+n[2]*(z-a[2]))/n[1]
# candidates: polys whose xz bbox overlaps and mid y near the stair
step=0.5
ns=int(1/(step/Lh)); nt=int(1/(step/Wh))
cov=np.zeros((ns,nt),bool); used=set()
for i in range(ns):
    for j in range(nt):
        s=(i+.5)/ns; t=(j+.5)/nt
        x=o[0]+L[0]*s+W[0]*t; z=o[2]+L[2]*s+W[2]*t
        y=o[1]+(top[1][1]-o[1])*s+(top[3][1]-o[1])*t
        for k,v in enumerate(polys):
            if v[:,0].min()>x or v[:,0].max()<x or v[:,2].min()>z or v[:,2].max()<z: continue
            if (not only0 or areas.get(ids[k],0)==0) and inpoly(v,x,z) and abs(polyY(v,x,z)-y)<=ytol:
                cov[i,j]=True; used.add(k); break
print(d,'top face %.1f long x %.1f wide, area %.0f; coverage %.1f%%'%(Lh,Wh,Lh*Wh,100*cov.mean()))
w=cov.sum(1)*step   # covered width per slice
rows=np.where(cov.any(1))[0]
if len(rows):
    sl=w[rows]
    print(' slices with cover: %.0f%% of length; covered width per slice mean %.1f median %.1f max %.1f of %.1f'%(100*len(rows)/ns,sl.mean(),np.median(sl),sl.max(),Wh))
    cols=cov.sum(0)/ns
    print(' coverage by width position (t=0..1, 10 bins):',' '.join('%.0f'%(100*cols[k*(nt//10):(k+1)*(nt//10)].mean()) for k in range(10)))
    print(' polys used:',len(used))
    for k in sorted(used,key=lambda k:polys[k][:,1].mean()):
        v=polys[k]; print('   id %d area %s n=%d y %.1f..%.1f'%(ids[k],areas.get(ids[k],'?'),len(v),v[:,1].min(),v[:,1].max()))
    print(' t=0 edge is at xz',(o[0],o[2]),'; t=1 edge toward',(o[0]+W[0],o[2]+W[2]))
