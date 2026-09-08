# Motion clip authoring (in-house template clips)

Offline developer tooling that authors **CC0 template clips for the
text-to-motion library (#411/#837) from scratch** — procedural keyframes on a
real Mixamo-convention rig, no third-party motion data. The 12 clips shipped in
September 2026 (walk, run, idle, wave, jump, punch, kick, march, cheer, sit,
throw, dance) were produced with these scripts.

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

`actions.py` holds the action library and the **calibrated Mixamo local-axis
conventions** (derived from single-axis probe renders — same sign gives the same
anatomical motion on both sides because the Mixamo bind is mirrored):

| joint    | X+                    | Y+     | Z+              |
|----------|-----------------------|--------|-----------------|
| arm      | swing down toward body| twist  | swing forward   |
| forearm  | —                     | —      | elbow curl      |
| upleg    | thigh raise forward   | twist  | leg out to side |
| leg      | knee flexion (heel back) | —   | (never use)     |
| foot     | pitch                 | yaw    | roll            |
| spine    | bend forward          | twist  | side bend       |
| head     | look down             | turn   | tilt            |
| hips     | whole-body fwd lean   | yaw    | lateral roll    |

Authoring tips learned the hard way:

- **Cyclic clips must close**: `pose_fn(0) == pose_fn(1)` (the app loops them),
  and clip frame 0 is the retarget's delta reference — start near neutral.
- **Fast strikes need a hold plateau** (trapezoid envelope, not a narrow bump):
  the generate path's 12 fps smooth-bake averages away a peak that lives on a
  single keyframe (this is why the first punch draft lost its snap).
- **Idle must beat the library builder's min-energy gate** (0.004 mean rotation
  energy) — sub-perceptual sway gets dropped as "a pose".
