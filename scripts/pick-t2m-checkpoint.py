#!/usr/bin/env python3
"""Score archived t2m checkpoints on the actions a USER validated (#837).

ONE-TIME OFFLINE dev tool — NOT shipped.

Picking "the last epoch" is wrong: walk peaked and then regressed in earlier
runs, and the metrics disagree with each other (ep75 has the best walk refDist
0.269 while ep30 has the best facing across the validated actions). So score
every archived checkpoint on BOTH, restricted to the actions a human actually
confirmed are usable, and choose deliberately.

USER_GOOD below is real user feedback on v7.8 ep60, not a guess. Keep it in sync
when new feedback arrives — and note the caveat recorded in EVAL_NOTES: the
shoulder-line facing metric measures FACING only and does NOT capture limb
articulation, so it ranks actions unreliably. It informs the choice between
checkpoints of the SAME model; it does not replace looking at renders.
"""
import argparse
import glob
import importlib.util
import json
import os
import tempfile

import numpy as np
import onnxruntime as ort
import torch

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rd = _load("rd", "eval-t2m-refdist.py")
p6 = _load("p6", "prep-t2m-v6.py")
D=rd.prep5.D_CANON; PAR=p6.PAR
def pos(role,w):
    T=len(w); p=np.zeros((T,3),np.float32); r=role
    while PAR[r]>=0:
        p=p+p6.qrot(w[:,PAR[r]],np.broadcast_to(D[r],(T,3))); r=PAR[r]
    return p
def lat_err(q):
    ls=p6.qrot(q[:,11],np.broadcast_to(D[11],(len(q),3))); rs=p6.qrot(q[:,7],np.broadcast_to(D[7],(len(q),3)))
    v=ls-rs; v/=(np.linalg.norm(v,axis=-1,keepdims=True)+1e-9)
    return float(np.sqrt(v[:,1]**2+v[:,2]**2).mean())
_tr = _load("tr", "train-t2m-flow-v5.py")
_DIRS=torch.tensor(_tr.DIR_CANON,dtype=torch.float32)
def legchain(q):
    """The user's reported 'second knee': bend in the ankle instead of the knee.

    Real Walk.fbx scores 0.014 and the training data 0.000, while v7.8 ep60 —
    the checkpoint the user described as having 'almost a flat knee, slightly
    backwards, then a second knee below' — scores 0.222. So this is the metric
    that tracks the one defect the user named on the walk, and no other metric
    here captures it (they all measure facing, phase or stride, never WHICH
    joint bends).
    """
    return float(_tr.leg_chain_penalty(torch.from_numpy(q[None]).float(),_DIRS).item())
def scissor(q):
    la,ra=pos(17,q),pos(21,q); return float(np.abs(la[:,2]-ra[:,2]).max())
_ap = argparse.ArgumentParser(description=__doc__)
_ap.add_argument("--roots", default=os.path.expanduser("~/t2m_v78"),
                 help="comma-separated checkpoint roots to scan (each is "
                      "globbed for e*/ and ck*/ subdirs holding t2m.onnx)")
_ap.add_argument("--ref", default=os.path.expanduser("~/Downloads/Walk.fbx"),
                 help="real reference walk clip to score against")
_a = _ap.parse_args()

cache=os.path.join(tempfile.gettempdir(),"t2m_refcache")
_ref = os.path.expanduser(_a.ref)
if not os.path.exists(_ref):
    raise SystemExit(f"reference clip not found: {_ref} (pass --ref)")
cqw,vw=rd.canon_from_fbx(_ref,cache)
refs=rd.windows_of(cqw)
# actions the USER confirmed as good/usable on ep60 — these are what matter
USER_GOOD=["walk","run","wave","cough","death","pickup","attack","crouch",
           "salute","working","shake","punch"]
so=ort.SessionOptions(); so.log_severity_level=3
print(f"{'ckpt':10s} {'walk refD':>10s} {'walk latE':>10s} {'walk sciss':>11s} "
      f"{'walk legchn':>12s} {'good-act latE':>14s}")
print("           (lower)     (real .483)  (real ~1.0)   (real .014)      (lower)")
rows=[]
cands = []
for _root in _a.roots.split(","):
    _root = os.path.expanduser(_root.strip())
    cands += glob.glob(os.path.join(_root, "e*")) + glob.glob(os.path.join(_root, "ck*"))
cands = sorted(set(cands))
if not cands:
    raise SystemExit(f"no checkpoint dirs under {_a.roots} (pass --roots)")
for d in cands:
    f=os.path.join(d,"t2m.onnx")
    if not os.path.exists(f): continue
    s=ort.InferenceSession(f,so,providers=["CPUExecutionProvider"])
    vv=json.load(open(os.path.join(d,"t2m-vocab.json")))["vocab"]; zd=int(s.get_inputs()[1].shape[-1])
    def run(act,n):
        rng=np.random.default_rng(7); t=np.zeros((1,len(vv)),np.float32)
        if act not in vv: return None
        t[0,vv.index(act)]=1.0
        R=[]
        for _ in range(n):
            out=s.run(None,{"tokens":t,"seed":(rng.standard_normal((1,zd))*0.5).astype(np.float32)})[0][0]
            q=out.reshape(-1,22,10)[:,:,3:7]; q=q/np.linalg.norm(q,axis=-1,keepdims=True)
            R.append(q)
        return R
    wq=run("walk",24)
    wd=min(rd.best_distance(q,refs,vw) for q in wq)
    wl=float(np.mean([lat_err(q) for q in wq])); ws=float(np.mean([scissor(q) for q in wq]))
    wlc=float(np.mean([legchain(q) for q in wq]))
    ls=[]
    for a in USER_GOOD:
        qs=run(a,8)
        if qs: ls.append(np.mean([lat_err(q) for q in qs]))
    gl=float(np.mean(ls))
    print(f"{os.path.basename(d):10s} {wd:10.3f} {wl:10.3f} {ws:11.3f} "
          f"{wlc:12.3f} {gl:14.3f}")
    rows.append((gl,wd,wlc,os.path.basename(d)))
print("\nreal walk: refD floor 0.437 | latErr 0.483 | scissor 0.652 | legchain 0.014")
rows.sort()
print(f"BEST by good-action facing: {rows[0][3]}")
print(f"BEST by walk refDist:      {sorted(rows,key=lambda r:r[1])[0][3]}")
print(f"BEST by knee/ankle (the user's reported walk defect): "
      f"{sorted(rows,key=lambda r:r[2])[0][3]}")
