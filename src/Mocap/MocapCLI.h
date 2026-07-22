#ifndef MOCAPCLI_H
#define MOCAPCLI_H

// Headless `qtmesh mocap` subcommand (epic #869, Slice D #873):
//
//   qtmesh mocap <video> --face --mesh <meshfile> [-o out.glb]
//          [--clip-name NAME] [--fps 30] [--smooth-cutoff HZ] [--no-smooth]
//          [--map overrides.json] [--no-head] [--frames-dir DIR] [--json]
//
// video file -> FaceCapPredictor -> One-Euro smoothing -> MocapRecorder ->
// morph-weight clip (+ head clip) on the mesh -> optional re-export.
// `--frames-dir` replaces the video with an image sequence
// (ImageSequenceFrameSource) — the headless-CI/debug path that needs no
// Qt Multimedia decode.
//
// Always compiled; a non-ENABLE_MOCAP build prints the standard
// "rebuild with -DENABLE_MOCAP" message (the Alembic pattern).

namespace MocapCLI {

int run(int argc, char* argv[]);

}  // namespace MocapCLI

#endif  // MOCAPCLI_H
