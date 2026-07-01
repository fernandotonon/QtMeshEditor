#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Re-preprocess CMU BVH into a CLEAN text-to-motion cache (#411).

ONE-TIME OFFLINE dev tool — NOT shipped. Fixes the data-prep problems that
capped the model quality (diagnosed from the original cache):
  1. 98% of windows were NEAR-STATIC (idle frames) — the model learned "hold a
     pose" → the wobble/fold we saw. FIX: keep only windows whose own motion
     exceeds a threshold (drop idle lead-in/out + still clips).
  2. 35% of windows had 2+ CONFLICTING action labels (e.g. walk+turn) — noisy
     conditioning. FIX: keep SINGLE-label windows only.
  3. Labels were clip-level applied to every window including the static parts.
     FIX: (1)+(2) together mean each kept window is a clean, moving, single-
     action example.

Output: a drop-in replacement cache (same keys mo[N,T,22,4] quats + tk[N,V])
that train-t2m-onnx*.py can consume unchanged.

Usage:
  python prep-t2m-clean.py --bvh /tmp/rmib_train/bvh_raw \
      --annotations /tmp/rmib_train/bvh_raw/MotionCapture/cmu-mocap-annotations.csv \
      --out /tmp/t2m_clean.npz --min-move 0.004
"""
import argparse, csv, glob, os, re
import numpy as np

CANON = ["hip","abdomen","chest","neck","neck1","head","rcollar","rshoulder",
         "relbow","rhand","lcollar","lshoulder","lelbow","lhand","rbuttock",
         "rhip","rknee","rfoot","lbuttock","lhip","lknee","lfoot"]
DROP = ("jaw","oris","tongue","levator","special","eye","orbicularis",
        "temporalis","oculi","risorius","finger","metacarpal","toe","thumb","__")
VOCAB = ["walk","run","jog","jump","dance","march","climb","kick","punch","sit",
         "stretch","throw","wave","boxing","turn","forward"]
J = len(CANON); T = 40

def is_core(n): return not any(k in n.lower() for k in DROP)

def euler_to_quat_vec(rad, order):
    Tn = rad.shape[0]; axis_of = {"X":0,"Y":1,"Z":2}
    def axisq(angle, axis):
        h = angle*0.5; q = np.zeros((Tn,4),np.float32)
        q[:,3]=np.cos(h); q[:,axis]=np.sin(h); return q
    def qmul(a,b):
        ax,ay,az,aw=a[:,0],a[:,1],a[:,2],a[:,3]; bx,by,bz,bw=b[:,0],b[:,1],b[:,2],b[:,3]
        o=np.empty_like(a)
        o[:,0]=aw*bx+ax*bw+ay*bz-az*by; o[:,1]=aw*by-ax*bz+ay*bw+az*bx
        o[:,2]=aw*bz+ax*by-ay*bx+az*bw; o[:,3]=aw*bw-ax*bx-ay*by-az*bz
        return o
    q=np.zeros((Tn,4),np.float32); q[:,3]=1.0
    for ci,ch in enumerate(order): q=qmul(q,axisq(rad[:,ci],axis_of[ch]))
    return q

def load_single_labels(csv_path):
    """motion-id -> SINGLE action index, only for clips matching exactly one word."""
    labels={}
    with open(csv_path,newline="") as f:
        for row in csv.reader(f):
            if len(row)<2 or not row[0] or row[0]=="Motion": continue
            mid,desc=row[0].strip(),row[1].lower()
            hits=[i for i,w in enumerate(VOCAB) if re.search(r"\b"+re.escape(w),desc)]
            if len(hits)==1:                 # SINGLE-label only (fix #2)
                labels[mid]=hits[0]
    from collections import Counter
    c=Counter(labels.values())
    print(f"single-label clips: {len(labels)}  per-action: "
          + " ".join(f"{VOCAB[i]}={c.get(i,0)}" for i in range(len(VOCAB))))
    return labels

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--bvh",required=True)
    ap.add_argument("--annotations",required=True)
    ap.add_argument("--out",default="/tmp/t2m_clean.npz")
    ap.add_argument("--min-move",type=float,default=0.004,
                    help="drop windows whose mean frame-to-frame quat motion is below this")
    a=ap.parse_args()
    from bvh import Bvh
    labels=load_single_labels(a.annotations)
    files=sorted(glob.glob(os.path.join(a.bvh,"**/*.bvh"),recursive=True))
    print(f"found {len(files)} BVH files")
    motions,toks=[],[]; skipped=static_drop=0
    for path in files:
        base=os.path.basename(path); m_=re.match(r"(\d+_\d+)",base)
        mid=m_.group(1) if m_ else None
        if mid is None or mid not in labels: skipped+=1; continue
        try: m=Bvh(open(path).read())
        except Exception: skipped+=1; continue
        if [n for n in m.get_joints_names() if is_core(n)]!=CANON: skipped+=1; continue
        frames=np.array(m.frames,dtype=np.float32); nF=len(frames)
        if nF<T: continue
        col,rotcols,roto=0,{},{}
        for j in m.get_joints_names():
            chans=m.joint_channels(j); rc,order=[],""
            for ci,c in enumerate(chans):
                if c.endswith("rotation"): rc.append(col+ci); order+=c[0]
            if j in CANON and len(order)==3: rotcols[j],roto[j]=rc,order
            col+=len(chans)
        quats=np.zeros((nF,J,4),np.float32)
        for ji,j in enumerate(CANON):
            if j not in rotcols: quats[:,ji,3]=1.0; continue
            quats[:,ji]=euler_to_quat_vec(np.deg2rad(frames[:,rotcols[j]]),roto[j])
        ai=labels[mid]
        for s in range(0,nF-T+1,max(1,T//2)):
            win=quats[s:s+T]
            mv=np.abs(win[1:]-win[:-1]).mean()       # this WINDOW's own motion
            if mv<a.min_move:                         # fix #1+#3: drop idle windows
                static_drop+=1; continue
            vec=np.zeros(len(VOCAB),np.float32); vec[ai]=1.0
            motions.append(win); toks.append(vec)
    print(f"kept {len(motions)} windows (skipped {skipped} files, dropped {static_drop} static windows)")
    if not motions: raise SystemExit("No windows kept — lower --min-move?")
    mo=np.stack(motions); tk=np.stack(toks)
    # report kept per-action distribution
    cnt=tk.sum(0).astype(int)
    print("kept per-action:", " ".join(f"{VOCAB[i]}={cnt[i]}" for i in range(len(VOCAB))))
    np.savez(a.out,mo=mo,tk=tk)
    print(f"wrote {a.out}  mo{mo.shape} tk{tk.shape}")

if __name__=="__main__": main()
