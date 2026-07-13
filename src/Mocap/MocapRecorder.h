#ifndef MOCAPRECORDER_H
#define MOCAPRECORDER_H

// Turns a FaceSample stream into ordinary animation clips (epic #869,
// Slice D #873) — the only Ogre-touching mocap piece so far.
//
//  - Morph weights land as weight keyframes on a named mesh VAT_POSE clip via
//    MorphAnimationManager::writeWeightKeyOn (the #519 pipeline: dope sheet,
//    timeline playback and glTF morph-weight export all work downstream).
//    Epsilon run-length suppression keeps near-constant channels from writing
//    30 keys/sec; first/last confident samples always key; frames with
//    confidence 0 write nothing, and gaps > 0.5 s get hold keys at both edges
//    so interpolation doesn't sweep through them.
//  - Head pose (camera-relative) is re-based on the take's FIRST confident
//    sample (the calibration frame: looking at the camera at start == rig
//    neutral) and keyed as rotation deltas on the Head bone (skinned mesh,
//    resolved via MotionInbetween::canonicalIndexForBone) in a skeletal clip
//    named "<clipName>_Head", or as node-TRS deltas via NodeAnimationManager
//    for a static mesh.
//
// Smoothing is the CALLER's job (One-Euro over the stream before recording);
// the recorder stores what it is given. GUI/MCP wrap a take in ONE
// RecordMocapClipCommand (src/commands/) so Ctrl+Z removes the whole take.

#ifdef ENABLE_MOCAP

#include "FaceCapMapper.h"
#include "FaceCapPredictor.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace Ogre {
class Entity;
}

namespace MocapRecorder {

struct FaceRecordOptions {
    QString clipName = QStringLiteral("FaceCap");  // morph weight clip name
    bool head = true;                // also record head pose
    bool replaceExisting = true;     // clip with the same name is replaced
    double weightEpsilon = 0.01;     // suppress keys that don't change
    double headEpsilonRad = 0.006;   // ~0.35 deg — head key suppression
    double gapHoldSeconds = 0.5;     // face-lost gap that forces hold keys
};

struct FaceRecordReport {
    int framesProcessed = 0;
    int framesNoFace = 0;
    int keyframesWritten = 0;        // morph weight keys
    int headKeyframesWritten = 0;
    QStringList matchedChannels;
    QStringList unmatchedCanonical;
    QStringList unmatchedMesh;
    QString headTarget;              // "bone:<name>" | "node" | "none"
    QString clipName;
    double clipLength = 0.0;         // seconds
    QString error;                   // non-empty = nothing was written

    bool ok() const { return error.isEmpty(); }
};

// Writes the take onto `entity`. Samples must be time-ascending; times are
// re-based so the first confident sample is t=0. Main thread only.
FaceRecordReport recordFace(Ogre::Entity* entity,
                            const std::vector<FaceSample>& samples,
                            const FaceCapMapper::Mapping& mapping,
                            const FaceRecordOptions& options = {});

// Head-bone resolution (exposed for the GUI gate + tests): the first bone of
// the entity's skeleton whose name resolves to the canonical Head role, or
// empty when the entity is not skinned / has no such bone.
QString resolveHeadBone(Ogre::Entity* entity);

}  // namespace MocapRecorder

#endif  // ENABLE_MOCAP
#endif  // MOCAPRECORDER_H
