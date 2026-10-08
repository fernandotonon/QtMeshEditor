#!/usr/bin/env python3
"""Measure retarget fidelity: world-orientation error vs the NATIVE clip.

The honest test for "does this animation look right". Per-joint ANGLE RANGE
is not: a retarget can reproduce the source's range exactly while placing the
limb completely wrong, which is how a broken walk passed review twice.

A self-retarget (same rig as source) should score ~0 deg. Anything above a few
degrees is visible.

  scripts/creature-retarget-error.py native.glb NativeClip retarget.glb creature_Clip
"""
import json, struct, math, sys
import numpy as np

def load(p):
    d = open(p, 'rb').read()
    jl = struct.unpack('<I', d[12:16])[0]
    j = json.loads(d[20:20 + jl]); bo = 20 + jl + 8
    def acc(i):
        a = j['accessors'][i]; bv = j['bufferViews'][a['bufferView']]
        off = bo + bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        comp = {'VEC3': 3, 'VEC4': 4, 'SCALAR': 1}[a['type']]
        return np.frombuffer(d, dtype=np.float32,
                             count=a['count'] * comp, offset=off).reshape(a['count'], comp)
    return j, acc

def qmul(a, b):
    x1, y1, z1, w1 = a; x2, y2, z2, w2 = b
    return np.array([w1*x2 + x1*w2 + y1*z2 - z1*y2,
                     w1*y2 - x1*z2 + y1*w2 + z1*x2,
                     w1*z2 + x1*y2 - y1*x2 + z1*w2,
                     w1*w2 - x1*x2 - y1*y2 - z1*z2])

def world_sequence(path, clip, bone):
    j, acc = load(path)
    nodes = {i: n for i, n in enumerate(j['nodes'])}
    par = {}
    for i, n in nodes.items():
        for ch in n.get('children', []): par[ch] = i
    hits = [i for i, n in nodes.items() if n.get('name') == bone]
    if not hits: return None
    chain = []; i = hits[0]
    while i is not None: chain.append(i); i = par.get(i)
    anims = [a for a in j.get('animations', []) if clip in a['name']]
    if not anims: return None
    anim = anims[0]
    rot = {}
    for ch in anim['channels']:
        if ch['target']['path'] == 'rotation':
            rot[ch['target']['node']] = acc(anim['samplers'][ch['sampler']]['output'])
    if not rot: return None
    L = min(len(v) for v in rot.values())
    seq = []
    for f in range(L):
        w = np.array([0, 0, 0, 1.0])
        for nd in reversed(chain):
            q = (rot[nd][f] if nd in rot and f < len(rot[nd])
                 else np.array(nodes[nd].get('rotation', [0, 0, 0, 1]), dtype=float))
            w = qmul(w, q)
        seq.append(w)
    return seq

def resample(s, n=24):
    return [s[int(i * (len(s) - 1) / (n - 1))] for i in range(n)]

def main():
    if len(sys.argv) < 5:
        print(__doc__); return 2
    nat, natclip, ret, retclip = sys.argv[1:5]
    bones = sys.argv[5:] or ['FrontLeg.L', 'FrontUpLeg.L', 'FrontLowLeg.L',
                             'BackLeg.L', 'BackUpLeg.L', 'BackLowLeg.L']
    worst = 0.0
    for b in bones:
        a = world_sequence(nat, natclip, b)
        c = world_sequence(ret, retclip, b)
        if a is None or c is None:
            print(f"{b:16s} (absent)"); continue
        A, C = resample(a), resample(c)
        diffs = [math.degrees(2 * math.acos(min(1, abs(float(np.dot(A[i], C[i]))))))
                 for i in range(len(A))]
        mean = sum(diffs) / len(diffs)
        worst = max(worst, mean)
        print(f"{b:16s} mean {mean:6.1f} deg   max {max(diffs):6.1f}")
    print(f"\nWORST MEAN: {worst:.1f} deg")
    return 0

if __name__ == '__main__':
    sys.exit(main())
