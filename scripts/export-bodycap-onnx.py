#!/usr/bin/env python3
"""SAM 3D Body / MHR spike tooling for performance capture (#870 / #874).

ONE-TIME, OFFLINE developer tool — NOT shipped with the app, and the app never
runs Python. See docs/MOCAP_SPIKE.md for the full spike record and
THIRD_PARTY_AI_MODELS.md for the SAM License verdict.

WHAT IT DOES TODAY
  --mhr-assets <dir>   Extract the Momentum Human Rig (MHR, Apache-2.0)
                       skeleton definition from the released mhr_model.pt
                       (assets.zip of https://github.com/facebookresearch/MHR,
                       v1.0.1) into mhr_skeleton.json:
                         { "joints": [ { "name", "parent",           # index, -1 root
                                         "translationOffset": [x,y,z],  # cm, parent-relative
                                         "preRotation": [x,y,z,w],      # parent-relative
                                         "restWorldRotation": [x,y,z,w],
                                         "restWorldPosition": [x,y,z] } ] }
                       Slice E's retarget needs restWorldRotation for the same
                       W . clip . W^-1 conjugation AnimationMerger::applyMotionClip
                       does with cmuRestWorld. Momentum rest-pose semantics:
                         worldRot(j)  = worldRot(parent) * preRotation(j)
                         worldPos(j)  = worldPos(parent) + worldRot(parent) * translationOffset(j)
                       (joint local animation rotations compose AFTER the
                       pre-rotation: local = preRot * eulerXYZ(joint_params)).

  --ckpt <model.ckpt>  SAM 3D Body ONNX export. NOT RUNNABLE YET in this repo:
                       the checkpoints on HF (facebook/sam-3d-body-dinov3) are
                       GATED — the account whose token sits in
                       ~/.cache/huggingface/token must click through the SAM
                       License acceptance on the model page first (as of
                       2026-07 our token gets 403 on file downloads). The
                       community port that proves the export path is
                       https://github.com/AmmarkoV/Fast-SAM-3D-Body /
                       SAM3DBody-cpp: backbone.onnx (DINOv3-H+, ~4.8 GB fp32,
                       [1,3,H,W] -> [1280,32,32] features), decoder.onnx
                       (~93 MB, 6-layer transformer -> [B,1024] pose token ->
                       FFN heads -> 519 MHR params: global 6D rotation +
                       per-joint Euler XYZ + shape + hands + face).
                       When access is granted, follow that recipe here and
                       validate against the python inference from
                       https://github.com/facebookresearch/sam-3d-body.

USAGE
    python scripts/export-bodycap-onnx.py --mhr-assets .mocap_work/mhr_assets/assets \
        --out .mocap_work/out/mhr_skeleton.json
"""

import argparse
import json
import sys


def quat_mul(a, b):
    """(x,y,z,w) Hamilton product a*b."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def quat_rotate(q, v):
    qv = (v[0], v[1], v[2], 0.0)
    qc = (-q[0], -q[1], -q[2], q[3])
    r = quat_mul(quat_mul(q, qv), qc)
    return (r[0], r[1], r[2])


def extract_mhr_skeleton(assets_dir, out_path):
    import torch  # local import: only needed for this mode

    model = torch.jit.load(f"{assets_dir}/mhr_model.pt", map_location="cpu")
    sk = model.character_torch.skeleton
    names = list(sk.joint_names)
    parents = sk.joint_parents.tolist()
    offsets = sk.joint_translation_offsets.tolist()
    prerot = sk.joint_prerotations.tolist()  # (x,y,z,w), parent-relative
    n = len(names)
    assert len(parents) == len(offsets) == len(prerot) == n

    world_rot = [None] * n
    world_pos = [None] * n
    joints = []
    for j in range(n):
        p = parents[j]
        assert p < j, "joints must be parent-before-child ordered"
        if p < 0:
            world_rot[j] = tuple(prerot[j])
            world_pos[j] = tuple(offsets[j])
        else:
            world_rot[j] = quat_mul(world_rot[p], tuple(prerot[j]))
            off = quat_rotate(world_rot[p], tuple(offsets[j]))
            wp = world_pos[p]
            world_pos[j] = (wp[0] + off[0], wp[1] + off[1], wp[2] + off[2])
        joints.append({
            "name": names[j],
            "parent": p,
            "translationOffset": [round(v, 6) for v in offsets[j]],
            "preRotation": [round(v, 8) for v in prerot[j]],
            "restWorldRotation": [round(v, 8) for v in world_rot[j]],
            "restWorldPosition": [round(v, 6) for v in world_pos[j]],
        })

    doc = {
        "format": "qtmesh-mhr-skeleton-v1",
        "source": "facebookresearch/MHR v1.0.1 assets (Apache-2.0)",
        "units": "cm",
        "quaternionOrder": "xyzw",
        "jointCount": n,
        "joints": joints,
    }
    with open(out_path, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"{n} joints -> {out_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mhr-assets", help="dir containing mhr_model.pt")
    ap.add_argument("--out", default=".mocap_work/out/mhr_skeleton.json")
    ap.add_argument("--ckpt", help="SAM 3D Body model.ckpt (gated; see header)")
    args = ap.parse_args()

    if args.mhr_assets:
        extract_mhr_skeleton(args.mhr_assets, args.out)
    if args.ckpt:
        sys.exit("SAM 3D Body ONNX export: checkpoint access is gated (see the "
                 "header comment). Accept the SAM License on "
                 "https://huggingface.co/facebook/sam-3d-body-dinov3 with the "
                 "HF account of ~/.cache/huggingface/token, then implement the "
                 "Fast-SAM-3D-Body recipe here (tracked in #874).")
    if not args.mhr_assets and not args.ckpt:
        sys.exit("nothing to do: pass --mhr-assets and/or --ckpt")


if __name__ == "__main__":
    main()
