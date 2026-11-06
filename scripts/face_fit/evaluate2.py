import numpy as np, glob, re, lmk
from evaluate import norm, model, nme_to, W
infl=np.linalg.norm(W[:45].reshape(45,468,3)[:,:,:2],axis=(1,2)); order=np.argsort(-infl)
def solve_k(photo,c0,lam,K,box=3.0):
    modes=order[:K]; c=c0.copy()
    for _ in range(6):
        m=model(c); R,s,t=lmk.umeyama(norm(photo),m); A=((s*(R@norm(photo).T)).T+t).reshape(-1)
        w=np.tile([1.0,1.0,0.25],468); Wl=W[:45].T[:,modes]*w[:,None]
        y=(A-W[45]-W[:45].T[:,[i for i in range(45) if i not in modes]]@c0[[i for i in range(45) if i not in modes]])*w
        cm=np.linalg.solve(Wl.T@Wl+lam*np.eye(K),Wl.T@y+lam*c0[modes]); c=c0.copy(); c[modes]=np.clip(cm,-box,box)
    return c
if __name__=="__main__":
    rows=[]
    for f in sorted(glob.glob("*_landmarks.f32")):
        b=f.replace("_landmarks.f32",""); P=np.fromfile(f,np.float32).reshape(-1,3)[:468].astype(np.float64)
        m=re.search(r"Photo shape coefficients: ([^\n]+)",open(b+".log").read())
        if not m: continue
        c0=np.array([float(x) for x in m.group(1).split(',')]); r=[nme_to(model(c0),P)[0]]
        for K in (6,12,20):
            r.append(nme_to(model(solve_k(P,c0,0.05,K)),P)[0])
        rows.append(r)
    a=np.array(rows); print("n=",len(a)," SAM  K=6  K=12  K=20 :",np.round(a.mean(0),4)); print("fraction of photos improved vs SAM:",[(a[:,k]<a[:,0]).mean().round(2) for k in (1,2,3)])
