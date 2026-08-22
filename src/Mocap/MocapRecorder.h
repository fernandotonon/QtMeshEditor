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
#include "MocapLiveTypes.h"

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
    QString headError;               // non-empty = head clip skipped (e.g. a
                                     // pre-existing node clip we won't clobber);
                                     // weight keys still written — not fatal

    bool ok() const { return error.isEmpty(); }
};

// Writes the take onto `entity`. Samples must be time-ascending; times are
// re-based so the first confident sample is t=0. Main thread only.
FaceRecordReport recordFace(Ogre::Entity* entity,
                            const std::vector<FaceSample>& samples,
                            const FaceCapMapper::Mapping& mapping,
                            const FaceRecordOptions& options = {});

// ---- body capture (Slice E #874) -------------------------------------------

struct BodyRecordOptions {
    QString clipName = QStringLiteral("BodyCap");
    bool replaceExisting = true;
    QString algorithmUsed = QStringLiteral("pose-ik");  // for the report
    QString fallbackReason;                             // why not sam3dbody
    // Roles body bake must not write (FaceCap owns the head chain when Head
    // is enabled — typically Neck | Neck1 | Head so webcam look-down does not
    // stack with the face solve).
    uint32_t skipRolesMask = 0;
};

struct BodyRecordReport {
    QString clipName;
    QString algorithmUsed;
    QString fallbackReason;
    int framesProcessed = 0;
    int rolesResolved = 0;     // distinct canonical roles that landed on bones
    int tracksWritten = 0;     // skeleton bones keyed
    double clipLength = 0.0;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

// clipQuats: [frame][22 canonical roles] WORLD quats (x,y,z,w) — the
// PoseIKSolver / SAM3DBody output stream, uniformly sampled at `fps`. Runs
// AnimationMerger::applyMotionClip's world-frame retarget (delta vs frame 0 =
// the calibration frame; rotation-only keys; root locked). Requires a skinned
// entity resolving >= half the canonical roles (the humanoid gate).
BodyRecordReport recordBody(
    Ogre::Entity* entity,
    const std::vector<std::vector<std::array<float, 4>>>& clipQuats, int fps,
    const BodyRecordOptions& options = {});

// Same retarget path as the live preview: BodyRetargeter + landmark directions.
// `frames` must be time-ascending BodyLiveFrame samples (world + visibility kept).
BodyRecordReport recordBodyLive(
    Ogre::Entity* entity,
    const std::vector<BodyLiveFrame>& frames, int fps,
    const BodyRecordOptions& options = {});

// Head-bone resolution (exposed for the GUI gate + tests): the first bone of
// the entity's skeleton whose name resolves to the canonical Head role, or
// empty when the entity is not skinned / has no such bone.
QString resolveHeadBone(Ogre::Entity* entity);

}  // namespace MocapRecorder

#endif  // ENABLE_MOCAP
#endif  // MOCAPRECORDER_H
