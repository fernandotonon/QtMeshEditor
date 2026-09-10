"""Signed knee-bend metric, CALIBRATED against known poses:
   cross_x > 0  => correct flexion (heel toward buttock)
   cross_x < 0  => HYPEREXTENSION (knee bending backwards)
Validated: thigh vertical + knee X-60 -> cross_x +0.889 (correct);
           thigh vertical + knee X+60 -> cross_x -0.841 (broken)."""
import math
from glbanim import Glb, quat_mul, quat_from_euler, MIXAMO
_g = Glb('rumba.glb'); _nodes = _g.json['nodes']
_parent = {}
for _i, _n in enumerate(_nodes):
    for _c in _n.get('children', []): _parent[_c] = _i

def _qrot(q, v):
    x, y, z, w = q
    t = (2*(y*v[2]-z*v[1]), 2*(z*v[0]-x*v[2]), 2*(x*v[1]-y*v[0]))
    return (v[0]+w*t[0]+y*t[2]-z*t[1], v[1]+w*t[1]+z*t[0]-x*t[2], v[2]+w*t[2]+x*t[1]-y*t[0])

def bdir(pose, bone, child):
    o = {}
    for k, e in pose.items():
        nm = MIXAMO[k]; i = _g.node_index(nm)
        bind = tuple(_nodes[i].get('rotation', [0, 0, 0, 1]))
        ey, ez = (-e[1], -e[2]) if k.startswith('r_') else (e[1], e[2])
        o[nm] = quat_mul(bind, quat_from_euler(e[0], ey, ez))
    i = _g.node_index(bone); chain = []
    while i is not None: chain.append(i); i = _parent.get(i)
    q = (0, 0, 0, 1)
    for k in reversed(chain):
        q = quat_mul(q, o.get(_nodes[k].get('name', ''),
                              tuple(_nodes[k].get('rotation', [0, 0, 0, 1]))))
    d = _qrot(q, tuple(_nodes[_g.node_index(child)].get('translation', [0, 0, 0])))
    L = math.sqrt(sum(x*x for x in d)) or 1.0
    return tuple(x/L for x in d)

def knee_bend(pose, side='r'):
    """Returns +deg for correct flexion, -deg for hyperextension."""
    up = 'mixamorig:RightUpLeg' if side == 'r' else 'mixamorig:LeftUpLeg'
    lo = 'mixamorig:RightLeg' if side == 'r' else 'mixamorig:LeftLeg'
    ft = 'mixamorig:RightFoot' if side == 'r' else 'mixamorig:LeftFoot'
    th = bdir(pose, up, lo); sh = bdir(pose, lo, ft)
    cross_x = th[1]*sh[2] - th[2]*sh[1]
    dot = max(-1.0, min(1.0, sum(th[k]*sh[k] for k in range(3))))
    ang = math.degrees(math.acos(dot))
    # NB: no left-side sign flip — the LEG chain is not mirrored for X
    # (verified: the same knee X moves both shins the same way), unlike arms.
    return ang if cross_x > 0 else -ang
