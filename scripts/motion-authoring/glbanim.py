#!/usr/bin/env python3
"""Minimal glb animation author: load a rigged .glb, REPLACE its animations
with procedurally authored rotation channels on named joints, save a new .glb.

No external deps — raw glTF 2.0 JSON + BIN chunk handling.

Authored rotations are LOCAL deltas applied onto each node's bind local
rotation: final_local = bind_local * delta  (delta in the joint's own bind
frame). Translation channels are written only for the root (hips) when
requested (bob/travel), everything else keeps bind translation.
"""
import json, math, struct, sys


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def quat_from_euler(x_deg=0.0, y_deg=0.0, z_deg=0.0, order="XYZ"):
    """Intrinsic rotations composed in `order` (each about the joint's own
    bind-local axis)."""
    def axis_quat(axis, deg):
        h = math.radians(deg) * 0.5
        s = math.sin(h)
        return {
            "X": (s, 0.0, 0.0, math.cos(h)),
            "Y": (0.0, s, 0.0, math.cos(h)),
            "Z": (0.0, 0.0, s, math.cos(h)),
        }[axis]
    q = (0.0, 0.0, 0.0, 1.0)
    vals = {"X": x_deg, "Y": y_deg, "Z": z_deg}
    for ax in order:
        q = quat_mul(q, axis_quat(ax, vals[ax]))
    return q


def normalize(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


class Glb:
    def __init__(self, path):
        raw = open(path, "rb").read()
        magic, version, length = struct.unpack_from("<III", raw, 0)
        assert magic == 0x46546C67, "not a glb"
        off = 12
        self.json = None
        self.bin = b""
        while off < length:
            clen, ctype = struct.unpack_from("<II", raw, off)
            off += 8
            chunk = raw[off:off + clen]
            off += clen
            if ctype == 0x4E4F534A:
                self.json = json.loads(chunk.decode("utf-8"))
            elif ctype == 0x004E4942:
                self.bin = chunk
        assert self.json is not None
        self.extra_bin = bytearray()

    # ---- node lookup -------------------------------------------------------
    def node_index(self, name_contains):
        for i, n in enumerate(self.json.get("nodes", [])):
            nm = n.get("name", "")
            if nm == name_contains:
                return i
        for i, n in enumerate(self.json.get("nodes", [])):
            nm = n.get("name", "").lower()
            if name_contains.lower() in nm:
                return i
        return -1

    def node_bind_rotation(self, idx):
        n = self.json["nodes"][idx]
        r = n.get("rotation", [0, 0, 0, 1])
        return tuple(r)

    def node_bind_translation(self, idx):
        n = self.json["nodes"][idx]
        t = n.get("translation", [0, 0, 0])
        return tuple(t)

    # ---- buffer append -----------------------------------------------------
    def _append(self, data_bytes, target=None):
        base = len(self.bin) + len(self.extra_bin)
        pad = (-base) % 4
        self.extra_bin += b"\x00" * pad
        base += pad
        self.extra_bin += data_bytes
        bv = {"buffer": 0, "byteOffset": base, "byteLength": len(data_bytes)}
        self.json.setdefault("bufferViews", []).append(bv)
        return len(self.json["bufferViews"]) - 1

    def _accessor(self, bv, comp_type, count, acc_type, mn=None, mx=None):
        acc = {"bufferView": bv, "componentType": comp_type,
               "count": count, "type": acc_type}
        if mn is not None:
            acc["min"] = mn
        if mx is not None:
            acc["max"] = mx
        self.json.setdefault("accessors", []).append(acc)
        return len(self.json["accessors"]) - 1

    def add_time_accessor(self, times):
        data = struct.pack("<%df" % len(times), *times)
        bv = self._append(data)
        return self._accessor(bv, 5126, len(times), "SCALAR",
                              [min(times)], [max(times)])

    def add_quat_accessor(self, quats):
        flat = [c for q in quats for c in q]
        data = struct.pack("<%df" % len(flat), *flat)
        bv = self._append(data)
        return self._accessor(bv, 5126, len(quats), "VEC4")

    def add_vec3_accessor(self, vecs):
        flat = [c for v in vecs for c in v]
        data = struct.pack("<%df" % len(flat), *flat)
        bv = self._append(data)
        return self._accessor(bv, 5126, len(vecs), "VEC3")

    # ---- animation ---------------------------------------------------------
    def set_animation(self, name, rot_tracks, trans_tracks=None):
        """rot_tracks: {node_idx: (times, [quat,...])} — ABSOLUTE local quats.
        trans_tracks: {node_idx: (times, [vec3,...])}."""
        samplers, channels = [], []
        for node, (times, quats) in rot_tracks.items():
            t_acc = self.add_time_accessor(times)
            q_acc = self.add_quat_accessor([normalize(q) for q in quats])
            samplers.append({"input": t_acc, "output": q_acc,
                             "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": node, "path": "rotation"}})
        for node, (times, vecs) in (trans_tracks or {}).items():
            t_acc = self.add_time_accessor(times)
            v_acc = self.add_vec3_accessor(vecs)
            samplers.append({"input": t_acc, "output": v_acc,
                             "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": node, "path": "translation"}})
        self.json["animations"] = [{
            "name": name, "samplers": samplers, "channels": channels}]

    def save(self, path):
        # merge extra bin
        full_bin = bytes(self.bin) + bytes(self.extra_bin)
        pad_bin = (-len(full_bin)) % 4
        full_bin += b"\x00" * pad_bin
        self.json.setdefault("buffers", [{}])
        self.json["buffers"][0]["byteLength"] = len(full_bin)
        js = json.dumps(self.json, separators=(",", ":")).encode("utf-8")
        js += b" " * ((-len(js)) % 4)
        total = 12 + 8 + len(js) + 8 + len(full_bin)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))
            f.write(struct.pack("<II", len(js), 0x4E4F534A))
            f.write(js)
            f.write(struct.pack("<II", len(full_bin), 0x004E4942))
            f.write(full_bin)


# Mixamo joint names used by the authoring DSL.
MIXAMO = {
    "hips": "mixamorig:Hips",
    "spine": "mixamorig:Spine",
    "spine1": "mixamorig:Spine1",
    "spine2": "mixamorig:Spine2",
    "neck": "mixamorig:Neck",
    "head": "mixamorig:Head",
    "l_shoulder": "mixamorig:LeftShoulder",
    "l_arm": "mixamorig:LeftArm",
    "l_forearm": "mixamorig:LeftForeArm",
    "l_hand": "mixamorig:LeftHand",
    "r_shoulder": "mixamorig:RightShoulder",
    "r_arm": "mixamorig:RightArm",
    "r_forearm": "mixamorig:RightForeArm",
    "r_hand": "mixamorig:RightHand",
    "l_upleg": "mixamorig:LeftUpLeg",
    "l_leg": "mixamorig:LeftLeg",
    "l_foot": "mixamorig:LeftFoot",
    "l_toe": "mixamorig:LeftToeBase",
    "r_upleg": "mixamorig:RightUpLeg",
    "r_leg": "mixamorig:RightLeg",
    "r_foot": "mixamorig:RightFoot",
    "r_toe": "mixamorig:RightToeBase",
}


def author(src_glb, out_glb, anim_name, fps, seconds, pose_fn,
           hips_translate_fn=None):
    """pose_fn(t01) -> {joint_key: (x_deg, y_deg, z_deg)} local-delta Eulers.
    hips_translate_fn(t01) -> (dx, dy, dz) added to hips bind translation."""
    g = Glb(src_glb)
    nframes = max(2, int(round(fps * seconds)) + 1)
    times = [i / fps for i in range(nframes)]

    joints = {}
    for key, name in MIXAMO.items():
        idx = g.node_index(name)
        if idx >= 0:
            joints[key] = idx

    rot_tracks = {}
    poses = [pose_fn(i / (nframes - 1)) for i in range(nframes)]
    for key, idx in joints.items():
        bind = g.node_bind_rotation(idx)
        quats = []
        for p in poses:
            e = p.get(key, (0.0, 0.0, 0.0))
            order = "XYZ" if len(e) == 3 else e[3]
            delta = quat_from_euler(e[0], e[1], e[2], order=order)
            quats.append(quat_mul(bind, delta))
        rot_tracks[idx] = (times, quats)

    trans_tracks = None
    if hips_translate_fn and "hips" in joints:
        hidx = joints["hips"]
        bt = g.node_bind_translation(hidx)
        vecs = []
        for i in range(nframes):
            d = hips_translate_fn(i / (nframes - 1))
            vecs.append((bt[0] + d[0], bt[1] + d[1], bt[2] + d[2]))
        trans_tracks = {hidx: (times, vecs)}

    g.set_animation(anim_name, rot_tracks, trans_tracks)
    g.save(out_glb)
    missing = [k for k in MIXAMO if k not in joints]
    return {"joints": len(joints), "missing": missing, "frames": nframes}


if __name__ == "__main__":
    # smoke: identity pose
    src, out = sys.argv[1], sys.argv[2]
    r = author(src, out, "authored_test", 30, 1.0, lambda t: {})
    print(r)
