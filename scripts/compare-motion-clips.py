#!/usr/bin/env python3
"""Motion retarget evaluation harness (#837 Slice D).

OFFLINE dev tool — NOT shipped. Two convention-safe comparisons between a
REFERENCE animation and a RETARGETED/generated one:

1. joint-deltas: per-canonical-joint delta-ANGLE trajectories (each clip's
   frames vs its own frame 0) — absolute world quats are NOT comparable
   across rigs (bone-axis conventions differ; the same lesson as skin-weight
   L1), but delta angles are. Reports per-joint amplitude + mean |Δ| error.
2. sheets: pose-shape IoU between two isometric sprite sheets — each cell's
   silhouette is bbox-cropped and scale-normalized so framing/render-style
   differences don't drown the pose signal; per direction the best cyclic
   frame alignment is used.

Reference workflow (LOCAL evaluation only — e.g. Mixamo downloads may not
enter the corpus/training data per Adobe ToS and THIRD_PARTY_AI_MODELS.md):
  qtmesh anim ref.fbx --dump-canonical ref.json
  qtmesh anim target.fbx --generate walk -o gen.glb        # eval library
  qtmesh anim gen.glb --dump-canonical gen.json --animation generated_walk
  python3 scripts/compare-motion-clips.py joint-deltas ref.json gen.json
  qtmesh isometric ref.fbx  --animation mixamo.com     -o ref_%02d.png --frames 6
  qtmesh isometric gen.glb  --animation generated_walk -o gen_%02d.png --frames 6
  python3 scripts/compare-motion-clips.py sheets "ref_%02d.png" "gen_%02d.png"

Baselines recorded 2026-07-11 (Mixamo Walk self-retarget, see PR #843):
mean joint error 1.84°, legs ≤0.15°, IoU 0.816 (pipeline self-ceiling 1.0).
"""
import json
import math
import sys

JN = ["hip", "abdomen", "chest", "neck", "neck1", "head",
      "rcollar", "rshoulder", "relbow", "rhand",
      "lcollar", "lshoulder", "lelbow", "lhand",
      "rbuttock", "rhip", "rknee", "rfoot",
      "lbuttock", "lhip", "lknee", "lfoot"]


def q_ang(a, b):
    return 2 * math.degrees(math.acos(min(1.0, abs(sum(x * y for x, y in zip(a, b))))))


def clip_deltas(path):
    c = json.load(open(path))["clips"][0]
    q = c["quats"]
    return [[q_ang(q[f][j], q[0][j]) for f in range(len(q))]
            for j in range(len(JN))]


def cmd_joint_deltas(ref_path, gen_path):
    A, B = clip_deltas(ref_path), clip_deltas(gen_path)
    n = min(len(A[0]), len(B[0]))
    print(f"{'joint':<10} {'refAmp':>7} {'genAmp':>7} {'meanErr':>8}")
    errs = []
    for j in range(len(JN)):
        err = sum(abs(A[j][f] - B[j][f]) for f in range(n)) / n
        errs.append(err)
        print(f"{JN[j]:<10} {max(A[j][:n]):7.1f} {max(B[j][:n]):7.1f} {err:8.2f}")
    print(f"\nTOTAL mean |Δangle| err: {sum(errs) / len(errs):.2f} deg")


def cmd_sheets(path_a, path_b, rows=8, grid=64):
    from PIL import Image

    def cells(path):
        im = Image.open(path).convert("L")
        w, h = im.size
        ch = h // rows
        cols = max(1, round(w / ch))
        cw = w // cols
        px = im.load()
        out = []
        for r in range(rows):
            row = []
            for c in range(cols):
                pts = [(x - c * cw, y - r * ch)
                       for y in range(r * ch, (r + 1) * ch)
                       for x in range(c * cw, (c + 1) * cw) if px[x, y] > 45]
                if not pts:
                    row.append(frozenset())
                    continue
                xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
                x0, y0 = min(xs), min(ys)
                s = min((grid - 1) / max(1, max(xs) - x0),
                        (grid - 1) / max(1, max(ys) - y0))
                row.append(frozenset((round((x - x0) * s), round((y - y0) * s))
                                     for x, y in pts))
            out.append(row)
        return out

    def iou(a, b):
        if not a and not b:
            return 1.0
        if not a or not b:
            return 0.0
        return len(a & b) / len(a | b)

    A, B = cells(path_a), cells(path_b)
    cols = len(A[0])
    scores = []
    for r in range(rows):
        best = max(sum(iou(A[r][c], B[r][(c + s) % cols])
                       for c in range(cols)) / cols for s in range(cols))
        scores.append(best)
    print("per-direction IoU:", " ".join(f"{v:.3f}" for v in scores))
    print(f"MEAN IoU: {sum(scores) / len(scores):.4f}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "joint-deltas":
        cmd_joint_deltas(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "sheets":
        cmd_sheets(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
