# Performance Capture (mocap)

Record a video of yourself — or point a webcam at yourself live — and
reproduce the performance on a mesh: facial expressions land as morph-target
weight keyframes, head rotation on the Head bone (or the node for static
meshes), and full-body pose as a skeletal clip on a humanoid rig. Everything
is recorded as ordinary animation clips: they play on the timeline, show in
the dope sheet, undo with a single Ctrl+Z, and export through the normal
exporters.

Requires a build with `-DENABLE_MOCAP=ON -DENABLE_ONNX=ON` (release macOS +
Linux builds ship with it; Windows/MinGW is pending Qt Multimedia
verification). Models (all Apache-2.0 Google MediaPipe conversions, ~31 MB
total) download on first use to `<AppData>/ai_models/mocap/{face,pose}/`.

## Quick start

```bash
# facial performance from a video onto an ARKit-blendshape head:
qtmesh mocap talk.mp4 --face --mesh avatar.glb -o out.glb

# full-body performance onto a rigged humanoid:
qtmesh mocap dance.mp4 --body --mesh rigged.fbx -o out.glb

# both in one pass over the same video:
qtmesh mocap take.mp4 --face --body --mesh character.glb -o out.glb
```

Live mode: **Animation Mode → Mode Tools → Performance Capture** — pick a
camera, `Preview` drives the selection in real time, `● Record` writes the
take as a clip (status line shows the result; Ctrl+Z discards it).

## What the mesh needs

- **Face capture** drives **ARKit-style blendshape morph targets**
  (`jawOpen`, `mouthSmileLeft`, `eyeBlinkRight`, `browInnerUp`, … — the
  52-name vocabulary MediaPipe/ARKit standardized). Sources of compatible
  heads: [Ready Player Me](https://readyplayer.me) avatars export all 52;
  Character Creator / iClone heads use recognized aliases (`Jaw_Open`,
  `Mouth_Smile_L`, …). You can also author targets manually in the
  Edit-Mode **Vertex Morph Animation** section.
  Name matching is forgiving (case/separators/`_L`/`.R` side suffixes all
  normalize), and **unmatched channels are always reported**, never silently
  dropped. Custom names bind via a JSON override sidecar:

  ```json
  { "map": { "jawOpen": "MyJawTarget" }, "ignore": ["tongueOut"] }
  ```

  passed as `--map overrides.json` (CLI) / `map_path` (MCP).

- **Head pose** needs a bone that resolves as the canonical Head
  (`Head`, `mixamorig:Head`, …). Static meshes get node-TRS keyframes
  instead. The take's first confident frame calibrates neutral ("look at
  the camera at the start"); in live mode the `Neutral` button re-bases it.

- **Body capture** needs a **humanoid skeleton** resolving at least half of
  the 22 canonical roles (hips/spine/neck/head, both arms, both legs —
  Mixamo and most generic naming conventions resolve). Unrigged meshes: run
  `qtmesh rig --skeleton humanoid --skin` first. The root stays locked to
  the standing pose (v1 accepts some foot slide).

## Backends

- **Face / head / body-fallback**: converted Google MediaPipe models
  (Apache-2.0), small and fast — the face pipeline runs ~90 fps on an
  M-series CPU.
- **Body quality path (`--algo sam3dbody`)**: Meta's SAM 3D Body is wired as
  the preferred backend but its checkpoints are gated behind a license
  click-through, so today every request falls back to the analytic
  **pose-ik** backend with `algorithmUsed`/`fallbackReason` in the report.
  See `THIRD_PARTY_AI_MODELS.md` for the licensing decision record.
  `--algo pose-ik` / `--no-model` force the fallback explicitly.

## Tuning

- `--fps N` (default 30): capture rate; video frames are decimated to it.
- `--smooth-cutoff HZ` (default 1.0) / `--no-smooth`: One-Euro filter
  minimum cutoff — lower is smoother at rest, `beta` tracks fast motion.
  The live panel exposes the same smoothing.
- `--clip-name NAME`: default `FaceCap` / `BodyCap` (`<name>_Head` for the
  head track, `<name>_Body` when `--face --body` run together).
- `--frames-dir DIR`: use an image sequence instead of a video (headless
  debugging/CI; no video decode involved).
- Model base URL override: `QTMESH_MOCAP_MODEL_BASE_URL` /
  `QSettings ai/mocapModelBaseUrl`; offline guard `QTMESH_MOCAP_NO_DOWNLOAD`.

## macOS camera permission

On first **Preview**, macOS asks to allow camera access; click Allow and the
app then appears under System Settings → Privacy & Security → Camera. For the
prompt to appear the app must be **signed with an Apple Developer ID and
notarized** — macOS silently denies camera access to ad-hoc-signed apps
(no dialog, and the app never shows in the Camera list). The entitlements
(`cfg/QtMeshEditor.entitlements`) and `NSCameraUsageDescription` are in place,
so an official notarized release build prompts normally; a locally-built /
ad-hoc dev build will NOT get the prompt. If you hit that on a dev build, use
the file-based capture (`qtmesh mocap <video>`) instead — it needs no camera
permission and exercises the identical pipeline.

## Known limitations (v1)

- Single person per frame; the highest-scoring detection wins.
- Head pose is camera-relative — walking around the camera reads as head
  rotation. Keep the camera static.
- Body root is locked (no root motion); some foot slide is expected.
- Live mode drives face, head, and (humanoid rig) body; the SAM 3D Body
  quality backend is offline-only (CLI/MCP), body-live uses pose-ik.
- Live camera needs a notarized build on macOS (see above); the CLI/MCP
  video paths work regardless.
- Video decode is playback-driven (a 60 s video takes 60 s to capture).
- Reimporting an exported glTF loses morph-target NAMES (they come back as
  `Shape_N` — an Assimp exporter gap); re-capturing onto a reimported mesh
  needs a `--map` sidecar.

## MCP tools

`capture_face_from_video`, `capture_body_from_video` (heavy, file in /
scene-or-file out), `list_capture_devices`, `start_live_capture`,
`stop_live_capture` (GUI-attached sessions only).
