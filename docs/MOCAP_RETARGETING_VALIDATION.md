# Camera body retargeting validation

## Scope

The capture branch was merged with `origin/master` at `1c57b95b` (3.48.0),
preserving the branch's existing Trellis changes. The merge is `3c1b0970`.
The retargeting changes do not require a new pose-model download.

## What changed

- Camera, video preview, CLI and MCP use one landmark-smoothing/input path.
  Body rotations are solved from the filtered landmarks, not filtered again
  independently from the geometry shown in the debug view.
- MediaPipe depth is converted with `(x, -y, -z)`: its smaller Z values are
  closer to the camera. The previous Y-only flip changed handedness.
- The live solver uses anatomical world frames and each target bone's actual
  bind axes. It no longer switches to a different solver near neutral or
  caps high knees at an arbitrary angle. Knee bend planes and transported
  limb poles stabilize roll; missing joints hold their last local pose.
- Heading calibration combines labeled shoulder and hip lines. It removes
  camera heading only, retaining captured lean and full subsequent turns.
- Hip translation is estimated from image-space position and torso scale,
  scaled to the target rig and written into ordinary skeletal keyframes.
  `--in-place` / MCP `root_motion:false` disable translation without disabling
  turns. Translation respects a rotated/scaled hip parent, not the hip's
  animated local axes.
- Face-driven head orientation is converted to local space after posing the
  body. Combined recordings bake this compensated orientation in the body
  clip, avoiding doubled body yaw.
- Recording retains source timestamps and the preview's neutral reference.
  Invalid takes do not overwrite an existing animation. Recalibration during
  recording is disallowed so a take cannot mix reference frames.
- Debug forward kinematics no longer rotates already-posed offsets twice;
  invisible/nonfinite landmark edges are omitted.
- Skeleton-updated culling bounds follow translated avatars in preview and
  on recorded entities. The debug overlay retains a fixed bind-height scale.
- Combined head-only face capture on a humanoid without facial morphs is
  reported as part of the body clip; failed body calibration falls back to
  ordinary face/head recording. Temporary master-skeleton poses are reset
  before export so compensation cannot change the exported bind pose.

## Regression coverage

The broader selected regression run passed 119 tests across 14 suites
(animation merger, mocap controller/recorder, face pose/geometry/mapping,
video sources, filters and the new body solver). The opt-in video test skips
in that run and is executed separately with each supplied video.

The new geometric fixture exercises relaxed-pose calibration, high knees,
both thigh/shin directions, toe direction, arbitrary bind bone roll, a rig
rotated 73 degrees, a complete 360-degree turn, visibility loss, NaNs,
calibration rejection, initial torso lean, rotated/scaled armature parents,
root-motion recording, irregular timestamps, recording started after preview
calibration, in-place output, undo/redo, and face/body head compensation.

The real-video test performs inference on PNG frames, checks the target's
actual child-bone positions against independently computed landmark segment
directions, renders the skinned model, bakes the take, and checks playback at
the beginning, middle and end. Optional GLB export enables a separate
re-import/render check.

## Supplied-video replay

Both supplied videos were sampled at 10 FPS, without landmark smoothing for
the direction-oracle tests. All sampled frames had a usable torso and left
thigh/shin. The observed maximum direction error was approximately 0.04
degrees (floating-point precision), not a claim about pose-estimation accuracy.

| Video | Frames | Reliable thigh/shin segments | Rigs |
| --- | ---: | ---: | --- |
| August 22, 22:53:40 | 429 | 1,715 | Game avatar (scene GLB), human Mixamo (Rumba FBX) |
| August 17, 23:48:54 | 169 | 676 | Game avatar (scene GLB) |

The human rig was replayed again after the final culling-bounds changes.
The old executable's replay was saved as a visual baseline: it inherited
the authored animation's bent-leg/arm pose instead of following the capture
directly.

The rebuilt application's actual MP4/CLI path was also checked:

- August 17: 161 sampled frames, 16.842-second body clip with variable hip
  translation on all three axes. Exported GLB re-import and playback passed.
- August 22 with `--in-place`: 396 sampled frames, 42.851-second clip. All
  exported hip-translation keys are constant, while hip rotation varies.
  Exported GLB re-import and playback passed.
- The full-body August 17 footage produced no confident face detections.
  A closer face crop passed the real face-inference/tracking test; a
  three-second torso crop produced 30 face/body frames and 30 compensated
  head keys in `CombinedValidation_Body`, with no separate head clip. This
  export also re-imported and rendered successfully. This cropped-input
  check does not claim successful face tracking in the original wide shot.

The MP4 source is playback-driven and can sample fewer than exactly ten
frames per second; source timestamps preserve the take's duration.

Local, uncommitted validation artifacts are in `build_local/mocap-validation/`:
`aug22-source-model.png`, before/after human-rig contact sheets, animated GLBs,
CLI playback contact sheets, and regression/replay JSON results. The rebuilt
application is `build_local/bin/QtMeshEditor`.

## Reproduce

Build with mocap, ONNX and tests enabled. Run the focused tests under a real
X11 display or Xvfb:

```bash
xvfb-run -a build_local/bin/UnitTests \
  --gtest_filter='BodyRetargeterGeometryTest.*:BodyPoseStream.*:BodyRootMotion.*:PoseIKDebug.*:PoseIKSolver.*:OneEuro*'
```

`ReplayVideoFrames` skips unless its input is explicitly supplied. Extract
frames with `ffmpeg -i <video> -vf fps=10 <frames-dir>/frame-%04d.png`, then set:

- `QTMESH_MOCAP_TEST_FRAMES`: extracted PNG directory.
- `QTMESH_MOCAP_TEST_MODELS`: existing `mocap/pose` model-cache directory.
- `QTMESH_MOCAP_TEST_MESH`: optional humanoid mesh (otherwise a synthetic
  rolled-bone rig is used).
- `QTMESH_MOCAP_TEST_RENDER`: optional existing directory for skinned PNGs.
- `QTMESH_MOCAP_TEST_OUTPUT`: optional JSON with landmarks/bone positions.
- `QTMESH_MOCAP_TEST_EXPORT`: optional animated GLB destination.

Run `BodyRetargeterGeometryTest.ReplayVideoFrames` with those variables.
`QTMESH_ONNX_PREFER_GPU=0` selects the CPU for reproducible model inference.
Tests do not require a camera or model download.

## Remaining limits

These checks establish that the target follows the captured geometry and
that recording reproduces it; they cannot make monocular inference ground
truth. Ambiguous depth, crossed/occluded limbs, loose clothing and model
tracking errors can still affect the captured pose. Global depth is a
weak-perspective estimate without calibrated camera intrinsics; keep the
camera fixed. There is no foot-contact lock, so some foot slide remains
possible. The supplied videos do not contain a full body revolution; that
case is covered by synthetic 360-degree tests, not a live-camera claim.

MediaPipe's coordinate convention is documented in the
[official pose documentation](https://github.com/google-ai-edge/mediapipe/blob/master/docs/solutions/pose.md).
