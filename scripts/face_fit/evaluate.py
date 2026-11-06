import numpy as np, glob, re, os, lmk
M=np.load("lmk_model.npz"); W=M["W"]
def norm(P):
    c=P.mean(0); Q=P-c; s=np.sqrt((Q[:,:2]**2).sum(1).mean()); return Q/s
def model(c): return (np.append(c,1.0)@W).reshape(468,3)
def nme_to(model_lm,photo_lm):
    R,s,t=lmk.umeyama(norm(photo_lm),model_lm); A=(s*(R@norm(photo_lm).T)).T+t
    iod=np.linalg.norm(model_lm[33,:2]-model_lm[263,:2])
    e=np.linalg.norm((A-model_lm)[:,:2],axis=1)/iod
    return e.mean(), A
def solve(photo,c0,lam,box=4.0):
    c=c0.copy()
    for _ in range(6):
        m=model(c); R,s,t=lmk.umeyama(norm(photo),m); A=((s*(R@norm(photo).T)).T+t).reshape(-1)
        Wl=W[:45].T; y=A-W[45]       # (1404,) = Wl c
        # weights: x,y full, z lower
        w=np.tile([1.0,1.0,0.25],468)
        Aw=Wl*w[:,None]; yw=y*w
        c=np.linalg.solve(Aw.T@Aw+lam*np.eye(45), Aw.T@yw+lam*c0)
        c=np.clip(c,-box,box)
    return c
rows=[]
for f in sorted(glob.glob("*_landmarks.f32")):
    b=f.replace("_landmarks.f32",""); P=np.fromfile(f,np.float32).reshape(-1,3)[:468].astype(np.float64)
    log=open(b+".log").read(); m=re.search(r"Photo shape coefficients: ([^\n]+)",log)
    if not m: continue
    c_sam=np.array([float(x.replace(',','.')) if False else float(x) for x in m.group(1).split(',')])
    e_mean,_=nme_to(model(np.zeros(45)),P); e_sam,_=nme_to(model(c_sam),P)
    out=[b,e_mean,e_sam]
    for lam in (0.0,0.05,0.5):
        c=solve(P,c_sam,lam); e,_=nme_to(model(c),P); out.append(e); out.append(float(np.abs(c).max()))
    rows.append(out)
print("photo   mean-face  SAM    | fit lam=0 (maxcoef)  | lam=.05 (maxcoef) | lam=.5 (maxcoef)")
for r in rows: print(f"{r[0]:6s}  {r[1]:.4f}   {r[2]:.4f} | {r[3]:.4f} ({r[4]:.1f})      | {r[5]:.4f} ({r[6]:.1f})     | {r[7]:.4f} ({r[8]:.1f})")
if rows:
    a=np.array([r[1:] for r in rows]); print("MEAN    ",*[f"{x:.4f}" for x in a.mean(0)[[0,1,2,4,6]]])
# scale reference: different people
Ls=[norm(np.fromfile(f,np.float32).reshape(-1,3)[:468].astype(np.float64)) for f in sorted(glob.glob("*_landmarks.f32"))]
pair=[]
for i in range(len(Ls)):
    for j in range(len(Ls)):
        if i!=j: pair.append(nme_to(Ls[i],Ls[j]*1.0)[0])
if pair: print("different-people NME (photo vs photo), mean:",np.mean(pair))
