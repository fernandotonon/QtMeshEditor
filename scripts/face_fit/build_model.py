import numpy as np, glob, os
from PIL import Image
import lmk
C=np.load("samples/coefs.npy"); L=[]; ok=[]; logits=[]
for i in range(len(C)):
    p,l=lmk.detect(Image.open(f"renders/s{i:03d}.png").convert("RGB"))
    ok.append(p is not None and l>2); logits.append(l); L.append(p if p is not None else np.zeros((468,3)))
L=np.array(L); ok=np.array(ok)
print("detected",ok.sum(),"of",len(C),"logit min/med",min(logits),np.median(logits))
np.savez("model_raw.npz",C=C,L=L,ok=ok)
