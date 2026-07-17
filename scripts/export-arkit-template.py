#!/usr/bin/env python3
"""Pack the ICT-FaceKit ARKit template into QtMeshEditor's face-rig bundle (#890).

ONE-TIME, OFFLINE developer tool — NOT shipped. Produces the compact binary
`arkit_template.bin` that `src/FaceRig/ArkitTemplate.cpp` reads and the app
downloads on first use to AppData/ai_models/facerig/. The face-rig feature
(#889) uses it as the deformation-transfer SOURCE.

INPUT
  A directory of ICT-FaceKit FaceXModel .obj files (MIT, USC-ICT):
  generic_neutral_mesh.obj + the per-expression meshes (same topology, so a
  blendshape = expr - neutral). Download from
  https://github.com/USC-ICT/ICT-FaceKit/tree/master/FaceXModel

OUTPUT  arkit_template.bin  — little-endian:
  magic   "QMFRT1\0\0"                (8 bytes)
  int32   vertexCount V
  int32   faceCount   F
  int32   shapeCount  S               (== 52)
  float32 neutral[V*3]                (template neutral positions)
  int32   faces[F*3]                  (triangle vertex indices)
  then S shape records:
    char[32] name  (ASCII, NUL-padded — a FaceCap::kBlendshapeNames entry)
    float32  delta[V*3]  (expr - neutral; most verts are ~0)
  ("_neutral" is NOT stored as a shape — it is the base.)

The 52 ARKit names + the ICT->ARKit mapping are baked here; some ARKit
channels are SINGLE/centered (browInnerUp, cheekPuff, mouthClose, mouthFunnel,
mouthPucker, jaw*, mouthRoll*/Shrug*) while ICT splits a few as _L/_R — for
those the ARKit delta is the SUM of the ICT halves. The rest map 1:1.

USAGE
  python scripts/export-arkit-template.py --ict-dir .facerig_work/ict_full \
      --out .facerig_work/out/arkit_template.bin
"""

import argparse
import os
import struct
import sys

import numpy as np

# 52 ARKit names in FaceCap::kBlendshapeNames order (index 0 "_neutral" is the
# base, not a shape). Each maps to a list of ICT stems whose deltas SUM to it.
ARKIT = [
    ("browDownLeft", ["browDown_L"]), ("browDownRight", ["browDown_R"]),
    ("browInnerUp", ["browInnerUp_L", "browInnerUp_R"]),
    ("browOuterUpLeft", ["browOuterUp_L"]), ("browOuterUpRight", ["browOuterUp_R"]),
    ("cheekPuff", ["cheekPuff_L", "cheekPuff_R"]),
    ("cheekSquintLeft", ["cheekSquint_L"]), ("cheekSquintRight", ["cheekSquint_R"]),
    ("eyeBlinkLeft", ["eyeBlink_L"]), ("eyeBlinkRight", ["eyeBlink_R"]),
    ("eyeLookDownLeft", ["eyeLookDown_L"]), ("eyeLookDownRight", ["eyeLookDown_R"]),
    ("eyeLookInLeft", ["eyeLookIn_L"]), ("eyeLookInRight", ["eyeLookIn_R"]),
    ("eyeLookOutLeft", ["eyeLookOut_L"]), ("eyeLookOutRight", ["eyeLookOut_R"]),
    ("eyeLookUpLeft", ["eyeLookUp_L"]), ("eyeLookUpRight", ["eyeLookUp_R"]),
    ("eyeSquintLeft", ["eyeSquint_L"]), ("eyeSquintRight", ["eyeSquint_R"]),
    ("eyeWideLeft", ["eyeWide_L"]), ("eyeWideRight", ["eyeWide_R"]),
    ("jawForward", ["jawForward"]), ("jawLeft", ["jawLeft"]),
    ("jawOpen", ["jawOpen"]), ("jawRight", ["jawRight"]),
    ("mouthClose", ["mouthClose"]),
    ("mouthDimpleLeft", ["mouthDimple_L"]), ("mouthDimpleRight", ["mouthDimple_R"]),
    ("mouthFrownLeft", ["mouthFrown_L"]), ("mouthFrownRight", ["mouthFrown_R"]),
    ("mouthFunnel", ["mouthFunnel"]), ("mouthLeft", ["mouthLeft"]),
    ("mouthLowerDownLeft", ["mouthLowerDown_L"]),
    ("mouthLowerDownRight", ["mouthLowerDown_R"]),
    ("mouthPressLeft", ["mouthPress_L"]), ("mouthPressRight", ["mouthPress_R"]),
    ("mouthPucker", ["mouthPucker"]), ("mouthRight", ["mouthRight"]),
    ("mouthRollLower", ["mouthRollLower"]), ("mouthRollUpper", ["mouthRollUpper"]),
    ("mouthShrugLower", ["mouthShrugLower"]), ("mouthShrugUpper", ["mouthShrugUpper"]),
    ("mouthSmileLeft", ["mouthSmile_L"]), ("mouthSmileRight", ["mouthSmile_R"]),
    ("mouthStretchLeft", ["mouthStretch_L"]), ("mouthStretchRight", ["mouthStretch_R"]),
    ("mouthUpperUpLeft", ["mouthUpperUp_L"]), ("mouthUpperUpRight", ["mouthUpperUp_R"]),
    ("noseSneerLeft", ["noseSneer_L"]), ("noseSneerRight", ["noseSneer_R"]),
]

MAGIC = b"QMFRT1\0\0"


def load_obj(path):
    V, F = [], []
    with open(path) as f:
        for ln in f:
            if ln.startswith("v "):
                V.append([float(x) for x in ln.split()[1:4]])
            elif ln.startswith("f "):
                # Fan-triangulate: ICT-FaceKit meshes are QUADS. Taking only
                # the first 3 indices dropped every quad's second triangle,
                # shipping a template with half its faces missing (holed
                # renders, fragmented connectivity).
                idx = [int(t.split("/")[0]) - 1 for t in ln.split()[1:]]
                for k in range(1, len(idx) - 1):
                    F.append([idx[0], idx[k], idx[k + 1]])
    return np.asarray(V, np.float64), np.asarray(F, np.int32)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ict-dir", required=True)
    ap.add_argument("--out", default=".facerig_work/out/arkit_template.bin")
    args = ap.parse_args()

    neutral, faces = load_obj(os.path.join(args.ict_dir, "generic_neutral_mesh.obj"))
    V = len(neutral)
    print(f"neutral: {V} verts / {len(faces)} tris")

    shapes = []
    for arkit_name, ict_stems in ARKIT:
        delta = np.zeros((V, 3), np.float64)
        missing = []
        for stem in ict_stems:
            p = os.path.join(args.ict_dir, stem + ".obj")
            if not os.path.exists(p):
                missing.append(stem)
                continue
            ev, _ = load_obj(p)
            if len(ev) != V:
                sys.exit(f"topology mismatch: {stem} has {len(ev)} verts, "
                         f"neutral {V}")
            delta += ev - neutral
        if missing:
            print(f"  !! {arkit_name}: missing ICT {missing} — zero shape")
        # count verts moved by a MEANINGFUL amount (0.1% of the head
        # diagonal), not floating-point dust, so the log reflects real motion.
        _diag = float(np.linalg.norm(neutral.max(0) - neutral.min(0)))
        moved = int((np.linalg.norm(delta, axis=1) > 1e-3 * _diag).sum())
        shapes.append((arkit_name, delta.astype(np.float32)))
        print(f"  {arkit_name:22s} <- {'+'.join(ict_stems):24s} moved {moved} verts")

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<iii", V, len(faces), len(shapes)))
        f.write(neutral.astype("<f4").tobytes())
        f.write(faces.astype("<i4").tobytes())
        for name, delta in shapes:
            nm = name.encode("ascii")[:31]
            f.write(nm + b"\0" * (32 - len(nm)))
            f.write(delta.astype("<f4").tobytes())

    size = os.path.getsize(args.out)
    print(f"\n{len(shapes)} shapes -> {args.out} ({size/1e6:.1f} MB)")


if __name__ == "__main__":
    sys.exit(main())
