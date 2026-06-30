#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Extract the Quaternius Universal Animation Library (CC0) into canonical t2m
windows and APPEND to the clean CMU cache (#411).

ONE-TIME OFFLINE dev tool — NOT shipped. The Quaternius lib is CC0 (verified:
repo LICENSE = CC0 1.0), a single glTF with ~46 named clips on a clean Rigify
humanoid (DEF-* joints). Per-clip it's cleaner/better-labelled than CMU, so it
supplements the thin actions in the CMU clean set. We parse the glTF animation
samplers by hand (no glTF lib needed — numpy over the .bin buffer), map DEF-*
joints to our 22 canonical joints, take each joint's LOCAL rotation track,
resample to 40-frame / 30fps windows, label by clip name → our action vocab,
and append to the existing clean cache.

Usage:
  python prep-quaternius-t2m.py --gltf /tmp/qter_anim/glTF/AnimationLibrary_Godot_Standard.gltf \
      --cache /tmp/t2m_clean.npz --out /tmp/t2m_clean_plus.npz
"""
import argparse, json, os, struct
import numpy as np

CANON = ["hip","abdomen","chest","neck","neck1","head","rcollar","rshoulder",
         "relbow","rhand","lcollar","lshoulder","lelbow","lhand","rbuttock",
         "rhip","rknee","rfoot","lbuttock","lhip","lknee","lfoot"]
VOCAB = ["walk","run","jog","jump","dance","march","climb","kick","punch","sit",
         "stretch","throw","wave","boxing","turn","forward"]
J = len(CANON); T = 40; FPS = 30

# Quaternius DEF-* joint -> our canonical joint. (neck1/collars/buttock have no
# direct DEF equivalent; we leave them identity — they're minor and the retarget
# tolerates missing canon roles.)
DEF2CANON = {
    "DEF-hips":"hip", "DEF-spine.001":"abdomen", "DEF-spine.002":"chest",
    "DEF-spine.003":"chest", "DEF-neck":"neck", "DEF-head":"head",
    "DEF-upper_arm.L":"lshoulder", "DEF-forearm.L":"lelbow", "DEF-hand.L":"lhand",
    "DEF-upper_arm.R":"rshoulder", "DEF-forearm.R":"relbow", "DEF-hand.R":"rhand",
    "DEF-thigh.L":"lhip", "DEF-shin.L":"lknee", "DEF-foot.L":"lfoot",
    "DEF-thigh.R":"rhip", "DEF-shin.R":"rknee", "DEF-foot.R":"rfoot",
}
# clip-name substring -> action vocab word
CLIP2ACTION = [
    ("Walk_Formal","walk"), ("Walk_Loop","walk"), ("Jog_Fwd","jog"),
    ("Sprint","run"), ("Jump","jump"), ("Dance","dance"),
    ("Punch","punch"), ("Sitting_Idle","sit"), ("Sitting_Talking","sit"),
    ("Crouch_Fwd","walk"), ("Push_Loop","walk"), ("Swim_Fwd","walk"),
    ("Sword_Attack","kick"), ("Spell_Simple_Shoot","throw"), ("Roll","jump"),
]

CTYPE = {5126:('f',4), 5123:('H',2), 5125:('I',4)}   # FLOAT, U16, U32
NCOMP = {'SCALAR':1,'VEC3':3,'VEC4':4,'QUAT':4}

def action_for(name):
    for sub,act in CLIP2ACTION:
        if sub in name: return VOCAB.index(act)
    return -1

def slerp(q0,q1,t):
    d=np.dot(q0,q1)
    if d<0: q1=-q1; d=-d
    if d>0.9995: q=q0+t*(q1-q0); return q/np.linalg.norm(q)
    th0=np.arccos(d); th=th0*t
    q2=q1-q0*d; q2/=np.linalg.norm(q2)
    return q0*np.cos(th)+q2*np.sin(th)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--gltf",required=True)
    ap.add_argument("--cache",required=True)
    ap.add_argument("--out",required=True)
    a=ap.parse_args()
    gdir=os.path.dirname(a.gltf)
    g=json.load(open(a.gltf))
    buf=open(os.path.join(gdir,g['buffers'][0]['uri']),'rb').read()
    bviews=g['bufferViews']; accs=g['accessors']; nodes=g['nodes']

    def read_accessor(ai):
        acc=accs[ai]; bv=bviews[acc['bufferView']]
        off=bv.get('byteOffset',0)+acc.get('byteOffset',0)
        n=acc['count']; nc=NCOMP[acc['type']]; fmt,sz=CTYPE[acc['componentType']]
        cnt=n*nc
        vals=struct.unpack_from('<'+fmt*cnt, buf, off)
        arr=np.array(vals,dtype=np.float32 if fmt=='f' else np.float64).reshape(n,nc)
        return arr

    # node index -> canonical index (via DEF name map)
    node_canon={}
    for ni,nd in enumerate(nodes):
        nm=nd.get('name','')
        if nm in DEF2CANON: node_canon[ni]=CANON.index(DEF2CANON[nm])

    win_mo=[]; win_tk=[]
    kept_by_action={}
    for anim in g.get('animations',[]):
        nm=anim.get('name','?'); act=action_for(nm)
        if act<0: continue
        # gather per-target-node rotation tracks
        chans=anim['channels']; samps=anim['samplers']
        rot_tracks={}   # canon_idx -> (times[K], quats[K,4])
        tmax=0.0
        for ch in chans:
            if ch['target']['path']!='rotation': continue
            node=ch['target']['node']
            if node not in node_canon: continue
            s=samps[ch['sampler']]
            times=read_accessor(s['input'])[:,0]
            quats=read_accessor(s['output'])   # [K,4] xyzw
            rot_tracks[node_canon[node]]=(times,quats)
            tmax=max(tmax,float(times[-1]) if len(times) else 0.0)
        if not rot_tracks or tmax<=0: continue
        # resample to fixed 30fps; tile/trim to 40-frame windows
        nF=max(T,int(round(tmax*FPS)))
        sample_t=np.arange(nF)/FPS
        pose=np.zeros((nF,J,4),np.float32); pose[:,:,3]=1.0   # identity default
        for cidx,(times,quats) in rot_tracks.items():
            for fi,tt in enumerate(sample_t):
                # find bracketing keyframes
                k=np.searchsorted(times,tt)
                if k<=0: q=quats[0]
                elif k>=len(times): q=quats[-1]
                else:
                    t0,t1=times[k-1],times[k]; f=(tt-t0)/max(1e-6,t1-t0)
                    q=slerp(quats[k-1].astype(np.float64),quats[k].astype(np.float64),f)
                pose[fi,cidx]=q/ (np.linalg.norm(q)+1e-9)
        # windowing (stride T//2), keep dynamic windows only (match CMU prep)
        for s in range(0,nF-T+1,max(1,T//2)):
            win=pose[s:s+T]
            mv=np.abs(win[1:]-win[:-1]).mean()
            if mv<0.0025: continue
            vec=np.zeros(len(VOCAB),np.float32); vec[act]=1.0
            win_mo.append(win); win_tk.append(vec)
            kept_by_action[VOCAB[act]]=kept_by_action.get(VOCAB[act],0)+1
        # also always keep at least the first window even if slightly static
        if nF>=T and not any(np.abs(pose[s:s+T][1:]-pose[s:s+T][:-1]).mean()>=0.0025 for s in range(0,nF-T+1,max(1,T//2))):
            vec=np.zeros(len(VOCAB),np.float32); vec[act]=1.0
            win_mo.append(pose[:T]); win_tk.append(vec)
            kept_by_action[VOCAB[act]]=kept_by_action.get(VOCAB[act],0)+1
    print(f"Quaternius windows kept: {len(win_mo)}  by action: {kept_by_action}")
    if not win_mo: raise SystemExit("No Quaternius windows extracted — check joint/clip maps.")

    qmo=np.stack(win_mo); qtk=np.stack(win_tk)
    d=np.load(a.cache); cmo,ctk=d['mo'],d['tk']
    mo=np.concatenate([cmo,qmo],0); tk=np.concatenate([ctk,qtk],0)
    np.savez(a.out,mo=mo,tk=tk)
    print(f"merged: CMU {cmo.shape[0]} + Quaternius {qmo.shape[0]} = {mo.shape[0]} windows -> {a.out}")
    print("combined per-action:", " ".join(f"{VOCAB[i]}={int(tk[:,i].sum())}" for i in range(len(VOCAB))))

if __name__=="__main__": main()
