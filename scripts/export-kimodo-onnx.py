#!/usr/bin/env python3
"""Export NVIDIA Kimodo (Kimodo-SOMA-RP-v1.2) to ONNX for QtMeshEditor's native runtime (dev tool, not shipped).

QtMeshEditor runs Kimodo without Python: the text encoder through its bundled llama.cpp
(scripts/export-kimodo-text-encoder-gguf.py) and the motion model through ONNX Runtime. This script writes:

  kimodo_denoiser.onnx    the two-stage transformer denoiser (B=1, dynamic frame count)
  kimodo_decode.onnx      un-normalized features -> SOMA-30 local/global rotations, root, posed joints, contacts
  kimodo_encode.onnx      SOMA-30 local rotations + root -> un-normalized features (post-processing round trip)
  kimodo_transition.onnx  last frames of a segment -> the next segment's transition conditioning
  kimodo_meta.json        normalization stats, diffusion schedule, skeletons, post-processing rig

The sampling loop, classifier-free guidance, segment stitching and foot post-processing run in C++
(src/AIMotion/KimodoNative.cpp) exactly as kimodo/model/kimodo_model.py does them; --verify replays that C++
algorithm in numpy + onnxruntime and compares it with Kimodo's own PyTorch output on the same noise.

    python3 scripts/export-kimodo-onnx.py --out ~/kimodo_export/onnx --verify
"""

import argparse
import json
import math
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

MODEL = "Kimodo-SOMA-RP-v1.2"
TRANSITION_FRAMES = 5   # kimodo's default num_transition_frames (ai/kimodo/generate.py --transition-frames)
FEATURE_SIZE = 369


class _NoText:
    """Stand-in text encoder: the export never encodes text (text features are graph inputs)."""

    device, dtype = "cpu", None

    def to(self, device=None, dtype=None):
        return self

    def eval(self):
        return self

    def __call__(self, texts):
        raise RuntimeError("text is encoded outside the graphs")


def _plain_fk(rot_mats, joints, parents, idx_levs, root_idx: int):
    """kimodo.skeleton.kinematics.forward_kinematics without @torch.jit.script: the scripted loop over
    `idx_levs` exports as an untyped ONNX sequence loop; a plain loop unrolls over the (constant) levels."""
    joints = torch.unsqueeze(joints, dim=-1)
    is_root = (torch.arange(joints.shape[1]) == root_idx).reshape(1, -1, 1, 1)
    rel_joints = torch.where(is_root, joints, joints - joints[:, parents.clamp(min=0)])
    R = rot_mats.reshape(-1, 3, 3)
    t = rel_joints.reshape(-1, 3, 1)
    transforms_mat = torch.cat([F.pad(R, [0, 0, 0, 1]), F.pad(t, [0, 0, 0, 1], value=1.0)], dim=2).reshape(
        -1, joints.shape[1], 4, 4)
    transforms = torch.zeros_like(transforms_mat)
    transforms[:, root_idx] = transforms_mat[:, root_idx]
    for indices in idx_levs:
        transforms[:, indices] = torch.matmul(transforms[:, parents[indices]], transforms_mat[:, indices])
    return transforms[:, :, :3, 3], transforms[:, :, :3, :3]


class _EinopsShim:
    """kinematics.fk repeats the rest joints with einops `b=len(local_joint_rots)`: len() turns the (frame)
    batch into a Python int, which pins the exported frame count. expand() keeps it symbolic."""

    def __init__(self, real):
        self._real = real

    def repeat(self, tensor, pattern, **sizes):
        if pattern == "j k -> b j k" and set(sizes) == {"b"}:
            return tensor[None].expand(sizes["b"], -1, -1)
        return self._real.repeat(tensor, pattern, **sizes)

    def __getattr__(self, name):
        return getattr(self._real, name)


def load_kimodo():
    import kimodo.skeleton.kinematics as kin
    kin.einops = _EinopsShim(kin.einops)
    kin.len = lambda t: t.shape[0] if isinstance(t, torch.Tensor) else len(t)   # module global shadows the builtin
    from kimodo.motion_rep.stats import Stats
    kin.forward_kinematics = _plain_fk
    Stats.__hash__ = object.__hash__   # content hash calls .numpy(), which torch.export's fake tensors refuse
    from kimodo import load_model
    model = load_model(MODEL, device="cpu", default_family="Kimodo", text_encoder=_NoText())
    for mod in model.modules():   # fresh float32 copies: a cast through .data keeps a float64 view base,
        for name, b in list(mod._buffers.items()):   # which torch.export rejects
            if b is not None and (b.dtype == torch.float64 or b._base is not None):
                mod._buffers[name] = (b.float() if b.dtype == torch.float64 else b).clone().contiguous()
        for name, p in list(mod._parameters.items()):
            if p is not None and p.dtype == torch.float64:
                p.data = p.data.float().clone()
    for m in model.modules():   # non-persistent buffers registered on plain modules / stats objects
        for k, v in list(vars(m).items()):
            if isinstance(v, torch.Tensor) and v.dtype == torch.float64:
                setattr(m, k, v.float())
    # FK depth levels are data-dependent when computed inside the trace: precompute them per skeleton, keyed by
    # the (static) joint count, before any export runs.
    original_levels = kin.compute_idx_levels
    levels = {}
    for skel in (model.skeleton, model.skeleton.somaskel77, model.motion_rep.skeleton):
        levels[int(skel.joint_parents.shape[0])] = original_levels(skel.joint_parents.cpu())

    def cached_levels(parents):
        n = int(parents.shape[0])
        return levels[n] if n in levels else original_levels(parents)
    kin.compute_idx_levels = cached_levels
    kin.skeleton_idx_levs = lambda skeleton, device: cached_levels(skeleton.joint_parents)
    st = model.motion_rep.stats
    st.mean, st.std_eps = st.mean.float(), st.std_eps.float()
    if hasattr(st, "_cast_cache"):
        st._cast_cache = {}
    model.eval()
    return model


def _mha_forward(self, query, key, value, key_padding_mask=None, need_weights=False, attn_mask=None,
                 average_attn_weights=True, is_causal=False):
    """nn.MultiheadAttention (batch_first, self-attention) with explicit ops: the stock module freezes the
    sequence length into Reshape constants when traced."""
    B, N, E = query.shape
    H = self.num_heads
    q, k, v = F.linear(query, self.in_proj_weight, self.in_proj_bias).chunk(3, dim=-1)
    q = q.reshape(B, N, H, E // H).transpose(1, 2)
    k = k.reshape(B, N, H, E // H).transpose(1, 2)
    v = v.reshape(B, N, H, E // H).transpose(1, 2)
    att = (q @ k.transpose(-2, -1)) * (1.0 / math.sqrt(E // H))
    if key_padding_mask is not None:
        m = key_padding_mask[:, None, None, :]
        att = att.masked_fill(m, float("-inf")) if m.dtype == torch.bool else att + m
    o = (att.softmax(-1) @ v).transpose(1, 2).reshape(B, N, E)
    return self.out_proj(o), None


class Denoiser(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.m = model.denoiser.model
        for mod in self.m.modules():
            if isinstance(mod, torch.nn.MultiheadAttention):
                assert mod._qkv_same_embed_dim and mod.batch_first
                mod.forward = _mha_forward.__get__(mod)

    def forward(self, x, x_pad_mask, text_feat, timesteps, first_heading_angle, motion_mask, observed_motion):
        text_mask = torch.ones(text_feat.shape[:2], dtype=torch.bool)
        return self.m(x, x_pad_mask, text_feat, text_mask, timesteps, first_heading_angle, motion_mask,
                      observed_motion)


class Decode(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.rep = model.motion_rep

    def forward(self, features):
        o = self.rep.inverse(features, is_normalized=False, return_numpy=False)
        return (o["local_rot_mats"], o["global_rot_mats"], o["root_positions"], o["posed_joints"],
                o["foot_contacts"].float(), o["smooth_root_pos"])


class Encode(torch.nn.Module):
    """Pose -> features. The smoothed root path is an INPUT: Kimodo computes it with a scipy sparse-LU ADMM
    smoother (kimodo/motion_rep/smooth_root.py) that ONNX cannot express; QtMeshEditor ports it to C++
    (KimodoSmoothRoot) and feeds the result here."""

    def __init__(self, model):
        super().__init__()
        self.rep = model.motion_rep

    def forward(self, local_rot_mats, root_positions, smooth_root_pos):
        import kimodo.motion_rep.reps.kimodo_motionrep as krep
        # every frame is valid (B=1): lengths = T, derived from the shape so torch.export keeps T symbolic
        lengths = torch.ones(1, dtype=torch.long) * local_rot_mats.shape[1]
        original = krep.get_smooth_root_pos
        krep.get_smooth_root_pos = lambda hips: smooth_root_pos
        try:
            return self.rep(local_rot_mats, root_positions, to_normalize=False, lengths=lengths)
        finally:
            krep.get_smooth_root_pos = original


class Transition(torch.nn.Module):
    """kimodo_model._multiprompt's transition block: the previous segment's last frames become full-body +
    end-effector constraints at the start of the next segment. Also returns what post-processing needs."""

    def __init__(self, model):
        super().__init__()
        self.model = model
        self.rep = model.motion_rep

    def forward(self, latest):
        from kimodo.constraints import EndEffectorConstraintSet, FullBodyConstraintSet
        from kimodo.motion_rep.feature_utils import compute_heading_angle
        from kimodo.postprocess import extract_input_motion_from_constraints
        sk = self.model.skeleton
        n = latest.shape[1]
        last = self.rep.inverse(latest, is_normalized=False, return_numpy=False)
        root2d = last["smooth_root_pos"][..., [0, 2]]
        idx = torch.arange(n)
        fb = FullBodyConstraintSet(sk, idx, last["posed_joints"][0], last["global_rot_mats"][0], root2d[0])
        ee = EndEffectorConstraintSet(sk, idx, last["posed_joints"][0], last["global_rot_mats"][0], root2d[0],
                                      joint_names=["LeftHand", "RightHand", "LeftFoot", "RightFoot"])
        lengths = torch.tensor([n])
        observed, mask = self.rep.create_conditions_from_constraints_batched([[fb, ee]], lengths,
                                                                             to_normalize=False, device="cpu")
        last_root2d = root2d[:, 0]
        # motion_rep.translate_2d(observed, -last_root2d), written functionally: its in-place add on a slice view
        # of a clone is silently dropped by the TorchScript tracer (the exported graph returned untranslated roots).
        sr = self.rep.slice_dict["smooth_root_pos"].start
        observed = torch.cat([observed[..., :sr],
                              observed[..., sr:sr + 1] - last_root2d[:, None, 0:1],
                              observed[..., sr + 1:sr + 2],
                              observed[..., sr + 2:sr + 3] - last_root2d[:, None, 1:2],
                              observed[..., sr + 3:]], dim=-1)
        mask = mask.float()
        observed = observed * mask
        heading = compute_heading_angle(last["posed_joints"], sk)[:, 0]
        hip_in, rot_in = extract_input_motion_from_constraints([fb, ee], sk, n, sk.nbjoints)
        return observed, mask, heading, last_root2d, hip_in, rot_in


def export(module, args, path, inputs, outputs, dynamic):
    t0 = time.time()
    torch.onnx.export(module, args, path, input_names=inputs, output_names=outputs, dynamic_axes=dynamic,
                      opset_version=17, dynamo=False, do_constant_folding=True)
    print(f"  {os.path.basename(path)}: {os.path.getsize(path) / 1e6:.1f} MB in {time.time() - t0:.1f}s", flush=True)


def export_dynamo(module, args, path, inputs, outputs, dynamic_shapes):
    """torch.export-based exporter for the feature <-> pose graphs: Kimodo's helpers build shapes from Python
    tuples and broadcast assignments, which the TorchScript tracer freezes to the example frame count."""
    t0 = time.time()
    prog = torch.onnx.export(module, args, input_names=inputs, output_names=outputs, dynamic_shapes=dynamic_shapes,
                             dynamo=True, opset_version=18, external_data=False, optimize=True)
    prog.save(path)
    print(f"  {os.path.basename(path)}: {os.path.getsize(path) / 1e6:.1f} MB in {time.time() - t0:.1f}s", flush=True)


def working_rig(model):
    from kimodo.postprocess import create_working_rig_from_skeleton
    from kimodo.skeleton import SOMASkeleton30, SOMASkeleton77
    sk = model.skeleton
    off = 0.02 if isinstance(sk, (SOMASkeleton30, SOMASkeleton77)) else 0.007
    rig = create_working_rig_from_skeleton(sk, above_ground_offset=off)
    names = [j.name for j in rig]
    tags = {j.retarget_tag: i for i, j in enumerate(rig) if j.retarget_tag}
    return {
        "aboveGroundOffset": off,
        "parents": [names.index(j.parent) if j.parent in names else -1 for j in rig],
        "translations": [[float(v) for v in j.t_pose_translation] for j in rig],
        "rotationsXYZW": [[float(v) for v in j.t_pose_rotation] for j in rig],
        "leftHand": tags["LeftHand"], "rightHand": tags["RightHand"],
        "leftFoot": tags["LeftFoot"], "rightFoot": tags["RightFoot"],
        "hasDoubleAnkleJoints": False,
    }


def write_meta(model, path):
    sk = model.skeleton
    s77 = sk.somaskel77
    d = model.diffusion
    meta = {
        "format": "qtmesh-kimodo-onnx-v1",
        "model": MODEL,
        "fps": float(model.fps),
        "featureSize": int(model.motion_rep.motion_rep_dim),
        "slices": {k: [v.start, v.stop] for k, v in model.motion_rep.slice_dict.items()},
        "mean": model.motion_rep.stats.mean.float().tolist(),
        "stdEps": model.motion_rep.stats.std_eps.float().tolist(),
        "diffusion": {"baseSteps": int(d.num_base_steps),
                      "alphasCumprodBase": d.alphas_cumprod_base.double().tolist()},
        "cfg": {"type": model.denoiser.cfg_type_default, "weights": [2.0, 2.0]},
        "transitionFrames": TRANSITION_FRAMES,
        "textDim": 4096,
        "skeleton30": {"names": list(sk.bone_order_names), "parents": sk.joint_parents.tolist(),
                       "neutralJoints": sk.neutral_joints.float().tolist(),
                       "slice77": [int(i) for i in sk.get_skel_slice(s77)]},
        "skeleton77": {"names": list(s77.bone_order_names), "parents": s77.joint_parents.tolist(),
                       "neutralJoints": s77.neutral_joints.float().tolist(),
                       "relaxedHandsRestPose": s77.relaxed_hands_rest_pose.float().tolist()},
        "postprocess": {"contactThreshold": 0.5, "rootMargin": 0.04, "rig": working_rig(model)},
        "licence": "NVIDIA Open Model License (Kimodo-SOMA-RP-v1.2 weights); see LICENSE-NVIDIA-OML.txt",
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(meta, f)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--verify", action="store_true", help="replay the native algorithm against PyTorch")
    ap.add_argument("--skip-export", action="store_true")
    args = ap.parse_args()
    out = os.path.abspath(os.path.expanduser(args.out))
    os.makedirs(out, exist_ok=True)
    model = load_kimodo()
    D = model.motion_rep.motion_rep_dim
    assert D == FEATURE_SIZE, D

    if not args.skip_export:
        print("exporting", flush=True)
        T = 64
        g = torch.Generator().manual_seed(0)
        export(Denoiser(model).eval(),
               (torch.randn(1, T, D, generator=g), torch.ones(1, T, dtype=torch.bool), torch.randn(1, 1, 4096, generator=g),
                torch.tensor([500]), torch.zeros(1), torch.zeros(1, T, D), torch.zeros(1, T, D)),
               os.path.join(out, "kimodo_denoiser.onnx"),
               ["x", "x_pad_mask", "text_feat", "timesteps", "first_heading_angle", "motion_mask", "observed_motion"],
               ["pred_clean"],
               {"x": {1: "T"}, "x_pad_mask": {1: "T"}, "motion_mask": {1: "T"}, "observed_motion": {1: "T"},
                "pred_clean": {1: "T"}})
        feats = sample_features(model, 48)
        Tdim = torch.export.Dim("T", min=6, max=4096)
        export_dynamo(Decode(model).eval(), (feats,), os.path.join(out, "kimodo_decode.onnx"), ["features"],
                      ["local_rot_mats", "global_rot_mats", "root_positions", "posed_joints", "foot_contacts",
                       "smooth_root_pos"], {"features": {1: Tdim}})
        with torch.no_grad():
            dec = Decode(model)(feats)
        export_dynamo(Encode(model).eval(), (dec[0], dec[2], dec[5]),
                      os.path.join(out, "kimodo_encode.onnx"),
                      ["local_rot_mats", "root_positions", "smooth_root_pos"], ["features"],
                      {"local_rot_mats": {1: Tdim}, "root_positions": {1: Tdim}, "smooth_root_pos": {1: Tdim}})
        export(Transition(model).eval(), (feats[:, -TRANSITION_FRAMES:],), os.path.join(out, "kimodo_transition.onnx"),
               ["latest"], ["observed", "mask", "first_heading_angle", "last_root_2d", "hip_input", "rotations_input"], {})
        write_meta(model, os.path.join(out, "kimodo_meta.json"))

    if args.verify:
        import verify_kimodo_native as V   # noqa: E402  (scripts/ on sys.path)
        return V.run(model, out)
    return 0


def sample_features(model, T):
    """Realistic un-normalized features for tracing (a short generation would need text; the model's own
    stats-mean motion keeps every branch on valid rotations)."""
    st = model.motion_rep.stats
    x = st.mean[None, None].repeat(1, T, 1).clone()
    x[..., model.motion_rep.slice_dict["smooth_root_pos"]] += torch.linspace(0, 1, T)[None, :, None]
    return x.float()


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.exit(main())
