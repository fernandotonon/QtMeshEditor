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

def _slerp(a, b, t):
    d = float(np.dot(a, b))
    if d < 0: b, d = -b, -d
    if d > 0.9995:
        q = a + t * (b - a)
        return q / np.linalg.norm(q)
    th = math.acos(max(-1.0, min(1.0, d)))
    s = math.sin(th)
    return (math.sin((1 - t) * th) / s) * a + (math.sin(t * th) / s) * b


def _sample(times, values, t, interp='LINEAR'):
    """Rotation at time t, interpolating like the player does.

    Channels in one clip can carry DIFFERENT keyframe counts (measured 2, 31
    and 34 in a single Quaternius walk), so sampling by INDEX compares
    mismatched moments across bones and reports tens of degrees of error for a
    clip that is actually correct. Always sample by TIME.
    """
    if t <= times[0]: return values[0]
    if t >= times[-1]: return values[-1]
    i = int(np.searchsorted(times, t)) - 1
    i = max(0, min(i, len(times) - 2))
    if interp == 'STEP':
        return values[i]          # a held rotation must not be interpolated
    span = times[i + 1] - times[i]
    u = 0.0 if span <= 0 else (t - times[i]) / span
    return _slerp(values[i], values[i + 1], u)


def world_sequence(path, clip, bone, samples=24, tmax=None):
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
            s = anim['samplers'][ch['sampler']]
            interp = s.get('interpolation', 'LINEAR')
            if interp == 'CUBICSPLINE':
                # Output holds in/value/out tangents per key, so reading it as
                # plain rotations would compare tangents and report nonsense.
                raise SystemExit(
                    f"error: {path} clip '{clip}' uses CUBICSPLINE "
                    "interpolation, which this script cannot sample")
            rot[ch['target']['node']] = (acc(s['input']).ravel(),
                                         acc(s['output']), interp)
    if not rot: return None
    t0 = min(v[0][0] for v in rot.values())
    t1 = max(v[0][-1] for v in rot.values())
    # Compare over a COMMON window. The dead-tail trim shortens a retargeted
    # clip (2.667s native -> 1.333s trimmed), and normalising each to [0,1]
    # would stretch one against the other and report a phase error as a pose
    # error.
    if tmax is not None: t1 = min(t1, t0 + tmax)
    seq = []
    for k in range(samples):
        t = t0 + (t1 - t0) * k / (samples - 1)
        w = np.array([0, 0, 0, 1.0])
        for nd in reversed(chain):
            if nd in rot:
                times, vals, interp = rot[nd]
                q = _sample(times, vals, t, interp)
            else:
                q = np.array(nodes[nd].get('rotation', [0, 0, 0, 1]), dtype=float)
            w = qmul(w, q)
        seq.append(w)
    return seq

def resample(s, n=24):
    """No-op: world_sequence already returns `n` TIME-aligned samples."""
    return s

def main():
    if len(sys.argv) < 5:
        print(__doc__); return 2
    nat, natclip, ret, retclip = sys.argv[1:5]
    bones = sys.argv[5:] or ['FrontLeg.L', 'FrontUpLeg.L', 'FrontLowLeg.L',
                             'BackLeg.L', 'BackUpLeg.L', 'BackLowLeg.L']
    # Duration of the RETARGETED clip bounds the comparison window.
    def duration(path, clip):
        j, acc = load(path)
        for a in j.get('animations', []):
            if clip not in a['name']: continue
            # The LONGEST channel, not the first: if the first channel ends
            # early the window would be short while world_sequence still
            # samples the full rotation range, turning a phase offset into a
            # reported pose error.
            end = 0.0
            for ch in a['channels']:
                s = a['samplers'][ch['sampler']]
                end = max(end, float(j['accessors'][s['input']]['max'][0]))
            return end or None
        return None
    win = duration(ret, retclip)

    worst = 0.0
    compared = 0
    for b in bones:
        a = world_sequence(nat, natclip, b, tmax=win)
        c = world_sequence(ret, retclip, b)
        if a is None or c is None:
            print(f"{b:16s} (absent)"); continue
        compared += 1
        A, C = resample(a), resample(c)
        diffs = [math.degrees(2 * math.acos(min(1, abs(float(np.dot(A[i], C[i]))))))
                 for i in range(len(A))]
        mean = sum(diffs) / len(diffs)
        worst = max(worst, mean)
        print(f"{b:16s} mean {mean:6.1f} deg   max {max(diffs):6.1f}")
    if compared == 0:
        # Printing "WORST MEAN: 0.0" here would read as a PERFECT retarget when
        # in fact the clip name was wrong or no requested bone exists.
        print("\nERROR: no bone could be compared "
              "(wrong clip name, or none of the requested bones exist)")
        return 2
    print(f"\nWORST MEAN: {worst:.1f} deg  ({compared} bones)")
    return 0

if __name__ == '__main__':
    sys.exit(main())
