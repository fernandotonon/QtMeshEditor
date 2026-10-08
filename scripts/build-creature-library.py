#!/usr/bin/env python3
"""Build the CREATURE motion library (#1073) from the CC0 corpus.

The humanoid libraries have `build-motion-library-v*.py`; this is their
counterpart for the non-humanoid body plans (quadruped, winged biped).

It shells out to `qtmesh anim --dump-creature`, which is the SAME extractor
the editor uses, so a clip in the library is sampled exactly as a clip
applied live -- the two cannot drift.

Rebuild after ANY change to the canonical skeletons or the bone mapper:
the clips store per-role world orientations, so a mapper change silently
re-points every clip's data at different bones.

Usage:
  scripts/build-creature-library.py --corpus ~/motion_corpus/raw \
      -o creature-library.json
"""
import argparse, json, re, subprocess, sys
from pathlib import Path

# Verbatim action labels -> the library's vocabulary.
CANON = {
    "walkslow": "walk", "walkfast": "run", "sprint": "run",
    "running": "run", "walking": "walk", "jumping": "jump",
    "dying": "death", "die": "death", "attacking": "attack",
    "bite": "attack", "hit": "hit", "hitreact": "hit",
    "idling": "idle", "flying": "fly", "flap": "fly",
}
KEEP = {"walk", "run", "idle", "jump", "attack", "death", "hit", "fly"}


def action_of(anim: str) -> str | None:
    """Last path segment of an animation name -> a canonical action.

    Deliberately NOT a greedy prefix strip: an earlier `^[A-Za-z]*_?` regex
    ate a bare "Walk" entirely and silently dropped every farm animal, while
    the dinosaurs survived only because of their `Apatosaurus_` prefix.
    """
    tail = anim.split("|")[-1]
    tail = re.sub(r"^[A-Za-z]+_(?=[A-Za-z])", "", tail)  # "Apatosaurus_Walk"
    key = re.sub(r"[^a-z]", "", tail.lower())
    key = CANON.get(key, key)
    return key if key in KEEP else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True, type=Path)
    ap.add_argument("--qtmesh", default="./build_local/bin/qtmesh")
    ap.add_argument("-o", "--output", required=True, type=Path)
    ap.add_argument("--max-per-action", type=int, default=16)
    ap.add_argument("--work", type=Path, default=Path("/tmp/creature_dumps"))
    a = ap.parse_args()

    a.work.mkdir(parents=True, exist_ok=True)
    clips, refused = [], 0
    for fbx in sorted(a.corpus.rglob("*.fbx")):
        dump = a.work / (fbx.stem + ".json")
        r = subprocess.run([a.qtmesh, "anim", str(fbx), "--dump-creature", str(dump)],
                           capture_output=True, text=True)
        if r.returncode != 0 or not dump.exists():
            refused += 1
            continue
        try:
            d = json.loads(dump.read_text())
        except json.JSONDecodeError:
            refused += 1
            continue
        plan = d.get("bodyPlan")
        if plan not in ("quadruped", "wingedBiped"):
            refused += 1
            continue
        for c in d.get("clips", []):
            act = action_of(c.get("animation", ""))
            if not act or len(c.get("quats", [])) < 2:
                continue
            clips.append({
                "action": act,
                "source": f"Quaternius — {fbx.stem} — "
                          f"{c['animation'].split('|')[-1]}",
                "category": "creature",
                "skeleton": plan,
                "frames": len(c["quats"]),
                "quality": 1.0,
                "quats": c["quats"],
                "restWorld": c["restWorld"],
                # Root displacement channel: what makes jump/death/attack
                # readable. Absent in dumps from an older build, in which case
                # the retarget stays rotation-only for that clip.
                "rootOffset": c.get("rootOffset", []),
            })

    # Drop exact duplicates. The first build shipped Cow|Walk and Horse|Walk
    # twice each (indices 4/79 and 5/80) because the corpus contains the same
    # pack under two paths, which wasted cap slots on identical motion.
    seen_sig, uniq = set(), []
    for c in clips:
        sig = (c["action"], c["skeleton"], c["frames"],
               repr(c["quats"][0]) if c["quats"] else "")
        if sig in seen_sig:
            continue
        seen_sig.add(sig)
        uniq.append(c)
    clips = uniq

    # Cap per (action, plan) so one well-covered rig cannot dominate.
    capped, seen = [], {}
    for c in clips:
        k = (c["action"], c["skeleton"])
        seen[k] = seen.get(k, 0) + 1
        if seen[k] <= a.max_per_action:
            capped.append(c)

    if not capped:
        print("error: no clips survived -- refusing to write an empty library",
              file=sys.stderr)
        return 1

    a.output.write_text(json.dumps(
        {"schema": "qtmesh-creature-library-v1", "fps": 30, "clips": capped}))
    print(f"{len(capped)} clips ({refused} assets refused) -> {a.output}")
    for k in sorted(seen):
        print(f"  {k[0]:8s} {k[1]:12s} {min(seen[k], a.max_per_action)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
