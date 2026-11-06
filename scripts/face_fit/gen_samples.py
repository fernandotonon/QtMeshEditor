import numpy as np, subprocess, os, sys
S=os.path.dirname(os.path.abspath(__file__))+"/samples"
N=int(sys.argv[1]) if len(sys.argv)>1 else 160
rng=np.random.default_rng(1234)
C=np.clip(rng.normal(0,1.0,(N,45)),-2.5,2.5); C[0]=0
np.save(S+"/coefs.npy",C)
for i in range(N):
    out=f"{S}/s{i:03d}.glb"
    if os.path.exists(out): continue
    arg=",".join(f"{j}={C[i,j]:.4f}" for j in range(45))
    subprocess.run(["/Users/fernandotonon/fable/QtMeshEditor/build_local/bin/qtmesh","character","--body",arg,"--bust","-o",out],capture_output=True,env={**os.environ,"QTMESH_NO_TELEMETRY":"1"})
open(S+"/gen.done","w").write("ok")
