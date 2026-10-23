#!/usr/bin/env python3
"""Convert the MakeHuman system-assets hairstyles (CC0) into hair_character.qmcb for the character generator.

MakeHuman fits its hair to its own base mesh (hm08); the generator's body is the MHR mesh. So, once, offline:
  0. evaluate the hair on the raw MakeHuman base mesh from its .mhclo binding (the OBJ is the style on MakeHuman's modified default human),
  1. warp the MakeHuman head onto the MHR neutral head (similarity ICP, then a few RBF rounds on nearest-point matches; ~2 mm mean on
     the cranium),
  2. take each hair vertex through that warp and BIND it to the MHR neutral surface: triangle + barycentric weights + the offset in the
     triangle's frame (tangent, bitangent, normal), clamped a little above the scalp,
  3. store the binding, the UVs / polygons and the diffuse texture (resized, PNG) per style.
At runtime the same binding is evaluated on the SHAPED body (positions of the three vertices + the offset in their new frame), so a
hairstyle follows every head shape; skin weights come from the bound vertices.

INPUTS (read-only; MakeHuman assets are CC0, see LICENSE.ASSETS.md in https://github.com/makehumancommunity/makehuman)
  --assets    the unzipped makehuman_system_assets_cc0.zip (it holds hair/<style>/<style>.obj + .mhmat + *_diffuse.png)
  --base      makehuman/data/3dobjs/base.obj (the hm08 base mesh; its first 13380 vertices are the body)
  --mhr       the character generator's mhr_character.qmcb
  --out       output directory: hair_character.qmcb + the MakeHuman licence text is copied by hand next to it

QMCB1 container as in build-character-assets.py (u32 blobCount, per blob name/dtype/dims/bytes).
USAGE
  python scripts/build-hair-assets.py --assets x --base base.obj --mhr .character_work/out/mhr_character.qmcb --out .character_work/out
"""
import argparse, glob, io, json, os, re, struct
import numpy as np
from PIL import Image
from scipy.interpolate import RBFInterpolator
from scipy.spatial import cKDTree

DT = {np.dtype("<f4"): 0, np.dtype("<f2"): 1, np.dtype("<i4"): 2, np.dtype("u1"): 3}
MH_BODY_VERTS = 13380


def write_bundle(path, blobs):
    with open(path, "wb") as f:
        f.write(b"QMCB1\0\0\0")
        f.write(struct.pack("<I", len(blobs)))
        for name, arr in blobs.items():
            arr = np.ascontiguousarray(arr)
            if arr.dtype == np.float32: arr = arr.astype("<f4")
            if arr.dtype in (np.int32, np.int64): arr = arr.astype("<i4")
            nb = name.encode()
            f.write(struct.pack("<I", len(nb))); f.write(nb)
            f.write(struct.pack("<B", DT[arr.dtype])); f.write(struct.pack("<I", arr.ndim))
            for d in arr.shape: f.write(struct.pack("<Q", d))
            raw = arr.tobytes(); f.write(struct.pack("<Q", len(raw))); f.write(raw)
    print("wrote", path, os.path.getsize(path) // 1024, "KB")


def read_bundle(path):
    d = open(path, "rb").read()
    assert d[:8] == b"QMCB1\0\0\0"
    n, = struct.unpack("<I", d[8:12]); o = 12; out = {}
    dt = {0: "<f4", 1: "<f2", 2: "<i4", 3: "u1"}
    for _ in range(n):
        l, = struct.unpack("<I", d[o:o + 4]); o += 4; name = d[o:o + l].decode(); o += l
        t, nd = struct.unpack("<BI", d[o:o + 5]); o += 5
        dims = struct.unpack("<" + "Q" * nd, d[o:o + 8 * nd]); o += 8 * nd
        bl, = struct.unpack("<Q", d[o:o + 8]); o += 8
        out[name] = np.frombuffer(d[o:o + bl], dtype=dt[t]).reshape(dims); o += bl
    return out


def read_obj(path):
    v, vt, polys = [], [], []
    for line in open(path):
        p = line.split()
        if not p: continue
        if p[0] == "v": v.append([float(x) for x in p[1:4]])
        elif p[0] == "vt": vt.append([float(x) for x in p[1:3]])
        elif p[0] == "f":
            polys.append([[int(i) - 1 if i else -1 for i in tok.split("/")[:2]] for tok in p[1:]])
    return np.array(v), np.array(vt), polys


def eval_mhclo(path, base):
    """Hair vertex positions on the RAW MakeHuman base mesh from the .mhclo binding: sum of w_i * base[v_i] + offset * axis scales
    (scale = |base[a].axis - base[b].axis| / d from the x/y/z_scale lines). The OBJ next to it is the style on MakeHuman's default
    (modified) human, which is not the mesh the head warp was fitted on."""
    scales, rows = {}, []
    for line in open(path):
        p = line.split()
        if not p or p[0] == "#": continue
        if p[0] in ("x_scale", "y_scale", "z_scale"): scales[p[0][0]] = (int(p[1]), int(p[2]), float(p[3]))
        elif len(p) >= 9 and p[0].isdigit():
            rows.append([float(x) for x in p[:9]])
    s = np.array([abs(base[scales[ax][0], i] - base[scales[ax][1], i]) / scales[ax][2] for i, ax in enumerate("xyz")])
    r = np.array(rows)
    idx, w, off = r[:, :3].astype(int), r[:, 3:6], r[:, 6:9]
    return (base[idx] * w[:, :, None]).sum(1) + off * s


def similarity(P, Q):
    mp, mq = P.mean(0), Q.mean(0)
    Pc, Qc = P - mp, Q - mq
    U, S, Vt = np.linalg.svd(Pc.T @ Qc)
    R = (U @ Vt).T
    if np.linalg.det(R) < 0:
        Vt[-1] *= -1
        R = (U @ Vt).T
    s = S.sum() / (Pc ** 2).sum()
    return s, R, mq - s * (R @ mp)


class HeadWarp:
    """MakeHuman units -> MHR centimetres, accurate on the head."""
    def __init__(self, mh_body, mhr):
        s0 = (mhr[:, 1].max() - mhr[:, 1].min()) / (mh_body[:, 1].max() - mh_body[:, 1].min())
        self.s0, self.y0 = s0, mh_body[:, 1].min()
        src = self.base(mh_body[mh_body[:, 1] > mh_body[:, 1].max() - 2.3])
        dst = mhr[mhr[:, 1] > mhr[:, 1].max() - 23.0]
        tree = cKDTree(dst)
        cur = src.copy()
        self.sim = (1.0, np.eye(3), np.zeros(3))
        for _ in range(30):
            _, i = tree.query(cur)
            s, R, t = similarity(cur, dst[i])
            cur = (s * (R @ cur.T)).T + t
        self.sim = similarity(src, cur)
        rng = np.random.default_rng(0)
        self.rbfs = []
        for _ in range(6):
            d, i = tree.query(cur)
            ctrl = np.where(d < 2.5)[0]
            ctrl = ctrl[rng.permutation(len(ctrl))[:700]]
            rbf = RBFInterpolator(cur[ctrl], (dst[i[ctrl]] - cur[ctrl]) * 0.8, kernel="thin_plate_spline", smoothing=8.0)
            self.rbfs.append(rbf)
            cur = cur + rbf(cur)
        d, _ = tree.query(cur)
        cr = cur[:, 1] > cur[:, 1].max() - 12
        print(f"head warp: cranium mean {d[cr].mean() * 10:.2f} mm, max {d[cr].max() * 10:.1f} mm")

    def base(self, p):
        q = p * self.s0
        q[:, 1] -= self.y0 * self.s0
        return q

    def __call__(self, p):
        s, R, t = self.sim
        q = (s * (R @ self.base(p).T)).T + t
        for rbf in self.rbfs:
            q = q + rbf(q)
        return q


def closest_on_triangle(p, a, b, c):
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = ab @ ap, ac @ ap
    if d1 <= 0 and d2 <= 0: return a, np.array([1.0, 0, 0])
    bp = p - b; d3, d4 = ab @ bp, ac @ bp
    if d3 >= 0 and d4 <= d3: return b, np.array([0, 1.0, 0])
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        v = d1 / (d1 - d3); return a + v * ab, np.array([1 - v, v, 0])
    cp = p - c; d5, d6 = ab @ cp, ac @ cp
    if d6 >= 0 and d5 <= d6: return c, np.array([0, 0, 1.0])
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        w = d2 / (d2 - d6); return a + w * ac, np.array([1 - w, 0, w])
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); return b + w * (c - b), np.array([0, 1 - w, w])
    den = 1.0 / (va + vb + vc); v, w = vb * den, vc * den
    return a + ab * v + ac * w, np.array([1 - v - w, v, w])


def frame(a, b, c):
    t1 = b - a; t1 = t1 / (np.linalg.norm(t1) + 1e-12)
    n = np.cross(b - a, c - a); n = n / (np.linalg.norm(n) + 1e-12)
    return t1, np.cross(n, t1), n


def bind(points, base, faces, min_above_scalp):
    cent = base[faces].mean(1)
    tree = cKDTree(cent)
    tri = np.zeros((len(points), 3), np.int32); bary = np.zeros((len(points), 3), np.float32); off = np.zeros((len(points), 3), np.float32)
    for k, p in enumerate(points):
        best = None
        for t in tree.query(p, 12)[1]:
            a, b, c = base[faces[t]]
            q, w = closest_on_triangle(p, a, b, c)
            d = np.linalg.norm(p - q)
            if best is None or d < best[0]: best = (d, t, q, w)
        d, t, q, w = best
        a, b, c = base[faces[t]]
        t1, t2, n = frame(a, b, c)
        o = p - q
        loc = np.array([o @ t1, o @ t2, o @ n])
        if d < 3.0 and loc[2] < min_above_scalp:   # a hair vertex that hugs / pokes into the scalp stays just above it
            loc[2] = min_above_scalp
        tri[k], bary[k], off[k] = faces[t], w, loc
    return tri, bary, off


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", required=True)
    ap.add_argument("--base", required=True)
    ap.add_argument("--mhr", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--texture-size", type=int, default=1024)
    ap.add_argument("--styles", default="", help="comma list; default every hairstyle found")
    a = ap.parse_args()
    mhr = read_bundle(a.mhr)
    base = mhr["base"].astype(np.float64)
    faces = mhr["faces"].astype(np.int64)
    mh_v, _, _ = read_obj(a.base)
    warp = HeadWarp(mh_v[:MH_BODY_VERTS], base)
    blobs, styles = {}, []
    want = [s for s in a.styles.split(",") if s]
    for d in sorted(glob.glob(os.path.join(a.assets, "hair", "*"))):
        name = os.path.basename(d)
        if want and name not in want: continue
        objs = glob.glob(os.path.join(d, "*.obj"))
        diffuse = glob.glob(os.path.join(d, "*_diffuse.png"))
        mat = glob.glob(os.path.join(d, "*.mhmat"))
        if not objs or not diffuse: continue
        v, vt, polys = read_obj(objs[0])
        clo = glob.glob(os.path.join(d, "*.mhclo"))
        if not clo: continue
        pos = eval_mhclo(clo[0], mh_v)   # the FULL base: some styles bind to its helper-hair vertices (ids >= 13380)
        assert len(pos) == len(v), f"{name}: {len(pos)} bound vertices vs {len(v)} in the OBJ"
        pts = warp(pos)
        tri, bary, off = bind(pts, base, faces, 0.15)
        img = Image.open(diffuse[0]).convert("RGBA").resize((a.texture_size, a.texture_size), Image.LANCZOS)
        png = io.BytesIO(); img.save(png, "PNG", optimize=True)
        p = f"hair.{name}."
        blobs[p + "tri"] = tri; blobs[p + "bary"] = bary; blobs[p + "off"] = off
        blobs[p + "uv"] = vt.astype(np.float32)
        blobs[p + "poly_size"] = np.array([len(q) for q in polys], np.int32)
        blobs[p + "poly_vertex"] = np.array([c[0] for q in polys for c in q], np.int32)
        blobs[p + "poly_uv"] = np.array([c[1] for q in polys for c in q], np.int32)
        blobs[p + "texture"] = np.frombuffer(png.getvalue(), np.uint8)
        styles.append({"name": name, "vertices": int(len(v)), "polygons": len(polys)})
        print(f"{name}: {len(v)} vertices, {len(polys)} polygons, texture {len(png.getvalue()) // 1024} KB")
    blobs["meta"] = np.frombuffer(json.dumps({"styles": styles, "source": "MakeHuman system assets (CC0)", "textureSize": a.texture_size}).encode(), np.uint8)
    os.makedirs(a.out, exist_ok=True)
    write_bundle(os.path.join(a.out, "hair_character.qmcb"), blobs)


if __name__ == "__main__":
    main()
