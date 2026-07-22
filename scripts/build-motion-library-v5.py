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

CANON_COUNT = 22
FPS = 30
MODEL_EXTS = (".glb", ".gltf", ".fbx", ".dae")

# animation-name (normalized) → action. Order matters: first hit wins.
KEYWORDS = [
    ("tpose", None), ("t_pose", None), ("bind", None), ("rest", None),
    # compound names FIRST — "walk"/"run" below would swallow them
    ("crouchwalk", "crouch"), ("crouchrun", "crouch"),
    ("sneakwalk", "sneak"), ("sneakrun", "sneak"),
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


STOPWORDS = {"armature", "action", "anim", "animation", "animations",
             "mixamo", "com", "take", "takes", "fbx", "rig", "rigged",
             "character", "model", "mesh", "skeleton", "base", "layer",
             "scene", "root", "main", "default", "final", "new", "test"}
# junk that survives normalization but is not an action
BAD_ACTIONS = {"ation", "bot", "jad", "loose", "pose", "still", "static"}


def norm_anim_name(name):
    n = name.lower()
    n = re.sub(r"^.*\|", "", n)                 # "Armature|Walk" → "walk"
    n = re.sub(r"[^a-z]+", " ", n)               # squash first, THEN drop
    words = [w for w in n.split() if w not in STOPWORDS]
    return " ".join(words)


# Fold verbatim-word actions onto a canonical base so we don't split a
# handful of clips across near-duplicate labels ("waving"→"wave"). Keeps the
# runtime kSynonyms table and these labels consistent.
CANON_ACTION = {
    "waving": "wave", "singing": "sing", "walking": "walk",
    "running": "run", "jumping": "jump", "dancing": "dance",
    "kicking": "kick", "punching": "punch", "crawling": "crawl",
    "climbing": "climb", "rolling": "roll", "swimming": "swim",
    "sitting": "sit", "praying": "pray", "dying": "death",
    "shakehand": "shake", "handshake": "shake",
}


def action_for(anim_name, tags):
    n = norm_anim_name(anim_name)
    for kw, action in KEYWORDS:
        if kw in n.replace(" ", ""):
            return action                        # None = deliberate skip
    # single clean word → keep verbatim (widens the prompt vocabulary)
    words = n.split()
    if len(words) == 1 and 3 <= len(words[0]) <= 16 \
            and words[0] not in BAD_ACTIONS \
            and not any(sw in words[0] for sw in STOPWORDS if len(sw) > 3):
        return CANON_ACTION.get(words[0], words[0])
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


def qmul(a, b):  # [x,y,z,w]
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def qrot(q, v):
    return qmul(qmul(q, [v[0], v[1], v[2], 0.0]),
                [-q[0], -q[1], -q[2], q[3]])[:3]


def vnorm(v):
    l = math.sqrt(sum(x * x for x in v))
    return [x / l for x in v] if l > 1e-9 else None


# Actions that legitimately go horizontal — no uprightness gate for these.
HORIZONTAL_OK = {"death", "roll", "crawl", "swim", "fall", "sleep"}

# canonical role indices
HIP, ABDOMEN, CHEST, NECK = 0, 1, 2, 3
RHIP, LHIP = 15, 19


def clip_quality(action, quats, rest_world, rest_dir, resolved_roles,
                 mean_energy, min_energy):
    """Score a clip 0..1 (#855): uprightness under its own reference triple
    (catches mis-mapped rigs — quadrupeds pass the role gate but retarget
    horizontal), reference completeness, and a sane energy band. Returns
    (quality, drop_reason|None)."""
    dirs_ok = sum(1 for d in (rest_dir or [])
                  if abs(d[0]) > 1e-6 or abs(d[1]) > 1e-6 or abs(d[2]) > 1e-6)
    completeness = dirs_ok / CANON_COUNT
    roles = (resolved_roles or 0) / CANON_COUNT

    # Uprightness: track spine-up (hip→abdomen) and thigh-down direction dots
    # under the animation quats. A biped stays roughly vertical along the
    # spine with thighs hanging down; quadrupeds / dinosaurs / mis-oriented
    # rigs go horizontal on one or both — retargeting bent-over onto a biped.
    upness = 0.35  # unmeasurable → low (do NOT assume upright)
    if rest_world and rest_dir and dirs_ok:
        def axis(role):
            d = vnorm(rest_dir[role])
            if d is None:
                return None
            q = rest_world[role]
            return qrot([-q[0], -q[1], -q[2], q[3]], d)
        # SPINE cue: the torso should point up. Use the first resolvable
        # torso bone (hip→abdomen→chest→neck) — the bind restDir itself must
        # already point roughly up (+Y): a horizontal-torso rig (T-rex, whose
        # abdomen/chest dirs are ~[0,0,-1]) fails here BEFORE animation even
        # applies. This is what the hip-only cue missed when hip restDir=0.
        spine_bind, a_spine, spine_role = None, None, None
        for r in (HIP, ABDOMEN, CHEST, NECK):
            d = vnorm(rest_dir[r])
            if d is not None:
                spine_bind, a_spine, spine_role = d, axis(r), r
                break
        thigh_axes = [(r, axis(r)) for r in (RHIP, LHIP)]
        thigh_axes = [(r, a) for r, a in thigh_axes if a is not None]
        spine_up, thigh_down, ns, nt = 0.0, 0.0, 0, 0
        spine_up_min = 1.0                       # worst (lowest) frame
        for f in range(0, len(quats), 3):
            if a_spine is not None:
                su = qrot(quats[f][spine_role], a_spine)[1]
                spine_up += su; ns += 1
                spine_up_min = min(spine_up_min, su)
            if thigh_axes:
                thigh_down += sum(-qrot(quats[f][r], a)[1]
                                  for r, a in thigh_axes) / len(thigh_axes)
                nt += 1
        spine_up = spine_up / ns if ns else None
        thigh_down = thigh_down / nt if nt else None
        if action in HORIZONTAL_OK:
            upness = 0.7  # horizontal by design: neutral, no gate
        else:
            # Bind-frame gate: the torso's REST direction must already be
            # roughly vertical, else the rig is non-biped (horizontal torso)
            # regardless of the animation.
            if spine_bind is not None and spine_bind[1] < 0.4:
                return 0.0, (f"non-biped torso (bind spine-up "
                             f"{spine_bind[1]:.2f})")
            if ns or nt:
                # Animated gate: torso must stay up, thighs hang down.
                if spine_up is not None and spine_up < 0.5:
                    return 0.0, f"not upright (spine-up {spine_up:.2f})"
                # Topple gate: even if the MEAN stays up, reject clips whose
                # torso pitches head-below-horizontal in any frame — a
                # ground/fall kick or a mid-clip topple that averages out but
                # renders as the character lying down on a biped rig.
                if spine_up is not None and spine_up_min < -0.25:
                    return 0.0, (f"topples mid-clip (spine-up min "
                                 f"{spine_up_min:.2f})")
                if spine_up is None and thigh_down is not None \
                        and thigh_down < 0.3:
                    return 0.0, f"not upright (thigh-down {thigh_down:.2f})"
                up_terms = [max(0.0, v) for v in (spine_up, thigh_down)
                            if v is not None]
                upness = sum(up_terms) / len(up_terms) if up_terms else 0.35
    elif action in HORIZONTAL_OK:
        upness = 0.7

    # Placeholder-arm gate: game walk/run cycles are often authored with
    # STATIC (T-pose) arms — legs stride while the arms stick straight out.
    # Retargeted, that reads as a broken clip. If the legs carry real motion
    # but BOTH arms are near-frozen, drop the take.
    ARM_ROLES = (7, 8, 11, 12)
    LEG_ROLES = (15, 16, 19, 20)
    if len(quats) > 1 and action not in ("idle", "sit", "sleep"):
        # Energy RELATIVE to the chest (role 2): world quats inherit the
        # torso's sway, so locally-frozen arms still show world energy.
        # rel = chest^-1 * bone isolates the limb's own motion.
        def rel(f, r):
            c = quats[f][2]
            b = quats[f][r]
            ci = [-c[0], -c[1], -c[2], c[3]]
            return qmul(ci, b)
        def group_energy(roles):
            tot = 0.0
            for f in range(1, len(quats)):
                tot += sum(quat_angle(rel(f - 1, r), rel(f, r))
                           for r in roles) / len(roles)
            return tot / (len(quats) - 1)
        legs_e = group_energy(LEG_ROLES)
        arms_e = group_energy(ARM_ROLES)
        if legs_e > 0.004 and arms_e < 0.0015:
            return 0.0, (f"static placeholder arms (rel arm energy "
                         f"{arms_e:.4f} vs legs {legs_e:.4f})")

    # Neck-up gate: some rigs export the neck/head bone with an INVERTED
    # axis — the extracted direction points down for the whole clip and the
    # retargeted head renders thrown back / buried in the torso. For any
    # upright action, the neck chain's animated direction must stay roughly
    # up.
    if action not in HORIZONTAL_OK and rest_world and rest_dir:
        for r in (3, 4, 5):                       # neck, neck1, head
            d = vnorm(rest_dir[r])
            if d is None:
                continue
            q = rest_world[r]
            a_s = qrot([-q[0], -q[1], -q[2], q[3]], d)
            tot = sum(qrot(quats[f][r], a_s)[1] for f in range(len(quats)))
            if tot / max(1, len(quats)) < 0.3:
                return 0.0, (f"neck/head direction not upright (role {r} "
                             f"mean up-dot {tot / max(1, len(quats)):.2f}) — "
                             "inverted neck axis, head renders thrown back")
            break                                  # first resolvable is enough

    # Horizontal-arm gate for plain locomotion: zombie-shamble / T-pose-armed
    # walk cycles hold the upper arms near-horizontal for the whole clip
    # (mean |up-component| of the upper-arm direction ~0 vs 0.6-0.95 on a
    # natural walk). Retargeted onto a generic character under a generic
    # "walk" prompt they read broken — drop them from locomotion actions.
    RSHO, LSHO = 7, 11
    if action in ("walk", "run", "march") and rest_world and rest_dir:
        downdots = []
        for r in (RSHO, LSHO):
            d = vnorm(rest_dir[r])
            if d is None:
                continue
            q = rest_world[r]
            a_s = qrot([-q[0], -q[1], -q[2], q[3]], d)
            tot = 0.0
            for f in range(len(quats)):
                tot += qrot(quats[f][r], a_s)[1]      # SIGNED up-component
            downdots.append(tot / max(1, len(quats)))
        if not downdots:
            # Arm roles UNRESOLVED: the retarget leaves the target's arms at
            # its bind pose — a literal T-pose held for the whole clip.
            return 0.0, (f"arm roles unresolved — target arms would freeze "
                         f"in the bind T-pose during {action}")
        # judge each arm separately, on the SIGNED up-component: a natural
        # locomotion arm hangs (mean Y ~ -0.6..-0.95). Horizontal zombie arms
        # (~0) AND raised arms (+) both read broken — an abs() gate passed a
        # straight-up arm as if it were hanging (several corpus rigs carry a
        # per-bone axis inversion that renders one arm skyward).
        if max(downdots) > -0.25:
            return 0.0, (f"arm(s) not hanging (upper-arm signed up-dots "
                         f"{[round(u, 2) for u in downdots]}) — zombie/"
                         f"T-pose/raised style, not a generic {action}")

    # Energy band: below = a pose, way above = spasm/mis-mapped.
    lo, hi, cap = min_energy * 2.0, 0.10, 0.20
    if mean_energy <= lo:
        energy = max(0.0, (mean_energy - min_energy) / max(1e-9, lo - min_energy))
    elif mean_energy <= hi:
        energy = 1.0
    else:
        energy = max(0.0, (cap - mean_energy) / (cap - hi))

    q = 0.45 * upness + 0.25 * completeness + 0.15 * roles + 0.15 * energy
    q = max(0.0, min(1.0, q))
    if q < 0.35:
        return q, f"quality {q:.2f} below floor"
    return q, None


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
    # Exact join on the recorded on-disk dir (newer manifests), slug-based
    # fuzzy fallback for corpora scraped before `dir` was recorded.
    for a in manifest.get("assets", []):
        d = a.get("dir", "")
        if d and (d.endswith("/" + dirname) or d == dirname):
            return a
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
                    # Sidecar cache: extraction dominates rebuild time, so
                    # dumps persist next to the model file and are reused
                    # when newer than it (delete *.canonical.json to force).
                    cache = fpath + ".canonical.json"
                    dump = None
                    if os.path.exists(cache) \
                            and os.path.getmtime(cache) >= os.path.getmtime(fpath):
                        try:
                            dump = json.load(open(cache))
                        except Exception:
                            dump = None
                    if dump is None:
                        try:
                            r = subprocess.run(
                                [qtmesh, "anim", fpath,
                                 "--dump-canonical", cache],
                                capture_output=True, text=True, timeout=600)
                            if r.returncode != 0 \
                                    or not os.path.exists(cache) \
                                    or not os.path.getsize(cache):
                                continue
                            dump = json.load(open(cache))
                        except Exception:
                            continue
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
                        rest_world = c.get("restWorld") \
                            if len(c.get("restWorld", [])) == CANON_COUNT \
                            else None
                        rest_dir = c.get("restDir") \
                            if len(c.get("restDir", [])) == CANON_COUNT \
                            else None
                        quality, drop = clip_quality(
                            action, w, rest_world, rest_dir,
                            c.get("resolvedRoles", 0),
                            sum(e) / max(1, len(e)), args.min_energy)
                        if drop:
                            print(f"  - {action:<10} {title[:38]:<40}"
                                  f" {c.get('animation')} DROPPED: {drop}")
                            continue
                        seen.add(fp); seen.add(sem)
                        counts[action] = counts.get(action, 0) + 1
                        clip = {
                            "action": action,
                            "source": f"{title} — {c.get('animation')}",
                            "frames": len(w),
                            "quality": round(quality, 3),
                            "quats": w,
                        }
                        # Source-bind orientations + canonical bind bone
                        # directions → the bind-referenced retarget path.
                        # Older sidecar caches predate these fields; delete
                        # *.canonical.json to re-extract.
                        if rest_world:
                            clip["restWorld"] = rest_world
                        if rest_dir:
                            clip["restDir"] = rest_dir
                        clips.append(clip)
                        print(f"  + {action:<10} {title[:38]:<40}"
                              f" {c.get('animation')} ({len(w)}f, q={quality:.2f})")

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
