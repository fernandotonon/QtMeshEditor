# Motion clip authoring (in-house template clips)

Offline developer tooling that authors **CC0 template clips for the
text-to-motion library (#411/#837) from scratch** — procedural keyframes on a
real Mixamo-convention rig, no third-party motion data. The clips shipped in
September 2026 were produced with these scripts. **Seven shipped** (walk,
idle, wave, jump, march, cheer, hang); march, cheer, hang and wave fill
actions the corpus does not cover at all.

Seven more (run, punch, kick, sit, throw, dance, crawl) were authored,
reviewed and **withdrawn** — they never reached the quality of the real
mocap corpus takes, and a weak template makes the feature worse, not
better. Those actions are served by corpus clips instead. Treat that as the
bar: only ship an authored clip that beats what the corpus already has, or
that fills a genuine gap.

Not shipped; the app never runs Python.

## Pipeline

```bash
# 1. A Mixamo-rig glb to author onto (any Mixamo character export works)
qtmesh convert "Rumba Dancing.fbx" -o rumba.glb

# 2. Author every action (or a subset) — writes out_<action>.glb next to rumba.glb
python3 actions.py            # all actions
python3 actions.py walk wave  # a subset

# 3. Visually verify each clip frame-by-frame (judge the gait from the PROFILE row)
qtmesh isometric out_walk.glb --animation walk --frames 8 --directions 4 \
    --resolution 200 --elevation 8 -o walk_check.png

# 4. Lay out a corpus + build a library (see build-motion-library-v6.py)
mkdir -p corpus/raw/authored/walk && cp out_walk.glb corpus/raw/authored/walk/walk.glb
python3 ../build-motion-library-v6.py --corpus corpus --out authored-library.json

# 5. Merge additively into the live library and publish (motion/motion-library-v2.json
#    on the fernandotonon/QtMeshEditor-models HF repo) + update ATTRIBUTION-v2.md
```

The animation name written into the glb must be a **bare action keyword**
("walk", not "authored_walk") — `build-motion-library-v6.py`'s labeller drops
multi-word names that match no keyword.

## Authoring model

`glbanim.py` rewrites a glb's animations in place: for each Mixamo joint it
samples `pose_fn(t01) -> {joint: (x_deg, y_deg, z_deg)}` at 30 fps and writes
`final_local = bind_local * euler_delta` rotation channels (LINEAR). An optional
`hips_translate_fn(t01)` adds root bob/travel to the hips translation.

`actions.py` holds the action library. Angles are **anatomical**: the author
layer negates Y and Z for right-side joints, because the Mixamo right-side bind
frames are mirrored across the sagittal plane — the same raw local Y/Z means
the OPPOSITE anatomical motion on the right (numerically verified: left arm
Z+60 swings world-forward, right arm Z+60 swings world-BACKWARD; X is
symmetric). With the mirror layer, the same (x, y, z) always means the same
body motion on either side:

| joint    | X+                    | Y+     | Z+              |
|----------|-----------------------|--------|-----------------|
| arm      | swing down toward body| twist  | swing forward   |
| forearm  | —                     | —      | elbow curl      |
| upleg    | thigh raise forward   | twist  | leg out to side |
| leg (X−) | **knee flexion** — heel toward buttock | — | (never use) |
| leg      | **hyperextends** (wrong) | —      | (never use)     |
| foot     | pitch                 | yaw    | roll            |
| spine    | bend forward          | twist  | side bend       |
| head     | look down             | turn   | tilt            |
| hips     | whole-body fwd lean   | yaw    | lateral roll    |

Authoring tips learned the hard way:

- **Cyclic clips must close**: `pose_fn(0) == pose_fn(1)` (the app loops them),
  and clip frame 0 is the retarget's delta reference — start near neutral.
- **Whole-body pitch belongs on the SPINE chain, not the hips**: the
  retarget locks the root's orientation to the standing pose, so a
  hips-pitched crawl retargets as an upright kneel. Bow spine/spine1/spine2
  instead (the crawl clip is the reference).
- **The knee only bends ONE way: X-POSITIVE** (heel toward the buttock).
  Negative X swings the shin forward — hyperextension, a backwards-bending
  knee. Establish this with a RENDER, never a derived metric: author one glb
  with the same bend at both signs and look at the side view
  (`probe/knee_sign_side.png`). Two separate analytic metrics got this
  backwards and certified visibly broken clips as correct, costing two full
  rounds of bad builds.
- **`knee_check.py` reports +deg for flexion, −deg for hyperextension.** Its
  polarity is calibrated against the RENDER above, not against an assumption.
  Small negatives (~−3°) are the rig's bind offset, not a defect. The leg
  chain is NOT mirrored for X the way the arms are, so both legs use the same
  sign — an earlier version flipped the left side and invented a phantom
  left/right disagreement.
- **Check the FOOT's world position, not just bone directions.** A bone-angle
  metric can look right while the foot ends up at hip height behind the body.
  `probe_dir.py` gives directions; the foot-position walk in the same file is
  what catches kneeling/backward-kick errors.
- **Keep arms close to the body.** An upper arm held out to the side (world
  |X| > ~0.35 of its direction vector) sits near the retarget's shoulder
  singularity, and the arm can INVERT mid-clip — it swings backward while
  the elbow and legs still look right. The bind is a T-pose, so "arm down"
  is `r_arm X≈84`, not 72; `X=90` is straight down and `X≈-84` straight up.
  The approved walk/march clips measure mean |X| ≈ 0.16–0.20; anything much
  above that is a warning sign.
- **Watch stacked torso yaw.** Yaw on hips + spine + spine1 accumulates and
  rotates the whole arm chain: punch and throw had −42° and −50° total,
  which swung a forward punch out to the side and pushed it into that same
  singularity. Keep the total under ~15° and let the arm carry the motion.
- **Envelopes must not unwind.** A strike whose `release` decays back to 0
  returns the arm to neutral, so the action visibly stops mid-motion. Use a
  separate `follow` envelope that continues to the end of the clip (see
  `throw`).
- **Judge forward/back from the SIDE view** (`--directions 4`, row 1), never
  the front — a limb travelling at the camera is foreshortened and reads as
  stationary. Better still, measure it: `probe_dir.py` prints world bone
  directions with no rendering at all.
- **Fast strikes need a hold plateau** (trapezoid envelope, not a narrow bump):
  the generate path's 12 fps smooth-bake averages away a peak that lives on a
  single keyframe (this is why the first punch draft lost its snap).
- **Idle must beat the library builder's min-energy gate** (0.004 mean rotation
  energy) — sub-perceptual sway gets dropped as "a pose".
