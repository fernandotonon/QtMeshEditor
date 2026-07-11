#!/usr/bin/env python3
"""Build motion-library v5 from the harvested motion corpus (#839).

ONE-TIME, OFFLINE developer tool — NOT shipped; the app never runs Python.

Slice B of the text-to-motion v2 epic (#837): turns the license-filtered
corpus assembled by scrape-motion-corpus.py (#838) into the template clip
library the app downloads — replacing the 47-clip CMU-only v4 with hundreds
of real animation clips across a much wider action vocabulary.

Pipeline per corpus asset:
  1. `qtmesh anim <file> --dump-canonical tmp.json` — the editor's own
     loader + the SAME bone-role matcher the retarget uses maps the rig onto
     the 22-joint canonical skeleton and samples every skeletal animation at
     30 fps as WORLD-frame quats (the v3 library convention).
  2. Action labelling: normalized animation name matched against a keyword
     table (walk/run/attack/death/...); un-tabled single-word names are kept
     verbatim — MotionLibrary::matchPrompt does substring matching, so every
     new action widens the usable vocabulary.
  3. Active-window selection (max --max-frames): the window with the highest
     rotation energy, start snapped to the calmest nearby frame (the
     retarget deltas against clip frame 0 — a mid-swing start reads as a
     lurch). Static clips (bind/T-poses) are dropped.
  4. Dedup: sibling characters in one pack share armature actions — clips
     with identical (action, frames, sampled-quat fingerprint) collapse.

OUTPUT: motion-library.json in the EXISTING "qtmesh-motion-library-v3"
schema (frame:"world") — the shipped app consumes it unchanged — plus a copy
of the corpus ATTRIBUTION.md, which MUST ship wherever the library does.

USAGE
  python3 scripts/build-motion-library-v5.py --corpus ~/motion_corpus \
      --out motion-library.json [--qtmesh build_local/bin/qtmesh]
"""

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

CANON_COUNT = 22
FPS = 30
MODEL_EXTS = (".glb", ".gltf", ".fbx", ".dae")

# animation-name (normalized) → action. Order matters: first hit wins.
KEYWORDS = [
    ("tpose", None), ("t_pose", None), ("bind", None), ("rest", None),
    ("walk", "walk"), ("run", "run"), ("jog", "run"), ("sprint", "run"),
    ("idle", "idle"), ("stand", "idle"), ("breath", "idle"),
    ("jump", "jump"), ("hop", "jump"), ("leap", "jump"),
    ("dance", "dance"),
    ("die", "death"), ("death", "death"), ("dead", "death"), ("dying", "death"),
    ("attack", "attack"), ("slash", "attack"), ("stab", "attack"),
    ("swing", "attack"), ("bite", "attack"),
    ("punch", "punch"), ("kick", "kick"),
    ("shoot", "shoot"), ("fire", "shoot"), ("aim", "shoot"),
    ("cast", "cast"), ("spell", "cast"), ("magic", "cast"),
    ("wave", "wave"), ("hello", "wave"), ("greet", "wave"),
    ("sit", "sit"), ("crouch", "crouch"), ("sneak", "sneak"),
    ("crawl", "crawl"), ("climb", "climb"), ("swim", "swim"),
    ("fly", "fly"), ("fall", "fall"),
    ("hit", "hit"), ("damage", "hit"), ("hurt", "hit"), ("impact", "hit"),
    ("block", "block"), ("dodge", "dodge"), ("roll", "roll"),
    ("throw", "throw"), ("pick", "pickup"), ("interact", "interact"),
    ("victory", "cheer"), ("cheer", "cheer"), ("win", "cheer"),
    ("yes", "nod"), ("no", "shake"),
    ("eat", "eat"), ("drink", "eat"), ("sleep", "sleep"),
    ("open", "interact"), ("push", "push"), ("pull", "pull"),
]


def norm_anim_name(name):
    n = name.lower()
    n = re.sub(r"^.*\|", "", n)                 # "Armature|Walk" → "walk"
    n = re.sub(r"(armature|action|anim|mixamo\.com|takes?)", " ", n)
    n = re.sub(r"[^a-z]+", " ", n)
    return " ".join(n.split())


def action_for(anim_name, tags):
    n = norm_anim_name(anim_name)
    for kw, action in KEYWORDS:
        if kw in n.replace(" ", ""):
            return action                        # None = deliberate skip
    # single clean word → keep verbatim (widens the prompt vocabulary)
    words = n.split()
    if len(words) == 1 and 3 <= len(words[0]) <= 16:
        return words[0]
    for t in tags or []:
        for kw, action in KEYWORDS:
            if action and kw in str(t).lower():
                return action
    return None


def quat_angle(a, b):
    d = abs(sum(x * y for x, y in zip(a, b)))
    return 2.0 * math.acos(max(-1.0, min(1.0, d)))


def frame_energy(quats):
    """Mean joint rotation speed between consecutive frames (rad/frame)."""
    e = [0.0]
    for f in range(1, len(quats)):
        a = sum(quat_angle(quats[f - 1][j], quats[f][j])
                for j in range(CANON_COUNT)) / CANON_COUNT
        e.append(a)
    return e


def select_window(quats, max_frames):
    """Highest-energy window, start snapped to the calmest nearby frame."""
    T = len(quats)
    if T <= max_frames:
        return 0, T
    e = frame_energy(quats)
    best_s, best_sum = 0, -1.0
    window = sum(e[:max_frames])
    best_sum, best_s = window, 0
    for s in range(1, T - max_frames + 1):
        window += e[s + max_frames - 1] - e[s - 1]
        if window > best_sum:
            best_sum, best_s = window, s
    # snap the start to the calmest frame in the preceding half-second
    lo = max(0, best_s - FPS // 2)
    calm = min(range(lo, best_s + 1), key=lambda i: e[i]) if best_s > lo \
        else best_s
    return calm, min(T, calm + max_frames)


def fingerprint(action, quats):
    h = hashlib.sha1(action.encode())
    for f in (0, len(quats) // 2, len(quats) - 1):
        for j in range(0, CANON_COUNT, 3):
            h.update(("%.3f%.3f%.3f%.3f" % tuple(quats[f][j])).encode())
    h.update(str(len(quats)).encode())
    return h.hexdigest()


def find_qtmesh(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for c in (os.path.join(here, "..", "build_local", "bin", "qtmesh"),
              shutil.which("qtmesh")):
        if c and os.path.exists(c):
            return c
    sys.exit("qtmesh not found — pass --qtmesh")


def manifest_lookup(manifest, dirname):
    for a in manifest.get("assets", []):
        slug = re.sub(r"[^A-Za-z0-9._-]+", "_", a.get("title", "")).strip("_")
        if dirname in (slug[:80],) or dirname in slug:
            return a
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", default="motion-library.json")
    ap.add_argument("--qtmesh", default="")
    ap.add_argument("--max-frames", type=int, default=120)      # 4 s @ 30
    ap.add_argument("--min-frames", type=int, default=15)       # 0.5 s
    ap.add_argument("--min-roles", type=int, default=12)
    ap.add_argument("--min-energy", type=float, default=0.004,
                    help="mean rad/frame below which a clip is a pose")
    ap.add_argument("--max-per-action", type=int, default=12)
    args = ap.parse_args()

    qtmesh = find_qtmesh(args.qtmesh)
    corpus = os.path.expanduser(args.corpus)
    manifest = {}
    mpath = os.path.join(corpus, "manifest.json")
    if os.path.exists(mpath):
        manifest = json.load(open(mpath))

    joints = None
    clips, seen = [], set()
    counts = {}
    raw = os.path.join(corpus, "raw")
    for source in sorted(os.listdir(raw)):
        sdir = os.path.join(raw, source)
        if not os.path.isdir(sdir):
            continue
        for asset in sorted(os.listdir(sdir)):
            adir = os.path.join(sdir, asset)
            if not os.path.isdir(adir):
                continue
            prov = manifest_lookup(manifest, asset) or {}
            tags = prov.get("tags", [])
            title = prov.get("title", asset)
            for root, _d, files in os.walk(adir):
                for fn in sorted(files):
                    if not fn.lower().endswith(MODEL_EXTS):
                        continue
                    fpath = os.path.join(root, fn)
                    with tempfile.NamedTemporaryFile(
                            suffix=".json", delete=False) as tf:
                        tmp = tf.name
                    try:
                        r = subprocess.run(
                            [qtmesh, "anim", fpath, "--dump-canonical", tmp],
                            capture_output=True, text=True, timeout=600)
                        if r.returncode != 0 or not os.path.getsize(tmp):
                            continue
                        dump = json.load(open(tmp))
                    except Exception:
                        continue
                    finally:
                        try:
                            os.remove(tmp)
                        except OSError:
                            pass
                    joints = joints or dump.get("joints")
                    for c in dump.get("clips", []):
                        if c.get("resolvedRoles", 0) < args.min_roles:
                            continue
                        q = c.get("quats", [])
                        if len(q) < args.min_frames:
                            continue
                        action = action_for(c.get("animation", ""), tags)
                        if not action:
                            continue
                        s, epos = select_window(q, args.max_frames)
                        w = q[s:epos]
                        e = frame_energy(w)
                        if sum(e) / max(1, len(e)) < args.min_energy:
                            continue      # a pose, not a motion
                        # Sibling characters in one pack share armature
                        # actions but their rest bones differ subtly — the
                        # quat fingerprint alone misses them, so dedupe
                        # semantically too: same asset + animation + length
                        # IS the same motion.
                        sem = (title, norm_anim_name(c.get("animation", "")),
                               len(w))
                        fp = fingerprint(action, w)
                        if fp in seen or sem in seen:
                            continue      # duplicate take
                        if counts.get(action, 0) >= args.max_per_action:
                            continue
                        seen.add(fp); seen.add(sem)
                        counts[action] = counts.get(action, 0) + 1
                        clips.append({
                            "action": action,
                            "source": f"{title} — {c.get('animation')}",
                            "frames": len(w),
                            "quats": w,
                        })
                        print(f"  + {action:<10} {title[:38]:<40}"
                              f" {c.get('animation')} ({len(w)}f)")

    if not clips:
        sys.exit("no clips extracted — is the corpus downloaded/validated?")

    lib = {"schema": "qtmesh-motion-library-v3", "joints": joints,
           "fps": FPS, "frame": "world", "clips": clips}
    with open(args.out, "w") as f:
        json.dump(lib, f)
    att = os.path.join(corpus, "ATTRIBUTION.md")
    if os.path.exists(att):
        shutil.copy(att, os.path.join(
            os.path.dirname(os.path.abspath(args.out)) or ".",
            "ATTRIBUTION.md"))
    print(f"\nwrote {args.out}: {len(clips)} clips, "
          f"{len(counts)} actions, "
          f"{os.path.getsize(args.out) / 1e6:.1f} MB")
    for a in sorted(counts):
        print(f"  {a}: {counts[a]}")


if __name__ == "__main__":
    main()
