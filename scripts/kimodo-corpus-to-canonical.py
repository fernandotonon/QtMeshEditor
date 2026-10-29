#!/usr/bin/env python3
"""Convert a Kimodo corpus (scripts/build-kimodo-t2m-corpus.py) to canonical clips for t2m training (dev tool).

Each batch of interchange NPZs is applied to a reference humanoid rig with `qtmesh anim --apply-motion` (the
editor's own SOMA -> rig retarget) and read back with `qtmesh anim --dump-canonical` (the 22-joint canonical
skeleton + fingers the motion-library / t2m scripts already consume), so the canonical convention is never
re-implemented here. Clips that fail the quality gates (quality.jsonl from check-kimodo-t2m-corpus.py) are
skipped. Root travel is not part of the canonical format (rootY is): the t2m model is root-locked.

    python3 scripts/kimodo-corpus-to-canonical.py ~/t2m_kimodo/v1 --rig "media/models/Rumba Dancing.fbx" \
        --qtmesh build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor

Output: <corpus>/canonical/batch_NNN.json (dump-canonical schema, each clip tagged with id/text/seed) and
<corpus>/canonical/index.jsonl. Resumable per batch.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("corpus")
    ap.add_argument("--rig", required=True, help="reference humanoid rig (a Mixamo rig with fingers)")
    ap.add_argument("--qtmesh", required=True)
    ap.add_argument("--batch", type=int, default=40)
    ap.add_argument("--include-failed", action="store_true", help="also convert clips that failed a gate")
    args = ap.parse_args()

    root = os.path.expanduser(args.corpus)
    rows = [json.loads(l) for l in open(os.path.join(root, "corpus.jsonl"), encoding="utf-8") if l.strip()]
    quality = {}
    qpath = os.path.join(root, "quality.jsonl")
    if os.path.exists(qpath):
        quality = {q["id"]: q for q in map(json.loads, open(qpath, encoding="utf-8"))}
    elif not args.include_failed:
        print("no quality.jsonl: run scripts/check-kimodo-t2m-corpus.py first (or pass --include-failed)")
        return 2
    keep = [r for r in rows if args.include_failed or quality.get(r["id"], {}).get("ok")]
    out_dir = os.path.join(root, "canonical")
    os.makedirs(out_dir, exist_ok=True)
    index_path = os.path.join(out_dir, "index.jsonl")
    print(f"{len(keep)}/{len(rows)} clips to convert in batches of {args.batch}")

    with open(index_path, "a", encoding="utf-8") as index, tempfile.TemporaryDirectory() as tmp:
        for b in range(0, len(keep), args.batch):
            batch = keep[b:b + args.batch]
            out = os.path.join(out_dir, f"batch_{b // args.batch:03d}.json")
            if os.path.exists(out):
                continue
            fbx = os.path.join(tmp, "batch.fbx")
            cmd = [args.qtmesh, "anim", args.rig, "--name", "k", "-o", fbx]
            for r in batch:
                cmd += ["--apply-motion", os.path.join(root, r["file"])]
            subprocess.run(cmd, check=True, capture_output=True)
            dump = os.path.join(tmp, "batch.json")
            subprocess.run([args.qtmesh, "anim", fbx, "--dump-canonical", dump], check=True, capture_output=True)
            data = json.load(open(dump, encoding="utf-8"))
            by_name = {c["animation"]: c for c in data["clips"]}
            clips = []
            for i, r in enumerate(batch):
                name = "k" if i == 0 else f"k_v{i + 1}"   # MotionSceneApply's variant naming, in file order
                c = by_name.get(name)
                if c is None:
                    print(f"  missing {name} for {r['id']}")
                    continue
                c.update({"animation": r["id"], "id": r["id"], "text": r["text"], "seed": r["seed"],
                          "source": "kimodo-soma-rp-v1.2"})
                clips.append(c)
                index.write(json.dumps({"id": r["id"], "text": r["text"], "batch": os.path.basename(out),
                                        "frames": c["frames"]}) + "\n")
            data["clips"] = clips
            data["source"] = "kimodo corpus via qtmesh --apply-motion / --dump-canonical"
            tmp_out = out + ".part"
            with open(tmp_out, "w", encoding="utf-8") as f:
                json.dump(data, f)
            os.replace(tmp_out, out)
            index.flush()
            print(f"  {os.path.basename(out)}: {len(clips)} clips")
    return 0


if __name__ == "__main__":
    sys.exit(main())
