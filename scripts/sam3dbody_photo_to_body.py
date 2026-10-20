#!/usr/bin/env python3
"""Photo -> neutral MHR body mesh, with Meta's SAM 3D Body (#1100 spike; not wired into the editor yet).

Our code (MIT); it imports Meta's `sam_3d_body` package from YOUR local clone and loads checkpoints YOU downloaded after
accepting Meta's SAM License on Hugging Face (facebook/sam-3d-body-dinov3). Nothing of Meta's is bundled or redistributed.

  git clone https://github.com/facebookresearch/sam-3d-body            # Meta's code, SAM License
  python3.11 -m venv venv && venv/bin/pip install torch torchvision numpy onnx onnxruntime pillow opencv-python-headless \
      omegaconf hydra-core yacs pytorch-lightning einops timm braceexpand roma smplx trimesh termcolor submitit scikit-learn
  # CPU only: Meta's estimator hard-codes .cuda() in sam_3d_body_estimator.py and meta_arch/sam3d_body.py — change those
  # to .cpu()/"cpu" in YOUR clone (a local modification; do not redistribute it).
  venv/bin/python scripts/sam3dbody_photo_to_body.py --repo sam-3d-body --ckpt-dir <dir with model.ckpt, assets/mhr_model.pt> \
      --image photo.jpg --bbox 140,20,730,1225 --out-prefix out/person

Writes <prefix>_neutral_facing_pz.obj / _facing_nz.obj (the identity-only body in MHR's neutral A-pose, metres, Y up; the
MetaHuman import faces -Z, so use *_nz with `qtmesh dna fit --register`), and <prefix>_posed.obj (the pose in the photo,
camera space). Without --bbox a detector would be needed (detectron2); a hand-written box is enough for a single person.
"""
import argparse, os, sys, time
import numpy as np


def write_obj(path, v, f):
    with open(path, "w") as fh:
        for p in v:
            fh.write("v %f %f %f\n" % tuple(p))
        for t in f:
            fh.write("f %d %d %d\n" % (t[0] + 1, t[1] + 1, t[2] + 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--ckpt-dir", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--bbox", required=True, help="x1,y1,x2,y2 of the person in pixels")
    ap.add_argument("--out-prefix", required=True)
    a = ap.parse_args()
    sys.path.insert(0, a.repo)
    import torch
    from sam_3d_body import load_sam_3d_body, SAM3DBodyEstimator

    t0 = time.time()
    model, cfg = load_sam_3d_body(os.path.join(a.ckpt_dir, "model.ckpt"), device=torch.device("cpu"),
                                  mhr_path=os.path.join(a.ckpt_dir, "assets", "mhr_model.pt"))
    est = SAM3DBodyEstimator(sam_3d_body_model=model, model_cfg=cfg, human_detector=None, human_segmentor=None,
                             fov_estimator=None)
    print("loaded in %.1fs" % (time.time() - t0))
    bbox = np.array([[float(x) for x in a.bbox.split(",")]], dtype=np.float32)
    t0 = time.time()
    with torch.no_grad():
        o = est.process_one_image(a.image, bboxes=bbox)[0]
    print("inference %.1fs" % (time.time() - t0))
    f = np.asarray(est.faces)
    os.makedirs(os.path.dirname(os.path.abspath(a.out_prefix)), exist_ok=True)
    write_obj(a.out_prefix + "_posed.obj", np.asarray(o["pred_vertices"]), f)

    T = lambda k: torch.as_tensor(np.asarray(o[k]), dtype=torch.float32)[None]
    z = lambda n: torch.zeros(1, n)
    with torch.no_grad():   # identity only: predicted shape + scale, zero pose, zero expression
        verts = model.head_pose.mhr_forward(global_trans=z(3), global_rot=z(3), body_pose_params=z(133),
                                            hand_pose_params=z(108), scale_params=T("scale_params"),
                                            shape_params=T("shape_params"), expr_params=z(72),
                                            return_keypoints=False, return_joint_coords=True)[0][0].numpy()
    print("neutral body: height %.2f m" % (verts[:, 1].max() - verts[:, 1].min()))
    write_obj(a.out_prefix + "_neutral_facing_pz.obj", verts, f)
    write_obj(a.out_prefix + "_neutral_facing_nz.obj", verts * np.array([-1, 1, -1]), f[:, [0, 2, 1]])


if __name__ == "__main__":
    main()
