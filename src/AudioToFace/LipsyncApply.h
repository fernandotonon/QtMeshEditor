#ifndef AUDIOTOFACE_LIPSYNCAPPLY_H
#define AUDIOTOFACE_LIPSYNCAPPLY_H

// Audio2Face (#1019): the shared "solved weights -> morph keyframes" step.
//
// Extracted because three surfaces need it — `qtmesh lipsync`, the Inspector
// controller, and the MCP tool — and it encodes two rules that are silently
// wrong if reimplemented from memory:
//
//  1. A key carries EVERY matched pose, not just the ones that changed. ARKit
//     targets on a submesh share one VAT_POSE track, and Ogre interpolates a
//     pose missing from the next keyframe TOWARD ZERO
//     (`VertexAnimationTrack::applyToVertexData`: "Search for entry in
//     keyframe 2 list (if not there, will be 0)"). Keying per channel made
//     steady channels dip to 0 and back — measured at 39 dips across 14
//     channels on one 59-frame take. The epsilon decides WHEN to key, never
//     WHICH poses a key carries.
//  2. An existing clip of the same name is REPLACED, not merged into.
//     `writeWeightKeyOn` reuses tracks and only overwrites coincident times,
//     and clip length only grows, so re-running leaves stale keys and can
//     play past the end of the new take.
//
// This header is Ogre-aware (it writes keyframes) but surface-agnostic: no
// CLI printing, no Qt widgets, no undo stack. The GUI wraps the same result
// in its own undo command.

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <vector>

namespace Ogre { class Entity; }

namespace AudioToFace {

struct PredictResult;

/// The emotion vector's channel names, in the order the network expects.
/// Shared because the CLI, the Inspector and the MCP tool all accept emotion
/// by name and must agree on the ordering — the model takes an unlabelled
/// 26-wide tensor, so a mismatched index is silently the wrong emotion.
///
/// This is USER-SUPPLIED input, never predicted: NVIDIA's Audio2Emotion is
/// licensed for use only alongside Audio2Face and is deliberately not shipped.
inline constexpr const char* kEmotionNames[] = {
    "amazement", "anger", "cheekiness", "disgust", "fear",
    "grief", "joy", "outofbreath", "pain", "sadness",
};
inline constexpr int kEmotionCount = 10;

/// How solver pose names bind to the mesh's own morph-target names.
struct NameBinding {
    /// Resolved mesh target per solver pose index; empty = unmatched.
    std::vector<QString> poseTarget;
    QStringList matched;      ///< solver poses that found a target
    QStringList unmatched;    ///< solver poses with no target on this mesh
    QStringList ignored;      ///< poses the caller asked to skip
    QString error;            ///< non-empty = binding refused, do not proceed

    bool ok() const { return error.isEmpty() && !matched.isEmpty(); }
};

/// Match solver pose names to `meshTargets`, case-insensitively and ignoring
/// a leading underscore, so a rig spelling it `JawOpen` or `_jawOpen` drives.
/// `overrides` (canonical pose name -> exact mesh target) wins over matching
/// and is REFUSED when it names a target the mesh lacks — a caller asking for
/// a specific binding must not be silently given name matching instead.
NameBinding bindPoseNames(const QStringList& poseNames,
                          const QStringList& meshTargets,
                          const QHash<QString, QString>& overrides = {},
                          const QSet<QString>& ignore = {});

/// Frame indices that should become keyframes: the first, the last, and any
/// frame where a matched channel moved by at least `epsilon`.
std::vector<size_t> selectKeyFrames(const PredictResult& pred,
                                    const NameBinding& binding,
                                    float epsilon = 0.01f);

struct ApplyResult {
    int keyframesWritten = 0;
    QString error;
    bool ok() const { return error.isEmpty() && keyframesWritten > 0; }
};

/// Replace `clipName` on `entity` with the take, writing every matched pose at
/// each selected key time. Returns an error rather than throwing.
ApplyResult applyToEntity(Ogre::Entity* entity,
                          const QString& clipName,
                          const PredictResult& pred,
                          const NameBinding& binding,
                          float epsilon = 0.01f);

}  // namespace AudioToFace

#endif  // AUDIOTOFACE_LIPSYNCAPPLY_H
