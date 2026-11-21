#!/usr/bin/env python3
"""Export Meta's SAM 3D Body (body branch) + the MHR body model to ONNX for QtMeshEditor (#1100).

ONE-TIME, offline developer tool — NOT shipped. Produces, in --out-dir:
  sam3d_backbone.onnx (+ .onnx.data)  image[1,3,512,512] RGB 0..1 -> features[1,1280,32,32]   (fp32)
  sam3d_decoder.onnx                  features + ray/cond/affine/... -> shape/scale/pose/camera/model_params/vertices
  mhr_body.onnx                       shape[1,45] + model_params[1,204] -> vertices (cm), joint t, joint quat
  mhr_faces.i32                       MHR body topology (36,874 triangles, int32 LE)
  mhr_uv.bin                          MHR UV layout (19,455 texcoords + per-face UV indices; see the writer below)
and verifies each graph against the PyTorch module with onnxruntime. Upload with scripts/upload-sam3dbody-models.sh.

Inputs you must have (this repo ships none of Meta's code or weights):
  --meta-repo   a clone of https://github.com/facebookresearch/sam-3d-body      (SAM License)
  --checkpoint-dir  a download of the HF repo facebook/sam-3d-body-dinov3 (needs access): model.ckpt + assets/mhr_model.pt
The DINOv3 backbone code is fetched by Meta's package through torch.hub (facebookresearch/dinov3) — review its licence.
Meta's code hard-codes `.cuda()` in a few places: without CUDA this script turns Tensor.cuda into a no-op.

What the graphs are (vs Meta's PyTorch, measured on a real photo): decoder ~1e-6, backbone ~2e-5 (fp32 here, bf16 in
Meta's code), MHR ~1e-4 cm. Not reproduced: MHR pose correctives (664 MB; mean 2 mm / max 25 mm on a posed body) and the
face-expression model (the body head zeroes it).

Why the extra code (each piece was found by a failing export or a parity diff):
  * The camera encoder downsamples rays with an anti-aliased F.interpolate, which ONNX cannot express. The resample is
    separable and linear, so its exact 1-D weight matrices are extracted once and applied as two matmuls.
    GOTCHA: a size-1 spatial dim takes a different resample path in PyTorch — extract the weights with a dim of 8.
  * roma's euler conversion uses masked in-place assignment + torch.hypot; mhr_utils/mhr_head/camera_head use in-place
    index assignment on fresh tensors. The TorchScript exporter turns those into Add nodes with EMPTY inputs (an invalid
    graph that only fails when onnxruntime loads it), so functional twins (torch.where / gather / cat) replace them.
  * the dinov3 backbone type has NO x*16/12 keypoint-grid scaling (the ViT variants do).
  * `ori_img_size` is pruned from the decoder graph (the intrinsics centre is used instead).
"""
import argparse, math, os, sys, time
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import roma
import roma.euler as _re
import roma.internal


# ---- MHR as a plain torch module ------------------------------------------------------------
def qmul(a, b):   # xyzw
    ax, ay, az, aw = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    bx, by, bz, bw = b[..., 0], b[..., 1], b[..., 2], b[..., 3]
    return torch.stack([aw*bx + ax*bw + ay*bz - az*by,
                        aw*by - ax*bz + ay*bw + az*bx,
                        aw*bz + ax*by - ay*bx + az*bw,
                        aw*bw - ax*bx - ay*by - az*bz], -1)

def qrot(q, v):
    qv = q[..., :3]; w = q[..., 3:4]
    t = 2 * torch.cross(qv, v, dim=-1)
    return v + w * t + torch.cross(qv, t, dim=-1)

def euler_to_quat_zyx(e):   # R = Rz * Ry * Rx
    h = e * 0.5
    s, c = torch.sin(h), torch.cos(h)
    z = torch.zeros_like(s[..., 0])
    qx = torch.stack([s[..., 0], z, z, c[..., 0]], -1)
    qy = torch.stack([z, s[..., 1], z, c[..., 1]], -1)
    qz = torch.stack([z, z, s[..., 2], c[..., 2]], -1)
    return qmul(qmul(qz, qy), qx)

class MhrTorch(nn.Module):
    def __init__(self, ts_path, max_influences=None):
        super().__init__()
        m = torch.jit.load(ts_path, map_location="cpu")
        sd = {k: v.detach().clone() for k, v in m.state_dict().items()}
        g = lambda k: sd["character_torch." + k]
        self.register_buffer("base", g("blend_shape.base_shape").float())
        self.register_buffer("shape_vecs", g("blend_shape.shape_vectors").float().reshape(45, -1))   # (45, V*3)
        self.register_buffer("PT", g("parameter_transform.parameter_transform").float())
        self.register_buffer("offs", g("skeleton.joint_translation_offsets").float())
        self.register_buffer("prerot", g("skeleton.joint_prerotations").float())
        self.register_buffer("ibp", g("linear_blend_skinning.inverse_bind_pose").float())
        self.parents = [int(p) for p in g("skeleton.joint_parents").tolist()]
        assert all(p < j for j, p in enumerate(self.parents)), "FK order"
        sj = g("linear_blend_skinning.skin_indices_flattened").long()
        sw = g("linear_blend_skinning.skin_weights_flattened").float()
        sv = g("linear_blend_skinning.vert_indices_flattened").long()
        V = self.base.shape[0]
        cnt = torch.bincount(sv, minlength=V)
        K = int(cnt.max())
        idx = torch.zeros(V, K, dtype=torch.long); wt = torch.zeros(V, K)
        fill = torch.zeros(V, dtype=torch.long)
        for j, w, v in zip(sj.tolist(), sw.tolist(), sv.tolist()):
            idx[v, fill[v]] = j; wt[v, fill[v]] = w; fill[v] += 1
        self.register_buffer("skin_idx", idx)
        self.register_buffer("skin_w", wt)
        self.K = K
        self.nj = len(self.parents)
        self.register_buffer("faces", g("mesh.faces").long())

    def skeleton(self, model_params):
        B = model_params.shape[0]
        full = torch.cat([model_params, torch.zeros(B, 45)], 1)
        jp = (full @ self.PT.t()).reshape(B, self.nj, 7)
        lt = jp[..., :3] + self.offs
        lq = qmul(self.prerot.expand(B, -1, -1), euler_to_quat_zyx(jp[..., 3:6]))
        ls = torch.exp(jp[..., 6] * 0.6931471805599453)
        gt, gq, gs = [], [], []
        for j in range(self.nj):
            p = self.parents[j]
            if p < 0:
                gt.append(lt[:, j]); gq.append(lq[:, j]); gs.append(ls[:, j])
            else:
                gt.append(gt[p] + gs[p][:, None] * qrot(gq[p], lt[:, j]))
                gq.append(qmul(gq[p], lq[:, j]))
                gs.append(gs[p] * ls[:, j])
        return torch.stack(gt, 1), torch.stack(gq, 1), torch.stack(gs, 1)

    def forward(self, shape, model_params):
        """shape [B,45], model_params [B,204] -> verts [B,V,3] (cm), joint coords [B,127,3] (cm), joint rotations quat xyzw [B,127,4]"""
        B = shape.shape[0]
        rest = self.base[None] + (shape @ self.shape_vecs).reshape(B, -1, 3)
        gt, gq, gs = self.skeleton(model_params)
        it, iq, is_ = self.ibp[:, :3], self.ibp[:, 3:7], self.ibp[:, 7]
        ct = gt + gs[..., None] * qrot(gq, it.expand(B, -1, -1))
        cq = qmul(gq, iq.expand(B, -1, -1))
        cs = gs * is_
        # gather per-vertex influence transforms: (B,V,K,...)
        jt = ct[:, self.skin_idx]; jq = cq[:, self.skin_idx]; js = cs[:, self.skin_idx]
        x = rest[:, :, None, :].expand(-1, -1, self.K, -1)
        y = js[..., None] * qrot(jq, x) + jt
        verts = (y * self.skin_w[None, :, :, None]).sum(2)
        return verts, gt, gq

# ---- ONNX-exportable replacements for third-party code ----------------------------------------
torch.hypot = lambda a, b: torch.sqrt(a * a + b * b)
_orig_unitquat_to_euler = _re.unitquat_to_euler

def unitquat_to_euler(convention, quat, as_tuple=False, degrees=False, epsilon=1e-7):
    pi = math.pi; lamb = pi / 2
    extrinsic = convention.islower()
    if not extrinsic: convention = convention.lower()[::-1]
    quat, batch_shape = roma.internal.flatten_batch_dims(quat, end_dim=-2)
    i, j, k = (_re._elementary_basis_index(c) for c in convention)
    symmetric = i == k
    if symmetric: k = 3 - i - j
    sign = (i - j) * (j - k) * (k - i) // 2
    if symmetric:
        a, b, c, d = quat[:, 3], quat[:, i], quat[:, j], quat[:, k] * sign
    else:
        a = quat[:, 3] - quat[:, j]; b = quat[:, i] + quat[:, k] * sign
        c = quat[:, j] + quat[:, 3]; d = quat[:, k] * sign - quat[:, i]
    first, third = (0, 2) if extrinsic else (2, 0)
    a1 = 2 * torch.atan2(torch.hypot(c, d), torch.hypot(a, b))
    case1 = torch.abs(a1) <= epsilon; case2 = torch.abs(a1 - pi) <= epsilon; case12 = case1 | case2
    hs = torch.atan2(b, a); hd = torch.atan2(d, c)
    ang = [None, a1, None]
    ang[first] = hs - hd; ang[third] = hs + hd
    zero = torch.zeros_like(a1)
    ang[2] = torch.where(case12, zero, ang[2])
    ang[0] = torch.where(case1, 2 * hs, ang[0])
    ang[0] = torch.where(case2, 2 * (-1 if extrinsic else 1) * hd, ang[0])
    if not symmetric:
        ang[third] = ang[third] * sign; ang[1] = ang[1] - lamb
    out = []
    for f in ang:
        f = torch.where(f < -pi, f + 2 * pi, f); f = torch.where(f > pi, f - 2 * pi, f)
        if degrees: f = torch.rad2deg(f)
        out.append(roma.internal.unflatten_batch_dims(f, batch_shape))
    return tuple(out) if as_tuple else torch.stack(out, dim=-1)

def install():
    _re.unitquat_to_euler = unitquat_to_euler
    roma.unitquat_to_euler = unitquat_to_euler
    if hasattr(roma, "euler"): roma.euler.unitquat_to_euler = unitquat_to_euler

def install_mhr_utils():
    """compact_cont_to_model_params_body without scatter (index_put on a fresh zeros tensor exports an empty-input Add)."""
    import sam_3d_body.models.modules.mhr_utils as mu
    import sam_3d_body.models.heads.mhr_head as mh
    orig = mu.compact_cont_to_model_params_body
    i3 = torch.LongTensor([(0, 2, 4), (6, 8, 10), (12, 13, 14), (15, 16, 17), (18, 19, 20), (21, 22, 23), (24, 25, 26), (27, 28, 29), (34, 35, 36), (37, 38, 39), (44, 45, 46), (53, 54, 55), (64, 65, 66), (85, 69, 73), (86, 70, 79), (87, 71, 82), (88, 72, 76), (91, 92, 93), (112, 96, 100), (113, 97, 106), (114, 98, 109), (115, 99, 103), (130, 131, 132)]).flatten()
    i1 = torch.LongTensor([1, 3, 5, 7, 9, 11, 30, 31, 32, 33, 40, 41, 42, 43, 47, 48, 49, 50, 51, 52, 56, 57, 58, 59, 60, 61, 62, 63, 67, 68, 74, 75, 77, 78, 80, 81, 83, 84, 89, 90, 94, 95, 101, 102, 104, 105, 107, 108, 110, 111, 116, 117, 118, 119, 120, 121, 122, 123])
    it = torch.LongTensor([124, 125, 126, 127, 128, 129])
    order = torch.cat([i3, i1, it])                        # source slot -> param index
    inv = torch.full((133,), len(order), dtype=torch.long)  # param index -> source slot (len = zero slot)
    inv[order] = torch.arange(len(order))
    n3, n1 = len(i3) // 3, len(i1)
    def fn(c):
        c3 = c[..., : 2 * n3 * 3].unflatten(-1, (-1, 6)); c1 = c[..., 2 * n3 * 3: 2 * n3 * 3 + 2 * n1].unflatten(-1, (-1, 2)); ct = c[..., 2 * n3 * 3 + 2 * n1:]
        p3 = mu.batchXYZfrom6D(c3).flatten(-2, -1); p1 = torch.atan2(c1[..., -2], c1[..., -1])
        src = torch.cat([p3, p1, ct, torch.zeros_like(p1[..., :1])], -1)
        return src[..., inv]
    mu.compact_cont_to_model_params_body = fn; mh.compact_cont_to_model_params_body = fn
    return orig, fn, mu

def install_hand():
    import sam_3d_body.models.modules.mhr_utils as mu
    import sam_3d_body.models.heads.mhr_head as mh
    orig = mu.compact_cont_to_model_params_hand
    dofs = [3, 1, 1, 3, 1, 1, 3, 1, 1, 3, 1, 1, 2, 3, 1, 1]
    mc3 = torch.cat([torch.ones(2 * k).bool() * (k in [3]) for k in dofs]); mc1 = torch.cat([torch.ones(2 * k).bool() * (k in [1, 2]) for k in dofs])
    mp3 = torch.cat([torch.ones(k).bool() * (k in [3]) for k in dofs]); mp1 = torch.cat([torch.ones(k).bool() * (k in [1, 2]) for k in dofs])
    sel3 = torch.nonzero(mc3).flatten(); sel1 = torch.nonzero(mc1).flatten()
    order = torch.cat([torch.nonzero(mp3).flatten(), torch.nonzero(mp1).flatten()])
    inv = torch.full((27,), len(order), dtype=torch.long); inv[order] = torch.arange(len(order))
    def hand(c):
        c3 = c[..., sel3].unflatten(-1, (-1, 6)); c1 = c[..., sel1].unflatten(-1, (-1, 2))
        p3 = mu.batchXYZfrom6D(c3).flatten(-2, -1); p1 = torch.atan2(c1[..., -2], c1[..., -1])
        return torch.cat([p3, p1, torch.zeros_like(p1[..., :1])], -1)[..., inv]
    mu.compact_cont_to_model_params_hand = hand; mh.compact_cont_to_model_params_hand = hand
    def replace_hands(self, full, hand_pose_params):
        l, r = torch.split(hand_pose_params, [self.num_hand_pose_comps, self.num_hand_pose_comps], dim=1)
        lm = hand(self.hand_pose_mean + torch.einsum("da,ab->db", l, self.hand_pose_comps))
        rm = hand(self.hand_pose_mean + torch.einsum("da,ab->db", r, self.hand_pose_comps))
        n = full.shape[1]
        src = torch.arange(n)
        li = self.hand_joint_idxs_left.long(); ri = self.hand_joint_idxs_right.long()
        src = src.clone(); src[li] = n + torch.arange(len(li)); src[ri] = n + len(li) + torch.arange(len(ri))
        return torch.cat([full, lm, rm], 1)[:, src]
    mh.MHRHead.replace_hands_in_pose = replace_hands
    return orig, hand

def install_head():
    """Functional (no in-place index assignment) MHRHead.forward/mhr_forward, numerically identical for the body-only path."""
    import sam_3d_body.models.heads.mhr_head as mh
    from sam_3d_body.models.modules.mhr_utils import mhr_param_hand_mask
    keep = torch.ones(133); keep[mhr_param_hand_mask] = 0; keep[-3:] = 0
    flip = torch.tensor([1.0, -1.0, -1.0])
    def hand_masks(self):
        # The hand head zeroes every non-hand model parameter and every keypoint outside the right hand (21..41). Both are
        # in-place index assignments in Meta's code: precomputed constant masks keep the graph valid (see the docstring).
        if not hasattr(self, "_hand_masks"):
            off = set(int(i) for i in self.nonhand_param_idxs.tolist())
            pm = torch.tensor([0.0 if i in off else 1.0 for i in range(204)])
            km = torch.zeros(308, 1); km[21:42] = 1.0
            self._hand_masks = (pm, km)
        return self._hand_masks
    def mhr_forward(self, global_trans, global_rot, body_pose_params, hand_pose_params, scale_params, shape_params, expr_params=None,
                    return_keypoints=False, do_pcblend=True, return_joint_coords=False, return_model_params=False,
                    return_joint_rotations=False, scale_offsets=None, vertex_offsets=None):
        if self.enable_hand_model:
            # wrist-centric prediction -> body frame (Meta's MHRHead.mhr_forward, hand branch)
            g_new = mh.roma.rotmat_to_euler("xyz", mh.roma.euler_to_rotmat("xyz", global_rot) @ self.local_to_world_wrist)
            global_trans = -(mh.roma.euler_to_rotmat("xyz", g_new) @ (self.right_wrist_coords - self.root_coords)
                             + self.root_coords) + global_trans
            global_rot = g_new
        body_pose_params = body_pose_params[..., :130]
        scales = self.scale_mean[None, :] + scale_params @ self.scale_comps
        full = torch.cat([global_trans * 10, global_rot, body_pose_params], dim=1)
        full = self.replace_hands_in_pose(full, hand_pose_params)
        model_params = torch.cat([full, scales], dim=1)
        if self.enable_hand_model:
            model_params = model_params * self.hand_masks()[0]
        verts, skel = self.mhr(shape_params, model_params, expr_params)
        jc, jq, _ = torch.split(skel, [3, 4, 1], dim=2)
        verts = verts / 100; jc = jc / 100
        rots = mh.roma.unitquat_to_rotmat(jq)
        vj = torch.cat([verts, jc], dim=1)
        kp = (self.keypoint_mapping @ vj.permute(1, 0, 2).flatten(1, 2)).reshape(-1, vj.shape[0], 3).permute(1, 0, 2)
        if self.enable_hand_model:
            kp = kp * self.hand_masks()[1]
        return verts, kp, jc, model_params, rots
    mh.MHRHead.hand_masks = hand_masks
    def forward(self, x, init_estimate=None, do_pcblend=True, slim_keypoints=False):
        B = x.shape[0]
        pred = self.proj(x)
        if init_estimate is not None: pred = pred + init_estimate
        count = 6
        g6 = pred[:, :count]
        g_rot = mh.rot6d_to_rotmat(g6)
        g_euler = mh.roma.rotmat_to_euler("ZYX", g_rot)
        g_trans = torch.zeros_like(g_euler)
        cont = pred[:, count: count + self.body_cont_dim]; count += self.body_cont_dim
        euler = mh.compact_cont_to_model_params_body(cont) * keep
        shape = pred[:, count: count + self.num_shape_comps]; count += self.num_shape_comps
        scale = pred[:, count: count + self.num_scale_comps]; count += self.num_scale_comps
        hand = pred[:, count: count + self.num_hand_comps * 2]; count += self.num_hand_comps * 2
        face = pred[:, count: count + self.num_face_comps] * 0
        verts, j3d, jc, mp, jrots = self.mhr_forward(global_trans=g_trans, global_rot=g_euler, body_pose_params=euler, hand_pose_params=hand,
                                                      scale_params=scale, shape_params=shape, expr_params=face)
        j3d = j3d[:, :70] * flip; verts = verts * flip; jc = jc * flip
        return {"pred_pose_raw": torch.cat([g6, cont], 1), "pred_pose_rotmat": None, "global_rot": g_euler, "body_pose": euler,
                "shape": shape, "scale": scale, "hand": hand, "face": face, "pred_keypoints_3d": j3d.reshape(B, -1, 3),
                "pred_vertices": verts.reshape(B, -1, 3), "pred_joint_coords": jc.reshape(B, -1, 3),
                "faces": self.faces.cpu().numpy(), "joint_global_rots": jrots, "mhr_model_params": mp}
    mh.MHRHead.forward = forward; mh.MHRHead.mhr_forward = mhr_forward

def install_camera():
    import sam_3d_body.models.heads.camera_head as ch
    sgn = torch.tensor([-1.0, 1.0, -1.0])
    def perspective_projection(self, points_3d, pred_cam, bbox_center, bbox_size, img_size, cam_int, use_intrin_center=False):
        batch_size = points_3d.shape[0]
        pred_cam = pred_cam * sgn
        s, tx, ty = pred_cam[:, 0], pred_cam[:, 1], pred_cam[:, 2]
        bs = bbox_size * s * self.default_scale_factor + 1e-8
        focal_length = cam_int[:, 0, 0]
        tz = 2 * focal_length / bs
        if not use_intrin_center:
            cx = 2 * (bbox_center[:, 0] - (img_size[:, 0] / 2)) / bs; cy = 2 * (bbox_center[:, 1] - (img_size[:, 1] / 2)) / bs
        else:
            cx = 2 * (bbox_center[:, 0] - (cam_int[:, 0, 2])) / bs; cy = 2 * (bbox_center[:, 1] - (cam_int[:, 1, 2])) / bs
        pred_cam_t = torch.stack([tx + cx, ty + cy, tz], dim=-1)
        j3d_cam = points_3d + pred_cam_t.unsqueeze(1)
        j2d = ch.perspective_projection(j3d_cam, cam_int)
        return {"pred_keypoints_2d": j2d.reshape(batch_size, -1, 2), "pred_cam_t": pred_cam_t, "focal_length": focal_length,
                "pred_keypoints_2d_depth": j3d_cam.reshape(batch_size, -1, 3)[:, :, 2]}
    for name in dir(ch):
        c = getattr(ch, name)
        if isinstance(c, type) and hasattr(c, "perspective_projection") and name != "perspective_projection":
            c.perspective_projection = perspective_projection

# ---- exported graphs ----------------------------------------------------------------------
class MhrAsMeta(nn.Module):
    """Drop-in for Meta's `head_pose.mhr` (TorchScript): (shape, model_params, expr) -> (verts cm, skel_state [B,127,8])."""
    def __init__(self, mt: MhrTorch):
        super().__init__(); self.mt = mt
    def forward(self, shape, model_params, expr=None, apply_correctives=False):
        verts, gt, gq = self.mt(shape, model_params)
        ones = torch.ones_like(gt[..., :1])
        return verts, torch.cat([gt, gq, ones], -1)   # scale slot unused by the head
    def parameters(self, recurse=True): return iter(())


class BackboneGraph(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.enc = model.backbone
        self.register_buffer("mean", model.image_mean.clone().float())
        self.register_buffer("std", model.image_std.clone().float())
    def forward(self, img):                 # RGB 0..1
        x = (img - self.mean) / self.std
        return self.enc(x.float())          # [1,1280,32,32]


class DecoderGraph(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.m = model
        cfg = model.cfg
        self.use_intrin_center = bool(cfg.MODEL.DECODER.get("USE_INTRIN_CENTER", False))
        self.depth = len(model.decoder.layers)
        dummy = torch.zeros(1, 1, 3); dummy[..., -1] = -2
        with torch.no_grad():
            sparse, _ = model.prompt_encoder(keypoints=dummy)                # [1,1,1280]
            self.register_buffer("prompt_tok", model.prompt_to_token(sparse).detach().clone())   # [1,1,1024]
            self.register_buffer("dense_pe", model.prompt_encoder.get_dense_pe((32, 32)).detach().clone())  # [1,1280,32,32]
        self.register_buffer("init_pose", model.init_pose.weight.detach().clone())          # [1,519]
        self.register_buffer("init_cam", model.init_camera.weight.detach().clone())         # [1,3]
        self.register_buffer("hand_box_emb", model.hand_box_embedding.weight.detach().clone()[None])
        self.register_buffer("kp_emb", model.keypoint_embedding.weight.detach().clone()[None])
        self.register_buffer("kp3d_emb", model.keypoint3d_embedding.weight.detach().clone()[None])
        # CameraEncoder's rays are downsampled with F.interpolate(antialias=True), which ONNX cannot express. The resample is
        # separable and linear, so take its exact 1-D weight matrices once (32x512 each) and use two matmuls in the graph.
        ce = model.ray_cond_emb
        self.ce_scale = 1.0 / ce.patch_size
        with torch.no_grad():
            eye = torch.eye(512)
            # NB: a size-1 spatial dim takes a different resample path in PyTorch -> use width/height 8
            xy = eye.reshape(1, 512, 512, 1).expand(1, 512, 512, 8).contiguous()
            xx = eye.reshape(1, 512, 1, 512).expand(1, 512, 8, 512).contiguous()
            wy = F.interpolate(xy, scale_factor=(self.ce_scale, 1.0), mode="bilinear", align_corners=False, antialias=True)[0, :, :, 0]
            wx = F.interpolate(xx, scale_factor=(1.0, self.ce_scale), mode="bilinear", align_corners=False, antialias=True)[0, :, 0, :]
        self.register_buffer("ray_wy", wy.t().contiguous())     # (32,512)
        self.register_buffer("ray_wx", wx.t().contiguous())

    def pose_output(self, pose_token, bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size):
        m = self.m
        out = m.head_pose(pose_token, self.init_pose.expand(pose_token.shape[0], -1))
        pred_cam = m.head_camera(pose_token, self.init_cam.expand(pose_token.shape[0], -1))
        out["pred_cam"] = pred_cam
        cam = m.head_camera.perspective_projection(out["pred_keypoints_3d"], pred_cam, bbox_center, bbox_scale_b,
                                                   ori_img_size, cam_int, use_intrin_center=self.use_intrin_center)
        out.update(cam)
        k2 = out["pred_keypoints_2d"]
        kh = torch.cat([k2, torch.ones_like(k2[:, :, -1:])], -1) @ affine.transpose(1, 2)
        out["pred_keypoints_2d_cropped"] = kh[..., :2] / img_size.unsqueeze(1) - 0.5
        return out

    def ray_cond_embed(self, img_emb, rays):
        ce = self.m.ray_cond_emb
        B, D, h, w = img_emb.shape
        r = torch.einsum("ih,bchw,jw->bcij", self.ray_wy, rays, self.ray_wx)              # (B,2,32,32), == F.interpolate(antialias)
        r = r.permute(0, 2, 3, 1)
        r = torch.cat([r, torch.ones_like(r[..., :1])], -1).reshape(B, -1, 3)
        # Fourier features (same maths as FourierPositionEncoding, without the python loop over the batch)
        fb = torch.stack([torch.linspace(1.0, 32.0, 16) for _ in range(3)], 0)
        feats = (r[:, :, :, None] * fb[None, None]).reshape(B, h * w, -1)
        emb = torch.cat([r, torch.sin(3.141592653589793 * feats), torch.cos(3.141592653589793 * feats)], -1)   # (B,N,99)
        emb = emb.reshape(B, h, w, -1).permute(0, 3, 1, 2)
        z = torch.cat([img_emb, emb], 1)
        return ce.norm(ce.conv(z))

    def kp2d_update(self, start, image_embeddings, tokens, aug, out):
        """Functional twin of SAM3DBody.keypoint_token_update_fn (slice assignment -> cat; identical maths)."""
        m = self.m; n = m.keypoint_embedding.weight.shape[0]
        k2 = out["pred_keypoints_2d_cropped"][:, m.keypoint_embedding_idxs]
        depth = out["pred_keypoints_2d_depth"][:, m.keypoint_embedding_idxs]
        k01 = k2 + 0.5
        invalid = (k01[:, :, 0] < 0) | (k01[:, :, 0] > 1) | (k01[:, :, 1] < 0) | (k01[:, :, 1] > 1) | (depth < 1e-5)
        valid = (~invalid[:, :, None])
        new_aug = m.keypoint_posemb_linear(k2) * valid
        aug = torch.cat([aug[:, :start], new_aug, aug[:, start + n:]], 1)
        pts = k2 * 2
        feats = F.grid_sample(image_embeddings, pts[:, :, None, :], mode="bilinear", padding_mode="zeros", align_corners=False).squeeze(3).permute(0, 2, 1)
        feats = feats * valid
        add = m.keypoint_feat_linear(feats)
        tokens = torch.cat([tokens[:, :start], tokens[:, start:start + n] + add, tokens[:, start + n:]], 1)
        return tokens, aug

    def kp3d_update(self, start, aug, out):
        m = self.m; n = m.keypoint3d_embedding.weight.shape[0]
        k3 = out["pred_keypoints_3d"]
        k3 = k3 - (k3[:, [m.pelvis_idx[0]], :] + k3[:, [m.pelvis_idx[1]], :]) / 2
        k3 = k3[:, m.keypoint3d_embedding_idxs]
        return torch.cat([aug[:, :start], m.keypoint3d_posemb_linear(k3), aug[:, start + n:]], 1)

    def forward(self, img_emb, ray_cond, cond_info, bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size):
        m = self.m
        B = img_emb.shape[0]
        init_estimate = torch.cat([self.init_pose, self.init_cam], -1).unsqueeze(1)           # [1,1,522]
        tok0 = m.init_to_token_mhr(torch.cat([cond_info.view(B, 1, -1), init_estimate.expand(B, -1, -1)], -1)).view(B, 1, -1)
        prev = m.prev_to_token_mhr(init_estimate.expand(B, -1, -1)).view(B, 1, -1)
        img_emb = self.ray_cond_embed(img_emb, ray_cond)                                       # [B,1280,32,32]
        tokens = torch.cat([tok0, prev, self.prompt_tok.expand(B, -1, -1), self.hand_box_emb.expand(B, -1, -1),
                            self.kp_emb.expand(B, -1, -1), self.kp3d_emb.expand(B, -1, -1)], 1)  # [B,145,1024]
        aug = torch.zeros_like(tokens)
        aug = torch.cat([aug[:, :1], prev, self.prompt_tok.expand(B, -1, -1), aug[:, 3:]], 1)
        kps_start, kps3d_start = 5, 75
        ctx = img_emb.flatten(2).permute(0, 2, 1)
        ctx_pe = self.dense_pe.flatten(2).permute(0, 2, 1).expand(B, -1, -1)
        dec = m.decoder
        out = None
        for i, layer in enumerate(dec.layers):
            tokens, _ = layer(tokens, ctx, aug, ctx_pe, None)
            if i < self.depth - 1:
                out = self.pose_output(dec.norm_final(tokens)[:, 0], bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size)
                tokens, aug = self.kp2d_update(kps_start, img_emb, tokens, aug, out)
                aug = self.kp3d_update(kps3d_start, aug, out)
        out = self.pose_output(dec.norm_final(tokens)[:, 0], bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size)
        return (out["shape"], out["scale"], out["body_pose"], out["global_rot"], out["hand"], out["pred_cam_t"],
                out["pred_keypoints_3d"], out["pred_keypoints_2d"], out["mhr_model_params"], out["pred_vertices"])

def decoder_layer(layer, x, context, x_pe, context_pe, attn_mask):
    """Meta's decoder-layer forward with an explicit boolean self-attention mask (theirs builds it with an in-place
    diagonal fill, which exports an invalid graph). The two-way branch is off in this model (the v1 parity relies on it)."""
    if layer.repeat_pe and context_pe is not None:
        x_pe = layer.ln_pe_1(x_pe); context_pe = layer.ln_pe_2(context_pe)
    if layer.repeat_pe and not layer.skip_first_pe and x_pe is not None:
        q = k = layer.ln1(x) + x_pe; v = layer.ln1(x)
    else:
        q = k = v = layer.ln1(x)
    x = x + layer.self_attn(q=q, k=k, v=v, attn_mask=attn_mask)
    if layer.repeat_pe and context_pe is not None:
        q = layer.ln2_1(x) + x_pe; k = layer.ln2_2(context) + context_pe; v = layer.ln2_2(context)
    else:
        q = layer.ln2_1(x); k = v = layer.ln2_2(context)
    x = x + layer.cross_attn(q=q, k=k, v=v)
    return layer.ffn(layer.ln3(x), identity=x)


MAX_PROMPTS = 4   # wrists + elbows; a FIXED count: ONNX Runtime 1.31 takes ~100 s to load the graph with a dynamic one


class PromptDecoderGraph(nn.Module):
    """Promptable decoder, body OR hand branch, with keypoint prompts and the hand-box tokens (Meta's run_inference "full").

    Body (`hand=False`) inputs: the DecoderGraph inputs + prev_estimate [1,522] + prompt [1,K,3] (x,y in 0..1 of the crop,
    label: joint index, -1 = not a point, -2 = dummy). The first pass feeds the init estimate + one dummy prompt (== the v1
    graph); the keypoint-prompt re-run feeds the first pass's `pose_raw` + the wrist/elbow prompts.
    Hand (`hand=True`): the hand decoder on a hand crop (dummy prompt, init estimate built in).

    The prompt tokens sit at the END of the token list (Meta puts them third): attention + MLP are permutation-equivariant
    over tokens and each token carries its own augment, so the order does not change the result. There are always
    MAX_PROMPTS of them; `prompt_valid` masks the unused ones out of the self-attention (Meta's own x_mask rule: valid
    tokens never attend to masked ones), which is exactly Meta's single-person path that DROPS invalid prompts.
    Verified against Meta's forward_decoder/forward_decoder_hand below.
    """
    def __init__(self, model, hand=False):
        super().__init__()
        self.m = model; self.hand = hand
        sfx = "_hand" if hand else ""
        g = lambda n: getattr(model, n + sfx)
        self.init_to_token, self.prev_to_token = g("init_to_token_mhr"), g("prev_to_token_mhr")
        self.ce = g("ray_cond_emb")
        self.kp_posemb, self.kp_feat, self.kp3d_posemb = g("keypoint_posemb_linear"), g("keypoint_feat_linear"), g("keypoint3d_posemb_linear")
        self.kp_idx, self.kp3d_idx = list(g("keypoint_embedding_idxs")), list(g("keypoint3d_embedding_idxs"))
        self.head_pose, self.head_camera = g("head_pose"), g("head_camera")
        self.dec = g("decoder")
        self.use_intrin_center = bool(model.cfg.MODEL.DECODER.get("USE_INTRIN_CENTER", False))
        with torch.no_grad():
            dpe = model.hand_pe_layer((32, 32)).unsqueeze(0) if hand else model.prompt_encoder.get_dense_pe((32, 32))
            self.register_buffer("dense_pe", dpe.detach().clone())
            pe = model.prompt_encoder
            n = pe.num_body_joints
            self.register_buffer("pt_table", torch.stack([pe.point_embeddings[i].weight[0] for i in range(n)]).detach().clone())
            self.register_buffer("pt_invalid", pe.invalid_point_embed.weight[0].detach().clone())
            self.register_buffer("pt_nap", pe.not_a_point_embed.weight[0].detach().clone())
        self.register_buffer("init_pose", g("init_pose").weight.detach().clone())
        self.register_buffer("init_cam", g("init_camera").weight.detach().clone())
        self.register_buffer("hand_box_emb", model.hand_box_embedding.weight.detach().clone()[None])
        self.register_buffer("kp_emb", g("keypoint_embedding").weight.detach().clone()[None])
        self.register_buffer("kp3d_emb", g("keypoint3d_embedding").weight.detach().clone()[None])
        scale = 1.0 / self.ce.patch_size
        with torch.no_grad():
            eye = torch.eye(512)
            xy = eye.reshape(1, 512, 512, 1).expand(1, 512, 512, 8).contiguous()
            xx = eye.reshape(1, 512, 1, 512).expand(1, 512, 8, 512).contiguous()
            wy = F.interpolate(xy, scale_factor=(scale, 1.0), mode="bilinear", align_corners=False, antialias=True)[0, :, :, 0]
            wx = F.interpolate(xx, scale_factor=(1.0, scale), mode="bilinear", align_corners=False, antialias=True)[0, :, 0, :]
        self.register_buffer("ray_wy", wy.t().contiguous())
        self.register_buffer("ray_wx", wx.t().contiguous())

    def prompt_tokens(self, prompt):
        m = self.m
        pts, lab = prompt[..., :2], prompt[..., 2]
        pe = m.prompt_encoder.pe_layer._pe_encoding(pts.float())
        idx = torch.clamp(lab, 0, self.pt_table.shape[0] - 1).long()
        emb = torch.where((lab == -2)[..., None], self.pt_invalid,
                          torch.where((lab == -1)[..., None], self.pt_nap, pe + self.pt_table[idx]))
        return m.prompt_to_token(emb)

    def pose_output(self, pose_token, bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size):
        B = pose_token.shape[0]
        out = self.head_pose(pose_token, self.init_pose.expand(B, -1))
        pred_cam = self.head_camera(pose_token, self.init_cam.expand(B, -1))
        out["pred_cam"] = pred_cam
        cam = self.head_camera.perspective_projection(out["pred_keypoints_3d"], pred_cam, bbox_center, bbox_scale_b,
                                                      ori_img_size, cam_int, use_intrin_center=self.use_intrin_center)
        out.update(cam)
        k2 = out["pred_keypoints_2d"]
        kh = torch.cat([k2, torch.ones_like(k2[:, :, -1:])], -1) @ affine.transpose(1, 2)
        out["pred_keypoints_2d_cropped"] = kh[..., :2] / img_size.unsqueeze(1) - 0.5
        return out

    def ray_cond_embed(self, img_emb, rays):
        B, D, h, w = img_emb.shape
        r = torch.einsum("ih,bchw,jw->bcij", self.ray_wy, rays, self.ray_wx).permute(0, 2, 3, 1)
        r = torch.cat([r, torch.ones_like(r[..., :1])], -1).reshape(B, -1, 3)
        fb = torch.stack([torch.linspace(1.0, 32.0, 16) for _ in range(3)], 0)
        feats = (r[:, :, :, None] * fb[None, None]).reshape(B, h * w, -1)
        emb = torch.cat([r, torch.sin(3.141592653589793 * feats), torch.cos(3.141592653589793 * feats)], -1)
        emb = emb.reshape(B, h, w, -1).permute(0, 3, 1, 2)
        return self.ce.norm(self.ce.conv(torch.cat([img_emb, emb], 1)))

    def kp2d_update(self, start, image_embeddings, tokens, aug, out):
        n = self.kp_emb.shape[1]
        k2 = out["pred_keypoints_2d_cropped"][:, self.kp_idx]
        depth = out["pred_keypoints_2d_depth"][:, self.kp_idx]
        k01 = k2 + 0.5
        invalid = (k01[:, :, 0] < 0) | (k01[:, :, 0] > 1) | (k01[:, :, 1] < 0) | (k01[:, :, 1] > 1) | (depth < 1e-5)
        valid = (~invalid[:, :, None])
        aug = torch.cat([aug[:, :start], self.kp_posemb(k2) * valid, aug[:, start + n:]], 1)
        feats = F.grid_sample(image_embeddings, (k2 * 2)[:, :, None, :], mode="bilinear", padding_mode="zeros",
                              align_corners=False).squeeze(3).permute(0, 2, 1) * valid
        tokens = torch.cat([tokens[:, :start], tokens[:, start:start + n] + self.kp_feat(feats), tokens[:, start + n:]], 1)
        return tokens, aug

    def kp3d_update(self, start, aug, out):
        m = self.m; n = self.kp3d_emb.shape[1]
        k3 = out["pred_keypoints_3d"]
        k3 = k3 - (k3[:, [m.pelvis_idx[0]], :] + k3[:, [m.pelvis_idx[1]], :]) / 2
        k3 = k3[:, self.kp3d_idx]
        return torch.cat([aug[:, :start], self.kp3d_posemb(k3), aug[:, start + n:]], 1)

    def run(self, img_emb, ray_cond, cond_info, bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size,
            prev_estimate, prompt, prompt_valid=None):
        B = img_emb.shape[0]
        init_estimate = torch.cat([self.init_pose, self.init_cam], -1).unsqueeze(1)
        tok0 = self.init_to_token(torch.cat([cond_info.view(B, 1, -1), init_estimate.expand(B, -1, -1)], -1)).view(B, 1, -1)
        prev = self.prev_to_token(prev_estimate.view(B, 1, -1)).view(B, 1, -1)
        ptok = self.prompt_tokens(prompt)
        img_emb = self.ray_cond_embed(img_emb, ray_cond)
        tokens = torch.cat([tok0, prev, self.hand_box_emb.expand(B, -1, -1), self.kp_emb.expand(B, -1, -1),
                            self.kp3d_emb.expand(B, -1, -1), ptok], 1)
        n_kp, n_kp3d = self.kp_emb.shape[1], self.kp3d_emb.shape[1]
        zeros = torch.zeros_like(tokens[:, : 2 + n_kp + n_kp3d + 1])
        aug = torch.cat([zeros[:, :1], prev, zeros[:, 1: 1 + 2 + n_kp + n_kp3d], ptok], 1)
        hb_start, kps_start = 2, 4
        kps3d_start = kps_start + n_kp
        ctx = img_emb.flatten(2).permute(0, 2, 1)
        ctx_pe = self.dense_pe.flatten(2).permute(0, 2, 1).expand(B, -1, -1)
        depth = len(self.dec.layers)
        mask = None
        if prompt_valid is not None:
            keep = torch.cat([torch.ones_like(tokens[:, : tokens.shape[1] - prompt.shape[1], 0]), prompt_valid], 1)   # [B,N]
            eye = torch.eye(tokens.shape[1])[None]
            mask = (keep[:, :, None] * keep[:, None, :] + eye) > 0     # Meta's rule, diagonal set so a masked row is not empty
        for i, layer in enumerate(self.dec.layers):
            tokens = decoder_layer(layer, tokens, ctx, aug, ctx_pe, mask)
            if i < depth - 1:
                out = self.pose_output(self.dec.norm_final(tokens)[:, 0], bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size)
                tokens, aug = self.kp2d_update(kps_start, img_emb, tokens, aug, out)
                aug = self.kp3d_update(kps3d_start, aug, out)
        final = self.dec.norm_final(tokens)
        out = self.pose_output(final[:, 0], bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size)
        out["hand_box"] = self.m.bbox_embed(final[:, hb_start: hb_start + 2]).sigmoid()
        return out

    def forward(self, img_emb, ray_cond, cond_info, bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size,
                prev_estimate=None, prompt=None, use_prev=None, prompt_valid=None):
        B = img_emb.shape[0]
        init = torch.cat([self.init_pose, self.init_cam], -1).expand(B, -1)
        if self.hand:
            prev_estimate = init
            prompt = torch.tensor([[[0.0, 0.0, -2.0]]]).expand(B, -1, -1)
        else:
            # use_prev 0 = first pass (Meta feeds the init estimate as "previous"), 1 = keypoint-prompt re-run
            prev_estimate = torch.where(use_prev.view(-1, 1) > 0.5, prev_estimate, init)
        out = self.run(img_emb, ray_cond, cond_info, bbox_center, bbox_scale_b, ori_img_size, cam_int, affine, img_size,
                       prev_estimate, prompt, None if self.hand else prompt_valid)
        if self.hand:
            return (out["hand"], out["scale"], out["shape"], out["joint_global_rots"][:, 42],
                    out["pred_keypoints_2d"], out["pred_keypoints_2d_cropped"])
        pose_raw = torch.cat([out["pred_pose_raw"], out["shape"], out["scale"], out["hand"], out["face"], out["pred_cam"]], 1)
        return (out["shape"], out["scale"], out["body_pose"], out["global_rot"], out["hand"], out["pred_cam_t"],
                out["pred_keypoints_3d"], out["pred_keypoints_2d"], out["mhr_model_params"], pose_raw, out["hand_box"])


class HandFuseGraph(nn.Module):
    """Meta's run_inference "full", steps 3 + IK: drop both hand-decoder results into the body estimate.

    Inputs: `ori_body_pose` = the FIRST body pass (sets the reference local wrist), the body estimate to update (after the
    keypoint-prompt re-run, or the first pass again), both hand-decoder results AS THE GRAPH RETURNS THEM (the left one ran
    on the mirrored photo; it is un-mirrored here), and `valid` [1,2] (left, right) = the box-size / keypoints-inside /
    wrist-distance tests done by the caller. Outputs the fused shape + MHR model params and `angle_ok` [1,2] (criterion 1:
    the hand's wrist within 1.4 rad of the body's), which the caller ANDs into `valid` before choosing the prompts.
    """
    def __init__(self, model):
        super().__init__()
        hp = model.head_pose
        self.hp = hp
        self.register_buffer("joint_rotation", hp.joint_rotation.detach().clone())
        self.s_r_mean, self.s_l_mean = float(hp.scale_mean[8]), float(hp.scale_mean[9])
        self.s_r_std, self.s_l_std = float(hp.scale_comps[8, 8]), float(hp.scale_comps[9, 9])
        self.wrist_slots = [41, 43, 42, 31, 33, 32]          # left XZY, right XZY in body_pose
        self.thresh = 1.4

    @staticmethod
    def angle_diff(A, B):
        R = A @ B.transpose(-2, -1)
        c = (R[..., 0, 0] + R[..., 1, 1] + R[..., 2, 2] - 1) / 2
        return torch.acos(torch.clamp(c, -1.0, 1.0))

    @staticmethod
    def fix_wrist_euler(e, lx=(-2.2, 1.0), lz=(-2.2, 1.5), ly=(-1.2, 1.5)):
        x, z, y = e[..., 0], e[..., 1], e[..., 2]
        xa = torch.atan2(torch.sin(x + math.pi), torch.cos(x + math.pi))
        za = torch.atan2(torch.sin(-(z + math.pi)), torch.cos(-(z + math.pi)))
        ya = torch.atan2(torch.sin(y + math.pi), torch.cos(y + math.pi))
        v = lambda a, l: torch.clamp(l[0] - a, min=0.0) ** 2 + torch.clamp(a - l[1], min=0.0) ** 2
        alt = (v(xa, lx) + v(za, lz) + v(ya, ly)) < (v(x, lx) + v(z, lz) + v(y, ly))
        return torch.where(alt[..., None], torch.stack([xa, za, ya], -1), e)

    def forward(self, ori_body_pose, body_pose, global_rot, scale, shape, hand,
                l_hand, l_scale, l_shape, l_wrist, r_hand, r_scale, r_shape, r_wrist, valid):
        hp = self.hp
        # un-mirror the left result: scale slot, wrist rotation, and which half of the PCA hand vector is "the" hand
        l9 = ((self.s_r_mean + self.s_r_std * l_scale[:, 8]) - self.s_l_mean) / self.s_l_std
        flipm = torch.tensor([[1.0], [-1.0], [-1.0]])
        l_wrist = l_wrist * flipm
        upd_hand = torch.cat([l_hand[:, 54:], r_hand[:, 54:]], 1)
        upd_scale = torch.cat([scale[:, :8], r_scale[:, 8:9], l9[:, None], scale[:, 10:18],
                               (l_scale[:, 18:] + r_scale[:, 18:]) / 2], 1)
        upd_shape = torch.cat([shape[:, :40], (l_shape[:, 40:] + r_shape[:, 40:]) / 2], 1)
        # FK of the body with the new hands -> forearm global rotations -> local wrist that reproduces the hand's wrist
        rots = hp.mhr_forward(global_trans=global_rot * 0, global_rot=global_rot, body_pose_params=body_pose,
                              hand_pose_params=upd_hand, scale_params=upd_scale, shape_params=upd_shape)[4]
        lowarm = rots[:, [76, 40]]
        wz = lowarm @ self.joint_rotation[[77, 41]]
        pred = torch.stack([l_wrist, r_wrist], 1)
        fused = torch.einsum("kabc,kabd->kadc", pred, wz)
        wrist_xzy = self.fix_wrist_euler(roma.rotmat_to_euler("XZY", fused))
        ori = roma.euler_to_rotmat("XZY", ori_body_pose[:, self.wrist_slots].unflatten(1, (2, 3)))
        angle_ok = self.angle_diff(ori, fused) < self.thresh                       # [B,2]
        ok = angle_ok & (valid > 0.5)
        okf = ok.float()
        # body pose: the six wrist slots
        cur = body_pose[:, self.wrist_slots].unflatten(1, (2, 3))
        new6 = torch.where(ok[..., None], wrist_xzy, cur).flatten(1, 2)
        n = body_pose.shape[1]
        src = list(range(n))                       # a python list: an index_put on a tensor would export an invalid Add
        for k, slot in enumerate(self.wrist_slots):
            src[slot] = n + k
        body_pose = torch.cat([body_pose, new6], 1)[:, torch.tensor(src)]
        hand = torch.where(ok[..., None], upd_hand.unflatten(1, (2, 54)), hand.unflatten(1, (2, 54))).flatten(1, 2)
        s9 = torch.where(ok[:, 0], upd_scale[:, 9], scale[:, 9]); s8 = torch.where(ok[:, 1], upd_scale[:, 8], scale[:, 8])
        n_ok = okf.sum(1, keepdim=True)
        any_ok = n_ok > 0
        s18 = torch.where(any_ok, (l_scale[:, 18:] * okf[:, [0]] + r_scale[:, 18:] * okf[:, [1]]) / (n_ok + 1e-8), scale[:, 18:])
        sh40 = torch.where(any_ok, (l_shape[:, 40:] * okf[:, [0]] + r_shape[:, 40:] * okf[:, [1]]) / (n_ok + 1e-8), shape[:, 40:])
        scale = torch.cat([scale[:, :8], s8[:, None], s9[:, None], scale[:, 10:18], s18], 1)
        shape = torch.cat([shape[:, :40], sh40], 1)
        mp = hp.mhr_forward(global_trans=global_rot * 0, global_rot=global_rot, body_pose_params=body_pose,
                            hand_pose_params=hand, scale_params=scale, shape_params=shape)[3]
        return shape, mp, angle_ok.float()


def export_hands(model, out, check, sess):
    """sam3d_decoder_v2.onnx + sam3d_hand_decoder.onnx + sam3d_hand_fuse.onnx, each checked against Meta's own code path."""
    rng = np.random.default_rng(1)
    W, H, f = 1000.0, 1255.0, float(np.hypot(1000.0, 1255.0))
    def geo(scale, cx, cy):
        aff = [[512 / scale, 0, 256 - 512 / scale * cx], [0, 512 / scale, 256 - 512 / scale * cy]]
        ys, xs = np.mgrid[0:512, 0:512].astype(np.float32)
        ray = np.stack([((xs - aff[0][2]) / aff[0][0] - W / 2) / f, ((ys - aff[1][2]) / aff[1][1] - H / 2) / f])[None]
        return [torch.tensor(rng.normal(size=(1, 1280, 32, 32)).astype(np.float32)), torch.tensor(ray, dtype=torch.float32),
                torch.tensor([[(cx - W / 2) / f, (cy - H / 2) / f, scale / f]], dtype=torch.float32),
                torch.tensor([[cx, cy]], dtype=torch.float32), torch.tensor([scale], dtype=torch.float32),
                torch.tensor([[W, H]], dtype=torch.float32),
                torch.tensor([[[f, 0, W / 2], [0, f, H / 2], [0, 0, 1]]], dtype=torch.float32),
                torch.tensor([aff], dtype=torch.float32), torch.tensor([[512.0, 512.0]], dtype=torch.float32)]
    base_in = ["features", "ray_cond", "cond_info", "bbox_center", "bbox_scale", "ori_img_size", "cam_int", "affine", "crop_size"]

    model._batch_size, model._max_num_person = 1, 1      # Meta's [batch, person, ...] bookkeeping for its reference path
    def meta_batch(a):
        return {"bbox_center": a[3][None], "bbox_scale": torch.stack([a[4], a[4]], -1)[None], "ori_img_size": a[5][None],
                "cam_int": a[6], "affine_trans": a[7][None], "img_size": a[8][None], "ray_cond": a[1], "ray_cond_hand": a[1],
                "img": torch.zeros(1, 1, 3, 512, 512)}

    # ---- body decoder v2 ---------------------------------------------------------------------------------------
    g = PromptDecoderGraph(model, hand=False).eval()
    a = geo(1506.25, 435.0, 622.5)
    init = torch.cat([g.init_pose, g.init_cam], -1)
    dummy = torch.tensor([[[0.0, 0.0, -2.0]]])
    names = base_in + ["prev_estimate", "prompt", "use_prev", "prompt_valid"]
    off, on = torch.tensor([0.0]), torch.tensor([1.0])
    def pad(p):          # -> [1, MAX_PROMPTS, 3] + validity
        k = p.shape[1]
        filler = torch.tensor([[[0.0, 0.0, -2.0]]]).expand(1, MAX_PROMPTS - k, 3)
        return torch.cat([p, filler], 1), torch.cat([torch.ones(1, k), torch.zeros(1, MAX_PROMPTS - k)], 1)
    outs = ["shape", "scale", "body_pose", "global_rot", "hand", "cam_t", "kp3d", "kp2d", "model_params", "pose_raw", "hand_box"]
    print("exporting body decoder v2 ...", flush=True)
    with torch.no_grad():
        pd, vd = pad(dummy)
        torch.onnx.export(g, tuple(a + [init * 0, pd, off, vd]), out("sam3d_decoder_v2.onnx"), input_names=names, output_names=outs,
                          opset_version=17, dynamo=False, do_constant_folding=True)
        first = g(*a, init * 0, pd, off, vd)
        # Meta, first pass
        model.body_batch_idx, model.hand_batch_idx = [0], []
        cond = a[2]
        tok, mo = model.forward_decoder(a[0], init_estimate=None, keypoints=dummy, prev_estimate=None, condition_info=cond,
                                        batch=meta_batch(a))
        mo = mo[-1]
        check("v2 vs Meta (pass 1)", [mo["mhr_model_params"].numpy(),
                                       model.bbox_embed(tok).sigmoid().numpy()],
              [first[8].numpy(), first[10].numpy()])
        # Meta, keypoint-prompt re-run with 3 prompts
        prompt = torch.tensor([[[0.40, 0.30, 41.0], [0.62, 0.33, 62.0], [0.45, 0.52, 8.0]]])
        prev = first[9]
        pp, vp = pad(prompt)
        second = g(*a, prev, pp, on, vp)
        tok2, mo2 = model.forward_decoder(a[0], init_estimate=None, keypoints=prompt, prev_estimate=prev[:, None],
                                          condition_info=cond, batch=meta_batch(a))
        check("v2 vs Meta (prompted)", [mo2[-1]["mhr_model_params"].numpy()], [second[8].numpy()])
    s = sess(out("sam3d_decoder_v2.onnx"))
    have = {i.name for i in s.get_inputs()}   # ori_img_size is pruned (the intrinsics centre is used instead)
    for extra, ref in (([init * 0, pd, off, vd], first), ([prev, pp, on, vp], second)):
        feed = zip(names, a + extra)
        got = s.run(None, {n: x.numpy() for n, x in feed if n in have})
        check("sam3d_decoder_v2", [r.numpy() for r in ref], got)

    # ---- hand decoder ------------------------------------------------------------------------------------------
    gh = PromptDecoderGraph(model, hand=True).eval()
    ah = geo(160.0, 300.0, 700.0)
    houts = ["hand", "scale", "shape", "wrist_rot", "kp2d", "kp2d_cropped"]
    print("exporting hand decoder ...", flush=True)
    with torch.no_grad():
        torch.onnx.export(gh, tuple(ah), out("sam3d_hand_decoder.onnx"), input_names=base_in, output_names=houts,
                          opset_version=17, dynamo=False, do_constant_folding=True)
        href = gh(*ah)
        model.body_batch_idx, model.hand_batch_idx = [], [0]
        _, mh_ = model.forward_decoder_hand(ah[0], init_estimate=None, keypoints=dummy, prev_estimate=None,
                                            condition_info=ah[2], batch=meta_batch(ah))
        mh_ = mh_[-1]
        check("hand vs Meta", [mh_["hand"].numpy(), mh_["joint_global_rots"][:, 42].numpy(), mh_["pred_keypoints_2d"].numpy()],
              [href[0].numpy(), href[3].numpy(), href[4].numpy()])
    sh = sess(out("sam3d_hand_decoder.onnx")); have = {i.name for i in sh.get_inputs()}
    got = sh.run(None, {n: x.numpy() for n, x in zip(base_in, ah) if n in have})
    check("sam3d_hand_decoder", [r.numpy() for r in href], got)

    # ---- fusion ------------------------------------------------------------------------------------------------
    fg = HandFuseGraph(model).eval()
    with torch.no_grad():
        lh = gh(*geo(150.0, 600.0, 690.0))
    fin = (first[2], second[2], first[3], second[1], second[0], second[4],
           lh[0], lh[1], lh[2], lh[3], href[0], href[1], href[2], href[3], torch.tensor([[1.0, 1.0]]))
    fnames = ["ori_body_pose", "body_pose", "global_rot", "scale", "shape", "hand", "l_hand", "l_scale", "l_shape", "l_wrist",
              "r_hand", "r_scale", "r_shape", "r_wrist", "valid"]
    print("exporting hand fusion ...", flush=True)
    with torch.no_grad():
        torch.onnx.export(fg, fin, out("sam3d_hand_fuse.onnx"), input_names=fnames, output_names=["fused_shape", "model_params", "angle_ok"],
                          opset_version=17, dynamo=False, do_constant_folding=True)
        fref = fg(*fin)
    sf = sess(out("sam3d_hand_fuse.onnx")); have = {i.name for i in sf.get_inputs()}
    got = sf.run(None, {n: x.numpy() for n, x in zip(fnames, fin) if n in have})
    # ONNX Runtime vs the PyTorch HandFuseGraph only: the fusion logic itself (vs Meta's run_inference) is checked end to end
    # on a real photo (docs/IMAGE_BODY_PREDICTION.md: hands 1.9 / 4.1 mm from Meta's full pipeline).
    check("sam3d_hand_fuse (ORT vs torch)", [r.numpy() for r in fref], got)
    print("  angle_ok", fref[2].numpy())

# --------------------------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--meta-repo", required=True, help="clone of facebookresearch/sam-3d-body")
    ap.add_argument("--checkpoint-dir", required=True, help="facebook/sam-3d-body-dinov3 download (model.ckpt + assets/mhr_model.pt)")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--skip-backbone", action="store_true", help="skip the 3.4 GB backbone")
    ap.add_argument("--hands", action="store_true",
                    help="export ONLY the hand-refinement graphs (sam3d_decoder_v2 / sam3d_hand_decoder / sam3d_hand_fuse)")
    a = ap.parse_args()

    sys.path.insert(0, a.meta_repo)
    if not torch.cuda.is_available():
        # Meta's code calls .cuda() in a few places (hand-joint index tensors, ray grid): make it a no-op on CPU-only machines
        torch.Tensor.cuda = lambda self, *args, **kwargs: self
    os.makedirs(a.out_dir, exist_ok=True)
    out = lambda n: os.path.join(a.out_dir, n)
    ckpt, mhr_pt = os.path.join(a.checkpoint_dir, "model.ckpt"), os.path.join(a.checkpoint_dir, "assets", "mhr_model.pt")

    install(); install_mhr_utils(); install_hand(); install_head(); install_camera()
    from sam_3d_body import load_sam_3d_body

    model, cfg = load_sam_3d_body(ckpt, device=torch.device("cpu"), mhr_path=mhr_pt)
    model.head_pose.mhr = MhrAsMeta(MhrTorch(mhr_pt))
    if hasattr(model, "head_pose_hand"):
        model.head_pose_hand.mhr = MhrAsMeta(MhrTorch(mhr_pt))   # the hand head's TorchScript MHR cannot be traced either
    rng = np.random.default_rng(0)

    def check(name, ref, got):
        # relative to each output's magnitude: 2D keypoints are in pixels (hundreds), parameters are O(1)
        worst, which = 0.0, 0
        for k, (r, g) in enumerate(zip(ref, got)):
            r = np.asarray(r, dtype=np.float64)
            g = np.asarray(g, dtype=np.float64)
            # a NaN difference compares False against `worst` and would pass silently; the predictor rejects such outputs
            assert np.isfinite(r).all() and np.isfinite(g).all(), "%s output %d is not finite" % (name, k)
            d = float(np.abs(r - g).max()) / max(1.0, float(np.abs(r).max()))
            if d > worst:
                worst, which = d, k
        print("  %-24s max rel |torch-ort| %.2e (output %d)" % (name, worst, which), flush=True)
        assert worst < 5e-3, "%s does not match PyTorch" % name

    import onnxruntime as ort
    sess = lambda p: ort.InferenceSession(p, providers=["CPUExecutionProvider"])

    if a.hands:
        # Export + validate in a staging folder and publish the three graphs together: the predictor enables hand refinement
        # as soon as all three exist, so a half-exported or failed set must never land next to the other models.
        import shutil, tempfile
        stage = tempfile.mkdtemp(prefix=".hands-", dir=a.out_dir)
        try:
            export_hands(model, lambda n: os.path.join(stage, n), check, sess)
            for n in ("sam3d_decoder_v2.onnx", "sam3d_hand_decoder.onnx", "sam3d_hand_fuse.onnx"):
                os.replace(os.path.join(stage, n), out(n))
        finally:
            shutil.rmtree(stage, ignore_errors=True)
        print("done ->", a.out_dir)
        return

    # ---- decoder -----------------------------------------------------------------------------------------------
    g = DecoderGraph(model).eval()
    W, H, f = 1000.0, 1255.0, float(np.hypot(1000.0, 1255.0))
    scale, cx, cy = 1506.25, 435.0, 622.5
    aff = [[512 / scale, 0, 256 - 512 / scale * cx], [0, 512 / scale, 256 - 512 / scale * cy]]
    ys, xs = np.mgrid[0:512, 0:512].astype(np.float32)
    ray = np.stack([((xs - aff[0][2]) / aff[0][0] - W / 2) / f, ((ys - aff[1][2]) / aff[1][1] - H / 2) / f])[None]
    args = (torch.tensor(rng.normal(size=(1, 1280, 32, 32)).astype(np.float32)), torch.tensor(ray, dtype=torch.float32),
            torch.tensor([[(cx - W / 2) / f, (cy - H / 2) / f, scale / f]], dtype=torch.float32),
            torch.tensor([[cx, cy]], dtype=torch.float32), torch.tensor([scale], dtype=torch.float32),
            torch.tensor([[W, H]], dtype=torch.float32),
            torch.tensor([[[f, 0, W / 2], [0, f, H / 2], [0, 0, 1]]], dtype=torch.float32),
            torch.tensor([aff], dtype=torch.float32), torch.tensor([[512.0, 512.0]], dtype=torch.float32))
    in_names = ["features", "ray_cond", "cond_info", "bbox_center", "bbox_scale", "ori_img_size", "cam_int", "affine", "crop_size"]
    out_names = ["shape", "scale", "body_pose", "global_rot", "hand", "cam_t", "kp3d", "kp2d", "model_params", "vertices"]
    print("exporting decoder ...", flush=True)
    with torch.no_grad():
        torch.onnx.export(g, args, out("sam3d_decoder.onnx"), input_names=in_names, output_names=out_names, opset_version=17,
                          dynamo=False, do_constant_folding=True)
        ref = g(*args)
    s = sess(out("sam3d_decoder.onnx")); have = {i.name for i in s.get_inputs()}
    got = s.run(None, {n: x.numpy() for n, x in zip(in_names, args) if n in have})
    check("sam3d_decoder", [r.numpy() for r in ref], got)

    # ---- MHR body ----------------------------------------------------------------------------------------------
    mt = MhrTorch(mhr_pt).eval()
    shape, mp = torch.randn(1, 45) * 0.5, torch.randn(1, 204) * 0.2
    print("exporting MHR body ...", flush=True)
    with torch.no_grad():
        torch.onnx.export(mt, (shape, mp), out("mhr_body.onnx"), input_names=["shape", "model_params"],
                          output_names=["vertices", "joint_t", "joint_q"], opset_version=17, dynamo=False, do_constant_folding=True)
        ref = mt(shape, mp)
    got = sess(out("mhr_body.onnx")).run(None, {"shape": shape.numpy(), "model_params": mp.numpy()})
    check("mhr_body", [r.numpy() for r in ref], got)
    mt.faces.numpy().astype("<i4").tofile(out("mhr_faces.i32"))
    # UV layout for texturing: int32 nt, int32 nf, float32 texcoords[nt,2], int32 texcoord_faces[nf,3] (indices into
    # texcoords; one UV face per mesh face, so seams are UV-split vertices, not extra positions)
    import struct
    sd = torch.jit.load(mhr_pt, map_location="cpu").state_dict()
    tc, tf = sd["character_torch.mesh.texcoords"].numpy().astype("<f4"), sd["character_torch.mesh.texcoord_faces"].numpy().astype("<i4")
    with open(out("mhr_uv.bin"), "wb") as fh:
        fh.write(struct.pack("<ii", tc.shape[0], tf.shape[0])); fh.write(tc.tobytes()); fh.write(tf.tobytes())

    # ---- backbone ----------------------------------------------------------------------------------------------
    if not a.skip_backbone:
        import onnx, tempfile, shutil
        bb = BackboneGraph(model).eval().float()
        for p in bb.parameters(): p.data = p.data.float()
        img = torch.rand(1, 3, 512, 512)
        print("exporting backbone (this takes a while and ~8 GB of RAM) ...", flush=True)
        with tempfile.TemporaryDirectory() as tmp:   # the exporter scatters one file per tensor; fold them into ONE .data
            with torch.no_grad():
                torch.onnx.export(bb, (img,), os.path.join(tmp, "b.onnx"), input_names=["image"], output_names=["features"],
                                  opset_version=17, dynamo=False, do_constant_folding=True)
                ref = bb(img)
            m = onnx.load(os.path.join(tmp, "b.onnx"))
            # save_model APPENDS to an existing external-data file: a second export into the same dir would double it
            for stale in (out("sam3d_backbone.onnx"), out("sam3d_backbone.onnx.data")):
                if os.path.exists(stale): os.remove(stale)
            onnx.save_model(m, out("sam3d_backbone.onnx"), save_as_external_data=True, all_tensors_to_one_file=True,
                            location="sam3d_backbone.onnx.data", size_threshold=1024)
        got = sess(out("sam3d_backbone.onnx")).run(None, {"image": img.numpy()})
        check("sam3d_backbone", [ref.numpy()], got)

    print("done ->", a.out_dir)


if __name__ == "__main__":
    main()
