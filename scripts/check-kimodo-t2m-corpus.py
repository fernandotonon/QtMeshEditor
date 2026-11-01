#!/usr/bin/env python3
"""Quality gates for a Kimodo text-to-motion corpus (scripts/build-kimodo-t2m-corpus.py output).

Reads corpus.jsonl + clips/*.npz (QtMeshEditor motion interchange, SOMA-77), runs forward kinematics with
numpy only, and writes quality.jsonl: per-clip metrics plus a list of failed gates. Nothing is deleted;
a training script filters on `ok`.

Gates (thresholds are deliberately loose: they catch broken clips, not style):
  finite      every rotation / root value is finite
  slide       feet move < 20 cm/s horizontally while Kimodo marks them in contact
  ground      no foot more than 5 cm below the floor (floor = lowest foot point of the clip's first frame)
  hips        hips stay 0.15..1.4 m above the floor unless the prompt is about lying / sitting / crawling /
              falling, or leaves the floor plane on purpose (stairs, ladder, jump, vault: no ground gate either)
  jitter      95th-percentile joint acceleration < 120 m/s^2 (a teleporting joint, not a fast motion)
  short       fewer than 3 frames (no acceleration to measure)

    python3 scripts/check-kimodo-t2m-corpus.py ~/t2m_kimodo/v1
"""

import json
import os
import sys

import numpy as np

LOW_OK = ("lie", "lies", "lying", "crawl", "roll", "fall", "falls", "push up", "sit up", "ground", "floor", "kneel",
          "crouch", "squat", "slides", "trips", "sit", "sits", "handstand", "swim", "swims")
# Prompts whose motion leaves the floor plane on purpose: the first-frame floor does not apply to them.
VERTICAL_OK = ("stairs", "ladder", "climb", "climbs", "jump", "jumps", "vault", "vaults", "over", "slides",
               "handstand", "swim", "swims")


def fk(d):
    rot = d["local_rot_mats"].astype(np.float64)
    rest = d["rest_joints"].astype(np.float64)
    par = d["joint_parents"].astype(int)
    root = d["root_positions"].astype(np.float64)
    T, J = rot.shape[:2]
    off = rest.copy()
    for j in range(J):
        if par[j] >= 0:
            off[j] = rest[j] - rest[par[j]]
    G = np.zeros((T, J, 3, 3))
    P = np.zeros((T, J, 3))
    for j in range(J):
        p = par[j]
        if p < 0:
            G[:, j], P[:, j] = rot[:, j], root
        else:
            G[:, j] = G[:, p] @ rot[:, j]
            P[:, j] = P[:, p] + np.einsum("tij,j->ti", G[:, p], off[j])
    return P


def names_of(d):
    return [n.decode() if isinstance(n, bytes) else str(n) for n in d["joint_names"]]


def check(path, text):
    d = np.load(path)
    gates, m = [], {}
    if not all(np.isfinite(d[k]).all() for k in ("local_rot_mats", "root_positions")):
        return {"ok": False, "failed": ["finite"]}
    fps = float(d["fps"][0])
    P = fk(d)
    names = names_of(d)
    feet = [names.index(n) for n in ("LeftFoot", "RightFoot", "LeftToeBase", "RightToeBase") if n in names]
    hips = names.index("Hips")
    floor = P[0, feet, 1].min()
    m["ground_cm"] = float((P[:, feet, 1].min() - floor) * 100)
    m["hips_min_m"] = float((P[:, hips, 1] - floor).min())
    m["hips_max_m"] = float((P[:, hips, 1] - floor).max())
    vel = np.linalg.norm(np.diff(P[:, :, [0, 2]], axis=0), axis=-1) * fps          # [T-1, J] horizontal m/s
    contacts = d["foot_contacts"]
    # Kimodo's contact channels follow SOMASkeleton77.{left,right}_foot_joint_names: 6 channels = Foot, ToeBase,
    # ToeEnd per side (left first); the 4-channel variant drops ToeEnd (kimodo/demo/ui.py expands 4 -> 6 that way).
    layouts = {6: ("LeftFoot", "LeftToeBase", "LeftToeEnd", "RightFoot", "RightToeBase", "RightToeEnd"),
               4: ("LeftFoot", "LeftToeBase", "RightFoot", "RightToeBase")}
    layout = layouts.get(contacts.shape[1])
    if layout and all(n in names for n in layout):
        cj = [names.index(n) for n in layout]
        c = contacts[1:] > 0.5
        v = vel[:, cj]
        m["slide_cm_s"] = float(v[c].mean() * 100) if c.any() else 0.0
    else:
        m["slide_cm_s"] = 0.0
    if P.shape[0] < 3:   # no second difference: too short to judge (and to train on)
        return {"ok": False, "failed": ["short"], **m}
    acc = np.linalg.norm(np.diff(P, n=2, axis=0), axis=-1) * fps * fps
    m["jitter_p95"] = float(np.percentile(acc, 95))
    m["travel_m"] = float(np.linalg.norm(P[-1, hips, [0, 2]] - P[0, hips, [0, 2]]))
    words = text.lower().replace(",", " ").split()
    low_ok = any(w in words or (" " in w and w in text.lower()) for w in LOW_OK)
    vertical_ok = any(w in words for w in VERTICAL_OK)
    if m["slide_cm_s"] > 20:
        gates.append("slide")
    if m["ground_cm"] < -5 and not vertical_ok:
        gates.append("ground")
    if not (low_ok or vertical_ok) and (m["hips_min_m"] < 0.15 or m["hips_max_m"] > 1.4):
        gates.append("hips")
    if m["jitter_p95"] > 120:
        gates.append("jitter")
    return {"ok": not gates, "failed": gates, **m}


def main():
    root = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/t2m_kimodo/v1")
    rows = [json.loads(l) for l in open(os.path.join(root, "corpus.jsonl"), encoding="utf-8") if l.strip()]
    out, failed = [], {}
    for r in rows:
        q = check(os.path.join(root, r["file"]), r["text"])
        out.append({"id": r["id"], "text": r["text"], **q})
        for g in q["failed"]:
            failed[g] = failed.get(g, 0) + 1
    with open(os.path.join(root, "quality.jsonl"), "w", encoding="utf-8") as f:
        for q in out:
            f.write(json.dumps(q) + "\n")
    ok = sum(q["ok"] for q in out)
    print(f"{ok}/{len(out)} clips pass; failed gates: {failed or 'none'}")
    for k in ("slide_cm_s", "ground_cm", "jitter_p95", "travel_m"):
        v = np.array([q[k] for q in out if k in q])
        if len(v):
            print(f"  {k}: median {np.median(v):.2f}  p95 {np.percentile(v, 95):.2f}  max {v.max():.2f}")


if __name__ == "__main__":
    main()
