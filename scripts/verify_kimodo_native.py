"""Replay of QtMeshEditor's native Kimodo runtime (src/AIMotion/KimodoNative.cpp) in numpy + onnxruntime.

Used by scripts/export-kimodo-onnx.py --verify: runs Kimodo's own PyTorch multi-prompt generation (the path
ai/kimodo/generate.py takes, post-processing on) and this replay on the SAME text features and noise, and
compares the outputs. The replay only reads kimodo_meta.json + the ONNX graphs, so a match means the C++
port (which transcribes this file) has everything it needs.
"""

import json
import os
from types import SimpleNamespace

import numpy as np
import onnxruntime as ort
import torch


def mat_to_quat_wxyz(m):
    """kimodo.geometry.matrix_to_quaternion (PyTorch3D) in numpy; m [...,3,3] -> [...,4] (w,x,y,z)."""
    m00, m01, m02 = m[..., 0, 0], m[..., 0, 1], m[..., 0, 2]
    m10, m11, m12 = m[..., 1, 0], m[..., 1, 1], m[..., 1, 2]
    m20, m21, m22 = m[..., 2, 0], m[..., 2, 1], m[..., 2, 2]
    q_abs = np.sqrt(np.maximum(np.stack([1 + m00 + m11 + m22, 1 + m00 - m11 - m22,
                                         1 - m00 + m11 - m22, 1 - m00 - m11 + m22], -1), 0))
    cand = np.stack([
        np.stack([q_abs[..., 0] ** 2, m21 - m12, m02 - m20, m10 - m01], -1),
        np.stack([m21 - m12, q_abs[..., 1] ** 2, m10 + m01, m02 + m20], -1),
        np.stack([m02 - m20, m10 + m01, q_abs[..., 2] ** 2, m12 + m21], -1),
        np.stack([m10 - m01, m20 + m02, m21 + m12, q_abs[..., 3] ** 2], -1),
    ], -2) / (2.0 * np.maximum(q_abs, 0.1))[..., None]
    k = q_abs.argmax(-1)
    return np.take_along_axis(cand, k[..., None, None].repeat(4, -1), -2)[..., 0, :]


def quat_wxyz_to_mat(q):
    r, i, j, k = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    two_s = 2.0 / (q * q).sum(-1)
    o = np.stack([1 - two_s * (j * j + k * k), two_s * (i * j - k * r), two_s * (i * k + j * r),
                  two_s * (i * j + k * r), 1 - two_s * (i * i + k * k), two_s * (j * k - i * r),
                  two_s * (i * k - j * r), two_s * (j * k + i * r), 1 - two_s * (i * i + j * j)], -1)
    return o.reshape(q.shape[:-1] + (3, 3))


def smooth_root(root):
    """kimodo.motion_rep.smooth_root.get_smooth_root_pos (C++: KimodoSmoothRoot): x/z smoothed, y kept."""
    from kimodo.motion_rep.smooth_root import smooth_signal
    xz = smooth_signal(root[:, [0, 2]].astype(np.float32), np.full(root.shape[0], 0.06))
    return np.stack([xz[:, 0], root[:, 1], xz[:, 1]], -1).astype(np.float32)


class Native:
    def __init__(self, out_dir):
        self.meta = json.load(open(os.path.join(out_dir, "kimodo_meta.json")))
        so = ort.SessionOptions()
        so.intra_op_num_threads = max(1, (os.cpu_count() or 2) - 1)
        mk = lambda n: ort.InferenceSession(os.path.join(out_dir, n), so, providers=["CPUExecutionProvider"])
        self.den, self.dec = mk("kimodo_denoiser.onnx"), mk("kimodo_decode.onnx")
        self.enc, self.tra = mk("kimodo_encode.onnx"), mk("kimodo_transition.onnx")
        self.mean = np.asarray(self.meta["mean"], np.float32)
        self.std = np.asarray(self.meta["stdEps"], np.float32)
        self.D = self.meta["featureSize"]
        self.NT = self.meta["transitionFrames"]
        self.den_inputs = {i.name for i in self.den.get_inputs()}

    # --- diffusion schedule (kimodo/model/diffusion.py, float32 like the torch buffers) ---
    def schedule(self, steps):
        base = np.asarray(self.meta["diffusion"]["alphasCumprodBase"], np.float32)
        n = len(base)
        frac = (n - 1) / max(1, steps - 1)
        use = np.minimum(np.round(np.arange(n, dtype=np.float32) * np.float32(frac)).astype(np.int64), n - 1)
        acp = base[use]
        last = np.concatenate([[np.float32(1.0)], acp[:-1]]).astype(np.float32)
        betas = (1.0 - acp / last).astype(np.float32)
        acp = np.maximum(np.cumprod((1.0 - betas).astype(np.float32), dtype=np.float32), np.float32(1e-9))
        prev = np.concatenate([[np.float32(1.0)], acp[:-1]]).astype(np.float32)
        return use, acp, prev

    def denoise(self, x, text, t, heading, mask, obs):
        feed = {"x": x[None].astype(np.float32), "x_pad_mask": np.ones((1, x.shape[0]), bool),
                "text_feat": text[None, None].astype(np.float32), "timesteps": np.array([t], np.int64),
                "first_heading_angle": np.array([heading], np.float32),
                "motion_mask": mask[None].astype(np.float32), "observed_motion": obs[None].astype(np.float32)}
        return self.den.run(None, {k: v for k, v in feed.items() if k in self.den_inputs})[0][0]

    def sample(self, text, length, heading, mask, obs_norm, noise, steps):
        w_text, w_cons = self.meta["cfg"]["weights"]
        use, acp, prev = self.schedule(steps)
        zero_text = np.zeros_like(text)
        zero_mask = np.zeros_like(mask)
        constrained = bool(mask.any())
        x = noise.astype(np.float32)
        for i in reversed(range(steps)):
            t = int(use[i])
            p_text = self.denoise(x, text, t, heading, zero_mask, obs_norm)
            p_unc = self.denoise(x, zero_text, t, heading, zero_mask, obs_norm)
            p_cons = self.denoise(x, zero_text, t, heading, mask, obs_norm) if constrained else p_unc
            pred = p_unc + w_text * (p_text - p_unc) + w_cons * (p_cons - p_unc)
            eps = (np.sqrt(1.0 / acp[i]) * x - pred) / np.sqrt(1.0 / acp[i] - 1.0)
            x = (pred * np.sqrt(prev[i]) + np.sqrt(1.0 - prev[i]) * eps).astype(np.float32)
        return x

    def decode(self, feats):
        o = self.dec.run(None, {"features": feats[None].astype(np.float32)})
        return {k: v[0] for k, v in zip(["local", "global", "root", "posed", "contacts", "smooth"], o)}

    def encode(self, local, root):
        smooth = smooth_root(root)
        return self.enc.run(None, {"local_rot_mats": local[None].astype(np.float32),
                                   "root_positions": root[None].astype(np.float32),
                                   "smooth_root_pos": smooth[None].astype(np.float32)})[0][0]

    def postprocess(self, feats, full_body_frames=0, hip_in=None, rot_in=None):
        """kimodo.postprocess.post_process_motion through the native MotionCorrection entry point."""
        from motion_correction import motion_postprocess
        pp = self.meta["postprocess"]
        rig_meta = pp["rig"]
        d = self.decode(feats)
        T, J = d["local"].shape[:2]
        names = self.meta["skeleton30"]["names"]
        tags = {rig_meta["leftHand"]: "LeftHand", rig_meta["rightHand"]: "RightHand",
                rig_meta["leftFoot"]: "LeftFoot", rig_meta["rightFoot"]: "RightFoot"}
        rig = [SimpleNamespace(name=names[i], parent=names[p] if p >= 0 else None,
                               t_pose_translation=rig_meta["translations"][i],
                               t_pose_rotation=rig_meta["rotationsXYZW"][i], retarget_tag=tags.get(i))
               for i, p in enumerate(rig_meta["parents"])]
        hip = torch.from_numpy(d["root"][None].copy())
        rot = torch.from_numpy(mat_to_quat_wxyz(d["local"])[None].astype(np.float32))
        contacts = torch.from_numpy(d["contacts"][None].astype(np.float32))
        hin = np.zeros((T, 3), np.float32)
        rin = np.zeros((T, J, 4), np.float32)
        rin[..., 0] = 1.0
        masks = {k: torch.zeros(T) for k in ["FullBody", "LeftFoot", "RightFoot", "LeftHand", "RightHand", "Root"]}
        if full_body_frames:
            hin[:full_body_frames] = hip_in
            rin[:full_body_frames] = rot_in
            masks["FullBody"][:full_body_frames] = 1.0
        motion_postprocess.correct_motion(hip, rot, contacts, torch.from_numpy(hin), torch.from_numpy(rin), masks,
                                          pp["contactThreshold"], pp["rootMargin"], rig, False)
        return self.encode(quat_wxyz_to_mat(rot[0].numpy()), hip[0].numpy())

    def generate(self, segments, noises, steps, postprocess=True):
        """segments: [(text feature [4096], frames)]; noises: one [frames(+NT), D] array per segment."""
        generated = []
        for idx, (text, n) in enumerate(segments):
            first = not generated
            if first:
                length, heading = n, 0.0
                obs = np.zeros((length, self.D), np.float32)
                mask = np.zeros((length, self.D), np.float32)
            else:
                latest_motions = generated.pop()
                generated.append(latest_motions[:-self.NT])
                latest = latest_motions[-self.NT:]
                obs_t, mask_t, hd, root2d, hip_in, rot_in = self.tra.run(None, {"latest": latest[None].astype(np.float32)})
                length, heading = n + self.NT, float(hd[0])
                obs = np.concatenate([obs_t[0], np.zeros((n, self.D), np.float32)])
                mask = np.concatenate([mask_t[0], np.zeros((n, self.D), np.float32)])
            obs_norm = (obs - self.mean) / self.std
            x = self.sample(text, length, heading, mask, obs_norm, noises[idx], steps)
            motion = x * self.std + self.mean
            if not first:
                sr = self.meta["slices"]["smooth_root_pos"][0]
                motion = motion.copy()
                motion[:, sr + 0] += root2d[0, 0]
                motion[:, sr + 2] += root2d[0, 1]
                if postprocess:
                    motion = self.postprocess(motion, self.NT, hip_in, mat_to_quat_wxyz(rot_in) if rot_in.shape[-1] == 3 else rot_in)
                else:
                    trans = motion[:self.NT]
                    motion = motion[self.NT:]
                    alpha = np.linspace(1, 0, self.NT, dtype=np.float32)[:, None]
                    generated.append(latest[:self.NT] * alpha + (1 - alpha) * trans)
            elif postprocess:
                motion = self.postprocess(motion)
            generated.append(motion)
        full = np.concatenate(generated)
        d = self.decode(full)
        return self.to77(d)

    def to77(self, d):
        s77 = self.meta["skeleton77"]
        local = np.repeat(np.asarray(s77["relaxedHandsRestPose"], np.float32)[None], d["local"].shape[0], 0)
        local[:, self.meta["skeleton30"]["slice77"]] = d["local"]
        c = d["contacts"]
        return {"local_rot_mats": local, "root_positions": d["root"],
                "foot_contacts": np.concatenate([c[:, :2], c[:, 1:2], c[:, 2:4], c[:, 3:4]], -1)}


def run(model, out_dir) -> int:
    import kimodo.model.kimodo_model as km
    nat = Native(out_dir)
    ref_path = os.path.expanduser("~/kimodo_export/verify/ref_text.npy")
    feats = np.load(ref_path) if os.path.exists(ref_path) else np.random.default_rng(1).normal(0, 0.02, (4, 4096))
    feats = feats.astype(np.float32)
    D, NT = nat.D, nat.NT
    steps = 20
    worst = 0.0
    cases = {"single": ([(0, 60)], True), "two segments": ([(1, 45), (3, 50)], True),
             "two segments, no post-processing": ([(2, 40), (0, 40)], False)}
    for case, (segs, post) in cases.items():
        rng = np.random.default_rng(7)
        noises = [rng.standard_normal(((n if i == 0 else n + NT), D)).astype(np.float32) for i, (_, n) in enumerate(segs)]
        it = iter(noises)
        orig = torch.randn

        def fake_randn(*shape, **kw):
            a = next(it)
            return torch.from_numpy(a[None].copy()).to(kw.get("device", "cpu"))

        texts = [f"seg{i}" for i in range(len(segs))]
        table = {t: torch.from_numpy(feats[k])[None] for t, (k, _) in zip(texts, segs)}

        class Enc:
            def __call__(self, ts):
                return torch.stack([table[t] for t in ts]), [1] * len(ts)

        model.text_encoder = Enc()
        km.sanitize_texts = lambda xs: list(xs)
        km.torch.randn = fake_randn
        try:
            ref = model(texts, [n for _, n in segs], constraint_lst=[], num_samples=1, multi_prompt=True,
                        num_transition_frames=NT, post_processing=post, return_numpy=True, num_denoising_steps=steps)
        finally:
            km.torch.randn = orig
        got = nat.generate([(feats[k], n) for k, n in segs], noises, steps, postprocess=post)
        lr = np.asarray(ref["local_rot_mats"]).reshape(got["local_rot_mats"].shape)
        rp = np.asarray(ref["root_positions"]).reshape(got["root_positions"].shape)
        e_rot = float(np.abs(lr - got["local_rot_mats"]).max())
        e_root = float(np.abs(rp - got["root_positions"]).max())
        e_c = float(np.abs(np.asarray(ref["foot_contacts"], np.float32).reshape(got["foot_contacts"].shape) - got["foot_contacts"]).mean())
        print(f"  {case}: frames {lr.shape[0]}  max |rot| {e_rot:.2e}  max |root| {e_root * 100:.3f} cm  contacts mismatch {e_c:.3f}")
        worst = max(worst, e_rot)
    return 0 if worst < 1e-2 else 1


def write_fixture(model, out_dir, fixture_path, reference_path, segs, steps=20, postprocess=True, variations=1):
    """Fixture for the C++ runtime (QTMESH_KIMODO_NATIVE_FIXTURE): text_<s> features and noise_<v>_<s>, plus
    Kimodo's own PyTorch output on them (reference_<v>_local / _root / _contacts) to compare the C++ against.
    segs: [(text feature index into ~/kimodo_export/verify/ref_text.npy, frames)]."""
    import kimodo.model.kimodo_model as km
    nat = Native(out_dir)
    feats = np.load(os.path.expanduser("~/kimodo_export/verify/ref_text.npy")).astype(np.float32)
    D, NT = nat.D, nat.NT
    rng = np.random.default_rng(11)
    arrays = {f"text_{s}": feats[k] for s, (k, _) in enumerate(segs)}
    ref = {}
    for v in range(variations):
        noises = [rng.standard_normal(((n if i == 0 else n + NT), D)).astype(np.float32) for i, (_, n) in enumerate(segs)]
        for s, nz in enumerate(noises):
            arrays[f"noise_{v}_{s}"] = nz
        it = iter(noises)
        orig = torch.randn
        texts = [f"seg{i}" for i in range(len(segs))]
        table = {t: torch.from_numpy(feats[k])[None] for t, (k, _) in zip(texts, segs)}

        class Enc:
            def __call__(self, ts):
                return torch.stack([table[t] for t in ts]), [1] * len(ts)

        model.text_encoder = Enc()
        km.sanitize_texts = lambda xs: list(xs)
        km.torch.randn = lambda *s, **k: torch.from_numpy(next(it)[None].copy())
        try:
            out = model(texts, [n for _, n in segs], constraint_lst=[], num_samples=1, multi_prompt=True,
                        num_transition_frames=NT, post_processing=postprocess, return_numpy=True,
                        num_denoising_steps=steps)
        finally:
            km.torch.randn = orig
        ref[f"reference_{v}_local"] = np.asarray(out["local_rot_mats"], np.float32).reshape(-1, 77, 3, 3)
        ref[f"reference_{v}_root"] = np.asarray(out["root_positions"], np.float32).reshape(-1, 3)
        ref[f"reference_{v}_contacts"] = np.asarray(out["foot_contacts"], np.float32).reshape(-1, 6)
    np.savez(fixture_path, **arrays)
    np.savez(reference_path, **ref)
    print(f"wrote {fixture_path} and {reference_path}")
