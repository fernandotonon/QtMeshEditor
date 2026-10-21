#!/usr/bin/env python3
"""Pack the data the character generator needs into two small bundles (offline developer tool, NOT shipped).

  mhr_character.qmcb   the MHR (Momentum Human Rig, Apache-2.0) body: mean shape + 45 identity shape vectors, topology, UVs,
                       skin weights, the 127-joint skeleton at its zero pose.
  ict_character.qmcb   the ICT-FaceKit (MIT) head: neutral mesh with UVs and its 17 parts, the 100 PCA identity modes and the
                       51 ARKit expression shapes (fp16 deltas, ~16 MB + ~8 MB).

INPUT
  --mhr-model  mhr_model.pt (from the user's own Meta SAM 3D Body checkout; the same file scripts/export-sam3dbody-onnx.py uses)
  --ict-dir    a directory holding ICT-FaceKit FaceXModel/generic_neutral_mesh.obj + identity000..099.obj
  --ict-npz    ict.npz written by scripts/export-arkit-template.py's reader (neutral/faces/deltas/names; the expression deltas)

CONTAINER ("QMCB1")  little-endian
  magic "QMCB1\\0\\0\\0", u32 blobCount, then per blob:
    u32 nameLen, name bytes, u8 dtype (0 f32, 1 f16, 2 i32, 3 u8), u32 ndim, u64 dims[ndim], u64 byteLen, bytes
A "meta" u8 blob carries JSON (parts, joint names, expression names).

USAGE
  python scripts/build-character-assets.py --mhr-model mhr_model.pt --ict-dir .character_work/ict \\
      --ict-npz .character_work/ict.npz --out .character_work/out
"""
import argparse, json, os, struct
import numpy as np

DT = {np.dtype("<f4"): 0, np.dtype("<f2"): 1, np.dtype("<i4"): 2, np.dtype("u1"): 3}


def write_bundle(path, blobs):
    with open(path, "wb") as f:
        f.write(b"QMCB1\0\0\0")
        f.write(struct.pack("<I", len(blobs)))
        for name, arr in blobs.items():
            arr = np.ascontiguousarray(arr)
            if arr.dtype == np.float32: arr = arr.astype("<f4")
            if arr.dtype == np.int32 or arr.dtype == np.int64: arr = arr.astype("<i4")
            if arr.dtype == np.float16: arr = arr.astype("<f2")
            nb = name.encode()
            f.write(struct.pack("<I", len(nb))); f.write(nb)
            f.write(struct.pack("<B", DT[arr.dtype])); f.write(struct.pack("<I", arr.ndim))
            for d in arr.shape: f.write(struct.pack("<Q", d))
            raw = arr.tobytes(); f.write(struct.pack("<Q", len(raw))); f.write(raw)
    print("wrote", path, os.path.getsize(path) // 1024, "KB")


def meta_blob(obj):
    return np.frombuffer(json.dumps(obj).encode(), dtype="u1")



def clean_faces(base, faces, wjaw):
    """Triangles to drop (returns a keep mask): the zero-thickness SHEET that closes the lips (an edge of a few tenths of a mm whose
    jaw weight jumps from upper-lip to lower-lip values: it stretches over the opening when the jaw moves) and sliver triangles."""
    keep = np.ones(len(faces), bool)
    for t, f in enumerate(faces):
        for a, b in ((0, 1), (1, 2), (2, 0)):
            va, vb = f[a], f[b]
            if (abs(wjaw[va] - wjaw[vb]) > 0.35 and np.linalg.norm(base[va] - base[vb]) < 0.4
                    and base[va][2] > 11 and 154 < base[va][1] < 159):
                keep[t] = False
    A = 0.5 * np.linalg.norm(np.cross(base[faces[:, 1]] - base[faces[:, 0]], base[faces[:, 2]] - base[faces[:, 0]]), axis=1)
    e2 = sum(np.sum((base[faces[:, i]] - base[faces[:, (i + 1) % 3]]) ** 2, axis=1) for i in range(3))
    keep &= (4 * np.sqrt(3) * A / e2) >= 0.1
    return keep

def build_mhr(model_pt, joint_names=None):
    import torch
    sd = {k: v.detach().numpy() for k, v in torch.jit.load(model_pt, map_location="cpu").state_dict().items()}
    g = lambda k: sd["character_torch." + k]
    base = g("blend_shape.base_shape").astype(np.float32)
    vecs = g("blend_shape.shape_vectors").astype(np.float32)
    faces = g("mesh.faces").astype(np.int32)
    tc, tf = g("mesh.texcoords").astype(np.float32), g("mesh.texcoord_faces").astype(np.int32)
    parents = g("skeleton.joint_parents").astype(np.int32)
    offs = g("skeleton.joint_translation_offsets").astype(np.float32)
    prerot = g("skeleton.joint_prerotations").astype(np.float32)          # x y z w
    sj, sw, sv = g("linear_blend_skinning.skin_indices_flattened"), g("linear_blend_skinning.skin_weights_flattened"), \
        g("linear_blend_skinning.vert_indices_flattened")
    order = np.argsort(sv, kind="stable")
    sj, sw, sv = sj[order].astype(np.int32), sw[order].astype(np.float32), sv[order]
    counts = np.bincount(sv, minlength=base.shape[0]).astype(np.int32)
    if joint_names and "c_jaw" in joint_names:
        jaw = joint_names.index("c_jaw")
        wj = np.zeros(base.shape[0])
        for j, w, v in zip(sj, sw, sv):
            if j == jaw: wj[v] = w
        keep = clean_faces(base.astype(np.float64), faces, wj)
        print(f"cleaned {int((~keep).sum())} triangles (lip seam sheet + slivers)")
        global KEEP_MASK
        KEEP_MASK = keep
        faces, tf = faces[keep], tf[keep]
    return {"base": base, "shape_vectors": vecs, "faces": faces, "texcoords": tc, "texcoord_faces": tf, "parents": parents,
            "joint_offsets": offs, "joint_prerot": prerot, "skin_count": counts, "skin_joint": sj, "skin_weight": sw}




def symmetrize(out, names, base):
    """Make the shapes exactly left/right symmetric: a symmetric shape equals its mirror, a Left/Right pair are each other's mirror."""
    from scipy.spatial import cKDTree
    d, m = cKDTree(base).query(base * np.array([-1.0, 1.0, 1.0]))
    S = np.array([-1.0, 1.0, 1.0])
    mirror = lambda f: f[m] * S
    idx = {n: i for i, n in enumerate(names)}
    done = set()
    for n, i in idx.items():
        if n in done: continue
        if n.endswith("Left") and n[:-4] + "Right" in idx:
            j = idx[n[:-4] + "Right"]
            L = 0.5 * (out[i] + mirror(out[j]))
            out[i] = L; out[j] = mirror(L); done |= {n, n[:-4] + "Right"}
        elif not n.endswith(("Left", "Right")):
            out[i] = 0.5 * (out[i] + mirror(out[i])); done.add(n)
    print("mirror map max distance", float(d.max()))
    return out

def build_arkit_on_mhr(sd, joint_names, ict_npz, ict_dir_neutral=None):
    """Express the 51 ARKit shapes with MHR's OWN 72 face-expression vectors so the MHR head can be animated natively.

    Each ICT shape is transferred to the MHR head (ICT neutral aligned to MHR by chin + head height, nearest-vertex delta), then
    projected on the 72 expression vectors with a bounded ridge least squares; the eyelid shapes (blink / squint / wide), which the
    expression basis cannot express, are transferred directly in a mask around each eye. Returns (51, V, 3) float32 deltas in cm.
    """
    from scipy.optimize import lsq_linear
    from scipy.spatial import cKDTree
    E = sd["face_expressions_model.shape_vectors"].astype(np.float64)
    base = sd["character_torch.blend_shape.base_shape"].astype(np.float64)
    V = base.shape[0]
    sj = sd["character_torch.linear_blend_skinning.skin_indices_flattened"]
    sw = sd["character_torch.linear_blend_skinning.skin_weights_flattened"]
    sv = sd["character_torch.linear_blend_skinning.vert_indices_flattened"]
    dom = -np.ones(V, int); best = np.zeros(V)
    for j, w, v in zip(sj, sw, sv):
        if w > best[v]: best[v] = w; dom[v] = j
    headish = np.array([any(t in n for t in ("head", "jaw", "eye", "tongue", "teeth")) for n in joint_names])
    ish = headish[dom]
    cand = np.where(ish & (base[:, 2] > np.mean(base[ish, 2]) + 4))[0]
    chin = cand[np.argmin(base[cand, 1])]
    z = np.load(ict_npz)
    N = z["neutral"].astype(np.float64); D = z["deltas"].astype(np.float64); names = [str(x) for x in z["names"]]
    ichin = N[966]
    sc = (base[:, 1].max() - base[chin, 1]) / (N[:, 1].max() - ichin[1])
    W = (N - ichin) * sc + base[chin]
    W[:, 0] = N[:, 0] * sc   # ICT and MHR are both symmetric about x = 0: do not inherit the chin vertex's tiny lateral offset
    head = np.where(ish & (base[:, 1] > base[chin, 1] - 1.0))[0]
    dist, idx = cKDTree(W).query(base[head])
    ok = dist < 1.2
    H, I = head[ok], idx[ok]
    Eh = E[:, H, :].reshape(72, -1).T
    # eye centres in MHR space: mean of the ICT iris vertices (parts iris_l 22221..23020, iris_r 23791..24590)
    eyeC = [W[22221:23021].mean(0), W[23791:24591].mean(0)]
    # the jaw is a ROTATION about the c_jaw joint: a linear mix of expression vectors cannot reproduce it (the fit saturated and tore
    # the nose), so the jaw shapes are the real skinned motion of the jaw joint: v' = v + w_jaw * (R (v - pivot) + pivot - v)
    par = sd["character_torch.skeleton.joint_parents"].astype(int)
    off = sd["character_torch.skeleton.joint_translation_offsets"].astype(np.float64)
    pre = sd["character_torch.skeleton.joint_prerotations"].astype(np.float64)
    def quat_mat(q):
        x, y, zq, w = q / np.linalg.norm(q)
        return np.array([[1-2*(y*y+zq*zq), 2*(x*y-zq*w), 2*(x*zq+y*w)], [2*(x*y+zq*w), 1-2*(x*x+zq*zq), 2*(y*zq-x*w)], [2*(x*zq-y*w), 2*(y*zq+x*w), 1-2*(x*x+y*y)]])
    Rg = [None] * len(par); Pg = [None] * len(par)
    for j in range(len(par)):
        Rl = quat_mat(pre[j])
        if par[j] < 0: Rg[j] = Rl; Pg[j] = off[j]
        else: Rg[j] = Rg[par[j]] @ Rl; Pg[j] = Pg[par[j]] + Rg[par[j]] @ off[j]
    jaw = joint_names.index("c_jaw")
    wjaw = np.zeros(V)
    for j, w, v in zip(sj, sw, sv):
        if j == jaw: wjaw[v] = w
    pivot = Pg[jaw]
    def rot(axis, deg):
        t = np.radians(deg); c, s_ = np.cos(t), np.sin(t)
        if axis == "x": return np.array([[1, 0, 0], [0, c, -s_], [0, s_, c]])
        return np.array([[c, 0, s_], [0, 1, 0], [-s_, 0, c]])
    def jaw_field(R, shift=(0, 0, 0)):
        moved = (base - pivot) @ R.T + pivot + np.array(shift)
        return (moved - base) * wjaw[:, None]
    jaw_shapes = {"jawOpen": jaw_field(rot("x", 22.0)), "jawLeft": jaw_field(rot("y", 9.0)), "jawRight": jaw_field(rot("y", -9.0)),
                  "jawForward": jaw_field(np.eye(3), (0, 0, 1.2))}
    out = np.zeros((len(names), V, 3), np.float32)
    ridge = 0.03 * np.linalg.norm(Eh, axis=0).mean()   # relative to the basis scale
    A = np.vstack([Eh, ridge * np.eye(72)])
    for k, nm in enumerate(names):
        t = (D[k][I] * sc).reshape(-1)
        r = lsq_linear(A, np.concatenate([t, np.zeros(72)]), bounds=(-3, 3), lsmr_tol="auto", max_iter=300)
        field = np.tensordot(r.x, E, axes=(0, 0))                      # (V, 3)
        if nm.startswith(("eyeBlink", "eyeSquint", "eyeWide", "browDown")) or nm.startswith("eyeLook") is False and False:
            pass
        if nm.startswith(("eyeBlink", "eyeSquint", "eyeWide")):
            side = 0 if nm.endswith("Left") else 1
            # the ICT "left" eye is at +x in ICT; pick the centre by sign of x to be safe
            cs = sorted(eyeC, key=lambda c: -c[0])
            c = cs[0] if nm.endswith("Left") else cs[1]
            d = np.linalg.norm(base[head] - c, axis=1)
            m = np.clip(1 - (d / 3.2) ** 2, 0, 1) * ok
            direct = np.zeros((V, 3)); direct[H] = D[k][I] * sc
            mm = np.zeros(V); mm[head] = m
            field = field * (1 - mm[:, None]) + direct * mm[:, None]
        if nm in jaw_shapes: field = jaw_shapes[nm]
        out[k] = field.astype(np.float32)
        if nm not in jaw_shapes:
            print(f"  {nm:22s} target max {np.abs(t).max():.2f} cm, fitted max {np.abs(field[H]).max():.2f} cm")
    # The MHR face rests with the eyes SHUT (a groove where the lids meet): open them a little in the neutral pose so the eyeballs
    # show, and make the blink shapes the way back to shut.
    eye_open = np.zeros((V, 3), np.float32)
    cs = sorted(eyeC, key=lambda c: -c[0])
    sheet = np.zeros(V, bool)
    core = np.zeros(V)
    for side, c in enumerate(cs):
        c = c + np.array([0.0, EYE_GROOVE_DY, 0.0])   # the MHR lid groove sits ~7 mm above the ICT eyeball centre
        slit = c[1]
        dx = (base[:, 0] - c[0]) / 1.9; dy = (base[:, 1] - c[1]) / 1.2
        m = np.clip(1 - dx * dx - dy * dy, 0, 1) ** 0.7
        m = m * ((base[:, 2] > c[2] - 0.5) & ((base[:, 2] - c[2]) < 4.0))
        eye_open[:, 1] += (np.where(base[:, 1] > slit, UPPER_LID, -LOWER_LID) * m).astype(np.float32)
        sheet |= (m > 0.15) & (np.abs(base[:, 1] - slit) < 1.0)
        core = np.maximum(core, m)
        nm = "eyeBlink" + ("Left" if side == 0 else "Right")
        k = names.index(nm)
        out[k] = np.zeros((V, 3), np.float32)
        out[k][:, 1] = -(np.where(base[:, 1] > slit, UPPER_LID, -LOWER_LID) * m)
    out = symmetrize(out, names, base)
    return out, names, eye_open, sheet, core, [c + np.array([0.0, EYE_GROOVE_DY, 0.0]) for c in cs]

ALMOND_A, ALMOND_B, ALMOND_B_LOW, ALMOND_SHIFT = 1.28, 0.66, 0.30, 0.22   # cm half axes of the eye opening
UPPER_LID, LOWER_LID = 0.30, 0.05   # cm the lids part by (the upper lid folds up into the crease)
CORE_DROP = 2.0   # (disabled: the lids keep their sculpted skin; only the sealing sheet between them is cut)
EYE_GROOVE_DY = -0.45   # cm
PARTS = [  # README of ICT-FaceKit: name, vertex range, polygon range (inclusive)
    ("face", 0, 9408, 0, 9229), ("head_neck", 9409, 11247, 9230, 11143), ("mouth_socket", 11248, 13293, 11144, 13225),
    ("eye_socket_l", 13294, 13677, 13226, 13629), ("eye_socket_r", 13678, 14061, 13630, 14033),
    ("gums_tongue", 14062, 17038, 14034, 17005), ("teeth", 17039, 21450, 17006, 21495),
    ("sclera_l", 21451, 22220, 21496, 22295), ("iris_l", 22221, 23020, 22296, 23093),
    ("sclera_r", 23021, 23790, 23094, 23893), ("iris_r", 23791, 24590, 23894, 24691),
    ("lacrimal_l", 24591, 24794, 24692, 24854), ("lacrimal_r", 24795, 24998, 24855, 25017),
    ("eye_blend_l", 24999, 25022, 25018, 25032), ("eye_blend_r", 25023, 25046, 25033, 25047),
    ("eye_occlusion_l", 25047, 25198, 25048, 25175), ("eye_occlusion_r", 25199, 25350, 25176, 25303),
    ("lashes_l", 25351, 26034, 25304, 25843), ("lashes_r", 26035, 26718, 25844, 26383),
]


def read_obj(path, want_topology):
    v, vt, polys = [], [], []
    with open(path) as fh:
        for line in fh:
            if line.startswith("v "):
                v.append([float(x) for x in line.split()[1:4]])
            elif want_topology and line.startswith("vt "):
                vt.append([float(x) for x in line.split()[1:3]])
            elif want_topology and line.startswith("f "):
                polys.append([tuple(int(i) - 1 if i else -1 for i in (t.split("/") + ["", ""])[:2]) for t in line.split()[1:]])
    return np.array(v, np.float32), np.array(vt, np.float32), polys


def build_ict(ict_dir, npz):
    neutral, vt, polys = read_obj(os.path.join(ict_dir, "generic_neutral_mesh.obj"), True)
    print("neutral", neutral.shape, "uv", vt.shape, "polys", len(polys))
    ident = []
    for i in range(100):
        v, _, _ = read_obj(os.path.join(ict_dir, "identity%03d.obj" % i), False)
        ident.append(v - neutral)
    ident = np.array(ident, np.float32)
    print("identity delta rms per mode (first 5):", np.sqrt((ident[:5] ** 2).mean((1, 2))))
    z = np.load(npz)
    assert z["neutral"].shape == neutral.shape and np.abs(z["neutral"] - neutral).max() < 1e-3, "ict.npz is a different neutral"
    sizes = np.array([len(p) for p in polys], np.int32)
    pv = np.array([c[0] for p in polys for c in p], np.int32)
    pt = np.array([c[1] for p in polys for c in p], np.int32)
    meta = {"parts": [{"name": n, "v0": a, "v1": b, "p0": c, "p1": d} for n, a, b, c, d in PARTS],
            "expression_names": [str(x) for x in z["names"]]}
    return {"neutral": neutral, "texcoords": vt, "poly_size": sizes, "poly_vertex": pv, "poly_texcoord": pt,
            "identity": ident.astype(np.float16), "expression": z["deltas"].astype(np.float16), "meta": meta_blob(meta)}


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--mhr-model", required=True); ap.add_argument("--mhr-joint-names")
    ap.add_argument("--ict-dir", required=True); ap.add_argument("--ict-npz", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--mhr-license", help="MHR LICENSE text (Apache-2.0); copied to LICENSE-MHR.txt")
    ap.add_argument("--ict-license", help="ICT-FaceKit LICENSE text (MIT); copied to LICENSE-ICT-FaceKit.txt")
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
    names = json.load(open(a.mhr_joint_names)) if a.mhr_joint_names else []
    mhr = build_mhr(a.mhr_model, names)
    import torch
    sd_all = {k: v.detach().numpy() for k, v in torch.jit.load(a.mhr_model, map_location="cpu").state_dict().items()}
    arkit, arkit_names, eye_open, sheet, core, eye_c = build_arkit_on_mhr(sd_all, names, a.ict_npz)
    # open the eye slits for real: drop the thin triangles that seal the lids together (they span the slit once the lids part)
    F = mhr["faces"]; TF = mhr["texcoord_faces"]; Bm = mhr["base"].astype(np.float64)
    area = 0.5 * np.linalg.norm(np.cross(Bm[F[:, 1]] - Bm[F[:, 0]], Bm[F[:, 2]] - Bm[F[:, 0]]), axis=1)
    ys = eye_open[:, 1]
    spans = (ys[F].max(1) > 0.05) & (ys[F].min(1) < -0.05)
    # the thin layer between the lids: the exact triangles a user removed by hand from a generated mesh (scripts/mhr_eye_layer_faces.json,
    # indices into MHR's ORIGINAL face list), made left/right symmetric. Nothing else of the lid skin is cut.
    layer = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "mhr_eye_layer_faces.json")))["faces"]
    orig_to_now = {int(o): i for i, o in enumerate(np.where(KEEP_MASK)[0])}
    drop = np.zeros(len(F), bool)
    for o in layer:
        if o in orig_to_now: drop[orig_to_now[o]] = True
    cen = Bm[F].mean(1)
    from scipy.spatial import cKDTree
    tree = cKDTree(cen)
    for i in np.where(drop)[0]:
        d, j2 = tree.query(cen[i] * np.array([-1.0, 1.0, 1.0]))
        if d < 0.05: drop[j2] = True
    print(f"opened the eye slits: dropped {int(drop.sum())} sealing triangles")
    mhr["eye_drop"] = drop.astype(np.uint8)   # applied at build time only when the eyes are opened (the lids stay intact otherwise)
    mhr["eye_open"] = eye_open.astype(np.float16)
    mhr["arkit_delta"] = arkit.astype(np.float16)
    mhr["meta"] = meta_blob({"joint_names": names, "arkit_names": arkit_names})
    write_bundle(os.path.join(a.out, "mhr_character.qmcb"), mhr)
    write_bundle(os.path.join(a.out, "ict_character.qmcb"), build_ict(a.ict_dir, a.ict_npz))
    import shutil
    for src, name in ((a.mhr_license, "LICENSE-MHR.txt"), (a.ict_license, "LICENSE-ICT-FaceKit.txt")):
        if src: shutil.copyfile(src, os.path.join(a.out, name))
