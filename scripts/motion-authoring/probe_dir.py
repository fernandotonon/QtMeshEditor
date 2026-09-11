"""Numeric world-space bone directions for an authored pose — no renders."""
import math
from glbanim import Glb, quat_mul, quat_from_euler, MIXAMO

g = Glb('rumba.glb')
nodes = g.json['nodes']
parent = {}
for i, n in enumerate(nodes):
    for c in n.get('children', []):
        parent[c] = i

def qrot(q, v):
    x, y, z, w = q
    t = (2*(y*v[2]-z*v[1]), 2*(z*v[0]-x*v[2]), 2*(x*v[1]-y*v[0]))
    return (v[0]+w*t[0]+y*t[2]-z*t[1],
            v[1]+w*t[1]+z*t[0]-x*t[2],
            v[2]+w*t[2]+x*t[1]-y*t[0])

def world_quat(idx, ov):
    q = (0, 0, 0, 1); chain = []
    while idx is not None:
        chain.append(idx); idx = parent.get(idx)
    for i in reversed(chain):
        q = quat_mul(q, ov.get(nodes[i].get('name', ''),
                     tuple(nodes[i].get('rotation', [0, 0, 0, 1]))))
    return q

def overrides(pose):
    """pose: {joint_key: (x,y,z)} anatomical -> local quats (mirror r_*)."""
    ov = {}
    for k, e in pose.items():
        name = MIXAMO[k]
        i = g.node_index(name)
        bind = tuple(nodes[i].get('rotation', [0, 0, 0, 1]))
        ey, ez = (-e[1], -e[2]) if k.startswith('r_') else (e[1], e[2])
        ov[name] = quat_mul(bind, quat_from_euler(e[0], ey, ez))
    return ov

CHILD = {
    'l_arm': 'mixamorig:LeftForeArm',   'r_arm': 'mixamorig:RightForeArm',
    'l_forearm': 'mixamorig:LeftHand',  'r_forearm': 'mixamorig:RightHand',
    'l_upleg': 'mixamorig:LeftLeg',     'r_upleg': 'mixamorig:RightLeg',
    'l_leg': 'mixamorig:LeftFoot',      'r_leg': 'mixamorig:RightFoot',
    'spine': 'mixamorig:Spine1',        'head': 'mixamorig:HeadTop_End',
}

def bone_dir(key, pose):
    """Unit world direction the bone points, given the pose."""
    b = g.node_index(MIXAMO[key]); c = g.node_index(CHILD[key])
    if b < 0 or c < 0: return None
    d = qrot(world_quat(b, overrides(pose)), tuple(nodes[c].get('translation', [0, 0, 0])))
    L = math.sqrt(sum(x*x for x in d)) or 1.0
    return tuple(round(x/L, 3) for x in d)

def report(label, pose, keys):
    parts = [f'{k}={bone_dir(k, pose)}' for k in keys]
    print(f'{label:22s} ' + '  '.join(parts))

# world: +X = character LEFT, +Y = up, +Z = FRONT (the way the model faces)
