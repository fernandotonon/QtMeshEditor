#!/usr/bin/env python3
"""Build motion-library V2 (52-joint, fingers-as-joints) from the corpus (#838).

ONE-TIME, OFFLINE developer tool — NOT shipped; the app never runs Python.

The V2 generation of build-motion-library-v5.py: same corpus pipeline, but
dumps run with `--v2` so fingers are canonical joints 22..51 instead of a
side-channel, and the output is the `qtmesh-motion-library-v4` schema. Also
adds the curation ship-gate: `--curation <curation.json>` / `--approved-only`
restricts the shipped set to the clips the user starred in the Animation
Library picker (keyed by clip `source`; auto-discovered from the AppData
motion dir when the flag is omitted).

Pipeline per corpus asset:
  1. `qtmesh anim <file> --dump-canonical tmp.json --v2` — the editor's own
     loader + the SAME bone-role matcher the retarget uses maps the rig onto
     the 52-joint canonical skeleton and samples every skeletal animation at
     30 fps as WORLD-frame quats.
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

OUTPUT: motion-library-v2.json in the "qtmesh-motion-library-v4" schema
(frame:"world", jointCount:52) — plus a copy of the corpus ATTRIBUTION.md,
which MUST ship wherever the library does.

USAGE
  python3 scripts/build-motion-library-v6.py --corpus ~/motion_corpus \
      --out motion-library-v2.json [--qtmesh build_local/bin/qtmesh] \
      [--curation curation.json --approved-only]
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
BAD_ACTIONS = {"ation", "bot", "jad", "loose", "pose", "still", "static",
               "tempmotion", "temp", "motion", "untitled", "clip", "newanim"}


def norm_anim_name(name):
    n = name.lower()
    n = re.sub(r"^.*\|", "", n)                 # "Armature|Walk" → "walk"
    n = re.sub(r"[^a-z]+", " ", n)               # squash first, THEN drop
    words = [w for w in n.split() if w not in STOPWORDS]
    return " ".join(words)


# Fold verbatim-word actions onto a canonical base so we don't split a
# handful of clips across near-duplicate labels ("waving"→"wave"). Keeps the
# runtime kSynonyms table and these labels consistent.
# Sources whose rigs have BLOCK/UNAUTHORED hands: their finger bones carry
# junk (never polished — the mesh shows no fingers), which splays a real
# hand's fingers on retarget. Zero the finger joints (22..51) for these so
# targets hold their bind hands. Substring match on the asset title.
FINGERLESS_SOURCES = [
    "Animated Human Low Poly",
]

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
    """Mean joint rotation speed between consecutive frames (rad/frame).
    Deliberately measures the 22 BODY joints only — on a 52-wide V2 clip the
    30 finger channels would dilute the signal the window selector needs."""
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

# License exclusion: Adobe Mixamo animations cannot be redistributed as a
# standalone library (Mixamo ToS), even when a Sketchfab uploader re-published
# the model under CC-BY — the CC-BY covers the upload, not the underlying
# Adobe animation data. Drop any clip whose animation/source name is Mixamo-
# derived. Matched case-insensitively against "<title> — <animation>".
MIXAMO_MARKERS = ("mixamo",)

# Manual review drop-list (#838 curation): specific (asset-title-substring,
# animation-substring) pairs the user reviewed and rejected — bad retargets
# (Fox-rig tip/hunch/invert) or off-action clips. Matched case-insensitively;
# animation "" matches any animation of that asset. Kept as (title, anim) so
# it survives library rebuilds (JSON indices are not stable).
REVIEW_DROP = [
    ("GIGI", ""),          # Fox rig — tips/hunches on every action reviewed
    ("KAI", ""),           # Fox rig — same
    ("Shar Pei", ""),      # dog rig — wrong body plan for humanoid retarget
    ("Square Head Character", "Loose"),  # shake [100] — weak/ambiguous
    ("Dance | Japanese Samurai", ""),    # dance [49] — crouched, off
    # These Quaternius packs mis-map the RIGHT upper-arm bone: it stays raised/
    # out (mean up-Y positive) the whole clip on every action instead of
    # hanging. Redundant with the clean Man/Woman (Oct/Dec 2017) packs, so drop
    # them wholesale. NB: match the FULL pack name — "Animated Men/Women
    # Characters" and "Man/Woman Animated" are DIFFERENT releases (the latter
    # are the good ones), so do NOT use a loose "Men"/"Man" substring.
    ("Animated Men Characters", ""),     # Feb 2019 — raised right arm
    ("Animated Women Characters", ""),   # Feb 2019 — raised right arm
    ("Alien Animated", ""),              # April 2019 — raised right arm
    ("Knight Character Animated", ""),   # Jul 2018 — raised right arm
    ("Rigged and Animated Humanoid", ""),  # bad retarget on BOTH Mixamo &
                                           # UniRig skeletons (user review)
    ("FNaf_DLC_moon_sun", ""),           # Moon/Sun man — jumpscare rig, bad
    ("Low_Poly_Zombie_Game_Animation", ""),  # weak zombie clips (user review)
]


def _excluded(title, anim):
    """Return a drop reason string, or None when the (asset, animation) is
    kept. License rules (Mixamo) and the manual review drop-list."""
    hay = f"{title} {anim}".lower()
    if any(m in hay for m in MIXAMO_MARKERS):
        return "mixamo (license: Adobe ToS, not redistributable)"
    for t, a in REVIEW_DROP:
        # Word-boundary match on the title: a short token like "KAI" must not
        # also drop "Samurai Kaiju" / "KAIROS".
        if (re.search(rf"\b{re.escape(t)}\b", title, re.IGNORECASE)
                and (not a or a.lower() in anim.lower())):
            return f"review drop-list ({t}{'/' + a if a else ''})"
    return None


def fix_first_frame_flip(quats):
    """Mini Chibi Kid (and similar) clips export frame 0 with a rotated/flipped
    hip while the rest of the clip is upright — a loop-seam artifact. If frame 0
    is a strong outlier vs frame 1 (hip up-Y flipped past horizontal) but the
    clip is otherwise upright, replace frame 0 with frame 1 so the retarget
    doesn't open on the glitch. Returns the (possibly repaired) list."""
    if len(quats) < 3:
        return quats
    def up_y(f):
        x, y, z, w = f[HIP]
        return 1.0 - 2.0 * (x * x + z * z)
    u0, u1, u2 = up_y(quats[0]), up_y(quats[1]), up_y(quats[2])
    # frame 0 inverted/tilted-past-horizontal but 1 & 2 upright → repair
    if u0 < 0.3 and u1 > 0.7 and u2 > 0.7:
        quats = list(quats)
        quats[0] = quats[1]
    return quats

# canonical role indices
HIP, ABDOMEN, CHEST, NECK = 0, 1, 2, 3
RHIP, LHIP = 15, 19

# Forward-locomotion actions where the body must FACE forward. Excludes
# strafes (deliberately angled), turns, and the HORIZONTAL_OK ground actions.
FORWARD_FACING = {"walk", "run", "march", "jog", "sprint"}


def _quat_fwd(q):
    """World +Z basis rotated by quat q=(x,y,z,w) → world forward vector."""
    x, y, z, w = q
    return (2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y))


def _mean_hip_yaw_deg(quats):
    """Mean PER-FRAME absolute yaw deviation (deg) of the hip's world forward
    from +Z. ~0 = every frame faces straight ahead; large = the stride is
    baked sideways (the stylized/quadruped-rig locomotion failure).

    We take |yaw| PER FRAME and then average — NOT |average of signed yaw|.
    Aggregating first hides opposing bad frames: a clip jittering +90°/−90°
    (or the ±180° wrap +179°/−179°) has a zero mean/resultant yet faces
    sideways every single frame. Per-frame-abs-then-mean flags both."""
    if not quats:
        return 0.0
    tot = 0.0
    for f in quats:
        fx, fy, fz = _quat_fwd(f[HIP])
        tot += abs(math.degrees(math.atan2(fx, fz)))
    return tot / len(quats)


def _inverted_frame_fraction(quats):
    """Fraction of frames where the hip's world UP-vector Y drops below +0.3
    (tilted past ~70° toward horizontal/upside-down). A clean upright clip
    holds ~+1 every frame (fraction ~0); a systematically mis-oriented rig
    (persistent axis offset — GIGI Fox, some dance rigs) reads 1.0. A rare
    transient loop-seam spike reads a few percent. Mirror-invariant."""
    if not quats:
        return 0.0
    bad = 0
    for f in quats:
        x, y, z, w = f[HIP]
        if 1.0 - 2.0 * (x * x + z * z) < 0.3:   # Y of R·(0,1,0)
            bad += 1
    return bad / len(quats)


def _mean_hip_pitch_deg(quats):
    """Mean PER-FRAME absolute pitch deviation (deg) of the hip's world forward
    from level. Clean upright walk/run stays within ~17°; a diving/lunging clip
    reads 70–80°. As with yaw, take |pitch| PER FRAME then average — averaging
    signed pitch first lets +70°/−70° frames cancel to 0° (and slip under the
    inversion cutoff too), so an off-axis clip would wrongly pass."""
    if not quats:
        return 0.0
    tot = 0.0
    for f in quats:
        _, fy, _ = _quat_fwd(f[HIP])
        tot += abs(math.degrees(math.asin(max(-1.0, min(1.0, fy)))))
    return tot / len(quats)


def clip_quality(action, quats, rest_world, rest_dir, resolved_roles,
                 mean_energy, min_energy):
    """Score a clip 0..1 (#855): uprightness under its own reference triple
    (catches mis-mapped rigs — quadrupeds pass the role gate but retarget
    horizontal), reference completeness, and a sane energy band. Returns
    (quality, drop_reason|None)."""
    # Completeness scores the BODY joints only: a V2 (52-joint) rest array
    # carries 30 finger entries that would otherwise push the ratio past 1.0
    # and saturate every clip's quality (defeating the quality² take-weighting
    # and the drop floor).
    dirs_ok = sum(1 for d in (rest_dir or [])[:CANON_COUNT]
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
        for f in range(0, len(quats), 3):
            if a_spine is not None:
                su = qrot(quats[f][spine_role], a_spine)[1]
                spine_up += su; ns += 1
            if thigh_axes:
                thigh_down += sum(-qrot(quats[f][r], a)[1]
                                  for r, a in thigh_axes) / len(thigh_axes)
                nt += 1
        spine_up = spine_up / ns if ns else None
        thigh_down = thigh_down / nt if nt else None
        if action in HORIZONTAL_OK:
            upness = 0.7  # horizontal by design: neutral, no gate
        else:
            # Bind-frame gate (KEPT): the torso's REST direction must already
            # be roughly vertical, else the rig is non-biped (horizontal torso:
            # quadruped / dino / spider) and CANNOT retarget onto the humanoid
            # canonical skeleton — it would render as horizontal garbage. This
            # is a BODY-PLAN check on the rest skeleton, independent of what the
            # animation does. (Multi-body-plan support is future work — task
            # #24.)
            if spine_bind is not None and spine_bind[1] < 0.4:
                return 0.0, (f"non-biped torso (bind spine-up "
                             f"{spine_bind[1]:.2f})")
            # ANIMATED uprightness gates REMOVED (#838): a humanoid rig may
            # LEAN, crouch, recline, throw its head back, or go to the ground
            # as legitimate motion — the old spine-up / mid-clip-topple /
            # thigh-down gates wrongly rejected (and their scoring skewed) those
            # clips. We now keep any clip on a biped rest skeleton regardless of
            # the animated torso pitch. Uprightness no longer factors into the
            # quality score; use the measured up-terms only as a soft signal.
            if ns or nt:
                up_terms = [max(0.0, v) for v in (spine_up, thigh_down)
                            if v is not None]
                upness = sum(up_terms) / len(up_terms) if up_terms else 0.5
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

    # Neck/head ANIMATED uprightness gate REMOVED (#838): a leaning/reclining/
    # head-thrown-back motion is legitimate on a humanoid and this gate wrongly
    # rejected it (and was implicated in the retarget tilt reported on the
    # Gregorio give-item clips). A genuinely INVERTED neck AXIS (rig export bug)
    # is a rig-level problem better handled in the retarget's spine-chain sanity
    # pass (AnimationMerger zeroes an inverted spine restDir), not by dropping
    # the whole clip here.

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

    # Directionality gate (#838 follow-up): a forward-locomotion clip whose hip
    # faces sideways renders as "walking sideways" once the root is locked at
    # retarget. Measured across the corpus, clean human walks/runs sit at
    # |mean hip yaw| < 10°, while stylized/quadruped rigs (Fox Warriors, Gynoid,
    # SpongeBob) land 38–153°. Cull at 25° — a wide, safe margin in the gap.
    # Locomotion facing/orientation gates. Scoped to FORWARD_FACING (walk/run/
    # march) — a locomotion clip must travel upright and forward, so an off-axis
    # or inverted hip is unambiguously broken there. Gestures / dance / crouch
    # legitimately lean or bend, so these gates would false-positive on them
    # (the spine-based topple gate above already protects non-locomotion
    # actions from genuine head-below-hips). All three reads are on the hip's
    # world-forward/up vectors: mirror-invariant and rig-agnostic.
    if action in FORWARD_FACING and len(quats) > 1:
        # Sideways: hip faces off +Z. Clean human walks/runs < 10°; stylized/
        # quadruped rigs (Fox, Gynoid, SpongeBob) land 38–153°. Cull at 25°.
        yaw = _mean_hip_yaw_deg(quats)
        if yaw > 25.0:
            return 0.0, (f"faces sideways (mean hip yaw {yaw:.0f}° off "
                         f"forward) — off-axis {action}, renders sideways")
        # Diving/lunging: clip runs face-down. Clean ≤17°; superhero dive
        # runs read 68–79°. Cull at 30°.
        pitch = _mean_hip_pitch_deg(quats)
        if pitch > 30.0:
            return 0.0, (f"pitches over (mean hip pitch {pitch:.0f}° off "
                         f"level) — diving/lunging, not an upright {action}")
        # Inverted: the hip tilts past horizontal for a sustained fraction of
        # the clip (a systematically mis-oriented rig renders head-down while
        # "walking"). Clean clips ~0%; mis-oriented rigs read ~100%.
        inv = _inverted_frame_fraction(quats)
        if inv > 0.15:
            return 0.0, (f"hip tilted/inverted for {inv:.0%} of the clip — "
                         f"mis-oriented rig, renders wrong for {action}")

    # NOTE: a knee-hyperextension gate was prototyped here (user-reported
    # backward-bending knees on the Fox/stylized rigs) but REMOVED. The signed
    # knee-flex read from quaternions is not mirror-invariant — the left leg's
    # local axis is mirrored on many rigs, so a straight-standing knee reads
    # +170° on the right but −30° on the left, producing FALSE positives that
    # culled clean Quaternius idle/jump clips. Crucially, every knee-broken
    # LOCOMOTION clip (the Fox rigs) is ALREADY caught by the yaw gate above —
    # the knee gate added zero unique locomotion coverage. A reliable knee
    # check needs joint POSITIONS (bind-relative hinge sign), not bare quats;
    # that belongs in the retargeting-math pass (foot-lock / IK), not here.

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
    # Curation ship-gate (#838): the app's Animation Library picker writes
    # curation.json ({"approved": ["<source>", ...]}, source = "title — anim").
    # --approved-only ships ONLY the user-approved clips; the rest stay in the
    # corpus for later improvement. --curation defaults to the app's file.
    ap.add_argument("--curation", default="",
                    help="path to curation.json (approved clip sources)")
    ap.add_argument("--approved-only", action="store_true",
                    help="keep only clips whose source is in --curation")
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
                    # when newer than it (delete *.canonical.v2.json to force).
                    # V2: separate cache so it never collides with the V1
                    # (22-joint) sidecar; passes --v2 for the 52-joint dump.
                    cache = fpath + ".canonical.v2.json"
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
                                 "--dump-canonical", cache, "--v2"],
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
                        anim = c.get("animation", "")
                        excl = _excluded(title, anim)
                        if excl:
                            print(f"  - {'':<10} {title[:38]:<40} {anim} "
                                  f"EXCLUDED: {excl}")
                            continue
                        q = c.get("quats", [])
                        if len(q) < args.min_frames:
                            continue
                        # Repair a rotated/flipped first frame (Mini Chibi etc.)
                        # before windowing, so a window opening at frame 0 is clean.
                        q = fix_first_frame_flip(q)
                        action = action_for(anim, tags)
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
                        # V2 dumps carry 52 joints (fingers folded in); V1 has
                        # 22. Accept the rest arrays at the DUMP's own joint
                        # width (the quats width), not the fixed body count —
                        # otherwise a 52-wide V2 rest array is wrongly dropped.
                        n_joints = len(q[0]) if q else CANON_COUNT
                        rest_world = c.get("restWorld") \
                            if len(c.get("restWorld", [])) == n_joints \
                            else None
                        rest_dir = c.get("restDir") \
                            if len(c.get("restDir", [])) == n_joints \
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
                        # #838 vertical descent: carry the per-frame hip Y
                        # offset, sliced to the SAME active window as the quats
                        # and re-based so frame 0 of the window reads ~0 (the
                        # retarget deltas the descent against its start frame).
                        ry = c.get("rootY")
                        if ry and len(ry) == len(q):
                            # rootY is a crouch DEPTH vs the rig's BIND-pose
                            # standing height (absolute, ≤ 0 leg-lengths), so an
                            # always-low crawl/sit keeps its real depth — do NOT
                            # re-anchor to the window (that would zero a clip
                            # that opens already crouched). Just window-slice and
                            # apply the 0.6 display gain (full-kneel hip-to-foot
                            # compression is nearly a whole leg; 0.6 lands a
                            # believable depth).
                            wry = ry[s:epos]
                            if wry:
                                clip["rootY"] = [
                                    round(0.6 * min(0.0, v), 5) for v in wry]
                        # V2 (schema v4): fingers are canonical JOINTS 22..51 in
                        # `quats`/`restWorld`/`restDir` already — no separate
                        # `fingers` side-channel. (The V1 builder copied
                        # c["fingers"] here; V2 dumps don't emit it.)
                        # Block-hand rigs ship WITHOUT finger data (targets
                        # hold their bind hands — see FINGERLESS_SOURCES).
                        if any(fs.lower() in title.lower()
                               for fs in FINGERLESS_SOURCES):
                            for fr in clip["quats"]:
                                for j in range(22, min(52, len(fr))):
                                    fr[j] = [0.0, 0.0, 0.0, 0.0]
                            for key in ("restWorld", "restDir"):
                                arr = clip.get(key)
                                if arr and len(arr) == 52:
                                    zero = [0.0]*len(arr[22])
                                    for j in range(22, 52):
                                        arr[j] = list(zero)
                        clips.append(clip)
                        print(f"  + {action:<10} {title[:38]:<40}"
                              f" {c.get('animation')} ({len(w)}f, q={quality:.2f})")

    if not clips:
        sys.exit("no clips extracted — is the corpus downloaded/validated?")

    # Curation ship-gate (#838): keep only user-approved clips when requested.
    if args.approved_only:
        cur = args.curation
        if not cur:
            # default to the app's curation file (macOS AppData layout first)
            home = os.path.expanduser("~")
            for cand in (
                os.path.join(home, "Library/Application Support/QtMeshEditor/"
                                   "QtMeshEditor/ai_models/motion/curation.json"),
                os.path.join(home, "Library/Application Support/QtMeshEditor/"
                                   "ai_models/motion/curation.json"),
                os.path.join(home, ".local/share/QtMeshEditor/QtMeshEditor/"
                                   "ai_models/motion/curation.json"),
            ):
                if os.path.exists(cand):
                    cur = cand
                    break
        if not cur or not os.path.exists(cur):
            sys.exit("--approved-only: no curation.json found "
                     "(pass --curation or mark clips in the app first)")
        approved = set(json.load(open(cur)).get("approved", []))
        before = len(clips)
        clips = [c for c in clips if c["source"] in approved]
        print(f"\ncuration gate: {before} -> {len(clips)} clips "
              f"({len(approved)} approved in {cur})")
        if not clips:
            sys.exit("no approved clips matched — check curation.json sources")
        counts.clear()
        for c in clips:
            counts[c["action"]] = counts.get(c["action"], 0) + 1

    # Schema v4 = 52-joint canonical skeleton (fingers folded in as joints
    # 22..51). The app's MotionLibrary loader reads v1..v4; v4 → jointCount 52.
    lib = {"schema": "qtmesh-motion-library-v4", "joints": joints,
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
