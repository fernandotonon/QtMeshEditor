#pragma once

// Apply an interchange motion (a SOMA skeleton, from Kimodo today) to ANY
// humanoid rig in the scene — the generated MHR characters, Mixamo, UniRig —
// through the incompatible-skeleton retargeter (#523). The target keeps its
// own skeleton, bind pose and skinning; only a new skeletal clip is written.
//
// SOMA names are first rewritten to their standard spellings with the
// explicit, versioned table in SomaSkeleton.h; joints without a counterpart
// (end markers, jaw/eyes, finger metacarpals) are kept OUT of the map and
// reported, so nothing is discarded silently.

#include "MotionInterchange.h"

#include <QString>
#include <QStringList>

namespace Ogre { class Skeleton; }

namespace AIMotion {

struct ApplyOptions {
    bool rootMotion = true;      ///< carry the root translation (scaled to the rig's height)
    bool replace = false;        ///< overwrite an existing clip of the same name
    float fps = 30.0f;           ///< keyframe rate of the written clip (resampled, duration kept)
};

struct ApplyOutcome {
    bool ok = false;
    QString error;
    std::string clip;
    int frames = 0;
    float length = 0.0f;
    int mappedBones = 0;
    int fingerBones = 0;
    QStringList unmappedTarget;     ///< rig bones nothing drives
    QStringList unmappedSource;     ///< SOMA joints with a standard name the rig lacks
    QStringList excludedSource;     ///< SOMA joints kept out on purpose: "name (role)"
    QStringList inferredFingers;    ///< "SOMA -> rig bone" pairs found by the unnamed-finger fallback
    QString mapVersion;
    Retarget::RetargetReport report;
};

/// The bone map SOMA -> `targetNames` (pure; exposed for tests and the
/// diagnostics view). Source names are the ORIGINAL SOMA names.
Retarget::BoneMap somaBoneMap(const Motion& m, const std::vector<std::string>& targetNames,
                              QStringList* unmappedSource = nullptr, QStringList* excludedSource = nullptr);

/// Fallback for rigs whose finger bones carry no finger names (UniRig: `joint_9`, `joint_10`, ...): under each mapped
/// hand, find five joint chains (after at most two single-child palm joints) and tell them apart from the bind pose —
/// the thumb is the chain rooted nearest the wrist, then index..pinky by distance from the thumb. Pairs are added to
/// `map` only for bones nothing drives yet; a hand that does not fit that shape is left alone and the reason is
/// appended to `notes`. Returns the "SOMA -> rig bone" pairs it added.
QStringList addUnnamedFingerChains(const Motion& m, const Retarget::RigDesc& target, Retarget::BoneMap* map,
                                   QStringList* notes = nullptr);

ApplyOutcome applyMotionToSkeleton(Ogre::Skeleton* skeleton, const Motion& motion, const std::string& clip,
                                   const ApplyOptions& options = {});

} // namespace AIMotion
