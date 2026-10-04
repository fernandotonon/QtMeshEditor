#ifndef AUDIOTOFACE_LIPSYNCCLI_H
#define AUDIOTOFACE_LIPSYNCCLI_H

// Audio2Face (#1019): `qtmesh lipsync` — audio in, a morph-weight clip on the
// mesh out.
//
//   qtmesh lipsync take.wav --mesh head.glb -o spoken.glb
//   qtmesh lipsync take.wav --mesh head.glb --fps 60 --clip Speech -o out.glb
//   qtmesh lipsync take.wav --mesh head.glb --emotion joy=0.6 -o out.glb
//   qtmesh lipsync take.wav --mesh head.glb --json
//
// Mirrors `MocapCLI` deliberately: the whole keyframe-writing half is shared
// with video face capture (#869), because `MocapRecorder::recordFace` takes
// samples plus a name mapping and does not care whether the weights came from
// a camera or a microphone.

namespace AudioToFace {
namespace LipsyncCLI {

/// `argv[0]` is the program, `argv[1]` is "lipsync". Returns a process exit
/// code. Ogre is initialised headless internally.
int run(int argc, char* argv[]);

}  // namespace LipsyncCLI
}  // namespace AudioToFace

#endif  // AUDIOTOFACE_LIPSYNCCLI_H
