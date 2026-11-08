import numpy as np, onnxruntime as ort, os
from PIL import Image
MODEL=os.path.expanduser("~/Library/Application Support/QtMeshEditor/QtMeshEditor/ai_models/facerig/face_landmarks.onnx")
_sess=None
def sess():
    global _sess
    if _sess is None: _sess=ort.InferenceSession(MODEL,providers=['CPUExecutionProvider'])
    return _sess
def run_pass(img,ox,oy,side):
    crop=img.crop((ox,oy,ox+side,oy+side)).convert("RGB").resize((256,256),Image.BILINEAR)
    x=(np.asarray(crop,np.float32)/255.0)[None]
    s=sess(); outs=s.run(None,{s.get_inputs()[0].name:x})
    lm=None; logit=-1e9
    for o in outs:
        if o.size>=1404: lm=o.reshape(-1,3)
        elif o.size==1 and logit==-1e9: logit=float(o.reshape(-1)[0])
    if lm is None: return None,-1e9
    sc=side/256.0
    return np.stack([lm[:,0]*sc+ox,lm[:,1]*sc+oy,lm[:,2]*sc],1),logit
def detect(img):
    W,H=img.size; side=min(W,H); ox=(W-side)//2; oy=(H-side)//2
    best,bl=run_pass(img,ox,oy,side)
    if best is None: return None,-1e9
    mn=best[:,:2].min(0); mx=best[:,:2].max(0); c=(mn+mx)/2; s1=int(max(mx-mn)*1.6)
    if s1>=32:
        s1=min(s1,W,H); ox1=int(np.clip(c[0]-s1/2,0,W-s1)); oy1=int(np.clip(c[1]-s1/2,0,H-s1))
        p2,l2=run_pass(img,ox1,oy1,s1)
        if p2 is not None and l2>=bl: return p2[:468],l2
    return best[:468],bl
def umeyama(src,dst,with_scale=True):
    """similarity (R,s,t) mapping src->dst (rows are points), proper rotation"""
    mu_s=src.mean(0); mu_d=dst.mean(0); S=src-mu_s; D=dst-mu_d
    U,sig,Vt=np.linalg.svd(D.T@S/len(src)); d=np.sign(np.linalg.det(U@Vt)); E=np.diag([1,1,d])
    R=U@E@Vt; var=(S**2).sum()/len(src); s=(sig*np.diag(E)).sum()/var if with_scale else 1.0
    t=mu_d-s*R@mu_s; return R,s,t
IOD=(33,263)
GROUPS={"oval":[10,338,297,332,284,251,389,356,454,323,361,288,397,365,379,378,400,377,152,148,176,149,150,136,172,58,132,93,234,127,162,21,54,103,67,109],
 "eyes":[33,7,163,144,145,153,154,155,133,173,157,158,159,160,161,246,263,249,390,373,374,380,381,382,362,398,384,385,386,387,388,466],
 "mouth":[61,146,91,181,84,17,314,405,321,375,291,409,270,269,267,0,37,39,40,185],
 "nose":[1,2,98,327,168,6,197,195,5,4,45,275,220,440,19,94,141,370,64,294]}
def nme(model_lm,photo_lm):
    """photo landmarks aligned to the model's by a 3D similarity; per-group mean xy error / inter-ocular distance"""
    R,s,t=umeyama(photo_lm,model_lm); A=(s*(R@photo_lm.T)).T+t
    iod=np.linalg.norm(model_lm[IOD[0],:2]-model_lm[IOD[1],:2])
    e=np.linalg.norm((A-model_lm)[:,:2],axis=1)/iod
    out={"all":float(e.mean())}
    for k,v in GROUPS.items(): out[k]=float(e[v].mean())
    return out
