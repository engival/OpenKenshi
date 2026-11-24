# nav_inset.py (kenshi-k16.7.138): navmesh boundary-edge inset vs collision shapes. Dev script; inputs are the kenshi-rwh.265 export layout (col_shapes.tsv + col_*.obj) and edge lists (knav_hkt_dump edges, or a live nav_edges.tsv).
# usage: nav_inset.py SHAPEDIR EDGEFILE shipped|live [INSTANCE]  -- XZ distance from boundary-edge samples to collision in the body band [y+3,y+17]; 0 = on or inside the footprint
import sys, numpy as np
# usage: inset.py SHAPEDIR EDGEFILE KIND(shipped|live|ours) [INST]
sd, ef, kind = sys.argv[1:4]
def quat(w,x,y,z):
    return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],[2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],[2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
BOXT=[(0,1,3),(0,3,2),(4,6,7),(4,7,5),(0,4,5),(0,5,1),(2,3,7),(2,7,6),(0,2,6),(0,6,4),(1,5,7),(1,7,3)]
WALL={9,10,27}; FURN={19,20,21,22,23,28}
shapes=[]
for ln in open(sd+'/col_shapes.tsv').read().split('\n')[1:]:
    f=ln.split('\t')
    if len(f)<12: continue
    sid,owner,k,g=f[0],f[1],f[2],int(f[3])
    if g in (0,2,13,5,15,14): continue
    if k=='box':
        h=np.array([float(v) for v in f[11].split('=')[1].split(',')]);p=np.array([float(v) for v in f[4:7]]);R=quat(*[float(v) for v in f[7:11]])
        c=np.array([[sx*h[0],sy*h[1],sz*h[2]] for sx in(-1,1) for sy in(-1,1) for sz in(-1,1)])
        V=c@R.T+p; T=V[BOXT]
    elif k in('convex','trimesh'):
        fn=f[11].split('obj=')[1]
        vs=[];tr=[]
        for l in open(f'{sd}/{fn}'):
            t=l.split()
            if t and t[0]=='v': vs.append([float(a) for a in t[1:4]])
            elif t and t[0]=='f': tr.append([int(a.split('/')[0])-1 for a in t[1:4]])
        if not tr: continue
        T=np.array(vs)[np.array(tr)]
    else: continue
    shapes.append((g,('wall' if g in WALL else 'furn'),T))
# edges
E=[]
if kind=='shipped':
    for ln in open(ef):
        t=ln.split('\t'); a=[float(v) for v in t[1].split()];b=[float(v) for v in t[2].split()];E.append((a,b))
else:
    inst=int(sys.argv[4])
    for ln in open(ef).read().split('\n')[1:]:
        t=ln.split('\t')
        if len(t)<11: continue
        if int(t[0])>>22!=inst or int(t[2])!=-1 or (int(t[3])&0x40): continue
        E.append(([float(v) for v in t[5:8]],[float(v) for v in t[8:11]]))
pts=[];
for a,b in E:
    a=np.array(a);b=np.array(b);n=max(1,int(np.linalg.norm(b-a)/1.0))
    for i in range(n): pts.append(a+(b-a)*(i+.5)/n)
P=np.array(pts)
def d2tri(P3,T):
    P=P3[:,[0,2]]
    # 2D (xz) distance from points to triangles T (n,3,3); returns (npts,ntri)
    A=T[:,0][:,[0,2]];B=T[:,1][:,[0,2]];C=T[:,2][:,[0,2]]
    def seg(A,B):
        AB=B-A; L=(AB**2).sum(1)+1e-12
        t=np.clip(((P[:,None,:]-A)*AB).sum(2)/L,0,1)
        return np.linalg.norm(P[:,None,:]-(A+t[...,None]*AB),axis=2)
    d=np.minimum(np.minimum(seg(A,B),seg(B,C)),seg(C,A))
    def sg(p,a,b): return (p[...,0]-b[...,0])*(a[...,1]-b[...,1])-(a[...,0]-b[...,0])*(p[...,1]-b[...,1])
    Pp=P[:,None,:]
    s1=sg(Pp,A,B);s2=sg(Pp,B,C);s3=sg(Pp,C,A)
    neg=(s1<0)|(s2<0)|(s3<0);pos=(s1>0)|(s2>0)|(s3>0)
    inside=~(neg&pos)
    return np.where(inside,0.0,d)
res={'wall':np.full(len(P),1e9),'furn':np.full(len(P),1e9)}
for g,cl,T in shapes:
    ymin=T[:,:,1].min(1);ymax=T[:,:,1].max(1)
    # triangle in band [y+3,y+17] of each point
    d=d2tri(P,T)
    ok=(ymax[None,:]>P[:,1:2]+3)&(ymin[None,:]<P[:,1:2]+17)
    d=np.where(ok,d,1e9).min(1)
    res[cl]=np.minimum(res[cl],d)
print(f'edges {len(E)} sample pts {len(P)}')
def st(x):
    x=x[x<1e8]
    if len(x)==0: return 'none'
    return 'n=%d p10=%.2f p25=%.2f med=%.2f p75=%.2f p90=%.2f frac<0.5=%.2f'%(len(x),*np.percentile(x,[10,25,50,75,90]),(x<0.5).mean())
for c in('wall','furn'): print(c,st(res[c]))
allm=np.minimum(res['wall'],res['furn']); print('any',st(allm))
# only points near something (<3u): distribution of nearest
for c in('wall','furn'):
    x=res[c][res[c]<3]; print(c,'within3u',st(x))
