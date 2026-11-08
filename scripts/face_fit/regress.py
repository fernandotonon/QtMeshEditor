import numpy as np, lmk
d=np.load("model_raw.npz"); C=d["C"]; L=d["L"]
def norm(P):
    c=P.mean(0); Q=P-c; s=np.sqrt((Q[:,:2]**2).sum(1).mean()); return Q/s
Ln=np.array([norm(p) for p in L]).reshape(len(L),-1)
idx=np.arange(len(C)); rng=np.random.default_rng(0); rng.shuffle(idx); te=idx[:30]; tr=idx[30:]
X=np.hstack([C,np.ones((len(C),1))])
def fit(lam,tr):
    A=X[tr]; R=lam*np.eye(46); R[-1,-1]=0
    return np.linalg.solve(A.T@A+R, A.T@Ln[tr])
iod=np.mean([np.linalg.norm(norm(p)[33,:2]-norm(p)[263,:2]) for p in L])
base=np.linalg.norm((Ln[te]-Ln[tr].mean(0)).reshape(len(te),468,3)[:,:,:2],axis=2).mean()/iod
for lam in (0.01,1,10,50):
    W=fit(lam,tr); pred=X[te]@W
    err=np.linalg.norm((pred-Ln[te]).reshape(len(te),468,3)[:,:,:2],axis=2).mean()/iod
    print(f"lam {lam}: held-out mean landmark err / IOD = {err:.4f}   (predicting the mean face: {base:.4f})")
W=fit(1,np.arange(len(C))); np.savez("lmk_model.npz",W=W,iod=iod)
# per-mode influence
infl=np.linalg.norm(W[:45].reshape(45,468,3)[:,:,:2],axis=(1,2)); print("mode influence (top 12):",np.argsort(-infl)[:12], np.round(np.sort(-infl)[:12]*-1,3))
