#!/usr/bin/env python3
"""Face-rig spike (#896): prove auto-ARKit-blendshape generation is viable.

ONE-TIME, OFFLINE feasibility prototype — NOT shipped, NOT wired to CMake/CI.
It validates the two algorithms Slices C/D will implement natively in C++:

  1. Non-rigid ICP (NRICP, Amberg et al. 2007 optimal-step) to fit the
     ICT-FaceKit generic-neutral template to an arbitrary USER neutral head,
     producing a per-template-vertex correspondence on the user surface.
  2. Deformation transfer (Sumner & Popovic 2004) of each of ICT's 52
     ARKit-named expression shapes onto the USER mesh topology.

Output: for each expression, a new blendshape (per-user-vertex delta) that,
added to the user neutral, reproduces that expression on the user's identity.

DATA + LICENSE
  Template = ICT-FaceKit (USC-ICT), MIT — generic_neutral_mesh.obj + the
  per-expression *.obj (same topology; shape = expr - neutral). See
  THIRD_PARTY_AI_MODELS.md.

CONTRACT this measures (for docs/FACE_RIG_SPIKE.md / Slices C-D):
  - template: N_t verts, F_t tris; 52 ARKit shape names (ICT naming ->
    FaceCap::kBlendshapeNames mapping printed below).
  - NRICP: point-to-point + stiffness-annealed regularization on the template
    edge graph; returns fitted template vertex positions lying on the user
    surface (= correspondence).
  - deformation transfer: per-triangle affine from (neutral tri -> expr tri)
    on the template, retargeted to the user tris via the correspondence,
    solved as one sparse least-squares for user vertex positions.

USAGE (offline, venv with numpy scipy trimesh):
  python scripts/spike-facerig.py \
      --template-dir .facerig_work/ict \
      --user .facerig_work/user_neutral.obj \
      --out-dir .facerig_work/out [--shapes jawOpen,mouthSmile_L,...]
"""

import argparse
import glob
import os
import sys

import numpy as np
import trimesh
from scipy.sparse import coo_matrix, csr_matrix, vstack
from scipy.sparse.linalg import lsqr
from scipy.spatial import cKDTree


# ICT expression-file stem -> ARKit-52 canonical name (FaceCap order). ICT
# splits L/R and uses slightly different stems; this is the mapping Slice B
# bakes in. (Subset shown covers the spike; full 52 in the doc.)
ICT_TO_ARKIT = {
    "browDown_L": "browDownLeft", "browDown_R": "browDownRight",
    "browInnerUp_L": "browInnerUp", "browInnerUp_R": "browInnerUp",
    "browOuterUp_L": "browOuterUpLeft", "browOuterUp_R": "browOuterUpRight",
    "cheekPuff_L": "cheekPuff", "cheekPuff_R": "cheekPuff",
    "cheekSquint_L": "cheekSquintLeft", "cheekSquint_R": "cheekSquintRight",
    "eyeBlink_L": "eyeBlinkLeft", "eyeBlink_R": "eyeBlinkRight",
    "eyeLookDown_L": "eyeLookDownLeft", "eyeLookDown_R": "eyeLookDownRight",
    "eyeLookIn_L": "eyeLookInLeft", "eyeLookIn_R": "eyeLookInRight",
    "eyeLookOut_L": "eyeLookOutLeft", "eyeLookOut_R": "eyeLookOutRight",
    "eyeLookUp_L": "eyeLookUpLeft", "eyeLookUp_R": "eyeLookUpRight",
    "eyeSquint_L": "eyeSquintLeft", "eyeSquint_R": "eyeSquintRight",
    "eyeWide_L": "eyeWideLeft", "eyeWide_R": "eyeWideRight",
    "jawForward": "jawForward", "jawLeft": "jawLeft", "jawRight": "jawRight",
    "jawOpen": "jawOpen",
    "mouthClose": "mouthClose",
    "mouthSmile_L": "mouthSmileLeft", "mouthSmile_R": "mouthSmileRight",
    "mouthFrown_L": "mouthFrownLeft", "mouthFrown_R": "mouthFrownRight",
}


def load_obj(path):
    # Manual v/f parse — trimesh loads these OBJs as multi-group Scenes and
    # reorders/duplicates verts, which breaks the shared-topology assumption
    # (shape delta = expr - neutral requires identical vertex order).
    V, F = [], []
    with open(path) as f:
        for ln in f:
            if ln.startswith("v "):
                V.append([float(x) for x in ln.split()[1:4]])
            elif ln.startswith("f "):
                # Fan-triangulate: ICT-FaceKit meshes are QUADS (same fix as
                # export-arkit-template.py — first-3-indices dropped half the
                # triangles).
                idx = [int(t.split("/")[0]) - 1 for t in ln.split()[1:]]
                for k in range(1, len(idx) - 1):
                    F.append([idx[0], idx[k], idx[k + 1]])
    return np.asarray(V, dtype=np.float64), np.asarray(F, dtype=np.int64)


# ---------------------------------------------------------------------------
# rigid pre-align (Umeyama with scale) — template onto user
# ---------------------------------------------------------------------------

def umeyama(src, dst):
    mu_s, mu_d = src.mean(0), dst.mean(0)
    S, D = src - mu_s, dst - mu_d
    cov = D.T @ S / len(src)
    U, d, Vt = np.linalg.svd(cov)
    R = U @ Vt
    if np.linalg.det(R) < 0:
        U[:, -1] *= -1
        R = U @ Vt
    var = (S ** 2).sum() / len(src)
    s = d.sum() / var
    t = mu_d - s * R @ mu_s
    return s, R, t


# ---------------------------------------------------------------------------
# NRICP (Amberg 2007 optimal-step, point-to-point)
# ---------------------------------------------------------------------------

def edge_incidence(faces, n):
    edges = set()
    for a, b, c in faces:
        for i, j in ((a, b), (b, c), (c, a)):
            edges.add((min(i, j), max(i, j)))
    edges = list(edges)
    rows, cols, vals = [], [], []
    for k, (i, j) in enumerate(edges):
        rows += [k, k]; cols += [i, j]; vals += [-1.0, 1.0]
    M = coo_matrix((vals, (rows, cols)), shape=(len(edges), n))
    return M.tocsr()


def nricp(tmpl_v, tmpl_f, user_v, user_f,
          stiffness=(50, 20, 8, 3, 1, 0.5), iters_per=3):
    """Fit template verts onto the user surface. Returns fitted positions X
    (template-indexed, lying near the user surface) = the correspondence."""
    n = len(tmpl_v)
    # correspondence-free rigid pre-align: match centroid + bbox scale (the
    # point sets differ in count, so a Procrustes needs correspondence we
    # don't have yet; NRICP refines from this coarse start). Axes already
    # agree (both +Y up, facing +Z) since ICT and the decimated user share
    # the source orientation; a real user mesh may need axis detection first.
    s = np.linalg.norm(user_v.max(0) - user_v.min(0)) / \
        max(np.linalg.norm(tmpl_v.max(0) - tmpl_v.min(0)), 1e-9)
    R = np.eye(3)
    t = user_v.mean(0) - s * tmpl_v.mean(0)
    X = (s * (tmpl_v @ R.T)) + t
    user_tri = user_v[user_f]
    user_centroids = user_tri.mean(1)
    tree = cKDTree(user_centroids)

    M = edge_incidence(tmpl_f, n)          # (E, n) node-arc incidence
    G = np.array([1, 1, 1, 1.0])           # per-vertex 3x4 affine weight
    kron = None                            # built lazily

    # unknown: per-vertex 3x4 affine A_i; template homogeneous coords
    Th = np.hstack([tmpl_v, np.ones((n, 1))])   # (n,4)

    for alpha in stiffness:
        for _ in range(iters_per):
            # data term: A_i * th_i ~= closest user point to current X_i
            _, tri_idx = tree.query(X)
            # closest point = project X onto that triangle (approx: centroid-nudged
            # to nearest vertex of the tri for a cheap point-to-point target)
            target = closest_on_tris(X, user_v, user_f, tri_idx)

            # Build sparse system:  [ alpha * (M kron G) ; D ] A = [ 0 ; target ]
            # A is stacked as (4n x 3). D picks th_i per row.
            D = csr_matrix((np.repeat(1.0, n),
                            (np.arange(n), np.arange(n))), shape=(n, n))
            # data rows: for each vertex, th_i (1x4) times A_i (4x3) -> point
            data_rows = build_data(Th)                      # (n, 4n)
            stiff = build_stiffness(M, alpha)               # (4E, 4n)
            Amat = vstack([stiff, data_rows]).tocsr()
            rhs = np.vstack([np.zeros((stiff.shape[0], 3)), target])
            Asol = np.zeros((4 * n, 3))
            for axis in range(3):
                Asol[:, axis] = lsqr(Amat, rhs[:, axis], atol=1e-6, btol=1e-6,
                                     iter_lim=400)[0]
            X = apply_affine(Th, Asol)
    return X


def build_data(Th):
    n = len(Th)
    rows, cols, vals = [], [], []
    for i in range(n):
        for k in range(4):
            rows.append(i); cols.append(4 * i + k); vals.append(Th[i, k])
    return coo_matrix((vals, (rows, cols)), shape=(n, 4 * n)).tocsr()


def build_stiffness(M, alpha):
    # (M kron I4) * alpha, gamma weighting on the translation column left at 1
    E, n = M.shape
    Mc = M.tocoo()
    rows, cols, vals = [], [], []
    for r, c, v in zip(Mc.row, Mc.col, Mc.data):
        for k in range(4):
            rows.append(4 * r + k); cols.append(4 * c + k); vals.append(alpha * v)
    return coo_matrix((vals, (rows, cols)), shape=(4 * E, 4 * n)).tocsr()


def apply_affine(Th, Asol):
    n = len(Th)
    X = np.zeros((n, 3))
    for i in range(n):
        Ai = Asol[4 * i:4 * i + 4, :]   # (4,3)
        X[i] = Th[i] @ Ai
    return X


def closest_on_tris(P, V, F, tri_idx):
    """Point-to-triangle projection of each P[i] onto tri F[tri_idx[i]]."""
    out = np.empty_like(P)
    for i in range(len(P)):
        a, b, c = V[F[tri_idx[i]]]
        out[i] = closest_point_triangle(P[i], a, b, c)
    return out


def closest_point_triangle(p, a, b, c):
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = ab @ ap, ac @ ap
    if d1 <= 0 and d2 <= 0:
        return a
    bp = p - b
    d3, d4 = ab @ bp, ac @ bp
    if d3 >= 0 and d4 <= d3:
        return b
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        return a + (d1 / (d1 - d3)) * ab
    cp = p - c
    d5, d6 = ab @ cp, ac @ cp
    if d6 >= 0 and d5 <= d6:
        return c
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        return a + (d2 / (d2 - d6)) * ac
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        return b + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * (c - b)
    denom = 1.0 / (va + vb + vc)
    v, w = vb * denom, vc * denom
    return a + ab * v + ac * w


# ---------------------------------------------------------------------------
# deformation transfer (Sumner & Popovic 2004), correspondence-based
# ---------------------------------------------------------------------------

def tri_frame(v0, v1, v2):
    """3x3 frame [e1 e2 n] for a triangle (n = normalized cross / sqrt)."""
    e1 = v1 - v0
    e2 = v2 - v0
    n = np.cross(e1, e2)
    ln = np.linalg.norm(n)
    n = n / np.sqrt(ln) if ln > 1e-12 else n
    return np.column_stack([e1, e2, n])


def deformation_transfer(user_v, user_f, src_neutral_corr, src_expr_corr):
    """Transfer the src (template, expressed via the correspondence) per-tri
    deformation onto the user mesh. src_*_corr are template-vertex positions
    (neutral / expression) already living on the user identity via NRICP; we
    solve for user vertex positions whose per-tri deformation matches.

    Simplified spike form: since the correspondence maps template verts onto
    the user surface 1:1, the transferred user delta is the correspondence
    delta resampled to user verts via nearest correspondence point."""
    # correspondence points move by (expr - neutral); map that displacement to
    # each user vertex by nearest correspondence point (KD-tree).
    disp = src_expr_corr - src_neutral_corr
    tree = cKDTree(src_neutral_corr)
    _, idx = tree.query(user_v)
    return disp[idx]        # per-user-vertex delta


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--template-dir", default=".facerig_work/ict")
    ap.add_argument("--user", required=True, help="user neutral head .obj")
    ap.add_argument("--out-dir", default=".facerig_work/out")
    ap.add_argument("--shapes", default="",
                    help="comma ICT stems (default: all *.obj present)")
    ap.add_argument("--gt-dir", default="",
                    help="optional: user-identity ground-truth expr .obj dir "
                         "(same names as template) for RMS quality")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    tv, tf = load_obj(os.path.join(args.template_dir, "generic_neutral_mesh.obj"))
    uv, uf = load_obj(args.user)
    print(f"template: {len(tv)} verts / {len(tf)} tris")
    print(f"user:     {len(uv)} verts / {len(uf)} tris")

    print("running NRICP (template -> user neutral)...")
    corr = nricp(tv, tf, uv, uf)
    fit_err = np.linalg.norm(
        corr - closest_on_tris(corr, uv, uf, cKDTree(uv[uf].mean(1)).query(corr)[1]),
        axis=1)
    diag = np.linalg.norm(uv.max(0) - uv.min(0))
    print(f"  NRICP surface fit: mean {fit_err.mean()/diag*100:.3f}% of diag, "
          f"max {fit_err.max()/diag*100:.3f}%")

    if args.shapes:
        stems = args.shapes.split(",")
    else:
        stems = [os.path.splitext(os.path.basename(p))[0]
                 for p in glob.glob(os.path.join(args.template_dir, "*.obj"))
                 if "neutral" not in p]

    report = {"template_verts": len(tv), "user_verts": len(uv),
              "nricp_mean_pct": float(fit_err.mean() / diag * 100), "shapes": {}}
    for stem in stems:
        ep = os.path.join(args.template_dir, stem + ".obj")
        if not os.path.exists(ep):
            print(f"  skip {stem} (missing)"); continue
        ev, _ = load_obj(ep)
        # express the template expression through the SAME correspondence:
        # correspondence of the expression = corr + (expr - neutral) template delta
        expr_corr = corr + (ev - tv)
        user_delta = deformation_transfer(uv, uf, corr, expr_corr)
        arkit = ICT_TO_ARKIT.get(stem, stem)
        mag = np.linalg.norm(user_delta, axis=1)
        entry = {"arkit": arkit, "max_disp_pct": float(mag.max() / diag * 100),
                 "moved_verts": int((mag > 1e-4 * diag).sum())}
        # optional GT RMS
        gt = os.path.join(args.gt_dir, stem + ".obj") if args.gt_dir else ""
        if gt and os.path.exists(gt):
            gtv, _ = load_obj(gt)
            rms = np.sqrt(((uv + user_delta - gtv) ** 2).sum(1).mean())
            entry["rms_pct"] = float(rms / diag * 100)
        report["shapes"][stem] = entry
        # write the blendshape target (neutral + delta) for visual inspection
        trimesh.Trimesh(uv + user_delta, uf, process=False).export(
            os.path.join(args.out_dir, f"user_{arkit}.obj"))
        print(f"  {stem:16s} -> {arkit:18s} maxDisp {entry['max_disp_pct']:.2f}% "
              f"moved {entry['moved_verts']} verts"
              + (f"  RMS {entry['rms_pct']:.3f}%" if "rms_pct" in entry else ""))

    import json
    with open(os.path.join(args.out_dir, "facerig_spike_report.json"), "w") as f:
        json.dump(report, f, indent=2)
    print(f"\nreport -> {os.path.join(args.out_dir, 'facerig_spike_report.json')}")


if __name__ == "__main__":
    sys.exit(main())
