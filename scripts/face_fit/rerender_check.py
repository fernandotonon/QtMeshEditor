import numpy as np, glob, re, subprocess, os, sys
from PIL import Image
sys.argv=[sys.argv[0]]
import lmk
from evaluate2 import solve_k, norm, nme_to
Q="/Users/fernandotonon/fable/QtMeshEditor/build_local/bin/qtmesh"
items=[]; lst=[]
for f in sorted(glob.glob("*_landmarks.f32")):
    b=f.replace("_landmarks.f32",""); P=np.fromfile(f,np.float32).reshape(-1,3)[:468].astype(np.float64)
    m=re.search(r"Photo shape coefficients: ([^\n]+)",open(b+".log").read())
    if not m: continue
    c0=np.array([float(x) for x in m.group(1).split(',')]); c1=solve_k(P,c0,0.05,12)
    for tag,c in (("sam",c0),("fit",c1)):
        out=f"chk/{b}_{tag}.glb"
        if not os.path.exists(out):
            subprocess.run([Q,"character","--body",",".join(f"{j}={c[j]:.4f}" for j in range(45)),"--bust","-o",out],capture_output=True,env={**os.environ,"QTMESH_NO_TELEMETRY":"1"})
        lst.append(f"{out} chk/{b}_{tag}.png")
    items.append((b,P,c0,c1))
open("chk/list.txt","w").write("\n".join(lst))
subprocess.run(["blender","-b","-P","render_batch.py","--","chk/list.txt"],capture_output=True)
res=[]
for b,P,c0,c1 in items:
    r=[]
    for tag in ("sam","fit"):
        lm,l=lmk.detect(Image.open(f"chk/{b}_{tag}.png").convert("RGB"))
        r.append(nme_to(norm(lm),P)[0] if lm is not None else np.nan)
    res.append(r); print(b,"SAM %.4f  fit %.4f"%tuple(r),"| max|dc| %.2f"%np.abs(c1-c0).max())
a=np.array(res); print("MEAN SAM %.4f fit %.4f ; improved on %d/%d photos"%(np.nanmean(a[:,0]),np.nanmean(a[:,1]),(a[:,1]<a[:,0]).sum(),len(a)))
