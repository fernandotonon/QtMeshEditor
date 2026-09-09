"""Measure world bone directions from an ANIMATED glb at sample times."""
import json, math, struct, sys

def load(path):
    raw = open(path, 'rb').read()
    off = 12; js = None; binc = b''
    while off < len(raw):
        clen, ctype = struct.unpack_from('<II', raw, off); off += 8
        ch = raw[off:off+clen]; off += clen
        if ctype == 0x4E4F534A: js = json.loads(ch.decode('utf-8'))
        elif ctype == 0x004E4942: binc = ch
    return js, binc

def acc_read(js, binc, ai):
    a = js['accessors'][ai]
    bv = js['bufferViews'][a['bufferView']]
    off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    n = a['count']
    ncomp = {'SCALAR':1,'VEC3':3,'VEC4':4}[a['type']]
    out = []
    for i in range(n):
        out.append(struct.unpack_from('<%df' % ncomp, binc, off + i*4*ncomp))
    return out

def qmul(a, b):
    ax,ay,az,aw=a; bx,by,bz,bw=b
    return (aw*bx+ax*bw+ay*bz-az*by, aw*by-ax*bz+ay*bw+az*bx,
            aw*bz+ax*by-ay*bx+az*bw, aw*bw-ax*bx-ay*by-az*bz)

def qrot(q, v):
    x,y,z,w=q
    t=(2*(y*v[2]-z*v[1]), 2*(z*v[0]-x*v[2]), 2*(x*v[1]-y*v[0]))
    return (v[0]+w*t[0]+y*t[2]-z*t[1], v[1]+w*t[1]+z*t[0]-x*t[2], v[2]+w*t[2]+x*t[1]-y*t[0])

def slerp_at(times, quats, t):
    if t <= times[0][0]: return quats[0]
    if t >= times[-1][0]: return quats[-1]
    for i in range(len(times)-1):
        t0, t1 = times[i][0], times[i+1][0]
        if t0 <= t <= t1:
            u = (t-t0)/max(1e-9, t1-t0)
            a, b = quats[i], quats[i+1]
            d = sum(a[k]*b[k] for k in range(4))
            if d < 0: b = tuple(-c for c in b); d = -d
            out = tuple(a[k] + u*(b[k]-a[k]) for k in range(4))
            L = math.sqrt(sum(c*c for c in out)) or 1
            return tuple(c/L for c in out)
    return quats[-1]

class Rig:
    def __init__(self, path, anim_name=None):
        self.js, self.bin = load(path)
        self.nodes = self.js['nodes']
        self.parent = {}
        for i,n in enumerate(self.nodes):
            for c in n.get('children',[]): self.parent[c]=i
        self.tracks = {}
        anims = self.js.get('animations',[])
        anim = None
        for a in anims:
            if anim_name is None or a.get('name')==anim_name: anim=a; break
        if anim is None and anims: anim = anims[0]
        self.anim_name = anim.get('name') if anim else None
        if anim:
            for ch in anim['channels']:
                if ch['target']['path']!='rotation': continue
                s = anim['samplers'][ch['sampler']]
                self.tracks[ch['target']['node']] = (
                    acc_read(self.js,self.bin,s['input']),
                    acc_read(self.js,self.bin,s['output']))
        self.dur = 0.0
        for tms,_ in self.tracks.values():
            if tms: self.dur = max(self.dur, tms[-1][0])
    def idx(self, name):
        for i,n in enumerate(self.nodes):
            if n.get('name')==name: return i
        return -1
    def local(self, i, t):
        if i in self.tracks:
            tms,qs = self.tracks[i]
            return slerp_at(tms,qs,t)
        return tuple(self.nodes[i].get('rotation',[0,0,0,1]))
    def world(self, i, t):
        q=(0,0,0,1); chain=[]
        while i is not None:
            chain.append(i); i=self.parent.get(i)
        for k in reversed(chain): q=qmul(q,self.local(k,t))
        return q
    def bone_dir(self, bone, child, t):
        b=self.idx(bone); c=self.idx(child)
        if b<0 or c<0: return None
        d=qrot(self.world(b,t), tuple(self.nodes[c].get('translation',[0,0,0])))
        L=math.sqrt(sum(x*x for x in d)) or 1
        return tuple(round(x/L,3) for x in d)
