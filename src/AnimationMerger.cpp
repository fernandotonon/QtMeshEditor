#include "AnimationMerger.h"
#include "AutoRig.h"
#include "SentryReporter.h"
#include "FootContact.h"
#include "MotionInbetween.h"
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreKeyFrame.h>
#include <QSet>
#include <QMap>
#include <QRegularExpression>
#include <cctype>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <limits>
#include <map>
#include <vector>
#include <cmath>

#ifdef ENABLE_MOCAP
#include "Mocap/PoseIKSolver.h"
#include "Mocap/MocapPoseIkFk.h"
#endif

// Registry: skeleton name → up-axis (1=Y-up, 2=Z-up).
// Populated by AnimationMerger::registerSkeletonUpAxis() at import time.
static QMap<QString, int> s_skeletonUpAxis;

// #854: last-applied arm-space angle, tracked PER SKELETON INSTANCE (not a
// process-global map — that pollutes across entities and across tests that
// share a process). Stored on the skeleton's first bone via UserObjectBindings
// under a per-animation key, so it lives and dies with the skeleton and is
// naturally isolated. Session-scoped only; export bakes the final keyframes.
namespace {
std::string armSpaceKey(const std::string& animName)
{
    return "qtme.armspace." + animName;
}
float getStoredArmSpace(Ogre::Skeleton* skel, const std::string& animName)
{
    if (!skel || skel->getNumBones() == 0) return 0.0f;
    const Ogre::Any& a = skel->getBone(0)->getUserObjectBindings()
                             .getUserAny(armSpaceKey(animName));
    return a.has_value() ? Ogre::any_cast<float>(a) : 0.0f;
}
void setStoredArmSpace(Ogre::Skeleton* skel, const std::string& animName,
                       float degrees)
{
    if (!skel || skel->getNumBones() == 0) return;
    skel->getBone(0)->getUserObjectBindings().setUserAny(
        armSpaceKey(animName), Ogre::Any(degrees));
}
} // namespace

void AnimationMerger::registerSkeletonUpAxis(const std::string& name, int upAxis) {
    s_skeletonUpAxis[QString::fromStdString(name)] = upAxis;
}

static int lookupUpAxis(const std::string& name) {
    return s_skeletonUpAxis.value(QString::fromStdString(name), 1); // default Y-up
}

// Remove common noise tokens from animation names before slugifying.
// e.g. "Armature|mixamo.com|Layer0" → "Armature|Layer0"
// e.g. "Armature|unreal_take|Layer0" → "Armature|Layer0"
// e.g. "Unreal Take" (UE FBX take name, space variant) → ""
static QString cleanAnimNoise(const QString& name)
{
    QString s = name;
    s.replace(QRegularExpression("\\|?mixamo\\.com\\|?"), "|");
    // Match both "unreal_take" (underscore) and "Unreal Take" (space) — UE FBX exports
    s.replace(QRegularExpression("\\|?unreal[_ ]take\\|?", QRegularExpression::CaseInsensitiveOption), "|");
    s.replace(QRegularExpression("\\|\\|+"), "|");
    if (s.startsWith('|')) s.remove(0, 1);
    if (s.endsWith('|')) s.chop(1);
    return s;
}

// Convert a name to a slug: lowercase, non-alphanumeric → underscore, collapsed, trimmed.
// e.g. "Hip Hop Dancing.fbx" → "hip_hop_dancing_fbx"
static QString slugify(const QString& name)
{
    QString s = name.toLower();
    s.replace(QRegularExpression("[^a-z0-9]+"), "_");
    s.replace(QRegularExpression("_+"), "_");
    if (s.startsWith('_')) s.remove(0, 1);
    if (s.endsWith('_')) s.chop(1);
    return s;
}

// Build a clean animation name. Only prepends the node/file prefix when the
// animation name is generic (e.g. "mixamo.com" → use prefix). If the animation
// already has a meaningful name (e.g. "jump", "idle"), just clean and return it.
static QString buildAnimName(const QString& prefix, const QString& animName)
{
    QString slugPrefix = slugify(prefix);
    QString cleanAnim = cleanAnimNoise(animName);
    QString slugAnim = slugify(cleanAnim);

    // Both empty — shouldn't happen, but guard against it
    if (slugAnim.isEmpty() && slugPrefix.isEmpty())
        return QStringLiteral("animation");

    // If after cleanup the name is empty or just "mixamo_com" residue,
    // use the node/file name as the animation name
    if (slugAnim.isEmpty())
        return slugPrefix;

    // Otherwise keep the meaningful animation name as-is
    return slugAnim;
}

// Deduplicate a name against an existing set, appending _2, _3, etc. if needed.
// Intentional numeric suffixes (e.g. "mm_attack_03") are preserved when the
// name is unique. _N stripping only kicks in when the name already collides,
// so repeated merges produce jump, jump_2, jump_3 (not jump_2_2, jump_2_2_2).
static QString deduplicateName(const QString& desired, QSet<QString>& existingNames)
{
    // If the desired name is free, use it as-is (preserve intentional _N suffixes).
    if (!existingNames.contains(desired)) {
        existingNames.insert(desired);
        return desired;
    }

    // Collision — strip any trailing _N to find the canonical base, then re-suffix.
    QString baseName = desired;
    QRegularExpression trailingSuffix("_(\\d+)$");
    auto match = trailingSuffix.match(baseName);
    if (match.hasMatch())
        baseName = baseName.left(match.capturedStart());

    int suffix = 2;
    QString candidate;
    do {
        candidate = baseName + "_" + QString::number(suffix++);
    } while (existingNames.contains(candidate));
    existingNames.insert(candidate);
    return candidate;
}

// Copy all animations from srcSkel into baseSkel, remapping bone handles by name.
// This bypasses Ogre's _mergeSkeletonAnimations hierarchy check, which rejects skeletons
// that are structurally compatible (same bone names) but differ in their root chain
// (e.g. a mesh skeleton wrapping 'root' under a mesh-name bone that the animation lacks).
//
// srcUpAxis / baseUpAxis: coordinate-system up-axis (1=Y-up, 2=Z-up).
// When the source was exported from a Z-up tool (e.g. Unreal Engine) and the base
// skeleton is Y-up (e.g. Mixamo), needsZupToYup converts the keyframe data.
//
// Additionally, AnimationProcessor stores translations in bone-local space (pre-multiplied
// by the inverse binding-pose orientation).  If the source and target skeletons have
// different binding-pose orientations for the same bone — e.g. an animation-only FBX has
// identity root while a mesh FBX has a baked R_x(+90°) root — the stored delta must be
// re-expressed in the target bone's local space.
// We apply: corrected = (q_src⁻¹ * q_dst) * stored
static void mergeAnimationsByName(Ogre::Skeleton* baseSkel, const Ogre::Skeleton* srcSkel,
                                   int srcUpAxis = 1, int baseUpAxis = 1)
{
    const bool needsZupToYup = (srcUpAxis == 2 && baseUpAxis == 1);

    // Z-up (Assimp/FBX after ConvertToLeftHanded) → Y-up (Ogre) for translations:
    //   R_x(-90°): (x,y,z) → (x, z, -y)
    // Rotations are stored in bone-local space and pass through unchanged —
    // the baked dest bone orientation provides the correct world-space result.

    // Build name→handle map for the base skeleton
    std::unordered_map<std::string, unsigned short> baseHandleByName;
    for (unsigned short i = 0; i < baseSkel->getNumBones(); ++i)
        baseHandleByName[baseSkel->getBone(i)->getName()] = i;

    unsigned short numAnims = srcSkel->getNumAnimations();
    for (unsigned short a = 0; a < numAnims; ++a)
    {
        const Ogre::Animation* srcAnim = srcSkel->getAnimation(a);
        Ogre::Animation* dstAnim = baseSkel->createAnimation(srcAnim->getName(), srcAnim->getLength());
        dstAnim->setInterpolationMode(srcAnim->getInterpolationMode());
        dstAnim->setRotationInterpolationMode(srcAnim->getRotationInterpolationMode());

        for (const auto& [srcHandle, srcTrack] : srcAnim->_getNodeTrackList())
        {
            if (srcHandle >= srcSkel->getNumBones())
                continue;
            const std::string& boneName = srcSkel->getBone(srcHandle)->getName();
            auto it = baseHandleByName.find(boneName);
            if (it == baseHandleByName.end())
                continue; // bone not in base — skip track

            unsigned short baseHandle = it->second;
            auto* dstTrack = dstAnim->createNodeTrack(baseHandle);
            dstTrack->setAssociatedNode(baseSkel->getBone(baseHandle));
            dstTrack->setUseShortestRotationPath(srcTrack->getUseShortestRotationPath());

            // Per-bone binding-pose orientation correction.
            // AnimationProcessor stores translate/rotate keyframes in the source bone's
            // local space (divided by the source bone's binding-pose orientation).
            // If source and target have different binding poses for this bone, re-express
            // the stored values in the target bone's local space:
            //   corrected = (q_dst⁻¹ * q_src) * stored
            const Ogre::Quaternion q_src = srcSkel->getBone(srcHandle)->getOrientation();
            const Ogre::Quaternion q_dst = baseSkel->getBone(baseHandle)->getOrientation();
            const Ogre::Quaternion boneCorrection = q_src.Inverse() * q_dst;
            const bool needsBoneCorrection = !boneCorrection.equals(Ogre::Quaternion::IDENTITY, Ogre::Radian(1e-4f));

            for (unsigned short k = 0; k < srcTrack->getNumKeyFrames(); ++k)
            {
                const auto* kf = srcTrack->getNodeKeyFrame(k);
                auto* dstKf = dstTrack->createNodeKeyFrame(kf->getTime());

                Ogre::Vector3 t = kf->getTranslate();
                Ogre::Quaternion r = kf->getRotation();

                // Step 1: correct translation for binding-pose orientation mismatch.
                // Translations are stored in bone-local space (AnimationProcessor divides by
                // the source bone's binding-pose orientation). If the target bone has a
                // different binding-pose orientation, re-express the vector in that space.
                // Rotations do NOT need this correction — the target binding pose already
                // provides the equivalent compensation via the scene-node/skeleton setup.
                if (needsBoneCorrection) {
                    t = boneCorrection * t;
                }
                // Step 2: convert coordinate system (Z-up source → Y-up base).
                // R_x(-90°) maps (x,y,z) → (x, z, -y): Z-up axis becomes Y-up.
                // Rotations pass through unchanged — bone-local storage makes them
                // self-consistent once the dest binding pose is baked to Y-up.
                if (needsZupToYup) {
                    t = Ogre::Vector3(t.x, t.z, -t.y);
                }

                dstKf->setTranslate(t);
                dstKf->setRotation(r);
                dstKf->setScale(kf->getScale());
            }
        }
    }
}

void AnimationMerger::renameAnimation(Ogre::Skeleton* skel,
                                       const std::string& oldName,
                                       const std::string& newName)
{
    if (oldName == newName || !skel->hasAnimation(oldName))
        return;

    Ogre::Animation* oldAnim = skel->getAnimation(oldName);
    Ogre::Animation* newAnim = skel->createAnimation(newName, oldAnim->getLength());
    newAnim->setInterpolationMode(oldAnim->getInterpolationMode());
    newAnim->setRotationInterpolationMode(oldAnim->getRotationInterpolationMode());

    // Copy all node tracks
    for (const auto& [handle, srcTrack] : oldAnim->_getNodeTrackList())
    {
        auto* dstTrack = newAnim->createNodeTrack(handle);
        if (srcTrack->getAssociatedNode())
            dstTrack->setAssociatedNode(srcTrack->getAssociatedNode());
        dstTrack->setUseShortestRotationPath(srcTrack->getUseShortestRotationPath());

        for (unsigned short k = 0; k < srcTrack->getNumKeyFrames(); ++k)
        {
            const auto* kf = srcTrack->getNodeKeyFrame(k);
            auto* dstKf = dstTrack->createNodeKeyFrame(kf->getTime());
            dstKf->setTranslate(kf->getTranslate());
            dstKf->setRotation(kf->getRotation());
            dstKf->setScale(kf->getScale());
        }
    }

    // #854: carry the arm-space applied-angle over to the new name — the
    // widened keyframes were copied above, so the tracked angle must follow
    // or currentArmSpace() would report 0 for the renamed clip and the next
    // slider drag would compute a wrong (absolute-from-0) delta.
    migrateArmSpaceKey(skel, oldName, newName);

    skel->removeAnimation(oldName);
}

void AnimationMerger::migrateArmSpaceKey(Ogre::Skeleton* skel,
                                         const std::string& oldAnim,
                                         const std::string& newAnim)
{
    if (!skel || oldAnim == newAnim || skel->getNumBones() == 0) return;
    auto& uob = skel->getBone(0)->getUserObjectBindings();
    const Ogre::Any& a = uob.getUserAny(armSpaceKey(oldAnim));
    if (a.has_value()) {
        setStoredArmSpace(skel, newAnim, Ogre::any_cast<float>(a));
        uob.eraseUserAny(armSpaceKey(oldAnim));
    }
}

int AnimationMerger::resampleAnimation(Ogre::Skeleton* skel,
                                       const std::string& animName,
                                       int targetKeyframes)
{
    if (!skel || !skel->hasAnimation(animName) || targetKeyframes < 2)
        return 0;

    Ogre::Animation* srcAnim = skel->getAnimation(animName);
    float length = srcAnim->getLength();

    // Count original keyframes across all tracks (use max track keyframe count)
    int originalMaxKeyframes = 0;
    for (const auto& [handle, track] : srcAnim->_getNodeTrackList())
    {
        int numKf = static_cast<int>(track->getNumKeyFrames());
        if (numKf > originalMaxKeyframes)
            originalMaxKeyframes = numKf;
    }

    // Collect track data: for each track, evaluate interpolated T/R/S at N evenly-spaced times
    struct TrackData {
        unsigned short handle;
        Ogre::Node* associatedNode;
        bool useShortestPath;
        struct KeyframeData {
            float time;
            Ogre::Vector3 translate;
            Ogre::Quaternion rotation;
            Ogre::Vector3 scale;
        };
        std::vector<KeyframeData> keyframes;
    };
    std::vector<TrackData> tracks;

    for (const auto& [handle, srcTrack] : srcAnim->_getNodeTrackList())
    {
        TrackData td;
        td.handle = handle;
        td.associatedNode = srcTrack->getAssociatedNode();
        td.useShortestPath = srcTrack->getUseShortestRotationPath();

        for (int i = 0; i < targetKeyframes; ++i)
        {
            float t = (targetKeyframes > 1)
                ? (static_cast<float>(i) * length / static_cast<float>(targetKeyframes - 1))
                : 0.0f;

            Ogre::TransformKeyFrame interpKf(nullptr, t);
            srcTrack->getInterpolatedKeyFrame(t, &interpKf);

            td.keyframes.push_back({
                t,
                interpKf.getTranslate(),
                interpKf.getRotation(),
                interpKf.getScale()
            });
        }
        tracks.push_back(std::move(td));
    }

    // Save animation properties
    float animLength = srcAnim->getLength();
    auto interpMode = srcAnim->getInterpolationMode();
    auto rotInterpMode = srcAnim->getRotationInterpolationMode();

    // Remove old animation and create new one with same name
    skel->removeAnimation(animName);
    Ogre::Animation* newAnim = skel->createAnimation(animName, animLength);
    newAnim->setInterpolationMode(interpMode);
    newAnim->setRotationInterpolationMode(rotInterpMode);

    // Recreate tracks with resampled keyframes
    for (const auto& td : tracks)
    {
        auto* newTrack = newAnim->createNodeTrack(td.handle);
        if (td.associatedNode)
            newTrack->setAssociatedNode(td.associatedNode);
        newTrack->setUseShortestRotationPath(td.useShortestPath);

        for (const auto& kfData : td.keyframes)
        {
            auto* kf = newTrack->createNodeKeyFrame(kfData.time);
            kf->setTranslate(kfData.translate);
            kf->setRotation(kfData.rotation);
            kf->setScale(kfData.scale);
        }
    }

    return originalMaxKeyframes - targetKeyframes;
}

int AnimationMerger::decimateAnimation(Ogre::Skeleton* skel,
                                       const std::string& animName,
                                       int step)
{
    if (!skel || !skel->hasAnimation(animName) || step < 2)
        return 0;

    Ogre::Animation* srcAnim = skel->getAnimation(animName);

    // Collect track data: for each track, keep only keyframes at indices 0, step, 2*step, ... and the last
    struct TrackData {
        unsigned short handle;
        Ogre::Node* associatedNode;
        bool useShortestPath;
        struct KeyframeData {
            float time;
            Ogre::Vector3 translate;
            Ogre::Quaternion rotation;
            Ogre::Vector3 scale;
        };
        std::vector<KeyframeData> keyframes;
        int originalCount;
    };
    std::vector<TrackData> tracks;

    int totalRemoved = 0;

    for (const auto& [handle, srcTrack] : srcAnim->_getNodeTrackList())
    {
        TrackData td;
        td.handle = handle;
        td.associatedNode = srcTrack->getAssociatedNode();
        td.useShortestPath = srcTrack->getUseShortestRotationPath();
        td.originalCount = static_cast<int>(srcTrack->getNumKeyFrames());

        int numKf = td.originalCount;
        for (int i = 0; i < numKf; ++i)
        {
            bool keep = (i % step == 0) || (i == numKf - 1);
            if (keep)
            {
                const auto* kf = srcTrack->getNodeKeyFrame(static_cast<unsigned short>(i));
                td.keyframes.push_back({
                    kf->getTime(),
                    kf->getTranslate(),
                    kf->getRotation(),
                    kf->getScale()
                });
            }
        }

        totalRemoved += (td.originalCount - static_cast<int>(td.keyframes.size()));
        tracks.push_back(std::move(td));
    }

    // Save animation properties
    float animLength = srcAnim->getLength();
    auto interpMode = srcAnim->getInterpolationMode();
    auto rotInterpMode = srcAnim->getRotationInterpolationMode();

    // Remove old and create new
    skel->removeAnimation(animName);
    Ogre::Animation* newAnim = skel->createAnimation(animName, animLength);
    newAnim->setInterpolationMode(interpMode);
    newAnim->setRotationInterpolationMode(rotInterpMode);

    for (const auto& td : tracks)
    {
        auto* newTrack = newAnim->createNodeTrack(td.handle);
        if (td.associatedNode)
            newTrack->setAssociatedNode(td.associatedNode);
        newTrack->setUseShortestRotationPath(td.useShortestPath);

        for (const auto& kfData : td.keyframes)
        {
            auto* kf = newTrack->createNodeKeyFrame(kfData.time);
            kf->setTranslate(kfData.translate);
            kf->setRotation(kfData.rotation);
            kf->setScale(kfData.scale);
        }
    }

    return totalRemoved;
}

// ---------------------------------------------------------------------------
// Redundant-keyframe simplification (tolerance-based, preserves sharp keys).
//
// A key K_i is "redundant" when the curve evaluated at K_i.time without that
// key — i.e. lerp/slerp between K_{i-1} and K_{i+1} — matches K_i within the
// configured tolerance. Walks each track once and removes redundant keys
// iteratively (a key adjacent to a removed key may become redundant itself
// after removal). First and last keyframes are always preserved.
//
// Mixamo-style clips bake one key per frame per bone with smooth motion
// between keys, so tolerance-based removal typically eliminates 60–80% of
// keys without visible drift.
// ---------------------------------------------------------------------------

namespace {

struct SimpleKey {
    float time;
    Ogre::Vector3 translate;
    Ogre::Quaternion rotation;
    Ogre::Vector3 scale;
};

// Choose the equivalent quaternion (q or -q) that lies on the same hemisphere
// as `ref`. Quaternion antipodes represent the same rotation but slerp/dot
// behave incorrectly across the hemisphere boundary.
static Ogre::Quaternion alignedTo(const Ogre::Quaternion& q, const Ogre::Quaternion& ref)
{
    return (q.Dot(ref) < 0.0f) ? Ogre::Quaternion(-q.w, -q.x, -q.y, -q.z) : q;
}

static bool keyIsRedundant(const SimpleKey& prev,
                           const SimpleKey& cur,
                           const SimpleKey& next,
                           const AnimationMerger::SimplifyTolerances& tol)
{
    // Degenerate: zero-width interval — the middle key cannot affect playback,
    // so treat it as redundant.
    const float span = next.time - prev.time;
    if (span <= 1e-7f)
        return true;

    const float t = (cur.time - prev.time) / span;
    if (t <= 0.0f || t >= 1.0f)
        return false;

    // Translation: linear lerp.
    const Ogre::Vector3 lerpT = prev.translate + (next.translate - prev.translate) * t;
    if ((lerpT - cur.translate).length() > tol.translation)
        return false;

    // Scale: linear lerp.
    const Ogre::Vector3 lerpS = prev.scale + (next.scale - prev.scale) * t;
    if ((lerpS - cur.scale).length() > tol.scale)
        return false;

    // Rotation: slerp on hemisphere-aligned quaternions, then compare with the
    // angular-distance metric (acos|dot|, doubled to give the rotation angle).
    const Ogre::Quaternion qPrev = prev.rotation;
    const Ogre::Quaternion qNext = alignedTo(next.rotation, qPrev);
    const Ogre::Quaternion qCur  = alignedTo(cur.rotation,  qPrev);
    const Ogre::Quaternion slerpR = Ogre::Quaternion::Slerp(t, qPrev, qNext, /*shortestPath*/ true);

    float dot = std::abs(slerpR.Dot(qCur));
    if (dot > 1.0f) dot = 1.0f;
    const float angleDeg = Ogre::Math::ACos(dot).valueDegrees() * 2.0f;
    if (angleDeg > tol.rotationDeg)
        return false;

    return true;
}

// Walk a track's keys, remove redundant ones iteratively. Always preserves
// the first and last keyframe. Returns the number of keys removed.
static int simplifyTrackKeys(std::vector<SimpleKey>& keys,
                             const AnimationMerger::SimplifyTolerances& tol)
{
    if (keys.size() < 3)
        return 0;

    // `kept` holds indices into the original `keys` array of keys we plan to keep.
    // We extend it left-to-right and only commit a middle key when we're sure it
    // can't be folded out by a later neighbor. Specifically, after appending
    // index i, we look back at the previous "tentative" index j = kept[size-2]:
    // if keys[j] is redundant given (kept[size-3], i) as neighbors, we drop j.
    // This handles runs of collinear keys correctly.
    std::vector<size_t> kept;
    kept.reserve(keys.size());
    kept.push_back(0);

    for (size_t i = 1; i + 1 < keys.size(); ++i) {
        kept.push_back(i);
        // Try to fold out the previous tentative middle key while possible.
        while (kept.size() >= 3) {
            const size_t a = kept[kept.size() - 3];
            const size_t b = kept[kept.size() - 2];
            const size_t c = kept[kept.size() - 1];
            if (keyIsRedundant(keys[a], keys[b], keys[c], tol)) {
                kept.erase(kept.begin() + (kept.size() - 2));
            } else {
                break;
            }
        }
    }
    // Always keep the last keyframe; fold middle ones against it too.
    kept.push_back(keys.size() - 1);
    while (kept.size() >= 3) {
        const size_t a = kept[kept.size() - 3];
        const size_t b = kept[kept.size() - 2];
        const size_t c = kept[kept.size() - 1];
        if (keyIsRedundant(keys[a], keys[b], keys[c], tol)) {
            kept.erase(kept.begin() + (kept.size() - 2));
        } else {
            break;
        }
    }

    if (kept.size() == keys.size())
        return 0;

    std::vector<SimpleKey> reduced;
    reduced.reserve(kept.size());
    for (auto idx : kept)
        reduced.push_back(keys[idx]);

    int removed = static_cast<int>(keys.size() - reduced.size());
    keys = std::move(reduced);
    return removed;
}

} // namespace

int AnimationMerger::simplifyAnimation(Ogre::Skeleton* skel,
                                       const std::string& animName,
                                       const SimplifyTolerances& tol)
{
    if (!skel || !skel->hasAnimation(animName))
        return 0;

    Ogre::Animation* srcAnim = skel->getAnimation(animName);

    struct TrackData {
        unsigned short handle = 0;
        Ogre::Node* associatedNode = nullptr;
        bool useShortestPath = true;
        std::vector<SimpleKey> keys;
    };

    std::vector<TrackData> tracks;
    int totalRemoved = 0;

    for (const auto& [handle, srcTrack] : srcAnim->_getNodeTrackList())
    {
        TrackData td;
        td.handle = handle;
        td.associatedNode = srcTrack->getAssociatedNode();
        td.useShortestPath = srcTrack->getUseShortestRotationPath();

        unsigned short numKf = srcTrack->getNumKeyFrames();
        td.keys.reserve(numKf);
        for (unsigned short k = 0; k < numKf; ++k) {
            const auto* kf = srcTrack->getNodeKeyFrame(k);
            td.keys.push_back({kf->getTime(), kf->getTranslate(), kf->getRotation(), kf->getScale()});
        }

        totalRemoved += simplifyTrackKeys(td.keys, tol);
        tracks.push_back(std::move(td));
    }

    if (totalRemoved == 0)
        return 0;

    const float animLength    = srcAnim->getLength();
    const auto interpMode     = srcAnim->getInterpolationMode();
    const auto rotInterpMode  = srcAnim->getRotationInterpolationMode();

    skel->removeAnimation(animName);
    Ogre::Animation* newAnim = skel->createAnimation(animName, animLength);
    newAnim->setInterpolationMode(interpMode);
    newAnim->setRotationInterpolationMode(rotInterpMode);

    for (const auto& td : tracks) {
        if (td.keys.empty())
            continue;
        auto* newTrack = newAnim->createNodeTrack(td.handle);
        if (td.associatedNode)
            newTrack->setAssociatedNode(td.associatedNode);
        newTrack->setUseShortestRotationPath(td.useShortestPath);
        for (const auto& k : td.keys) {
            auto* kf = newTrack->createNodeKeyFrame(k.time);
            kf->setTranslate(k.translate);
            kf->setRotation(k.rotation);
            kf->setScale(k.scale);
        }
    }

    return totalRemoved;
}

int AnimationMerger::bakeAnimationAtFps(Ogre::Skeleton* skel,
                                         const std::string& animName,
                                         int targetFps)
{
    if (!skel || targetFps <= 0) return 0;
    if (!skel->hasAnimation(animName)) return 0;
    Ogre::Animation* anim = skel->getAnimation(animName);
    if (!anim) return 0;

    const float step = 1.0f / static_cast<float>(targetFps);
    constexpr float kEps = 1e-4f;
    int totalKeys = 0;

    for (const auto& [handle, track] : anim->_getNodeTrackList()) {
        const unsigned short numKf = track->getNumKeyFrames();
        if (numKf < 2) {
            totalKeys += numKf;
            continue;
        }

        // Snapshot every existing keyframe so we can interpolate
        // against the original curve after stripping the interior.
        std::vector<SimpleKey> snap;
        snap.reserve(numKf);
        for (unsigned short k = 0; k < numKf; ++k) {
            const auto* kf = track->getNodeKeyFrame(k);
            snap.push_back({ kf->getTime(),
                             kf->getTranslate(),
                             kf->getRotation(),
                             kf->getScale() });
        }
        const float t0 = snap.front().time;
        const float t1 = snap.back().time;
        const float duration = t1 - t0;
        if (duration <= 0.0f) {
            totalKeys += numKf;
            continue;
        }

        // Strip every keyframe (we'll re-create endpoints + interior).
        for (int k = static_cast<int>(track->getNumKeyFrames()) - 1; k >= 0; --k) {
            track->removeKeyFrame(static_cast<unsigned short>(k));
        }

        // Helper: lerp full TRS between bracketing snapshot keys.
        auto sampleAt = [&snap](float t) -> SimpleKey {
            const SimpleKey* lo = &snap.front();
            const SimpleKey* hi = &snap.back();
            for (size_t i = 0; i + 1 < snap.size(); ++i) {
                if (t >= snap[i].time - 1e-4f
                    && t <= snap[i+1].time + 1e-4f) {
                    lo = &snap[i];
                    hi = &snap[i+1];
                    break;
                }
            }
            const float gap = hi->time - lo->time;
            const float u = gap > 1e-6f
                ? std::clamp((t - lo->time) / gap, 0.0f, 1.0f) : 0.0f;
            SimpleKey out;
            out.time      = t;
            out.translate = lo->translate + (hi->translate - lo->translate) * u;
            out.rotation  = Ogre::Quaternion::Slerp(u, lo->rotation, hi->rotation, true);
            out.scale     = lo->scale + (hi->scale - lo->scale) * u;
            return out;
        };

        // Insert uniform N-FPS grid: t0, t0+step, t0+2*step, ..., t1.
        const int sampleCount = static_cast<int>(std::ceil(duration / step)) + 1;
        for (int i = 0; i < sampleCount; ++i) {
            float t = t0 + i * step;
            if (t > t1 - kEps) t = t1;
            const SimpleKey s = sampleAt(t);
            auto* kf = track->createNodeKeyFrame(t);
            kf->setTranslate(s.translate);
            kf->setRotation(s.rotation);
            kf->setScale(s.scale);
            ++totalKeys;
            if (t >= t1 - kEps) break;
        }
    }
    return totalKeys;
}

AnimationMerger::InbetweenResult AnimationMerger::inbetweenAnimation(
    Ogre::Skeleton* skel, const std::string& animName,
    float t0, float t1, int gapFrames, const QString& modelPath,
    bool forceFallback)
{
    InbetweenResult res;
    if (!skel || !skel->hasAnimation(animName)) {
        res.error = QStringLiteral("Animation not found.");
        return res;
    }
    if (gapFrames <= 0) {
        res.error = QStringLiteral("gapFrames must be >= 1.");
        return res;
    }
    // Cap gapFrames: user-provided via CLI/MCP/GUI, and gapFrames(+1) drives both
    // model/fallback allocation and per-track keyframe inserts. 1000 frames in a
    // single gap is already absurd for any real clip.
    constexpr int kMaxGapFrames = 1000;
    if (gapFrames > kMaxGapFrames) {
        res.error = QStringLiteral("gapFrames %1 exceeds the maximum of %2.")
            .arg(gapFrames).arg(kMaxGapFrames);
        return res;
    }
    // Reject non-finite times: NaN/inf would slip past `t1 <= t0` (all comparisons
    // with NaN are false) and corrupt the sampling math downstream.
    if (!std::isfinite(t0) || !std::isfinite(t1)) {
        res.error = QStringLiteral("Start/end times must be finite.");
        return res;
    }
    if (t1 <= t0) {
        res.error = QStringLiteral("End time must be greater than start time.");
        return res;
    }
    Ogre::Animation* anim = skel->getAnimation(animName);
    if (!anim) { res.error = QStringLiteral("Animation not found."); return res; }

    // One stable track order = the channel layout. Per bone we pack 10 DoF:
    // [tx,ty,tz, qx,qy,qz,qw, sx,sy,sz]. Tracks that don't bracket the window
    // are still packed (so the model sees a full pose) but we only WRITE new
    // keys to tracks that have a real bracketing segment.
    struct TrackCtx {
        Ogre::NodeAnimationTrack* track = nullptr;
        std::string boneName;            // resolved from the skeleton by handle
        bool   bracketed = false;        // has a key <= t0 and a key >= t1
        SimpleKey startKey;              // pose at/just-before t0
        SimpleKey endKey;                // pose at/just-after t1
    };

    auto evalAt = [](Ogre::NodeAnimationTrack* tr, float t) -> SimpleKey {
        Ogre::TransformKeyFrame tmp(nullptr, t);
        tr->getInterpolatedKeyFrame(Ogre::TimeIndex(t), &tmp);
        return { t, tmp.getTranslate(), tmp.getRotation(), tmp.getScale() };
    };

    std::vector<TrackCtx> ctxs;
    const auto& trackList = anim->_getNodeTrackList();
    ctxs.reserve(trackList.size());
    for (const auto& [handle, track] : trackList) {
        TrackCtx c;
        c.track = track;
        // Tracks are keyed by bone HANDLE; resolve the bone name from the
        // skeleton for canonical-role matching. (hasBone() takes a name, not a
        // handle, so guard by handle range instead.)
        if (handle < skel->getNumBones())
            c.boneName = skel->getBone(handle)->getName();
        const unsigned short numKf = track->getNumKeyFrames();
        if (numKf >= 2) {
            const float first = track->getNodeKeyFrame(0)->getTime();
            const float last  = track->getNodeKeyFrame(numKf - 1)->getTime();
            // The window must be a genuine GAP: bracketed by keys at/outside
            // [t0,t1] AND with no existing keyframe strictly inside (t0,t1).
            // Inserting into an already-keyed span would stack new keys on top
            // of authored ones — skip those tracks (they're not a gap to fill).
            bool hasInteriorKey = false;
            for (unsigned short k = 0; k < numKf; ++k) {
                const float kt = track->getNodeKeyFrame(k)->getTime();
                if (kt > t0 + 1e-4f && kt < t1 - 1e-4f) { hasInteriorKey = true; break; }
            }
            if (first <= t0 + 1e-4f && last >= t1 - 1e-4f && !hasInteriorKey) {
                c.bracketed = true;
                c.startKey  = evalAt(track, t0);
                c.endKey    = evalAt(track, t1);
            }
        }
        if (!c.bracketed) {
            // Non-bracketed: hold the evaluated pose at t0 for both ends so the
            // model still receives a coherent full-skeleton pose, but we won't
            // write keys back to this track.
            c.startKey = c.endKey = (numKf > 0) ? evalAt(track, t0)
                                                : SimpleKey{ t0, Ogre::Vector3::ZERO,
                                                             Ogre::Quaternion::IDENTITY,
                                                             Ogre::Vector3::UNIT_SCALE };
        }
        ctxs.push_back(c);
    }

    if (ctxs.empty()) {
        res.error = QStringLiteral("Animation has no node tracks.");
        return res;
    }

    auto packPose = [](const SimpleKey& k, MotionInbetween::Pose& out, size_t at) {
        out[at+0] = k.translate.x; out[at+1] = k.translate.y; out[at+2] = k.translate.z;
        out[at+3] = k.rotation.x;  out[at+4] = k.rotation.y;
        out[at+5] = k.rotation.z;  out[at+6] = k.rotation.w;
        out[at+7] = k.scale.x;     out[at+8] = k.scale.y;     out[at+9] = k.scale.z;
    };
    auto writeKey = [&](Ogre::NodeAnimationTrack* track, float t,
                        const MotionInbetween::Pose& pose, size_t base) {
        Ogre::TransformKeyFrame* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3(pose[base+0], pose[base+1], pose[base+2]));
        Ogre::Quaternion q(pose[base+6], pose[base+3], pose[base+4], pose[base+5]); // (w,x,y,z)
        q.normalise();
        kf->setRotation(q);
        kf->setScale(Ogre::Vector3(pose[base+7], pose[base+8], pose[base+9]));
    };

    const float span = t1 - t0;
    auto interiorT = [&](int f) {
        return t0 + span * static_cast<float>(f + 1) / static_cast<float>(gapFrames + 1);
    };

    // --- Try the RMIB model via canonical-skeleton retargeting --------------
    // The model is fixed to canonicalJointCount() joints. Map each track to a
    // canonical index; if a model is present, enough roles resolve, and we're
    // not forcing fallback, pack the canonical [2, C220] pose, run predict()
    // (which fires the model since C matches), and scatter the prediction back
    // onto each matched bracketed track. Unmatched / non-bracketed tracks are
    // handled by the per-track spline pass below.
    const int CJ = MotionInbetween::canonicalJointCount();
    const size_t modelC = static_cast<size_t>(CJ) * 10;
    std::vector<int> canonToTrack(CJ, -1);          // canonical idx -> ctx idx
    int rolesResolved = 0;
    if (!forceFallback && !modelPath.isEmpty()) {
        for (size_t bi = 0; bi < ctxs.size(); ++bi) {
            const int ci = MotionInbetween::canonicalIndexForBone(
                QString::fromStdString(ctxs[bi].boneName));
            if (ci >= 0 && canonToTrack[ci] < 0) { canonToTrack[ci] = static_cast<int>(bi); ++rolesResolved; }
        }
    }
    // Require a strong majority of roles before trusting the model (a partial
    // skeleton would feed garbage to a fixed-layout net). ~75% of 22 = 17.
    const bool useModel = !forceFallback && !modelPath.isEmpty()
                          && rolesResolved >= (CJ * 3) / 4;

    std::vector<bool> handledByModel(ctxs.size(), false);
    if (useModel) {
        MotionInbetween::Pose startPose(modelC, 0.0f), endPose(modelC, 0.0f);
        std::vector<MotionInbetween::Channel> mlayout(modelC, MotionInbetween::Channel::Scalar);
        for (int cj = 0; cj < CJ; ++cj) {
            const size_t at = static_cast<size_t>(cj) * 10;
            mlayout[at+3] = MotionInbetween::Channel::QuatStart;
            mlayout[at+4] = MotionInbetween::Channel::QuatCont;
            mlayout[at+5] = MotionInbetween::Channel::QuatCont;
            mlayout[at+6] = MotionInbetween::Channel::QuatCont;
            const int bi = canonToTrack[cj];
            if (bi >= 0) { packPose(ctxs[bi].startKey, startPose, at);
                           packPose(ctxs[bi].endKey,   endPose,   at); }
            else { startPose[at+6] = endPose[at+6] = 1.0f;        // identity quat
                   startPose[at+7] = startPose[at+8] = startPose[at+9] = 1.0f;
                   endPose[at+7] = endPose[at+8] = endPose[at+9] = 1.0f; }
        }
        MotionInbetween::Options opts; opts.gapFrames = gapFrames;
        const MotionInbetween::Result mr =
            MotionInbetween::predict(startPose, endPose, mlayout, modelPath, opts);
        if (mr.ok && mr.usedModel) {
            res.usedModel = true;
            for (int cj = 0; cj < CJ; ++cj) {
                const int bi = canonToTrack[cj];
                if (bi < 0 || !ctxs[bi].bracketed) continue;
                for (int f = 0; f < gapFrames; ++f) {
                    writeKey(ctxs[bi].track, interiorT(f), mr.frames[f],
                             static_cast<size_t>(cj) * 10);
                    ++res.keyframesInserted;
                }
                ++res.tracksAffected;
                handledByModel[bi] = true;
            }
        } else {
            res.fallbackReason = mr.fallbackReason;   // model declined → spline below
        }
    }

    // --- Per-track spline pass (every bracketed track the model didn't do) ---
    for (size_t bi = 0; bi < ctxs.size(); ++bi) {
        if (!ctxs[bi].bracketed || handledByModel[bi]) continue;
        // Single-bone spline: layout is the 10-DoF for one bone.
        MotionInbetween::Pose s(10), e(10);
        packPose(ctxs[bi].startKey, s, 0);
        packPose(ctxs[bi].endKey,   e, 0);
        std::vector<MotionInbetween::Channel> oneLayout = {
            MotionInbetween::Channel::Scalar, MotionInbetween::Channel::Scalar,
            MotionInbetween::Channel::Scalar, MotionInbetween::Channel::QuatStart,
            MotionInbetween::Channel::QuatCont, MotionInbetween::Channel::QuatCont,
            MotionInbetween::Channel::QuatCont, MotionInbetween::Channel::Scalar,
            MotionInbetween::Channel::Scalar, MotionInbetween::Channel::Scalar };
        MotionInbetween::Options o; o.gapFrames = gapFrames; o.forceFallback = true;
        const auto sr = MotionInbetween::interpolateSpline(s, e, oneLayout, o);
        if (!sr.ok) continue;
        for (int f = 0; f < gapFrames; ++f) {
            writeKey(ctxs[bi].track, interiorT(f), sr.frames[f], 0);
            ++res.keyframesInserted;
        }
        ++res.tracksAffected;
        if (!res.usedModel && res.fallbackReason.isEmpty())
            res.fallbackReason = QStringLiteral("Used the spline fallback.");
    }

    if (res.tracksAffected == 0) {
        res.error = QStringLiteral(
            "No track had a keyframe pair bracketing [%1, %2] — nothing to fill.")
            .arg(t0).arg(t1);
        return res;
    }
    res.ok = true;
    return res;
}

std::vector<AnimationMerger::CanonicalClip>
AnimationMerger::extractCanonicalClips(Ogre::Entity* entity, int fps,
                                       const QString& onlyAnimation, bool v2)
{
    std::vector<CanonicalClip> out;
    if (!entity || !entity->hasSkeleton() || fps <= 0)
        return out;
    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    if (!skel)
        return out;

    // Body-role sampling always uses the 22-joint layout (all the spine/limb/
    // facing logic depends on it). For V2 we ADDITIONALLY sample the finger
    // bones and append them as joints 22..51 at finalize.
    const int J = MotionInbetween::canonicalJointCount();
    const int JV2 = MotionInbetween::canonicalJointCountV2();   // 52

    // Bone → canonical role, first match wins per role (the same matcher the
    // retarget uses, so round-trips are consistent by construction).
    std::vector<Ogre::Bone*> roleBone(static_cast<size_t>(J), nullptr);
    int resolved = 0;
    for (auto* bone : skel->getBones()) {
        const int role = MotionInbetween::canonicalIndexForBone(
            QString::fromStdString(bone->getName()));
        if (role >= 0 && role < J && !roleBone[static_cast<size_t>(role)]) {
            roleBone[static_cast<size_t>(role)] = bone;
            ++resolved;
        }
    }
    if (resolved == 0)
        return out;

    // #838 finger bones → finger slots (parallel to the body roles). Keep only
    // the first kFingerSegs (3) segments per finger; a rig with 2-segment
    // fingers (Biped) fills slots 0..1, a 3-segment rig (Mixamo) fills 0..2.
    std::vector<Ogre::Bone*> fingerBone(
        static_cast<size_t>(kFingerSlots), nullptr);
    int fingerResolved = 0;
    for (auto* bone : skel->getBones()) {
        const auto fr = MotionInbetween::fingerRoleForBone(
            QString::fromStdString(bone->getName()));
        if (!fr.valid()) continue;
        const int slot = fingerSlot(fr.side, fr.finger, fr.segment);
        if (slot >= 0 && !fingerBone[static_cast<size_t>(slot)]) {
            fingerBone[static_cast<size_t>(slot)] = bone;
            ++fingerResolved;
        }
    }

    // ── NAME-vs-ANATOMY chirality check (#951) ─────────────────────────
    // Some rigs ship with MIRRORED bone naming (Blender FBX −X-scale
    // exports: bones named "Left*" sit on the anatomical RIGHT — the
    // Quaternius "Animated Human" is the canonical case). The extraction
    // trusts names for the L/R roles AND derives its facing from the
    // named hip line (left = rhip→lhip), so a mirrored rig yields clips
    // canonically yawed 180°: invisible on self-retarget (rigid whole-clip
    // yaw) but cross-rig every punch goes backward-overhead and arm swings
    // mirror (user-reported). Detect chirality NAME-INDEPENDENTLY from the
    // TOES: toes point forward, so for correct naming the signed volume
    // det[named-left, up, toe-forward] = hipLine·(up×toe) is POSITIVE
    // (left×up=fwd convention ⇒ up×fwd=left). The toe is found
    // STRUCTURALLY (the foot bone's child with the largest horizontal
    // offset — no names), and the test is averaged over sampled frames of
    // every animation so a single odd pose can't flip it. (A knee-hinge
    // test was tried first and REVERTED: thigh turnout — dance! — rotates
    // the flexion axis, flipping the sign even on correctly-named rigs.)
    // Negative ⇒ names are mirrored ⇒ swap every L/R role and finger side,
    // making names match anatomy for everything downstream.
    {
        double chirality = 0.0;
        Ogre::Bone* feet[2] = {roleBone[17], roleBone[21]};
        Ogre::Bone* hipsLR[2] = {roleBone[15], roleBone[19]};
        Ogre::Bone* hipB = roleBone[0];
        Ogre::Bone* headB = roleBone[5] ? roleBone[5] : roleBone[3];
        if (hipsLR[0] && hipsLR[1] && hipB && headB
            && (feet[0] || feet[1])) {
            auto accumulate = [&]() {
                Ogre::Vector3 up = headB->_getDerivedPosition()
                                   - hipB->_getDerivedPosition();
                Ogre::Vector3 hipLine = hipsLR[1]->_getDerivedPosition()
                                        - hipsLR[0]->_getDerivedPosition();
                if (up.squaredLength() < 1e-12f
                    || hipLine.squaredLength() < 1e-12f)
                    return;
                up.normalise();
                hipLine.normalise();
                for (Ogre::Bone* foot : feet) {
                    if (!foot || foot->numChildren() == 0) continue;
                    // structural toe: the child with the largest offset
                    // component perpendicular to up
                    Ogre::Vector3 best = Ogre::Vector3::ZERO;
                    float bestH = 0.0f;
                    for (unsigned short ci = 0; ci < foot->numChildren();
                         ++ci) {
                        auto* ch = static_cast<Ogre::Bone*>(
                            foot->getChild(ci));
                        Ogre::Vector3 off = ch->_getDerivedPosition()
                                            - foot->_getDerivedPosition();
                        const Ogre::Vector3 h =
                            off - up * off.dotProduct(up);
                        if (h.squaredLength() > bestH) {
                            bestH = h.squaredLength();
                            best = h;
                        }
                    }
                    if (bestH < 1e-12f) continue;
                    best.normalise();
                    chirality += up.crossProduct(best).dotProduct(hipLine);
                }
            };
            for (unsigned short a = 0; a < skel->getNumAnimations(); ++a) {
                Ogre::Animation* anim = skel->getAnimation(a);
                if (!anim || anim->getLength() <= 0.0f) continue;
                const int nS = 8;
                for (int f = 0; f < nS; ++f) {
                    skel->reset(true);
                    anim->apply(skel, anim->getLength()
                                          * static_cast<float>(f)
                                          / static_cast<float>(nS - 1));
                    skel->_updateTransforms();
                    accumulate();
                }
            }
            skel->reset(true);
            skel->_updateTransforms();
        }
        // MEASUREMENT RESULT (2026-08-20): every tested rig (Mixamo Rumba,
        // Quaternius lowpoly, Gregorio) measures NEGATIVE — the Ogre import
        // presents ALL humanoids with the same mirrored named-left/anatomy
        // relationship, so naming is CONSISTENT across rigs and cancels in
        // cross-rig retargets. A one-sided swap here would therefore BREAK
        // retargets (source mirrored vs target) — keep this as a
        // diagnostic-only check that flags a rig whose chirality DIFFERS
        // from the fleet norm (positive = odd one out).
        if (qEnvironmentVariableIsSet("QTMESH_EXTRACT_DEBUG"))
            fprintf(stderr,
                    "[extract] toe chirality %.3f (fleet norm: negative; "
                    "positive would mean this rig mirrors differently)\n",
                    chirality);
    }

    // ── source-frame → canonical-frame conjugation ─────────────────────
    // Scraped rigs live in arbitrary file frames (Blender FBX armatures are
    // commonly Z-up), while the motion library's world convention is Y-up,
    // +Z-facing, +X-left. Guessing per format is fragile — derive the
    // source frame from the rig's own geometry instead: up = hip→head,
    // left = right-hip→left-hip (shoulders as fallback), forward = left×up.
    // Every sampled world quat is then conjugated: q' = C · q · C⁻¹.
    //
    // CRITICAL: the frame, reference orientations AND bone directions are
    // all measured at an ANIMATED calm frame of the clip — never at the
    // bind/reset pose. On many scraped rigs (Quaternius, Sketchfab glTF)
    // the animation worlds differ from the reset pose by a constant global
    // rotation (an armature-node ±90° X that Assimp bakes into one but not
    // the other), so mixing bind-measured references with animated quats
    // tips the retargeted body over. Measuring everything at one animated
    // frame makes the (restWorld, restDir, quats) triple consistent by
    // construction — the global offset cancels exactly.
    const auto deriveFrame = [&]() -> Ogre::Quaternion {
        // Reads the skeleton's CURRENT (applied) pose.
        Ogre::Quaternion frameC = Ogre::Quaternion::IDENTITY;
        auto posOf = [&](int role) -> const Ogre::Bone* {
            return (role >= 0 && role < J)
                       ? roleBone[static_cast<size_t>(role)] : nullptr;
        };
        const Ogre::Bone* hip  = posOf(0);   // "hip"
        const Ogre::Bone* head = posOf(5);   // "head"
        if (!head) head = posOf(3);          // "neck" fallback
        const Ogre::Bone* lSide = posOf(19); // "lhip"
        const Ogre::Bone* rSide = posOf(15); // "rhip"
        if (!lSide || !rSide) { lSide = posOf(11); rSide = posOf(7); }
        if (hip && head && lSide && rSide) {
            Ogre::Vector3 up = head->_getDerivedPosition()
                               - hip->_getDerivedPosition();
            Ogre::Vector3 left = lSide->_getDerivedPosition()
                                 - rSide->_getDerivedPosition();
            if (up.squaredLength() > 1e-12f
                && left.squaredLength() > 1e-12f) {
                up.normalise();
                left = left - up * left.dotProduct(up);   // orthogonalise
                if (left.squaredLength() > 1e-12f) {
                    left.normalise();
                    const Ogre::Vector3 fwd = left.crossProduct(up);
                    // Rotation taking the SOURCE basis (left, up, fwd) onto
                    // the canonical axes (+X, +Y, +Z).
                    Ogre::Matrix3 src;
                    src.SetColumn(0, left);
                    src.SetColumn(1, up);
                    src.SetColumn(2, fwd);
                    frameC = Ogre::Quaternion(src).Inverse();
                    frameC.normalise();
                }
            }
        }
        return frameC;
    };

    // Sample by applying each Animation DIRECTLY to the skeleton instance —
    // deterministic regardless of the entity's animation-state bookkeeping
    // (state-set application proved instance-dependent for hand-built
    // skeletons), and it leaves the entity's enabled states untouched.
    for (unsigned short a = 0; a < skel->getNumAnimations(); ++a) {
        Ogre::Animation* anim = skel->getAnimation(a);
        if (!anim) continue;
        const QString name = QString::fromStdString(anim->getName());
        if (!onlyAnimation.isEmpty()
            && name.compare(onlyAnimation, Qt::CaseInsensitive) != 0)
            continue;
        const float length = anim->getLength();
        if (length <= 0.0f)
            continue;

        CanonicalClip clip;
        clip.animation = name;
        clip.resolvedRoles = resolved;
        const int frames =
            std::max(2, static_cast<int>(std::lround(length * fps)) + 1);

        // Pass 1: sample RAW world orientations per frame per role, plus the
        // hip and a foot world POSITION per frame (for #838 vertical descent).
        std::vector<std::vector<Ogre::Quaternion>> raw(
            static_cast<size_t>(frames),
            std::vector<Ogre::Quaternion>(static_cast<size_t>(J)));
        std::vector<Ogre::Vector3> hipPos(static_cast<size_t>(frames),
                                          Ogre::Vector3::ZERO);
        std::vector<Ogre::Vector3> footPos(static_cast<size_t>(frames),
                                           Ogre::Vector3::ZERO);
        Ogre::Bone* hipBone = roleBone[0];                 // "hip"
        Ogre::Bone* footBone = roleBone[17];               // "rfoot"
        if (!footBone) footBone = roleBone[21];            // "lfoot" fallback
        // Bind-pose (T-pose STANDING) hip + foot positions — the absolute
        // reference for crouch depth. Using the clip's own max hip-height as
        // "standing" fails for always-low actions (a crawl never stands, so
        // relative-to-max reads ~0); the bind pose is the true upright height.
        Ogre::Vector3 bindHip = Ogre::Vector3::ZERO, bindFoot = Ogre::Vector3::ZERO;
        {
            skel->reset(true);
            skel->_updateTransforms();
            if (hipBone)  bindHip  = hipBone->_getDerivedPosition();
            if (footBone) bindFoot = footBone->_getDerivedPosition();
        }
        // Finger transfer uses DIRECTIONS (body-retarget style), NOT rotations:
        // the Biped finger bones have large, wildly-varying per-segment bind
        // axes (measured 300-350° about arbitrary axes), so copying local/world
        // rotation mangles them on a differently-axised Mixamo hand. But a
        // finger segment's POINTING direction (toward its child/tip) is
        // rig-independent — aim the target segment there each frame, like the
        // body limbs. Per frame we store each segment's canonical-frame world
        // pointing direction as [dx,dy,dz,0] (w=0 marks a direction, not a quat).
        // The child bone per slot (next segment along the finger), for the
        // pointing direction (segment → child position).
        std::vector<Ogre::Bone*> fingerChild(
            static_cast<size_t>(kFingerSlots), nullptr);
        for (int s = 0; s < kFingerSlots; ++s) {
            Ogre::Bone* b = fingerBone[static_cast<size_t>(s)];
            if (!b) continue;
            // child = the same finger's next segment bone, if resolved
            const int side = (s / (5 * kFingerSegs));
            const int rem = s - side * 5 * kFingerSegs;
            const int fgr = rem / kFingerSegs, seg = rem % kFingerSegs;
            if (seg + 1 < kFingerSegs) {
                const int cslot = fingerSlot(side, fgr, seg + 1);
                if (cslot >= 0) fingerChild[static_cast<size_t>(s)] =
                    fingerBone[static_cast<size_t>(cslot)];
            }
            // Fallback: the LAST resolved segment of a finger (e.g. Biped
            // Finger01, whose only child is a skipped "Finger0Nub") has no
            // finger-ROLE child, so its pointing direction — the actual
            // knuckle-bend curl signal — would be dropped. Use the bone's real
            // skeleton child (the nub/tip) instead. Without this, open vs
            // closed hands are indistinguishable (all curl lives past seg0).
            if (!fingerChild[static_cast<size_t>(s)]
                && b->numChildren() > 0) {
                if (auto* tip =
                        dynamic_cast<Ogre::Bone*>(b->getChild(0)))
                    fingerChild[static_cast<size_t>(s)] = tip;
            }
        }
        std::vector<std::vector<std::array<float, 4>>> fingerCurl;
        if (fingerResolved > 0)
            fingerCurl.assign(static_cast<size_t>(frames),
                std::vector<std::array<float, 4>>(
                    static_cast<size_t>(kFingerSlots), {0.f, 0.f, 0.f, 0.f}));
        // V2: raw world orientation per finger slot per frame (canonicalised at
        // finalize), so fingers become ordinary joints 22..51. Identity where a
        // slot has no bone.
        std::vector<std::vector<Ogre::Quaternion>> fingerWorld;
        if (v2 && fingerResolved > 0)
            fingerWorld.assign(static_cast<size_t>(frames),
                std::vector<Ogre::Quaternion>(static_cast<size_t>(kFingerSlots),
                                              Ogre::Quaternion::IDENTITY));

        for (int f = 0; f < frames; ++f) {
            skel->reset(true);
            anim->apply(skel, std::min(length,
                static_cast<float>(f) / static_cast<float>(fps)));
            skel->_updateTransforms();
            for (int j = 0; j < J; ++j)
                if (Ogre::Bone* b = roleBone[static_cast<size_t>(j)])
                    raw[static_cast<size_t>(f)][static_cast<size_t>(j)] =
                        b->_getDerivedOrientation();
            if (hipBone)
                hipPos[static_cast<size_t>(f)] = hipBone->_getDerivedPosition();
            if (footBone)
                footPos[static_cast<size_t>(f)] = footBone->_getDerivedPosition();
            // Finger pointing direction (segment → child), canonical frame.
            if (fingerResolved > 0) {
                for (int s = 0; s < kFingerSlots; ++s) {
                    Ogre::Bone* b = fingerBone[static_cast<size_t>(s)];
                    // V2: capture the finger bone's raw world orientation (even
                    // for the last segment that has no child direction) so it
                    // rides the joint retarget.
                    if (v2 && b)
                        fingerWorld[static_cast<size_t>(f)]
                                   [static_cast<size_t>(s)] =
                            b->_getDerivedOrientation();
                    Ogre::Bone* c = fingerChild[static_cast<size_t>(s)];
                    if (!b || !c) continue;
                    Ogre::Vector3 dir = c->_getDerivedPosition()
                                        - b->_getDerivedPosition();
                    if (dir.squaredLength() < 1e-12f) continue;
                    dir.normalise();
                    // stored in canonical frame at finalize (via C); raw world
                    // here.
                    fingerCurl[static_cast<size_t>(f)][static_cast<size_t>(s)] =
                        {static_cast<float>(dir.x), static_cast<float>(dir.y),
                         static_cast<float>(dir.z), 0.0f};
                }
            }
        }

        // Calm reference frame f*: minimum mean joint rotation speed —
        // typically a near-neutral lead-in/contact pose, the most reliable
        // place to read body geometry (the same heuristic the standing-pose
        // harvest and the library builder's window snap use).
        int fStar = 0;
        double bestE = std::numeric_limits<double>::max();
        for (int f = 1; f < frames; ++f) {
            double e = 0.0;
            for (int j = 0; j < J; ++j) {
                if (!roleBone[static_cast<size_t>(j)]) continue;
                const Ogre::Quaternion d =
                    raw[static_cast<size_t>(f - 1)][static_cast<size_t>(j)]
                        .Inverse()
                    * raw[static_cast<size_t>(f)][static_cast<size_t>(j)];
                e += 2.0 * std::acos(std::min(1.0,
                    std::abs(static_cast<double>(d.w))));
            }
            if (e < bestE) { bestE = e; fStar = f - 1; }
        }

        // Reference pose for C + the triple (restWorld/restDir). Normally the
        // calm frame f*, but if f*'s torso is NOT upright (a kneel/crouch clip
        // may have no upright frame — Gregório BuildLoop's calm frame leans
        // ~60°), the leveling frame C bakes that tilt in and the whole retarget
        // reclines. Detect it (f* torso-up vs bind torso-up) and, when f*
        // leans, reference the BIND pose instead so C is level AND the triple
        // is sampled in that same upright pose — keeping them consistent with
        // the per-frame quats (which are C·raw·Cinv either way).
        auto torsoUpNow = [&]() -> Ogre::Vector3 {
            const Ogre::Bone* hip = roleBone[0];
            const Ogre::Bone* head = roleBone[5] ? roleBone[5] : roleBone[3];
            if (!hip || !head) return Ogre::Vector3::ZERO;
            Ogre::Vector3 u = head->_getDerivedPosition()
                              - hip->_getDerivedPosition();
            return (u.squaredLength() > 1e-9f) ? u.normalisedCopy()
                                               : Ogre::Vector3::ZERO;
        };
        skel->reset(true); skel->_updateTransforms();
        const Ogre::Vector3 bindUp = torsoUpNow();
        skel->reset(true);
        anim->apply(skel, std::min(length,
            static_cast<float>(fStar) / static_cast<float>(fps)));
        skel->_updateTransforms();
        const Ogre::Vector3 starUp = torsoUpNow();
        // Reference-pose selection for C + the triple (restWorld/restDir). The
        // per-frame quats are C·raw(f)·Cinv, so C MUST level the same frame the
        // quats live in. Two cases the old bind-fallback got wrong:
        //  (a) crouch/kneel clips whose calm frame f* is bent — bind IS a valid
        //      upright reference AND is in the animation's frame, so bind works.
        //  (b) rigs whose BIND and ANIMATION are in DIFFERENT frames (Blender FBX
        //      with a −90°X armature node baked into the anim but not the bind,
        //      e.g. Quaternius Woman; game rips like the Half-Life zombie).
        //      Referencing bind levels a frame the quats aren't in → the whole
        //      body tips onto its side (X/Z axis).
        // Distinguish them: level a sample ANIMATED frame with the BIND-derived
        // C; if the result is NOT upright, bind and animation disagree (case b)
        // → reference the most-upright ANIMATED frame instead so C+quats share a
        // frame. Otherwise keep the old behaviour (f*, or bind for bent f*).
        const bool fStarBent =
            bindUp.squaredLength() > 1e-9f && starUp.squaredLength() > 1e-9f
            && bindUp.dotProduct(starUp) < 0.94f;   // f* torso >~20° off bind
        int refFrame = fStarBent ? -1 : fStar;   // -1 sentinel = bind
        {
            // provisional bind-derived C
            skel->reset(true); skel->_updateTransforms();
            const Ogre::Quaternion Cbind = deriveFrame();
            // level a sample animated frame (f*) and check torso-up is vertical
            skel->reset(true);
            anim->apply(skel, std::min(length,
                static_cast<float>(fStar) / static_cast<float>(fps)));
            skel->_updateTransforms();
            const Ogre::Vector3 u = torsoUpNow();
            const Ogre::Vector3 leveled = (u.squaredLength() > 1e-9f)
                ? (Cbind * u.normalisedCopy()) : Ogre::Vector3::ZERO;
            const bool animUpright =
                leveled.squaredLength() > 1e-9f && leveled.y > 0.82f;  // ~35°
            if (!animUpright) {
                // case (b): bind ≠ animation frame. Reference the most-upright
                // ANIMATED frame (torso-up closest to the bind torso-up).
                float best = -2.f; int bestF = fStar;
                for (int f = 0; f < frames; ++f) {
                    skel->reset(true);
                    anim->apply(skel, std::min(length,
                        static_cast<float>(f) / static_cast<float>(fps)));
                    skel->_updateTransforms();
                    const Ogre::Vector3 uf = torsoUpNow();
                    if (uf.squaredLength() < 1e-9f) continue;
                    const float d = bindUp.dotProduct(uf.normalisedCopy());
                    if (d > best) { best = d; bestF = f; }
                }
                refFrame = bestF;
            }
        }
        skel->reset(true);
        if (refFrame >= 0)
            anim->apply(skel, std::min(length,
                static_cast<float>(refFrame) / static_cast<float>(fps)));
        skel->_updateTransforms();
        Ogre::Quaternion C = deriveFrame();
        if (qEnvironmentVariableIsSet("QTMESH_EXTRACT_DEBUG")) {
            // bind-derived frame for comparison
            const Ogre::Quaternion Cref = C;
            skel->reset(true); skel->_updateTransforms();
            const Ogre::Quaternion Cbind2 = deriveFrame();
            const Ogre::Quaternion dq = Cref * Cbind2.Inverse();
            const float mismatch = 2.0f * std::acos(std::min(1.0f,
                std::abs(dq.w))) * 180.0f / float(M_PI);
            fprintf(stderr, "[extract] %s: refFrame=%d fStar=%d fStarBent=%d "
                    "C-vs-bindC mismatch=%.1fdeg\n",
                    anim->getName().c_str(), refFrame, fStar,
                    fStarBent ? 1 : 0, mismatch);
            // restore the reference pose
            skel->reset(true);
            if (refFrame >= 0)
                anim->apply(skel, std::min(length,
                    static_cast<float>(refFrame) / static_cast<float>(fps)));
            skel->_updateTransforms();
        }
        // CLIP-AVERAGED UP-LEVELING. A single reference frame (f*/bind/most-
        // upright) can still be BENT — a jump/pickup/sit clip on a rig whose
        // bind≠animation frame has NO upright frame, so C bakes that frame's
        // lean and the whole clip tips onto its side (the Quaternius Woman
        // jump/pickup/sit "on the Z axis"). Fix: after the provisional C, level
        // it against the clip-AVERAGED torso-up. Averaging over all frames
        // cancels transient bend (a jump rises+falls, a crouch dips+rises → the
        // mean torso-up is the true standing up), giving a stable upright frame
        // regardless of any single frame. A genuinely one-sided clip (always
        // bent, e.g. a pure crawl) still averages to its dominant up, which is
        // the right reference for it. Skip when the average is degenerate.
        {
            const Ogre::Bone* hipB  = roleBone[0];
            const Ogre::Bone* headB = roleBone[5] ? roleBone[5] : roleBone[3];
            if (hipB && headB) {
                // Soft-weighted torso-up (head-hip) over all frames, favouring
                // the upright ones (u.y^3): lying frames drop out, so one-sided
                // clips (death/fall) aren't dragged horizontal and oscillating
                // clips (jump/crouch) still average smoothly. Levels the body's
                // UP-tilt (the X/Z-axis lying bug). NOTE: this is position-based
                // (hip→head), so it cannot see a pelvis PITCH where the spine
                // stays vertical but the hip bone rotates (Quaternius Woman
                // residual back-lean) — that lives in the hip bone ORIENTATION,
                // which the retarget composes via restWorld; a safe fix for it
                // is out of reach without regressing legit-pitching crouches.
                Ogre::Vector3 meanUp = Ogre::Vector3::ZERO;
                for (int f = 0; f < frames; ++f) {
                    skel->reset(true);
                    anim->apply(skel, std::min(length,
                        static_cast<float>(f) / static_cast<float>(fps)));
                    skel->_updateTransforms();
                    Ogre::Vector3 u = headB->_getDerivedPosition()
                                      - hipB->_getDerivedPosition();
                    if (u.squaredLength() < 1e-9f) continue;
                    u = (C * u).normalisedCopy();
                    const float wy = std::max(0.0f, u.y);
                    meanUp += u * (wy * wy * wy);
                }
                if (meanUp.squaredLength() > 1e-6f) {
                    meanUp.normalise();
                    C = meanUp.getRotationTo(Ogre::Vector3::UNIT_Y) * C;
                }
            }
        }
        if (qEnvironmentVariableIsSet("QTMESH_EXTRACT_DEBUG")) {
            // RAW world anatomy at a few frames — no canonical conjugation.
            for (int f : {0, frames / 3, frames / 2, (2 * frames) / 3}) {
                skel->reset(true);
                anim->apply(skel, std::min(length,
                    static_cast<float>(f) / static_cast<float>(fps)));
                skel->_updateTransforms();
                auto pos = [&](int role) -> Ogre::Vector3 {
                    Ogre::Bone* b = roleBone[static_cast<size_t>(role)];
                    return b ? b->_getDerivedPosition() : Ogre::Vector3::ZERO;
                };
                const Ogre::Vector3 relbow = pos(8), rhand = pos(9);
                const Ogre::Vector3 lhip = pos(19), rhip = pos(15);
                const Ogre::Vector3 hip = pos(0), head = pos(5);
                Ogre::Vector3 fore = (rhand - relbow).normalisedCopy();
                Ogre::Vector3 hl = (lhip - rhip).normalisedCopy();
                Ogre::Vector3 up = (head - hip).normalisedCopy();
                const Ogre::Vector3 fwd = hl.crossProduct(up).normalisedCopy();
                fprintf(stderr,
                        "[extract-raw] %s f=%d foreW=(%5.2f %5.2f %5.2f) "
                        "hipL=(%5.2f %5.2f %5.2f) up=(%5.2f %5.2f %5.2f) "
                        "fore-dot-fwd=%5.2f fore-dot-up=%5.2f\n",
                        anim->getName().c_str(), f,
                        fore.x, fore.y, fore.z, hl.x, hl.y, hl.z,
                        up.x, up.y, up.z,
                        fore.dotProduct(fwd), fore.dotProduct(up));
            }
        }
        // re-pose at the reference frame for the restWorld/restDir sampling below
        skel->reset(true);
        if (refFrame >= 0)
            anim->apply(skel, std::min(length,
                static_cast<float>(refFrame) / static_cast<float>(fps)));
        skel->_updateTransforms();
        const Ogre::Quaternion Cinv = C.Inverse();
        std::vector<std::array<float, 4>> restWorld(
            static_cast<size_t>(J), {0.f, 0.f, 0.f, 1.f});
        std::vector<std::array<float, 3>> restDir(
            static_cast<size_t>(J), {0.f, 0.f, 0.f});
        for (int j = 0; j < J; ++j) {
            Ogre::Bone* b = roleBone[static_cast<size_t>(j)];
            if (!b) continue;
            const Ogre::Quaternion w =
                C * b->_getDerivedOrientation() * Cinv;
            restWorld[static_cast<size_t>(j)] = {
                static_cast<float>(w.x), static_cast<float>(w.y),
                static_cast<float>(w.z), static_cast<float>(w.w)};
        }
        const auto dirBetween = [&](int a, int bIdx,
                                    std::array<float, 3>& outDir) {
            const Ogre::Bone* ba = roleBone[static_cast<size_t>(a)];
            const Ogre::Bone* bb = roleBone[static_cast<size_t>(bIdx)];
            if (!ba || !bb) return false;
            Ogre::Vector3 v = bb->_getDerivedPosition()
                              - ba->_getDerivedPosition();
            if (v.squaredLength() < 1e-12f) return false;
            v = C * v;
            v.normalise();
            outDir = {static_cast<float>(v.x), static_cast<float>(v.y),
                      static_cast<float>(v.z)};
            return true;
        };
        for (int j = 0; j < J; ++j) {
            const int child = MotionInbetween::canonicalChildOf(j);
            if (child >= 0
                && dirBetween(j, child, restDir[static_cast<size_t>(j)]))
                continue;
            const int parent = MotionInbetween::canonicalParentOf(j);
            if (parent >= 0)
                dirBetween(parent, j, restDir[static_cast<size_t>(j)]);
        }

        // Spine-chain sanity (#838): the spine (hip→abdomen→chest→neck→neck1→
        // head, roles 0..5) is monotonically UPWARD on any humanoid. Some rigs
        // (e.g. the Samurai dance clip) stack a spine joint BELOW its parent in
        // bind pose — its bone direction points DOWN the hip→head axis. That
        // makes the source bone's own frame degenerate/inverted, so the aim-
        // based retarget folds the chest UNDER the hip and snaps the model in
        // half. We can't trust that bone's motion, so ZERO its restDir: the
        // retarget skips it (`squaredLength() <= 1e-8` guard) and the target
        // bone HOLDS its upright bind pose while the rest of the clip plays.
        {
            const Ogre::Bone* hipB  = roleBone[0];
            const Ogre::Bone* headB = roleBone[5];
            if (hipB && headB) {
                Ogre::Vector3 up = C * (headB->_getDerivedPosition()
                                        - hipB->_getDerivedPosition());
                if (up.squaredLength() > 1e-9f) {
                    up.normalise();
                    for (int j = 1; j <= 4; ++j) {   // abdomen..neck1
                        auto& d = restDir[static_cast<size_t>(j)];
                        const Ogre::Vector3 v(d[0], d[1], d[2]);
                        if (v.squaredLength() > 1e-9f
                            && v.dotProduct(up) < -0.2f) {   // clearly downward
                            d = {0.f, 0.f, 0.f};             // drop this bone
                        }
                    }
                }
            }
        }

        // Pass 2: conjugate the stored raw worlds into the canonical frame.
        clip.quats.reserve(static_cast<size_t>(frames));
        for (int f = 0; f < frames; ++f) {
            std::vector<std::array<float, 4>> pose(
                static_cast<size_t>(J), {0.f, 0.f, 0.f, 1.f});
            for (int j = 0; j < J; ++j) {
                if (!roleBone[static_cast<size_t>(j)]) continue;
                const Ogre::Quaternion w =
                    C * raw[static_cast<size_t>(f)][static_cast<size_t>(j)]
                    * Cinv;
                pose[static_cast<size_t>(j)] = {
                    static_cast<float>(w.x), static_cast<float>(w.y),
                    static_cast<float>(w.z), static_cast<float>(w.w)};
            }
            clip.quats.push_back(std::move(pose));
        }
        clip.frames = static_cast<int>(clip.quats.size());
        clip.restWorld = std::move(restWorld);
        clip.restDir = std::move(restDir);

        // #838 finger REST pointing direction per slot, at the reference pose
        // (the skeleton is still posed there — restWorld/restDir were just read
        // from it). The retarget transports the RELATIVE bend restDir→frameDir,
        // so a rig's finger rest convention no longer over-bends the target.
        if (fingerResolved > 0) {
            clip.fingerRestDir.assign(static_cast<size_t>(kFingerSlots),
                                      {0.f, 0.f, 0.f});
            for (int s = 0; s < kFingerSlots; ++s) {
                Ogre::Bone* b = fingerBone[static_cast<size_t>(s)];
                Ogre::Bone* c = fingerChild[static_cast<size_t>(s)];
                if (!b || !c) continue;
                Ogre::Vector3 dir = c->_getDerivedPosition()
                                    - b->_getDerivedPosition();
                if (dir.squaredLength() < 1e-12f) continue;
                dir = (C * dir).normalisedCopy();
                clip.fingerRestDir[static_cast<size_t>(s)] = {
                    static_cast<float>(dir.x), static_cast<float>(dir.y),
                    static_cast<float>(dir.z)};
            }
        }

        // ---- V2: fold the finger bones in as canonical joints 22..51 --------
        // Body joints 0..21 are already in quats/restWorld/restDir (22-wide).
        // Grow each to 52 and fill the finger joints from the sampled finger
        // world orientations (canonicalised via C, like the body) + the finger
        // bind world (reference pose, still active) + the rest dir just built.
        // V2 index for finger slot s == 22 + s (fingerSlot order matches
        // fingerJointIndexV2). Missing slots stay identity/zero and the retarget
        // skips them, exactly like an unresolved body role.
        // ALWAYS pad to 52 in V2 mode — even a FINGERLESS rig (low-poly toon /
        // chibi with no finger bones) must emit 52-wide clips or the v4 loader's
        // schema guard rejects the whole library. Absent finger joints stay
        // identity/zero and the retarget skips them (like an unresolved role).
        if (v2) {
            const bool haveFingers = fingerResolved > 0 && !fingerWorld.empty();
            // per-frame quats → 52 wide
            for (int f = 0; f < frames; ++f) {
                auto& pose = clip.quats[static_cast<size_t>(f)];
                pose.resize(static_cast<size_t>(JV2), {0.f, 0.f, 0.f, 1.f});
                if (!haveFingers) continue;
                for (int s = 0; s < kFingerSlots; ++s) {
                    if (!fingerBone[static_cast<size_t>(s)]) continue;
                    const Ogre::Quaternion w =
                        C * fingerWorld[static_cast<size_t>(f)]
                                       [static_cast<size_t>(s)] * Cinv;
                    pose[static_cast<size_t>(J + s)] = {
                        static_cast<float>(w.x), static_cast<float>(w.y),
                        static_cast<float>(w.z), static_cast<float>(w.w)};
                }
            }
            // restWorld → 52 (finger bind world, canonicalised). Sample now: the
            // reference pose is still applied (restWorld/restDir read from it).
            if (static_cast<int>(clip.restWorld.size()) == J) {
                clip.restWorld.resize(static_cast<size_t>(JV2),
                                      {0.f, 0.f, 0.f, 1.f});
                if (haveFingers)
                    for (int s = 0; s < kFingerSlots; ++s) {
                        Ogre::Bone* b = fingerBone[static_cast<size_t>(s)];
                        if (!b) continue;
                        const Ogre::Quaternion w =
                            C * b->_getDerivedOrientation() * Cinv;
                        clip.restWorld[static_cast<size_t>(J + s)] = {
                            static_cast<float>(w.x), static_cast<float>(w.y),
                            static_cast<float>(w.z), static_cast<float>(w.w)};
                    }
            }
            // restDir → 52 (reuse the finger rest dirs already computed)
            if (static_cast<int>(clip.restDir.size()) == J) {
                clip.restDir.resize(static_cast<size_t>(JV2), {0.f, 0.f, 0.f});
                if (haveFingers
                    && static_cast<int>(clip.fingerRestDir.size())
                           == kFingerSlots)
                    for (int s = 0; s < kFingerSlots; ++s)
                        clip.restDir[static_cast<size_t>(J + s)] =
                            clip.fingerRestDir[static_cast<size_t>(s)];
            }

            // FINGER REPLICATION. Many simple rigs (Quaternius Woman, etc.) have
            // ONE non-thumb finger bone standing in for all four — only the
            // thumb + "index" slots get data; middle/ring/pinky stay flat, so a
            // fist looks like it's missing three fingers. Copy the first
            // POPULATED non-thumb finger (per side) onto any empty non-thumb
            // finger, per segment: quats (all frames) + restWorld + restDir. The
            // thumb is never a donor/recipient (it moves independently).
            if (haveFingers) {
                auto slotHasData = [&](int side, int fgr, int seg) {
                    const int j = J + fingerSlot(side, fgr, seg);
                    for (int f = 0; f < frames; ++f) {
                        const auto& p = clip.quats[static_cast<size_t>(f)]
                                                  [static_cast<size_t>(j)];
                        if (std::abs(p[0]) > 1e-3f || std::abs(p[1]) > 1e-3f
                            || std::abs(p[2]) > 1e-3f) return true;
                    }
                    return false;
                };
                for (int side = 0; side < 2; ++side) {
                    // donor = first non-thumb finger (1..4) that carries data
                    int donor = -1;
                    for (int fgr = 1; fgr < 5 && donor < 0; ++fgr)
                        for (int seg = 0; seg < kFingerSegs; ++seg)
                            if (slotHasData(side, fgr, seg)) { donor = fgr; break; }
                    if (donor < 0) continue;   // no non-thumb finger data
                    for (int fgr = 1; fgr < 5; ++fgr) {
                        if (fgr == donor) continue;
                        // only replicate onto a finger that is ENTIRELY empty
                        bool anyData = false;
                        for (int seg = 0; seg < kFingerSegs && !anyData; ++seg)
                            anyData = slotHasData(side, fgr, seg);
                        if (anyData) continue;
                        for (int seg = 0; seg < kFingerSegs; ++seg) {
                            const int src = J + fingerSlot(side, donor, seg);
                            const int dst = J + fingerSlot(side, fgr, seg);
                            for (int f = 0; f < frames; ++f)
                                clip.quats[static_cast<size_t>(f)]
                                          [static_cast<size_t>(dst)] =
                                    clip.quats[static_cast<size_t>(f)]
                                              [static_cast<size_t>(src)];
                            if (static_cast<int>(clip.restWorld.size()) == JV2)
                                clip.restWorld[static_cast<size_t>(dst)] =
                                    clip.restWorld[static_cast<size_t>(src)];
                            if (static_cast<int>(clip.restDir.size()) == JV2)
                                clip.restDir[static_cast<size_t>(dst)] =
                                    clip.restDir[static_cast<size_t>(src)];
                        }
                    }
                }
            }
            clip.jointCountV2 = true;   // mark this clip as 52-joint (schema v4)
        }

        // #838 vertical root descent: how far the hip sinks toward the feet as
        // a CROUCH measure. Sign-safe by construction — we measure the hip's
        // height ABOVE the foot along the canonical +Y (always ≥ 0), take the
        // standing height as the MAX across the clip, and report each frame's
        // DROP below that as a NEGATIVE rootY in leg-lengths. This avoids the
        // per-rig sign chaos of raw hip-Y translation (some Quaternius rigs
        // read the hip RISING during pickup/sit) — a genuine crouch always
        // lowers the hip toward the planted foot regardless of rig axes.
        if (hipBone && footBone && frames > 0) {
            // Standing reference = the BIND-pose hip height above the foot
            // (canonical +Y). Absolute, so an always-low crawl still reads a
            // real drop — but fall back to the clip's own max if the bind pose
            // is degenerate (hip ≈ foot height, e.g. a T-pose that isn't
            // upright), so we never divide the descent away.
            const Ogre::Vector3 bh = C * bindHip;
            const Ogre::Vector3 bf = C * bindFoot;
            float standH = bh.y - bf.y;
            const float legLen = (bindHip - bindFoot).length();
            std::vector<float> hipAboveFoot(static_cast<size_t>(frames));
            float clipMaxH = 0.0f;
            for (int f = 0; f < frames; ++f) {
                const Ogre::Vector3 hp = C * hipPos[static_cast<size_t>(f)];
                const Ogre::Vector3 fp = C * footPos[static_cast<size_t>(f)];
                hipAboveFoot[static_cast<size_t>(f)] = hp.y - fp.y;
                clipMaxH = std::max(clipMaxH, hp.y - fp.y);
            }
            // If the clip ever stands TALLER than the bind pose (bind wasn't a
            // clean upright T-pose), anchor to the clip max so drops stay ≤ 0.
            standH = std::max(standH, clipMaxH);
            if (legLen > 1e-3f && standH > 1e-3f) {
                clip.rootY.reserve(static_cast<size_t>(frames));
                for (int f = 0; f < frames; ++f) {
                    const float drop =
                        (hipAboveFoot[static_cast<size_t>(f)] - standH) / legLen;
                    clip.rootY.push_back(std::clamp(drop, -1.0f, 0.0f));
                }
            }
        }
        if (fingerResolved > 0) {
            // Rotate each finger pointing DIRECTION into the canonical frame C
            // (w==0 → it's a direction vector; leave zero/unfilled slots alone).
            for (auto& frame : fingerCurl)
                for (auto& q : frame) {
                    if (q[0] == 0.f && q[1] == 0.f && q[2] == 0.f) continue;
                    const Ogre::Vector3 v = C * Ogre::Vector3(q[0], q[1], q[2]);
                    q = {static_cast<float>(v.x), static_cast<float>(v.y),
                         static_cast<float>(v.z), 0.0f};
                }
            clip.fingers = std::move(fingerCurl);
        }
        out.push_back(std::move(clip));
    }

    // Restore the bind pose so the on-screen entity isn't left mid-clip.
    skel->reset(true);
    skel->_updateTransforms();
    return out;
}

void AnimationMerger::debugDumpAnatomy(Ogre::Skeleton* skel,
                                       const std::string& animName,
                                       const char* tag)
{
    if (!skel || !skel->hasAnimation(animName)) return;
    Ogre::Animation* anim = skel->getAnimation(animName);
    const float len = anim->getLength();
    if (len <= 0.0f) return;
    Ogre::Bone* role[22] = {nullptr};
    for (auto* b : skel->getBones()) {
        const int c = MotionInbetween::canonicalIndexForBone(
            QString::fromStdString(b->getName()));
        if (c >= 0 && c < 22 && !role[c]) role[c] = b;
    }
    if (!role[8] || !role[9] || !role[15] || !role[19] || !role[0]
        || !(role[5] || role[3]))
        return;
    Ogre::Bone* headB = role[5] ? role[5] : role[3];
    for (int k = 0; k < 4; ++k) {
        skel->reset(true);
        anim->apply(skel, len * (0.05f + 0.3f * k));
        skel->_updateTransforms();
        const Ogre::Vector3 fore =
            (role[9]->_getDerivedPosition() - role[8]->_getDerivedPosition())
                .normalisedCopy();
        const Ogre::Vector3 hl =
            (role[19]->_getDerivedPosition()
             - role[15]->_getDerivedPosition()).normalisedCopy();
        const Ogre::Vector3 up =
            (headB->_getDerivedPosition() - role[0]->_getDerivedPosition())
                .normalisedCopy();
        const Ogre::Vector3 fwd = hl.crossProduct(up).normalisedCopy();
        fprintf(stderr,
                "[anat:%s] t=%.2f foreW=(%5.2f %5.2f %5.2f) "
                "fore-dot-fwd=%5.2f fore-dot-up=%5.2f\n",
                tag, len * (0.05f + 0.3f * k), fore.x, fore.y, fore.z,
                fore.dotProduct(fwd), fore.dotProduct(up));
    }
    skel->reset(true);
    skel->_updateTransforms();
}

bool AnimationMerger::detectBackwardFacing(Ogre::Entity* entity)
{
    // Escape hatch for exotic meshes where the foot-region heuristic guesses
    // wrong: QTMESH_T2M_YAW180=1 forces the flip, =0 disables it.
    const QByteArray force = qgetenv("QTMESH_T2M_YAW180");
    if (!force.isEmpty()) return force != "0";
    if (!entity || !entity->hasSkeleton()) return false;
    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    if (!skel) return false;

    // Ankle reference: bones resolving to the canonical foot roles (17/21).
    Ogre::Vector3 ankleSum = Ogre::Vector3::ZERO;
    int ankles = 0;
    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        Ogre::Bone* b = skel->getBone(i);
        const int c = MotionInbetween::canonicalIndexForBone(
            QString::fromStdString(b->getName()));
        if (c == 17 || c == 21) { ankleSum += b->_getDerivedPosition(); ++ankles; }
    }
    if (ankles == 0) return false;
    const Ogre::Vector3 ankle = ankleSum / static_cast<float>(ankles);

    std::vector<float> verts;
    std::vector<uint32_t> indices;
    if (!AutoRig::gatherGeometry(entity, verts, indices) || verts.size() < 9)
        return false;
    float minY = verts[1], maxY = verts[1];
    for (size_t v = 1; v < verts.size() / 3; ++v) {
        minY = std::min(minY, verts[3*v + 1]);
        maxY = std::max(maxY, verts[3*v + 1]);
    }
    const float h = maxY - minY;
    if (h < 1e-5f) return false;

    // Foot region: vertices below (a little above) ankle height. Toes extend
    // FORWARD of the ankle, so the region's Z centroid tells the facing.
    const float band = ankle.y + 0.06f * h;
    double zSum = 0.0; int n = 0;
    for (size_t v = 0; v < verts.size() / 3; ++v) {
        if (verts[3*v + 1] <= band) { zSum += verts[3*v + 2]; ++n; }
    }
    if (n < 16) return false;
    const float dz = static_cast<float>(zSum / n) - ankle.z;
    // conservative margin — skirts/long coats centre the low region near 0
    return dz < -0.02f * h;
}

namespace {
// Target-side bind data for the canonical retarget paths: per-bone bind
// worlds/locals/positions, parent indices, a parents-before-children
// traversal order, the first bone per canonical role, the target's
// raw-frame → canonical conjugation Ct (same derivation as the extractor:
// up = hip→head, left = rhip→lhip from bind positions), and the target's
// bind bone direction per role (canonical topology: role → child, leaf
// roles take the incoming direction).
struct TargetBindFrame {
    std::vector<Ogre::Quaternion> bindWorld, bindLocal;
    std::vector<Ogre::Vector3> bindPos;
    std::vector<int> parentIdx, roleBoneIdx, order;
    Ogre::Quaternion Ct = Ogre::Quaternion::IDENTITY;
    std::vector<Ogre::Vector3> tgtBindDir;
};

TargetBindFrame readTargetBindFrame(Ogre::Skeleton* skel,
                                    const std::vector<int>& boneToCanon,
                                    bool v2 = false)
{
    const int nBones = static_cast<int>(skel->getNumBones());
    // V2 (52 joints, fingers folded in) uses the V2 topology functions; V1 (22)
    // is unchanged. Body roles 0..21 are identical in both, so the facing
    // derivation below (roles 0/5/7/11/15/19) needs no branch.
    const int Jc = v2 ? MotionInbetween::canonicalJointCountV2()
                      : MotionInbetween::canonicalJointCount();
    auto canonChild = [v2](int i) {
        return v2 ? MotionInbetween::canonicalChildOfV2(i)
                  : MotionInbetween::canonicalChildOf(i);
    };
    auto canonParent = [v2](int i) {
        return v2 ? MotionInbetween::canonicalParentOfV2(i)
                  : MotionInbetween::canonicalParentOf(i);
    };
    TargetBindFrame tb;
    tb.bindWorld.resize(static_cast<size_t>(nBones));
    tb.bindLocal.resize(static_cast<size_t>(nBones));
    tb.bindPos.resize(static_cast<size_t>(nBones));
    tb.parentIdx.assign(static_cast<size_t>(nBones), -1);
    tb.roleBoneIdx.assign(static_cast<size_t>(Jc), -1);
    tb.tgtBindDir.assign(static_cast<size_t>(Jc), Ogre::Vector3::ZERO);
    skel->reset(true);
    skel->_updateTransforms();
    for (int i = 0; i < nBones; ++i) {
        Ogre::Bone* b = skel->getBone(static_cast<unsigned short>(i));
        tb.bindWorld[static_cast<size_t>(i)] = b->_getDerivedOrientation();
        tb.bindLocal[static_cast<size_t>(i)] = b->getOrientation();
        tb.bindPos[static_cast<size_t>(i)] = b->_getDerivedPosition();
        if (auto* p = dynamic_cast<Ogre::Bone*>(b->getParent()))
            tb.parentIdx[static_cast<size_t>(i)] = p->getHandle();
    }
    for (int i = 0; i < nBones; ++i) {
        const int c = boneToCanon[static_cast<size_t>(i)];
        if (c >= 0 && c < Jc && tb.roleBoneIdx[static_cast<size_t>(c)] < 0)
            tb.roleBoneIdx[static_cast<size_t>(c)] = i;
    }
    {
        auto rolePos = [&](int role) -> const Ogre::Vector3* {
            const int i = (role >= 0 && role < Jc)
                ? tb.roleBoneIdx[static_cast<size_t>(role)] : -1;
            return i >= 0 ? &tb.bindPos[static_cast<size_t>(i)] : nullptr;
        };
        const Ogre::Vector3* hip = rolePos(0);
        const Ogre::Vector3* head = rolePos(5);
        if (!head) head = rolePos(3);
        const Ogre::Vector3* lSide = rolePos(19);
        const Ogre::Vector3* rSide = rolePos(15);
        if (!lSide || !rSide) { lSide = rolePos(11); rSide = rolePos(7); }
        if (hip && head && lSide && rSide) {
            Ogre::Vector3 up = *head - *hip;
            Ogre::Vector3 left = *lSide - *rSide;
            if (up.squaredLength() > 1e-12f
                && left.squaredLength() > 1e-12f) {
                up.normalise();
                left = left - up * left.dotProduct(up);
                if (left.squaredLength() > 1e-12f) {
                    left.normalise();
                    const Ogre::Vector3 fwd = left.crossProduct(up);
                    Ogre::Matrix3 src;
                    src.SetColumn(0, left);
                    src.SetColumn(1, up);
                    src.SetColumn(2, fwd);
                    tb.Ct = Ogre::Quaternion(src).Inverse();
                    tb.Ct.normalise();
                }
            }
        }
    }
    auto targetDir = [&](int role) -> Ogre::Vector3 {
        const int child = canonChild(role);
        const int parent = canonParent(role);
        const int a = tb.roleBoneIdx[static_cast<size_t>(role)];
        if (a >= 0 && child >= 0) {
            const int bIdx = tb.roleBoneIdx[static_cast<size_t>(child)];
            if (bIdx >= 0) {
                Ogre::Vector3 v = tb.bindPos[static_cast<size_t>(bIdx)]
                                  - tb.bindPos[static_cast<size_t>(a)];
                if (v.squaredLength() > 1e-12f) return v;
            }
        }
        if (a >= 0 && parent >= 0) {
            const int pIdx = tb.roleBoneIdx[static_cast<size_t>(parent)];
            if (pIdx >= 0) {
                Ogre::Vector3 v = tb.bindPos[static_cast<size_t>(a)]
                                  - tb.bindPos[static_cast<size_t>(pIdx)];
                if (v.squaredLength() > 1e-12f) return v;
            }
        }
        return Ogre::Vector3::ZERO;
    };
    for (int c = 0; c < Jc; ++c) {
        Ogre::Vector3 v = targetDir(c);
        if (v.squaredLength() > 1e-12f) {
            v.normalise();
            tb.tgtBindDir[static_cast<size_t>(c)] = v;
        }
    }
    // Parents-before-children traversal (bone indices may not be ordered).
    tb.order.reserve(static_cast<size_t>(nBones));
    std::vector<char> placed(static_cast<size_t>(nBones), 0);
    bool progress = true;
    while (static_cast<int>(tb.order.size()) < nBones && progress) {
        progress = false;
        for (int i = 0; i < nBones; ++i) {
            if (placed[static_cast<size_t>(i)]) continue;
            const int pi = tb.parentIdx[static_cast<size_t>(i)];
            if (pi < 0 || placed[static_cast<size_t>(pi)]) {
                tb.order.push_back(i);
                placed[static_cast<size_t>(i)] = 1;
                progress = true;
            }
        }
    }
    for (int i = 0; i < nBones; ++i)   // cycles/orphans: append
        if (!placed[static_cast<size_t>(i)]) tb.order.push_back(i);
    return tb;
}

// HANDEDNESS COMPENSATION (shared by applyMotionClip + BodyRetargeter). Bone
// names are anatomically correct, but a mesh may face opposite CMU — then the
// rig's "Left" bones sit on −X while canonical left joints expect +X. Detect
// from world-X of a left vs right bone pair and swap canonical L/R indices.
void compensateCanonicalHandedness(Ogre::Skeleton* skel,
                                   std::vector<int>& boneToCanon)
{
    if (!skel) return;
    skel->reset(true);
    skel->_updateTransforms();
    const int nBones = static_cast<int>(boneToCanon.size());
    auto worldXForCanon = [&](int canon) -> double {
        for (int i = 0; i < nBones; ++i)
            if (boneToCanon[static_cast<size_t>(i)] == canon)
                return skel->getBone(static_cast<unsigned short>(i))
                    ->_getDerivedPosition().x;
        return 0.0;
    };
    double lx = worldXForCanon(11), rx = worldXForCanon(7);   // arms
    if (std::abs(lx - rx) < 1e-4) {
        lx = worldXForCanon(19);
        rx = worldXForCanon(15);  // legs
    }
    if (lx < rx - 1e-5) {
        static const int kLR[][2] = {{6, 10},  {7, 11},  {8, 12},  {9, 13},
                                     {14, 18}, {15, 19}, {16, 20}, {17, 21}};
        auto swapCanon = [&](int& c) {
            for (auto& p : kLR) {
                if (c == p[0]) {
                    c = p[1];
                    return;
                }
                if (c == p[1]) {
                    c = p[0];
                    return;
                }
            }
        };
        for (int i = 0; i < nBones; ++i)
            if (boneToCanon[static_cast<size_t>(i)] >= 0)
                swapCanon(boneToCanon[static_cast<size_t>(i)]);
    }
}

} // namespace

namespace {
// Same parent indices applyMotionClip uses for world-frame PoseIK quats.
constexpr int kParentCanon[22] = {
    -1, 0, 1, 2, 3, 4,   // hip, abdomen, chest, neck, neck1, head
     2, 6, 7, 8,          // rcollar, rshoulder, relbow, rhand
     2, 10, 11, 12,       // lcollar, lshoulder, lelbow, lhand
     0, 14, 15, 16,       // rbuttock, rhip, rknee, rfoot
     0, 18, 19, 20 };     // lbuttock, lhip, lknee, lfoot

// PoseIK leaves collar/buttock at identity (unresolved) — walk up to chest/hip
// for parent-relative articulation, matching MocapPoseDebugOverlay / PoseIK.
int effectiveParentRole(int role, uint32_t resolvedMask)
{
    int p = kParentCanon[role];
    while (p >= 0 && !(resolvedMask & (1u << static_cast<unsigned>(p))))
        p = kParentCanon[p];
    return p;
}
}  // namespace

// ── BodyRetargeter — the applyMotionClip direction-match, per single frame ──
struct BodyRetargeter::Impl {
    TargetBindFrame tb;
    std::vector<int> boneToCanon;   // per bone
    int nBones = 0;
    int Jc = 0;
    // Harvested STANDING pose per bone (the calmest frame of the rig's existing
    // authored animation) — the base local rotation each frame's articulation
    // delta is composed onto. This is what makes arms sit at the chest instead
    // of splayed at the T-pose bind. Mirrors applyMotionClip's legacy transport.
    std::vector<Ogre::Quaternion> standLocal;     // per bone
    std::vector<bool> haveStand;                  // per bone
    std::vector<Ogre::Quaternion> Mc, McInv;      // per bone (roll correction)
    std::vector<int> canonDup;                    // bones sharing a canonical role
    bool restsAreIdentity = true;
    bool haveAnyStand = false;
    // neutral (first-frame) canonical WORLD quats — reference for PoseIK alignment.
    std::array<std::array<float, 4>, 22> neutral{};
    uint32_t neutralResolvedMask = 0;
    bool haveNeutral = false;
    bool neutralHadTorso = false;
    bool yaw180 = false;
#ifdef ENABLE_MOCAP
    // Landmark direction retarget (live mocap): neutral ref dirs in skeleton
    // world + per-bone Qbase (bind aligned to neutral), same as applyMotionClip.
    std::vector<Ogre::Vector3> neutralDref;       // per canonical role
    std::vector<Ogre::Quaternion> dirQbase;       // per bone
    bool haveNeutralDir = false;
#endif
};

#ifdef ENABLE_MOCAP
namespace {
void collectCanonicalLiveDirections(
    const float* world33, const float* visibility33, int Jc,
    std::vector<Ogre::Vector3>& outCanonDir)
{
    std::array<std::array<float, 3>, PoseIK::kLandmarkCount> canon{};
    PoseIK::Solver::canonicalizeMediaPipeWorld(world33, canon);
    outCanonDir.assign(static_cast<size_t>(Jc), Ogre::Vector3::ZERO);
    for (int c = 0; c < Jc; ++c) {
        std::array<float, 3> dir{};
        const float minVis =
            (c >= PoseIK::RHip && c <= PoseIK::LFoot) ? 0.2f : 0.3f;
        if (PoseIK::Solver::canonicalLiveDirection(
                c, canon, visibility33, minVis, dir)) {
            Ogre::Vector3 v(dir[0], dir[1], dir[2]);
            if (v.squaredLength() > 1e-12f) {
                v.normalise();
                outCanonDir[static_cast<size_t>(c)] = v;
            }
        }
    }
}

bool legLandmarksReliable(int role, const float* visibility33)
{
    if (!visibility33)
        return false;
    auto vis = [&](int lm) { return visibility33[lm] >= 0.2f; };
    switch (role) {
    case PoseIK::RHip:
        return vis(24) && vis(26);
    case PoseIK::RKnee:
        return vis(26) && vis(28);
    case PoseIK::RFoot:
        return vis(28);
    case PoseIK::LHip:
        return vis(23) && vis(25);
    case PoseIK::LKnee:
        return vis(25) && vis(27);
    case PoseIK::LFoot:
        return vis(27);
    default:
        return false;
    }
}

void collectFingerDirsFromPoseLandmarks(
    const float* world33, const float* visibility33,
    std::array<std::array<float, 3>, AnimationMerger::kFingerSlots>& out,
    const float* screenCrop33x3,
    const std::array<Ogre::Quaternion, 2>* wristCanonQuats,
    const std::array<std::array<float, 4>, PoseIK::kCanonicalRoles>* handQuats,
    uint32_t handResolvedMask)
{
    AnimationMerger::collectFingerDirsFromPoseLandmarks(
        world33, visibility33, out, screenCrop33x3, wristCanonQuats, handQuats,
        handResolvedMask);
}
}  // namespace
#endif

#ifdef ENABLE_MOCAP
void AnimationMerger::collectFingerDirsFromPoseLandmarks(
    const float* world33, const float* visibility33,
    std::array<std::array<float, 3>, kFingerSlots>& out,
    const float* screenCrop33x3,
    const std::array<Ogre::Quaternion, 2>* wristCanonQuats,
    const std::array<std::array<float, 4>, PoseIK::kCanonicalRoles>* handQuats,
    uint32_t handResolvedMask)
{
    out.fill({0.f, 0.f, 0.f});
    if (!world33)
        return;
    std::array<std::array<float, 3>, PoseIK::kLandmarkCount> canon{};
    PoseIK::Solver::canonicalizeMediaPipeWorld(world33, canon);
    auto visible = [&](int lm) {
        return !visibility33 || visibility33[lm] >= 0.2f;
    };
    auto wristQuatForSide = [&](int side, int handRole) -> Ogre::Quaternion {
        if (wristCanonQuats
            && (handResolvedMask & (1u << static_cast<unsigned>(handRole)))
            != 0u)
            return (*wristCanonQuats)[static_cast<size_t>(side)];
        if (handQuats
            && (handResolvedMask & (1u << static_cast<unsigned>(handRole)))
            != 0u) {
            const auto& q = (*handQuats)[static_cast<size_t>(handRole)];
            return Ogre::Quaternion(q[3], q[0], q[1], q[2]);
        }
        return Ogre::Quaternion::IDENTITY;
    };
    struct FingerLm {
        int side;
        int finger;
        int handRole;
        int wrist;
        int tip;
    };
    static const FingerLm map[] = {
        {0, 0, PoseIK::RHand, 16, 22}, {0, 1, PoseIK::RHand, 16, 20},
        {0, 4, PoseIK::RHand, 16, 18},
        {1, 0, PoseIK::LHand, 15, 21}, {1, 1, PoseIK::LHand, 15, 19},
        {1, 4, PoseIK::LHand, 15, 17},
    };
    for (const FingerLm& m : map) {
        if (!visible(m.wrist) || !visible(m.tip))
            continue;
        MocapPoseIkFk::Vec3 dir{};
        const bool handOk =
            screenCrop33x3
            && (handResolvedMask & (1u << static_cast<unsigned>(m.handRole)))
            != 0u;
        if (handOk) {
            const Ogre::Quaternion wristQ =
                wristQuatForSide(m.side, m.handRole);
            if (wristQ == Ogre::Quaternion::IDENTITY && !wristCanonQuats
                && !handQuats)
                continue;
            dir = MocapPoseIkFk::fingerDirFromScreenCrop(
                screenCrop33x3, m.wrist, m.tip, wristQ);
        } else {
            const auto& w = canon[static_cast<size_t>(m.wrist)];
            const auto& t = canon[static_cast<size_t>(m.tip)];
            dir = MocapPoseIkFk::Vec3{t[0] - w[0], t[1] - w[1], t[2] - w[2]};
            const float l = MocapPoseIkFk::len(dir);
            if (l < 1e-8f)
                continue;
            dir = MocapPoseIkFk::mul(dir, 1.f / l);
        }
        if (dir[0] == 0.f && dir[1] == 0.f && dir[2] == 0.f)
            continue;
        const int slot = fingerSlot(m.side, m.finger, 0);
        if (slot >= 0)
            out[static_cast<size_t>(slot)] = {dir[0], dir[1], dir[2]};
    }
    // Middle/ring have no pose-model landmarks — leave unset so they stay at
    // bind rather than inheriting a bogus index+pinky blend (breaks spread
    // gestures like Vulcan salute where index/pinky diverge).
}
#endif

BodyRetargeter::BodyRetargeter(Ogre::Skeleton* skel, bool yaw180)
{
    if (!skel) return;
    d = std::make_shared<Impl>();
    d->yaw180 = yaw180;
    d->nBones = static_cast<int>(skel->getNumBones());
    d->Jc = MotionInbetween::canonicalJointCount();
    d->boneToCanon.assign(static_cast<size_t>(d->nBones), -1);
    for (int i = 0; i < d->nBones; ++i)
        d->boneToCanon[static_cast<size_t>(i)] =
            MotionInbetween::canonicalIndexForBone(QString::fromStdString(
                skel->getBone(static_cast<unsigned short>(i))->getName()));
    // Pose-ik mocap: anatomical name→role only (NO CMU handedness swap — that
    // swap is for BVH/library clips and would mirror live limb motion).
    d->tb = readTargetBindFrame(skel, d->boneToCanon);

    d->canonDup.assign(static_cast<size_t>(d->Jc), 0);
    for (int i = 0; i < d->nBones; ++i)
        if (d->boneToCanon[static_cast<size_t>(i)] >= 0)
            ++d->canonDup[static_cast<size_t>(d->boneToCanon[static_cast<size_t>(i)])];

    // ── Harvest the STANDING pose (calmest frame of the rig's first authored,
    //    non-generated animation) — the same pose applyMotionClip's legacy
    //    transport composes onto. Without it the live arms sit at the T-pose
    //    bind and look splayed. ──
    d->standLocal.assign(static_cast<size_t>(d->nBones), Ogre::Quaternion::IDENTITY);
    d->haveStand.assign(static_cast<size_t>(d->nBones), false);
    Ogre::Animation* ref = nullptr;
    for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
        Ogre::Animation* a = skel->getAnimation(ai);
        if (a && a->getName().rfind("generated_", 0) != 0) { ref = a; break; }
    }
    if (ref) {
        const float len = ref->getLength();
        const int samples = std::clamp(
            static_cast<int>(std::lround(len * 30.0f)) + 1, 2, 301);
        const auto& tracks = ref->_getNodeTrackList();
        std::vector<double> energy(static_cast<size_t>(samples), 0.0);
        for (const auto& [h, trk] : tracks) {
            if (!trk || trk->getNumKeyFrames() == 0) continue;
            Ogre::Quaternion prevQ;
            for (int f = 0; f < samples; ++f) {
                const float t = len * static_cast<float>(f)
                                / static_cast<float>(samples - 1);
                Ogre::TransformKeyFrame kf(nullptr, 0.0f);
                trk->getInterpolatedKeyFrame(ref->_getTimeIndex(t), &kf);
                const Ogre::Quaternion q = kf.getRotation();
                if (f > 0) {
                    const double dd = std::min(1.0, std::abs(
                        static_cast<double>(q.Dot(prevQ))));
                    energy[static_cast<size_t>(f)] += 2.0 * std::acos(dd);
                }
                prevQ = q;
            }
        }
        int calm = 1;
        for (int f = 2; f < samples; ++f)
            if (energy[static_cast<size_t>(f)] < energy[static_cast<size_t>(calm)])
                calm = f;
        const float tCalm = len * static_cast<float>(calm)
                            / static_cast<float>(samples - 1);
        for (const auto& [h, trk] : tracks) {
            if (!trk || trk->getNumKeyFrames() == 0) continue;
            if (h >= static_cast<unsigned short>(d->nBones)) continue;
            Ogre::TransformKeyFrame f0(nullptr, 0.0f);
            trk->getInterpolatedKeyFrame(ref->_getTimeIndex(tCalm), &f0);
            // A NodeAnimationTrack keyframe rotation is RELATIVE — Ogre applies
            // it as node->rotate(kf) ON TOP of the bone's reset/bind local
            // (applyToNode accumulates, it does NOT replace). evaluateFrame
            // returns an ABSOLUTE local for Bone::setOrientation() (which
            // replaces), so bake the bind in here: absoluteLocal = bind · kf.
            // Without this, non-identity-rest bones drive wrong and the figure
            // inverts under the live setOrientation path (baked-clip playback
            // hid it by re-accumulating the reset pose).
            d->standLocal[h] = d->tb.bindLocal[static_cast<size_t>(h)]
                               * f0.getRotation();
            d->haveStand[h] = true;
            d->haveAnyStand = true;
        }
    }
    // Rig bone rests identity? (common FBX humanoids → Mc heuristic valid;
    // UniRig/template no → skip Mc, use raw delta on the standing pose.)
    for (int i = 0; i < d->nBones && d->restsAreIdentity; ++i) {
        Ogre::Bone* b = skel->getBone(static_cast<unsigned short>(i));
        if (!b->getInitialOrientation().equals(Ogre::Quaternion::IDENTITY,
                                               Ogre::Radian(0.02f)))
            d->restsAreIdentity = false;
    }
    d->Mc.assign(static_cast<size_t>(d->nBones), Ogre::Quaternion::IDENTITY);
    d->McInv.assign(static_cast<size_t>(d->nBones), Ogre::Quaternion::IDENTITY);

    // valid only if at least half the canonical roles map (the humanoid gate)
    int mapped = 0;
    for (int c = 0; c < d->Jc; ++c)
        if (d->tb.roleBoneIdx[static_cast<size_t>(c)] >= 0) ++mapped;
    m_valid = mapped * 2 >= d->Jc;
}

bool BodyRetargeter::hasNeutralReference() const
{
    return m_valid && d && d->haveNeutral;
}

void BodyRetargeter::setNeutralReference(
    const std::array<std::array<float, 4>, 22>& canonicalQuats,
    uint32_t resolvedMask,
    const float* mediaPipeWorld33,
    const float* mediaPipeVisibility33)
{
    if (!m_valid || !d)
        return;
    const TargetBindFrame& tb = d->tb;
    const int Jc = d->Jc;
    auto clipQ = [&](const std::array<std::array<float, 4>, 22>& src, int joint)
        -> Ogre::Quaternion {
        const auto& q = src[static_cast<size_t>(joint)];
        return Ogre::Quaternion(q[3], q[0], q[1], q[2]);
    };
    d->neutral = canonicalQuats;
    d->neutralResolvedMask = resolvedMask;
    d->haveNeutral = true;
    constexpr uint32_t kTorsoMask =
        (1u << 0) | (1u << 1) | (1u << 2);  // hip, abdomen, chest
    d->neutralHadTorso = (resolvedMask & kTorsoMask) == kTorsoMask;
    if (d->haveAnyStand && d->restsAreIdentity && d->neutralHadTorso) {
        std::vector<Ogre::Quaternion> standW(static_cast<size_t>(d->nBones),
                                             Ogre::Quaternion::IDENTITY);
        for (int i : tb.order) {
            const int pi = tb.parentIdx[static_cast<size_t>(i)];
            const Ogre::Quaternion Wp = (pi >= 0)
                ? standW[static_cast<size_t>(pi)] : Ogre::Quaternion::IDENTITY;
            const Ogre::Quaternion localRot = d->haveStand[static_cast<size_t>(i)]
                ? d->standLocal[static_cast<size_t>(i)]
                : tb.bindLocal[static_cast<size_t>(i)];
            standW[static_cast<size_t>(i)] = Wp * localRot;
        }
        for (int i = 0; i < d->nBones; ++i) {
            const int c = d->boneToCanon[static_cast<size_t>(i)];
            if (c <= 0 || c >= Jc)
                continue;
            d->Mc[static_cast<size_t>(i)] =
                standW[static_cast<size_t>(i)].Inverse() * clipQ(d->neutral, c);
            d->McInv[static_cast<size_t>(i)] =
                d->Mc[static_cast<size_t>(i)].Inverse();
        }
    }
#ifdef ENABLE_MOCAP
    d->haveNeutralDir = false;
    if (mediaPipeWorld33) {
        const Ogre::Quaternion CtInv = tb.Ct.Inverse();
        std::vector<Ogre::Vector3> neutralCanon(
            static_cast<size_t>(Jc), Ogre::Vector3::ZERO);
        collectCanonicalLiveDirections(
            mediaPipeWorld33, mediaPipeVisibility33, Jc, neutralCanon);
        d->neutralDref.assign(static_cast<size_t>(Jc), Ogre::Vector3::ZERO);
        d->dirQbase.assign(static_cast<size_t>(d->nBones),
                           Ogre::Quaternion::IDENTITY);
        for (int c = 0; c < Jc; ++c) {
            const Ogre::Vector3& nc = neutralCanon[static_cast<size_t>(c)];
            if (nc.squaredLength() < 1e-12f)
                continue;
            Ogre::Vector3 ref = CtInv * nc;
            if (ref.squaredLength() < 1e-12f)
                continue;
            ref.normalise();
            d->neutralDref[static_cast<size_t>(c)] = ref;
            d->haveNeutralDir = true;
        }
        if (d->haveNeutralDir) {
            for (int i = 0; i < d->nBones; ++i) {
                const int c = d->boneToCanon[static_cast<size_t>(i)];
                if (c < 0 || c >= Jc || c == 0)
                    continue;
                if (d->neutralDref[static_cast<size_t>(c)].squaredLength()
                    < 1e-12f)
                    continue;
                if (tb.tgtBindDir[static_cast<size_t>(c)].squaredLength()
                    < 1e-12f)
                    continue;
                d->dirQbase[static_cast<size_t>(i)] =
                    tb.tgtBindDir[static_cast<size_t>(c)].getRotationTo(
                        d->neutralDref[static_cast<size_t>(c)])
                    * tb.bindWorld[static_cast<size_t>(i)];
            }
        }
    }
#endif
}

std::vector<std::pair<unsigned short, Ogre::Quaternion>>
BodyRetargeter::evaluateFrame(
    const std::array<std::array<float, 4>, 22>& canonicalQuats,
    uint32_t resolvedMask,
    uint32_t skipRolesMask,
    const float* mediaPipeWorld33,
    const float* mediaPipeVisibility33) const
{
    std::vector<std::pair<unsigned short, Ogre::Quaternion>> out;
    if (!m_valid || !d) return out;
    const TargetBindFrame& tb = d->tb;
    const int Jc = d->Jc;
    const int nBones = d->nBones;

    auto clipQ = [&](const std::array<std::array<float, 4>, 22>& src, int joint)
        -> Ogre::Quaternion {
        const auto& q = src[static_cast<size_t>(joint)];
        return Ogre::Quaternion(q[3], q[0], q[1], q[2]);
    };
    // PoseIK emits WORLD quats per role. Collar/buttock stay identity (unresolved)
    // — skip them and use chest/hip as the parent (same as the debug overlay).
    auto parentRelativeLocal = [&](const std::array<std::array<float, 4>, 22>& src,
                                 int role, uint32_t mask) -> Ogre::Quaternion {
        const int pc = effectiveParentRole(role, mask);
        if (pc >= 0 && pc < Jc)
            return clipQ(src, pc).Inverse() * clipQ(src, role);
        return clipQ(src, role);
    };
    const bool haveNeutral = d->haveNeutral;

#ifdef ENABLE_MOCAP
    // Live mocap: aim bind bones at landmark segment directions (same math as
    // applyMotionClip direction retarget) — matches the PoseIK debug overlay.
    if (mediaPipeWorld33 && haveNeutral && d->haveNeutralDir) {
        std::vector<Ogre::Vector3> liveCanon(
            static_cast<size_t>(Jc), Ogre::Vector3::ZERO);
        collectCanonicalLiveDirections(
            mediaPipeWorld33, mediaPipeVisibility33, Jc, liveCanon);
        const Ogre::Quaternion CtInv = tb.Ct.Inverse();
        auto quatDeltaArtic = [&](int boneIdx, int role,
                                  const Ogre::Quaternion& basePose)
            -> Ogre::Quaternion {
            if (role < 0 || !haveNeutral)
                return basePose;
            if (!(resolvedMask & (1u << static_cast<unsigned>(role))))
                return basePose;
            const Ogre::Quaternion localCur =
                parentRelativeLocal(canonicalQuats, role, resolvedMask);
            const Ogre::Quaternion localRef =
                parentRelativeLocal(d->neutral, role, d->neutralResolvedMask);
            Ogre::Quaternion delta = localRef.Inverse() * localCur;
            Ogre::Quaternion artic = delta;
            if (d->haveAnyStand && d->restsAreIdentity && d->neutralHadTorso) {
                artic = d->McInv[static_cast<size_t>(boneIdx)] * delta
                        * d->Mc[static_cast<size_t>(boneIdx)];
            } else if (d->yaw180 && !d->haveAnyStand && role > 0) {
                static const Ogre::Quaternion kYawPi(0.0f, 0.0f, 1.0f, 0.0f);
                artic = kYawPi.Inverse() * delta * kYawPi;
            }
            const int dup = std::max(1, d->canonDup[static_cast<size_t>(role)]);
            if (dup > 1)
                artic = Ogre::Quaternion::Slerp(
                    1.0f / static_cast<float>(dup), Ogre::Quaternion::IDENTITY,
                    artic, true);
            return basePose * artic;
        };
        std::vector<Ogre::Quaternion> W(static_cast<size_t>(nBones));
        for (int i : tb.order) {
            const Ogre::Quaternion base =
                (d->haveStand[static_cast<size_t>(i)]
                    ? d->standLocal[static_cast<size_t>(i)]
                    : tb.bindLocal[static_cast<size_t>(i)]);
            const int pi = tb.parentIdx[static_cast<size_t>(i)];
            const Ogre::Quaternion Wp =
                (pi >= 0) ? W[static_cast<size_t>(pi)]
                          : Ogre::Quaternion::IDENTITY;
            const int c = d->boneToCanon[static_cast<size_t>(i)];
            if (c < 0 || c >= Jc
                || (skipRolesMask & (1u << static_cast<unsigned>(c)))) {
                W[static_cast<size_t>(i)] = Wp * base;
                continue;
            }
            Ogre::Quaternion local;
            // Leg landmark directions are often wrong (standing hallucination when
            // knees are occluded). Prefer PoseIK quats for legs when resolved.
            // Hip stays on bind/direction path — quat hip rotation breaks W
            // propagation and distorts arms/torso direction retarget.
            const bool isLegRole =
                (c >= PoseIK::RButtock && c <= PoseIK::LFoot);
            const bool isHandRole =
                (c == PoseIK::RHand || c == PoseIK::LHand);
            const bool legQuatResolved =
                isLegRole
                && (resolvedMask & (1u << static_cast<unsigned>(c))) != 0u;
            const bool legDirOk =
                isLegRole
                && legLandmarksReliable(c, mediaPipeVisibility33)
                && liveCanon[static_cast<size_t>(c)].squaredLength() > 1e-12f
                && d->neutralDref[static_cast<size_t>(c)].squaredLength()
                       > 1e-12f
                && tb.tgtBindDir[static_cast<size_t>(c)].squaredLength()
                       > 1e-12f;
            if (c == 0) {
                // Torso/arm direction retarget reads W[hip] — keep hip on the
                // landmark-direction path. Full PoseIK hip quats use a torso
                // basis that does not match Mixamo bind and can flip the mesh
                // 180° while the debug overlay (raw PoseIK FK) still looks fine.
                Ogre::Quaternion localDir = base;
                if (liveCanon[static_cast<size_t>(c)].squaredLength() > 1e-12f
                    && d->neutralDref[static_cast<size_t>(c)].squaredLength()
                           > 1e-12f
                    && tb.tgtBindDir[static_cast<size_t>(c)].squaredLength()
                           > 1e-12f) {
                    Ogre::Vector3 ds = CtInv * liveCanon[static_cast<size_t>(c)];
                    ds.normalise();
                    const Ogre::Quaternion R =
                        d->neutralDref[static_cast<size_t>(c)].getRotationTo(ds);
                    const Ogre::Quaternion Wt =
                        R * d->dirQbase[static_cast<size_t>(i)];
                    localDir = Wp.Inverse() * Wt;
                    W[static_cast<size_t>(i)] = Wt;
                } else {
                    W[static_cast<size_t>(i)] = Wp * base;
                }
                local = localDir;
            } else if (isLegRole) {
                if (legDirOk) {
                    Ogre::Vector3 ds =
                        CtInv * liveCanon[static_cast<size_t>(c)];
                    ds.normalise();
                    const Ogre::Quaternion R =
                        d->neutralDref[static_cast<size_t>(c)].getRotationTo(ds);
                    const Ogre::Quaternion Wt =
                        R * d->dirQbase[static_cast<size_t>(i)];
                    local = Wp.Inverse() * Wt;
                    W[static_cast<size_t>(i)] = Wt;
                } else if (legQuatResolved) {
                    local = quatDeltaArtic(i, c, base);
                    W[static_cast<size_t>(i)] = Wp * local;
                } else {
                    local = base;
                    W[static_cast<size_t>(i)] = Wp * local;
                }
            } else if (isHandRole
                       && (resolvedMask & (1u << static_cast<unsigned>(c)))
                       != 0u) {
                local = quatDeltaArtic(i, c, base);
                W[static_cast<size_t>(i)] = Wp * local;
            } else if (liveCanon[static_cast<size_t>(c)].squaredLength() > 1e-12f
                       && d->neutralDref[static_cast<size_t>(c)].squaredLength()
                           > 1e-12f
                       && tb.tgtBindDir[static_cast<size_t>(c)].squaredLength()
                           > 1e-12f) {
                Ogre::Vector3 ds = CtInv * liveCanon[static_cast<size_t>(c)];
                ds.normalise();
                const Ogre::Vector3& dref =
                    d->neutralDref[static_cast<size_t>(c)];
                const bool quatResolved =
                    (resolvedMask & (1u << static_cast<unsigned>(c))) != 0u;
                // Stale / held landmark dirs that still match neutral block
                // PoseIK quaternions — otherwise arms/torso freeze at T-pose.
                const bool dirNearNeutral =
                    dref.dotProduct(ds) > 0.9995f;
                if (quatResolved && dirNearNeutral && c != 0) {
                    local = quatDeltaArtic(i, c, base);
                    W[static_cast<size_t>(i)] = Wp * local;
                } else {
                    const Ogre::Quaternion R = dref.getRotationTo(ds);
                    const Ogre::Quaternion Wt =
                        R * d->dirQbase[static_cast<size_t>(i)];
                    local = Wp.Inverse() * Wt;
                    W[static_cast<size_t>(i)] = Wt;
                }
            } else {
                local = quatDeltaArtic(i, c, base);
                W[static_cast<size_t>(i)] = Wp * local;
            }
            out.emplace_back(static_cast<unsigned short>(i), local);
        }
        return out;
    }
#endif
    (void)mediaPipeWorld33;
    (void)mediaPipeVisibility33;

    for (int i : tb.order) {
        const int c = d->boneToCanon[static_cast<size_t>(i)];
        if (c < 0 || c >= Jc) continue;
        if (skipRolesMask & (1u << static_cast<unsigned>(c)))
            continue;
        const Ogre::Quaternion base =
            (d->haveStand[static_cast<size_t>(i)]
                ? d->standLocal[static_cast<size_t>(i)]
                : tb.bindLocal[static_cast<size_t>(i)]);
        const bool resolved = (resolvedMask & (1u << static_cast<unsigned>(c))) != 0u;
        if (!resolved) {
            out.emplace_back(static_cast<unsigned short>(i), base);
            continue;
        }
        if (c == 0 || !haveNeutral) {
            out.emplace_back(static_cast<unsigned short>(i), base);
            continue;
        }
        // PoseIK alignment: parent-relative delta vs neutral, transported onto
        // the rig standing pose with the same Mc frame map as applyMotionClip.
        const Ogre::Quaternion localCur =
            parentRelativeLocal(canonicalQuats, c, resolvedMask);
        const Ogre::Quaternion localRef =
            parentRelativeLocal(d->neutral, c, d->neutralResolvedMask);
        Ogre::Quaternion delta = localRef.Inverse() * localCur;
        Ogre::Quaternion artic = delta;
        if (d->haveAnyStand && d->restsAreIdentity && d->neutralHadTorso) {
            artic = d->McInv[static_cast<size_t>(i)] * delta
                    * d->Mc[static_cast<size_t>(i)];
        } else if (d->yaw180 && !d->haveAnyStand && c > 0) {
            static const Ogre::Quaternion kYawPi(0.0f, 0.0f, 1.0f, 0.0f);
            artic = kYawPi.Inverse() * delta * kYawPi;
        }
        const int dup = std::max(1, d->canonDup[static_cast<size_t>(c)]);
        if (dup > 1)
            artic = Ogre::Quaternion::Slerp(1.0f / static_cast<float>(dup),
                                            Ogre::Quaternion::IDENTITY, artic,
                                            true);
        out.emplace_back(static_cast<unsigned short>(i), base * artic);
    }
    return out;
}

void BodyRetargeter::resetLiveNeutral()
{
    if (!d)
        return;
    d->haveNeutral = false;
    d->neutralHadTorso = false;
    d->neutralResolvedMask = 0;
#ifdef ENABLE_MOCAP
    d->haveNeutralDir = false;
    d->neutralDref.clear();
    d->dirQbase.clear();
#endif
}

float AnimationMerger::currentArmSpace(Ogre::Skeleton* skel,
                                       const std::string& animName)
{
    return getStoredArmSpace(skel, animName);
}

bool AnimationMerger::adjustArmSpace(Ogre::Skeleton* skel,
                                     const std::string& animName,
                                     float degrees)
{
    if (!skel || !skel->hasAnimation(animName))
        return false;
    Ogre::Animation* anim = skel->getAnimation(animName);
    if (!anim)
        return false;

    // Idempotent absolute application: revert whatever we applied before,
    // then apply the new absolute angle. The net delta this call injects is
    // (degrees − stored). The last-applied angle is tracked PER SKELETON
    // INSTANCE (on bone[0]'s UserObjectBindings, keyed by animation) — not a
    // process-global map, which would pollute across entities and tests.
    // currentArmSpace() exposes it so a UI can seed the slider. Export bakes
    // the final keyframes, so cross-session persistence isn't required.
    const float stored = getStoredArmSpace(skel, animName);
    const float delta = degrees - stored;
    if (std::abs(delta) < 1e-4f)
        return true;   // already at target — nothing to do (still success)

    // Bone → canonical role (same matcher as the retarget).
    const int nBones = static_cast<int>(skel->getNumBones());
    std::vector<int> boneToCanon(static_cast<size_t>(nBones), -1);
    for (int i = 0; i < nBones; ++i)
        boneToCanon[static_cast<size_t>(i)] =
            MotionInbetween::canonicalIndexForBone(QString::fromStdString(
                skel->getBone(static_cast<unsigned short>(i))->getName()));

    const TargetBindFrame tb = readTargetBindFrame(skel, boneToCanon);

    // Torso FORWARD axis in world space: the retarget's Ct maps the target's
    // raw frame onto canonical axes (X=left, Y=up, Z=forward), so Ct⁻¹·+Z is
    // the rig's forward direction in its own world.
    const Ogre::Vector3 fwd =
        (tb.Ct.Inverse() * Ogre::Vector3::UNIT_Z).normalisedCopy();

    // Per side: swing about the torso forward axis, sign mirrored between
    // sides so a POSITIVE `degrees` swings BOTH arms AWAY from the body
    // (widen) and negative tucks them in. About canonical +Z (forward) with
    // +Y up / +X left, a NEGATIVE rotation lifts the right arm outward and a
    // positive one lifts the left, so the right side takes −ang. Collars get
    // a fractional share (a small part of the reach).
    const Ogre::Radian ang = Ogre::Radian(Ogre::Degree(delta));
    const float kCollarShare = 0.25f;
    const Ogre::Quaternion swingR(-ang, fwd);
    const Ogre::Quaternion swingL(ang, fwd);
    const Ogre::Quaternion swingRc(ang * (-kCollarShare), fwd);
    const Ogre::Quaternion swingLc(ang * kCollarShare, fwd);

    // Distribute the role's swing across duplicate bones (multi-segment
    // shoulders) so the chain's total matches the requested angle.
    std::vector<int> canonDup(
        static_cast<size_t>(MotionInbetween::canonicalJointCount()), 0);
    for (int i = 0; i < nBones; ++i)
        if (boneToCanon[static_cast<size_t>(i)] >= 0)
            ++canonDup[static_cast<size_t>(boneToCanon[static_cast<size_t>(i)])];

    // Roles that carry an arm-space swing → the world swing for that role.
    auto worldSwingForRole = [&](int c) -> const Ogre::Quaternion* {
        switch (c) {
            case 7:  return &swingR;    // rshoulder
            case 11: return &swingL;    // lshoulder
            case 6:  return &swingRc;   // rcollar (fractional)
            case 10: return &swingLc;   // lcollar (fractional)
            default: return nullptr;
        }
    };

    bool touchedAny = false;
    for (int i = 0; i < nBones; ++i) {
        const int c = boneToCanon[static_cast<size_t>(i)];
        const Ogre::Quaternion* Sworld = worldSwingForRole(c);
        if (!Sworld)
            continue;
        Ogre::NodeAnimationTrack* trk = nullptr;
        if (anim->hasNodeTrack(static_cast<unsigned short>(i)))
            trk = anim->getNodeTrack(static_cast<unsigned short>(i));
        if (!trk || trk->getNumKeyFrames() == 0)
            continue;

        // Fractional swing when the role spans several bones.
        const int dup = std::max(1, canonDup[static_cast<size_t>(c)]);
        Ogre::Quaternion S = *Sworld;
        if (dup > 1)
            S = Ogre::Quaternion::Slerp(1.0f / static_cast<float>(dup),
                                        Ogre::Quaternion::IDENTITY, S,
                                        /*shortestPath=*/true);

        // applyToNode composes the keyframe as localApplied = bindLocal · kf,
        // and world = W_parent · localApplied = W_shoulder_bind · kf. To add
        // a world swing S at this bone (parents untouched), the new keyframe
        // is L · kf where L conjugates S into the bone's bind-local frame:
        //   L = W_bind⁻¹ · S · W_bind
        const Ogre::Quaternion Wbind = tb.bindWorld[static_cast<size_t>(i)];
        const Ogre::Quaternion L = Wbind.Inverse() * S * Wbind;

        const unsigned short nk = trk->getNumKeyFrames();
        for (unsigned short k = 0; k < nk; ++k) {
            Ogre::TransformKeyFrame* kf = trk->getNodeKeyFrame(k);
            kf->setRotation(L * kf->getRotation());
        }
        // TransformKeyFrame::setRotation does NOT invalidate the track's
        // interpolation caches (rotation spline / derived data), so a
        // following apply() would replay the PRE-edit rotations — the edit
        // appears to lag one call behind. Flag the track dirty so the next
        // evaluation rebuilds from the new keyframes.
        trk->_keyFrameDataChanged();
        touchedAny = true;
    }

    if (!touchedAny)
        return false;   // no arm role on this rig

    setStoredArmSpace(skel, animName, degrees);
    return true;
}

int AnimationMerger::smoothBakeAnimation(Ogre::Skeleton* skel,
                                          const std::string& animName,
                                          int sparseFps, int targetFps)
{
    if (!skel || sparseFps <= 0 || targetFps <= 0) return 0;
    if (sparseFps >= targetFps) return 0;   // nothing to low-pass
    if (bakeAnimationAtFps(skel, animName, sparseFps) == 0) return 0;
    return bakeAnimationAtFps(skel, animName, targetFps);
}

namespace {
std::string footPinKey(const std::string& animName)
{
    return "qtme.footpin." + animName;
}
}  // namespace

AnimationMerger::FootPinResult AnimationMerger::pinFeet(
    Ogre::Skeleton* skel, const std::string& animName, int blendFrames)
{
    FootPinResult res;
    if (!skel || !skel->hasAnimation(animName)) {
        res.error = QStringLiteral("animation not found");
        return res;
    }
    Ogre::Animation* anim = skel->getAnimation(animName);

    const int nBones = static_cast<int>(skel->getNumBones());
    std::vector<int> boneToCanon(static_cast<size_t>(nBones), -1);
    for (int i = 0; i < nBones; ++i)
        boneToCanon[static_cast<size_t>(i)] =
            MotionInbetween::canonicalIndexForBone(QString::fromStdString(
                skel->getBone(static_cast<unsigned short>(i))->getName()));
    const TargetBindFrame tb = readTargetBindFrame(skel, boneToCanon);
    // Bind-local POSITIONS (parent-relative) for the manual FK below — the
    // skeleton is still in its reset pose after readTargetBindFrame.
    std::vector<Ogre::Vector3> bindLocalPos(static_cast<size_t>(nBones));
    for (int i = 0; i < nBones; ++i)
        bindLocalPos[static_cast<size_t>(i)] =
            skel->getBone(static_cast<unsigned short>(i))->getPosition();

    // Leg chains: thigh / knee / foot roles per side (first bone per role).
    struct Leg { int thigh, shin, foot; };
    const Leg legs[2] = {
        {tb.roleBoneIdx[15], tb.roleBoneIdx[16], tb.roleBoneIdx[17]},   // right
        {tb.roleBoneIdx[19], tb.roleBoneIdx[20], tb.roleBoneIdx[21]},   // left
    };

    auto trackOf = [&](int bone) -> Ogre::NodeAnimationTrack* {
        if (bone < 0 || !anim->hasNodeTrack(static_cast<unsigned short>(bone)))
            return nullptr;
        auto* t = anim->getNodeTrack(static_cast<unsigned short>(bone));
        return (t && t->getNumKeyFrames() > 0) ? t : nullptr;
    };

    // Keyframe times come from the first available thigh track — generated
    // clips write one keyframe per frame on every mapped bone.
    Ogre::NodeAnimationTrack* timeSrc = trackOf(legs[0].thigh);
    if (!timeSrc) timeSrc = trackOf(legs[1].thigh);
    if (!timeSrc) {
        res.error = QStringLiteral("no leg tracks on this rig/animation");
        return res;
    }
    const int nk = static_cast<int>(timeSrc->getNumKeyFrames());
    if (nk < 4) {
        res.error = QStringLiteral("too few keyframes to detect contacts");
        return res;
    }
    std::vector<float> times(static_cast<size_t>(nk));
    for (int k = 0; k < nk; ++k)
        times[static_cast<size_t>(k)] =
            timeSrc->getNodeKeyFrame(static_cast<unsigned short>(k))->getTime();

    // ---- manual FK over the tracks (pure math; the live skeleton — which
    // may be a SkeletonInstance driving an on-screen entity — is untouched).
    std::vector<std::vector<Ogre::Quaternion>> Wrot(
        static_cast<size_t>(nk),
        std::vector<Ogre::Quaternion>(static_cast<size_t>(nBones)));
    std::vector<std::vector<Ogre::Vector3>> Wpos(
        static_cast<size_t>(nk),
        std::vector<Ogre::Vector3>(static_cast<size_t>(nBones)));
    for (int k = 0; k < nk; ++k) {
        const Ogre::TimeIndex ti =
            anim->_getTimeIndex(times[static_cast<size_t>(k)]);
        for (int i : tb.order) {
            Ogre::Quaternion lrot = tb.bindLocal[static_cast<size_t>(i)];
            Ogre::Vector3 lpos = bindLocalPos[static_cast<size_t>(i)];
            if (auto* trk = trackOf(i)) {
                Ogre::TransformKeyFrame kf(nullptr, 0.0f);
                trk->getInterpolatedKeyFrame(ti, &kf);
                lrot = lrot * kf.getRotation();       // applyToNode: rotate()
                lpos = lpos + kf.getTranslate();      // translate(TS_PARENT)
            }
            const int pi = tb.parentIdx[static_cast<size_t>(i)];
            if (pi >= 0) {
                Wrot[static_cast<size_t>(k)][static_cast<size_t>(i)] =
                    Wrot[static_cast<size_t>(k)][static_cast<size_t>(pi)] * lrot;
                Wpos[static_cast<size_t>(k)][static_cast<size_t>(i)] =
                    Wpos[static_cast<size_t>(k)][static_cast<size_t>(pi)]
                    + Wrot[static_cast<size_t>(k)][static_cast<size_t>(pi)] * lpos;
            } else {
                Wrot[static_cast<size_t>(k)][static_cast<size_t>(i)] = lrot;
                Wpos[static_cast<size_t>(k)][static_cast<size_t>(i)] = lpos;
            }
        }
    }

    // Detection runs in the CANONICAL frame (Ct maps rig world → X=left,
    // Y=up, Z=forward) so "ground" is a horizontal plane regardless of the
    // rig's own axes.
    auto toCanon = [&](const Ogre::Vector3& p) { return tb.Ct * p; };
    auto fromCanon = [&](const FootContact::V3& p) {
        return tb.Ct.Inverse() * Ogre::Vector3(p[0], p[1], p[2]);
    };
    auto v3 = [](const Ogre::Vector3& p) {
        return FootContact::V3{p.x, p.y, p.z};
    };

    for (const Leg& leg : legs) {
        if (leg.thigh < 0 || leg.shin < 0 || leg.foot < 0)
            continue;
        auto* thighTrk = trackOf(leg.thigh);
        auto* shinTrk = trackOf(leg.shin);
        auto* footTrk = trackOf(leg.foot);
        if (!thighTrk || !shinTrk
            || static_cast<int>(thighTrk->getNumKeyFrames()) != nk
            || static_cast<int>(shinTrk->getNumKeyFrames()) != nk
            || (footTrk && static_cast<int>(footTrk->getNumKeyFrames()) != nk))
            continue;   // mixed keyframe grids — not a generated clip
        // The rewrite below indexes shin/foot keyframes by k and assumes they
        // share timeSrc's times. Count alone isn't enough — an authored clip
        // can have equal counts at DIFFERENT times, which would write a
        // correction computed at time t onto a keyframe at t'. Verify the
        // grids actually align (generated clips do by construction).
        {
            bool aligned = true;
            for (int k = 0; k < nk && aligned; ++k) {
                const float t = times[static_cast<size_t>(k)];
                if (std::abs(thighTrk->getNodeKeyFrame(
                        static_cast<unsigned short>(k))->getTime() - t) > 1e-4f
                    || std::abs(shinTrk->getNodeKeyFrame(
                        static_cast<unsigned short>(k))->getTime() - t) > 1e-4f
                    || (footTrk && std::abs(footTrk->getNodeKeyFrame(
                        static_cast<unsigned short>(k))->getTime() - t) > 1e-4f))
                    aligned = false;
            }
            if (!aligned) continue;   // keyframe times don't match — skip leg
        }

        std::vector<FootContact::V3> footC(static_cast<size_t>(nk));
        for (int k = 0; k < nk; ++k)
            footC[static_cast<size_t>(k)] = v3(toCanon(
                Wpos[static_cast<size_t>(k)][static_cast<size_t>(leg.foot)]));
        const float legLen =
            (Wpos[0][static_cast<size_t>(leg.shin)]
             - Wpos[0][static_cast<size_t>(leg.thigh)]).length()
            + (Wpos[0][static_cast<size_t>(leg.foot)]
               - Wpos[0][static_cast<size_t>(leg.shin)]).length();
        auto spans = FootContact::detectContacts(footC, legLen);
        // Coverage guard: a single "contact" spanning most of the clip is a
        // misdetection (loose thresholds on a moving clip) — pinning it
        // freezes the leg for the whole animation. Genuine stance phases in
        // gait are well under this.
        spans.erase(std::remove_if(spans.begin(), spans.end(),
                        [&](const FootContact::Span& sp) {
                            return (sp.end - sp.start + 1)
                                   > (nk * 55) / 100;
                        }),
                    spans.end());
        if (spans.empty())
            continue;
        res.spans += static_cast<int>(spans.size());

        for (const auto& span : spans) {
            const FootContact::V3 anchor = footC[static_cast<size_t>(span.start)];
            for (int k = span.start; k <= span.end; ++k) {
                const float w = FootContact::blendWeight(span, k, blendFrames);
                if (w <= 0.0f)
                    continue;
                const size_t kc = static_cast<size_t>(k);
                const Ogre::Vector3 hipW = Wpos[kc][static_cast<size_t>(leg.thigh)];
                const Ogre::Vector3 kneeW = Wpos[kc][static_cast<size_t>(leg.shin)];
                const Ogre::Vector3 footW = Wpos[kc][static_cast<size_t>(leg.foot)];
                // pinned target: blend the current foot toward the anchor
                const FootContact::V3 cur = footC[kc];
                const FootContact::V3 tgtC{
                    cur[0] + (anchor[0] - cur[0]) * w,
                    cur[1] + (anchor[1] - cur[1]) * w,
                    cur[2] + (anchor[2] - cur[2]) * w};
                const FootContact::V3 kneeNewC = FootContact::solveKnee(
                    v3(toCanon(hipW)), v3(toCanon(kneeW)), v3(toCanon(footW)),
                    tgtC);
                const Ogre::Vector3 kneeNew = fromCanon(kneeNewC);
                const Ogre::Vector3 tgt = fromCanon(tgtC);

                Ogre::Vector3 dThighOld = kneeW - hipW;
                Ogre::Vector3 dThighNew = kneeNew - hipW;
                Ogre::Vector3 dShinOld = footW - kneeW;
                Ogre::Vector3 dShinNew = tgt - kneeNew;
                if (dThighOld.squaredLength() < 1e-12f
                    || dThighNew.squaredLength() < 1e-12f
                    || dShinOld.squaredLength() < 1e-12f
                    || dShinNew.squaredLength() < 1e-12f)
                    continue;
                dThighOld.normalise(); dThighNew.normalise();
                dShinOld.normalise(); dShinNew.normalise();
                const Ogre::Quaternion S1 = dThighOld.getRotationTo(dThighNew);
                const Ogre::Quaternion S2 =
                    (S1 * dShinOld).getRotationTo(dShinNew);

                // Rewrite the three keyframes as world premultipliers folded
                // into parent-relative deltas (keyframes compose as
                // local = bindLocal · kf; world = Wp · local).
                const Ogre::Quaternion& WpT =
                    (tb.parentIdx[static_cast<size_t>(leg.thigh)] >= 0)
                        ? Wrot[kc][static_cast<size_t>(
                              tb.parentIdx[static_cast<size_t>(leg.thigh)])]
                        : Ogre::Quaternion::IDENTITY;
                const Ogre::Quaternion Wthigh =
                    Wrot[kc][static_cast<size_t>(leg.thigh)];
                const Ogre::Quaternion WthighNew = S1 * Wthigh;
                auto* kfT = thighTrk->getNodeKeyFrame(
                    static_cast<unsigned short>(k));
                kfT->setRotation(
                    tb.bindLocal[static_cast<size_t>(leg.thigh)].Inverse()
                    * WpT.Inverse() * WthighNew);

                const Ogre::Quaternion& WpS =
                    Wrot[kc][static_cast<size_t>(
                        tb.parentIdx[static_cast<size_t>(leg.shin)])];
                const Ogre::Quaternion Wshin =
                    Wrot[kc][static_cast<size_t>(leg.shin)];
                const Ogre::Quaternion WshinNew = S2 * S1 * Wshin;
                auto* kfS = shinTrk->getNodeKeyFrame(
                    static_cast<unsigned short>(k));
                kfS->setRotation(
                    tb.bindLocal[static_cast<size_t>(leg.shin)].Inverse()
                    * (S1 * WpS).Inverse() * WshinNew);

                if (footTrk) {
                    // foot keeps its ORIGINAL world orientation (no toe pop
                    // inherited from the parent corrections)
                    const Ogre::Quaternion& WpF =
                        Wrot[kc][static_cast<size_t>(
                            tb.parentIdx[static_cast<size_t>(leg.foot)])];
                    auto* kfF = footTrk->getNodeKeyFrame(
                        static_cast<unsigned short>(k));
                    kfF->setRotation(
                        tb.bindLocal[static_cast<size_t>(leg.foot)].Inverse()
                        * (S2 * S1 * WpF).Inverse()
                        * Wrot[kc][static_cast<size_t>(leg.foot)]);
                }
                ++res.keyframesAdjusted;
            }
        }
        // setRotation does NOT invalidate the track caches (#854 lesson).
        thighTrk->_keyFrameDataChanged();
        shinTrk->_keyFrameDataChanged();
        if (footTrk) footTrk->_keyFrameDataChanged();
    }

    if (skel->getNumBones() > 0)
        skel->getBone(0)->getUserObjectBindings().setUserAny(
            footPinKey(animName), Ogre::Any(true));
    res.ok = true;
    return res;
}

int AnimationMerger::groundRootToFeet(Ogre::Skeleton* skel,
                                      const std::string& animName)
{
    if (!skel || !skel->hasAnimation(animName)) return 0;
    Ogre::Animation* anim = skel->getAnimation(animName);
    const int nBones = static_cast<int>(skel->getNumBones());

    std::vector<int> boneToCanon(static_cast<size_t>(nBones), -1);
    for (int i = 0; i < nBones; ++i)
        boneToCanon[static_cast<size_t>(i)] =
            MotionInbetween::canonicalIndexForBone(QString::fromStdString(
                skel->getBone(static_cast<unsigned short>(i))->getName()));
    const TargetBindFrame tb = readTargetBindFrame(skel, boneToCanon);
    // Bind-local positions for the manual FK (skeleton is in reset pose).
    std::vector<Ogre::Vector3> bindLocalPos(static_cast<size_t>(nBones));
    for (int i = 0; i < nBones; ++i)
        bindLocalPos[static_cast<size_t>(i)] =
            skel->getBone(static_cast<unsigned short>(i))->getPosition();

    const int rootIdx = tb.roleBoneIdx[0];
    const int rFoot = tb.roleBoneIdx[17];
    const int lFoot = tb.roleBoneIdx[21];
    if (rootIdx < 0 || (rFoot < 0 && lFoot < 0)) return 0;
    auto* rootTrk = anim->hasNodeTrack(static_cast<unsigned short>(rootIdx))
        ? anim->getNodeTrack(static_cast<unsigned short>(rootIdx)) : nullptr;
    if (!rootTrk || rootTrk->getNumKeyFrames() == 0) return 0;

    // Bind ground = the lowest foot's canonical-Y in the reset pose.
    auto canonY = [&](int bone) {
        return (tb.Ct * tb.bindPos[static_cast<size_t>(bone)]).y;
    };
    float groundY = std::numeric_limits<float>::max();
    if (rFoot >= 0) groundY = std::min(groundY, canonY(rFoot));
    if (lFoot >= 0) groundY = std::min(groundY, canonY(lFoot));

    // Root's parent-local UP direction (canonical +Y mapped into the frame the
    // keyframe translate is applied in) — for a top-of-hierarchy root this is
    // world up, matching applyMotionClip's descent convention.
    const Ogre::Vector3 dropAxis =
        tb.Ct.Inverse() * Ogre::Vector3(0.0f, 1.0f, 0.0f);

    const int nk = static_cast<int>(rootTrk->getNumKeyFrames());
    auto trackOf = [&](int bone) -> Ogre::NodeAnimationTrack* {
        if (bone < 0 || !anim->hasNodeTrack(static_cast<unsigned short>(bone)))
            return nullptr;
        return anim->getNodeTrack(static_cast<unsigned short>(bone));
    };

    int adjusted = 0;
    for (int k = 0; k < nk; ++k) {
        Ogre::TransformKeyFrame* rkf =
            rootTrk->getNodeKeyFrame(static_cast<unsigned short>(k));
        const float t = rkf->getTime();
        const Ogre::TimeIndex ti = anim->_getTimeIndex(t);
        // Manual FK at this time to find world foot positions.
        std::vector<Ogre::Quaternion> Wrot(static_cast<size_t>(nBones));
        std::vector<Ogre::Vector3> Wpos(static_cast<size_t>(nBones));
        for (int i : tb.order) {
            Ogre::Quaternion lrot = tb.bindLocal[static_cast<size_t>(i)];
            Ogre::Vector3 lpos = bindLocalPos[static_cast<size_t>(i)];
            if (auto* trk = trackOf(i)) {
                Ogre::TransformKeyFrame kf(nullptr, 0.0f);
                trk->getInterpolatedKeyFrame(ti, &kf);
                lrot = lrot * kf.getRotation();
                lpos = lpos + kf.getTranslate();
            }
            const int pi = tb.parentIdx[static_cast<size_t>(i)];
            if (pi >= 0) {
                Wrot[static_cast<size_t>(i)] =
                    Wrot[static_cast<size_t>(pi)] * lrot;
                Wpos[static_cast<size_t>(i)] =
                    Wpos[static_cast<size_t>(pi)]
                    + Wrot[static_cast<size_t>(pi)] * lpos;
            } else {
                Wrot[static_cast<size_t>(i)] = lrot;
                Wpos[static_cast<size_t>(i)] = lpos;
            }
        }
        float lowest = std::numeric_limits<float>::max();
        if (rFoot >= 0)
            lowest = std::min(lowest, (tb.Ct * Wpos[static_cast<size_t>(rFoot)]).y);
        if (lFoot >= 0)
            lowest = std::min(lowest, (tb.Ct * Wpos[static_cast<size_t>(lFoot)]).y);
        // How far the lowest foot floats above the bind ground (≥ 0). Lower the
        // root by that much so the foot re-plants. Never RAISE (max(0,...)).
        const float above = std::max(0.0f, lowest - groundY);
        if (above > 1e-4f) {
            rkf->setTranslate(rkf->getTranslate() - dropAxis * above);
            ++adjusted;
        }
    }
    if (adjusted > 0) rootTrk->_keyFrameDataChanged();
    return adjusted;
}

int AnimationMerger::applyFingerCurl(
    Ogre::Skeleton* skel, const std::string& animName,
    const std::vector<std::vector<std::array<float, 4>>>& clipFingers,
    int fps,
    const std::vector<std::array<float, 3>>& clipFingerRest)
{
    if (!skel || !skel->hasAnimation(animName) || clipFingers.empty())
        return 0;
    // Rows are indexed with kFingerSlots offsets below — reject any clip whose
    // rows aren't exactly that wide (parse enforces it, but extraction paths
    // and direct callers also feed this).
    for (const auto& row : clipFingers)
        if (static_cast<int>(row.size()) != kFingerSlots) return 0;
    if (fps <= 0) fps = 30;
    (void)clipFingerRest;   // superseded by the clip-mean rest computed below;
                            // kept in the signature for the stored-rest path.
    Ogre::Animation* anim = skel->getAnimation(animName);
    const int frames = static_cast<int>(clipFingers.size());
    const float dt = 1.0f / static_cast<float>(fps);
    const int nBones = static_cast<int>(skel->getNumBones());

    // The stored curl is C·(source world delta)·C⁻¹ (canonical frame). To
    // reproduce the same WORLD bend on the target, map it into the target's
    // world via Ct⁻¹ (Ct = target world→canonical), then express as the
    // finger bone's local keyframe delta. Need the target's Ct + per-bone bind
    // world/local + parent bind world — all from the reset (bind) pose.
    std::vector<int> boneToCanon(static_cast<size_t>(nBones), -1);
    for (int i = 0; i < nBones; ++i)
        boneToCanon[static_cast<size_t>(i)] =
            MotionInbetween::canonicalIndexForBone(QString::fromStdString(
                skel->getBone(static_cast<unsigned short>(i))->getName()));
    const TargetBindFrame tb = readTargetBindFrame(skel, boneToCanon);
    const Ogre::Quaternion CtInv = tb.Ct.Inverse();
    // Capture finger bones' bind world + bind local + parent bind world.
    skel->reset(true); skel->_updateTransforms();
    std::vector<Ogre::Quaternion> bindWorld(static_cast<size_t>(nBones));
    std::vector<Ogre::Quaternion> bindLocal(static_cast<size_t>(nBones));
    for (int i = 0; i < nBones; ++i) {
        Ogre::Bone* b = skel->getBone(static_cast<unsigned short>(i));
        bindWorld[static_cast<size_t>(i)] = b->_getDerivedOrientation();
        bindLocal[static_cast<size_t>(i)] = b->getOrientation();
    }
    auto parentBindWorld = [&](Ogre::Bone* b) -> Ogre::Quaternion {
        if (auto* p = dynamic_cast<Ogre::Bone*>(b->getParent()))
            return bindWorld[static_cast<size_t>(p->getHandle())];
        return Ogre::Quaternion::IDENTITY;
    };

    // Map the TARGET rig's finger bones by (side,finger,segment). Keep, per
    // (side,finger), the segments in order so we can redistribute.
    std::vector<std::vector<std::pair<int, Ogre::Bone*>>> tgt(
        2 * MotionInbetween::kFingerCount);
    for (int i = 0; i < nBones; ++i) {
        Ogre::Bone* b = skel->getBone(static_cast<unsigned short>(i));
        const auto fr = MotionInbetween::fingerRoleForBone(
            QString::fromStdString(b->getName()));
        if (!fr.valid()) continue;
        tgt[static_cast<size_t>(fr.side * MotionInbetween::kFingerCount
                                + fr.finger)].push_back({fr.segment, b});
    }
    int animated = 0;
    for (auto& v : tgt)
        std::sort(v.begin(), v.end(),
                  [](auto& a, auto& b){ return a.first < b.first; });

    // Target bind pointing direction per bone (segment → child, canonical
    // frame). Same signal the source stored — aiming one at the other bends
    // the finger the same way regardless of local axis conventions.
    auto childOfTarget = [&](Ogre::Bone* b) -> Ogre::Bone* {
        // the finger child = a child bone that is also a finger of the same
        // (side,finger) with segment+1; simplest: any finger-role child.
        for (unsigned short k = 0; k < b->numChildren(); ++k) {
            auto* c = dynamic_cast<Ogre::Bone*>(b->getChild(k));
            if (c && MotionInbetween::fingerRoleForBone(
                    QString::fromStdString(c->getName())).valid())
                return c;
        }
        // Fallback (mirror of the extraction side): the last finger segment's
        // only child is often a non-role "nub"/tip. Use it for the pointing
        // direction so the distal segment still aims — else the knuckle-bend
        // curl is lost on the target too.
        if (b->numChildren() > 0)
            return dynamic_cast<Ogre::Bone*>(b->getChild(0));
        return nullptr;
    };
    std::vector<Ogre::Vector3> tgtBindDir(static_cast<size_t>(nBones),
                                          Ogre::Vector3::ZERO);
    for (int i = 0; i < nBones; ++i) {
        Ogre::Bone* b = skel->getBone(static_cast<unsigned short>(i));
        Ogre::Bone* c = childOfTarget(b);
        if (!c) continue;
        Ogre::Vector3 d = c->_getDerivedPosition() - b->_getDerivedPosition();
        if (d.squaredLength() < 1e-12f) continue;
        tgtBindDir[static_cast<size_t>(i)] = (tb.Ct * d).normalisedCopy();
    }

    // Per-hand FLEXION AXIS (canonical frame): the axis a finger rotates about
    // to curl into the palm. Fingers 1..4 (index..pinky) lie in a plane; their
    // pointing dirs share it and their knuckle roots spread across it. The
    // flexion axis ⟂ that plane = mean(fingerDir) × (knuckleSpread). Derived
    // from TARGET bind geometry so the re-applied curl bends in the target's
    // own anatomical plane — the source curl's rotation AXIS is in a finger
    // frame ~100° misaligned from the target's, so transporting the rotation
    // directly wastes most of it; we transport only the scalar curl ANGLE and
    // rotate about THIS axis instead.
    std::array<Ogre::Vector3, 2> flexAxis = {Ogre::Vector3::ZERO,
                                             Ogre::Vector3::ZERO};
    for (int side = 0; side < 2; ++side) {
        Ogre::Vector3 meanDir = Ogre::Vector3::ZERO;
        std::vector<Ogre::Vector3> roots;
        for (int fgr = 1; fgr < MotionInbetween::kFingerCount; ++fgr) {
            auto& segs = tgt[static_cast<size_t>(
                side * MotionInbetween::kFingerCount + fgr)];
            if (segs.empty()) continue;
            Ogre::Bone* b = segs.front().second;   // seg0 (knuckle root)
            const int bi = b->getHandle();
            if (tgtBindDir[static_cast<size_t>(bi)].squaredLength() > 1e-9f) {
                meanDir += tgtBindDir[static_cast<size_t>(bi)];
                roots.push_back(tb.Ct * b->_getDerivedPosition());
            }
        }
        if (meanDir.squaredLength() < 1e-9f || roots.size() < 2) continue;
        meanDir.normalise();
        // FLEXION axis = the KNUCKLE LINE (index-root → pinky-root). Fingers
        // point along meanDir; curling folds them toward the palm — a rotation
        // in the plane of meanDir and the palm normal, whose axis is the knuckle
        // line. (meanDir × spread would be the palm-normal → ABDUCTION, i.e.
        // fingers splaying sideways, which barely closes the fist — the earlier
        // bug.) Orthogonalise the spread against meanDir so the axis is clean.
        Ogre::Vector3 spread = roots.back() - roots.front();
        spread = spread - meanDir * spread.dotProduct(meanDir);
        if (spread.squaredLength() < 1e-9f) continue;
        Ogre::Vector3 ax = spread.normalisedCopy();
        // Sign the axis so +rotation = FLEXION (fingers fold toward the palm).
        // The palm side is unambiguous from the THUMB: the thumb always sits on
        // the palm/flexion side of the hand. palmN = meanDir × spread is the
        // finger-plane normal (sign unknown); the thumb's offset from the finger
        // plane picks the correct sign. Fingers flex by rotating so the tip
        // gains a component toward +palmSide (the thumb side). This is rig-
        // independent (no bind micro-bend needed — rumba's fingers are dead
        // straight — and no source-frame correspondence).
        Ogre::Vector3 palmN = meanDir.crossProduct(spread.normalisedCopy());
        if (palmN.squaredLength() > 1e-9f) {
            palmN.normalise();
            // thumb (finger 0) seg0 position relative to the index knuckle
            Ogre::Vector3 palmSide = Ogre::Vector3::ZERO;
            auto& thumbSegs = tgt[static_cast<size_t>(
                side * MotionInbetween::kFingerCount + 0)];
            if (!thumbSegs.empty() && !roots.empty()) {
                const Ogre::Vector3 thumbPos =
                    tb.Ct * thumbSegs.front().second->_getDerivedPosition();
                const Ogre::Vector3 off = thumbPos - roots.front();
                const float side_ = off.dotProduct(palmN);
                if (std::abs(side_) > 1e-5f)
                    palmSide = palmN * (side_ >= 0.f ? 1.f : -1.f);
            }
            // Fallback if no thumb: assume palm faces the hand-bone's own
            // down/forward — use the raw palmN (may be wrong, but rare).
            if (palmSide.squaredLength() < 1e-9f) palmSide = palmN;
            // choose sign whose small +rotation moves meanDir toward palmSide
            const Ogre::Vector3 test =
                Ogre::Quaternion(Ogre::Radian(0.4f), ax) * meanDir;
            if ((test - meanDir).dotProduct(palmSide) < 0.f)
                ax = -ax;
        }
        flexAxis[static_cast<size_t>(side)] = ax;
    }

    // For each target finger segment, pull the proportional source segment's
    // pointing direction and AIM the target bone's bind direction at it.
    for (int side = 0; side < 2; ++side) {
        for (int fgr = 0; fgr < MotionInbetween::kFingerCount; ++fgr) {
            auto& segs = tgt[static_cast<size_t>(
                side * MotionInbetween::kFingerCount + fgr)];
            if (segs.empty()) continue;
            // How many source segments carry a direction (are populated)?
            int srcSegs = 0;
            for (int s = 0; s < kFingerSegs; ++s) {
                const int slot = fingerSlot(side, fgr, s);
                bool filled = false;
                for (int f = 0; f < frames && !filled; ++f) {
                    const auto& q = clipFingers[static_cast<size_t>(f)]
                                              [static_cast<size_t>(slot)];
                    if (q[0] != 0.f || q[1] != 0.f || q[2] != 0.f) filled = true;
                }
                if (filled) srcSegs = s + 1;
            }
            if (srcSegs == 0) continue;
            const int tgtSegs = static_cast<int>(segs.size());
            for (int ts = 0; ts < tgtSegs; ++ts) {
                int ss = (tgtSegs <= 1) ? 0
                    : static_cast<int>(std::round(
                          double(ts) * (srcSegs - 1) / (tgtSegs - 1)));
                ss = std::clamp(ss, 0, kFingerSegs - 1);
                const int slot = fingerSlot(side, fgr, ss);
                Ogre::Bone* b = segs[static_cast<size_t>(ts)].second;
                const int bi = b->getHandle();
                const Ogre::Vector3 dbind = tgtBindDir[static_cast<size_t>(bi)];
                if (dbind.squaredLength() < 1e-9f) continue;   // tip, no child
                const Ogre::Quaternion Lbind = bindLocal[static_cast<size_t>(bi)];
                const Ogre::Quaternion Pbind = parentBindWorld(b);
                const Ogre::Quaternion Wbind = bindWorld[static_cast<size_t>(bi)];
                // Source REST pointing dir for this slot = the finger's MEAN
                // direction across the clip. Aiming target-bind at the ABSOLUTE
                // source dir over-bends every finger by the gap between the two
                // rigs' finger rest conventions (measured 50–130° even on an
                // OPEN, static hand). Transporting the RELATIVE bend (rest→frame)
                // cancels that: a static hand → 0° bend, a grasping hand tracks
                // its real 0→~45° curl. The clip-mean is a rig-agnostic neutral
                // computed from the per-frame data itself — no library/extraction
                // change, works on every existing clip. (The stored
                // `clipFingerRest`, when present, isn't used here: the bind-pose
                // rest carries a constant convention offset the mean avoids.)
                Ogre::Vector3 dsrcRest = Ogre::Vector3::ZERO;
                {
                    Ogre::Vector3 acc = Ogre::Vector3::ZERO;
                    for (int f = 0; f < frames; ++f) {
                        const auto& qf = clipFingers[static_cast<size_t>(f)]
                                                    [static_cast<size_t>(slot)];
                        Ogre::Vector3 v(qf[0], qf[1], qf[2]);
                        if (v.squaredLength() > 1e-9f) acc += v.normalisedCopy();
                    }
                    if (acc.squaredLength() > 1e-9f)
                        dsrcRest = acc.normalisedCopy();
                }
                const bool useRel = dsrcRest.squaredLength() > 1e-9f;
                const Ogre::Vector3& fax = flexAxis[static_cast<size_t>(side)];
                const bool useScalar = useRel && fax.squaredLength() > 1e-9f;
                auto* trk = anim->hasNodeTrack(static_cast<unsigned short>(bi))
                    ? anim->getNodeTrack(static_cast<unsigned short>(bi))
                    : anim->createNodeTrack(static_cast<unsigned short>(bi), b);
                for (int f = 0; f < frames; ++f) {
                    const auto& q = clipFingers[static_cast<size_t>(f)]
                                              [static_cast<size_t>(slot)];
                    Ogre::Vector3 dsrc(q[0], q[1], q[2]);   // canonical dir
                    Ogre::Quaternion kf;
                    if (dsrc.squaredLength() > 1e-9f) {
                        dsrc.normalise();
                        Ogre::Quaternion aimC;
                        if (useScalar) {
                            // SCALAR-ANGLE CURL. The source finger frame is
                            // ~100° misaligned from the target's, so transporting
                            // the curl ROTATION directly bends in the wrong plane
                            // (fist won't close). Instead take only the source
                            // curl MAGNITUDE — the fold angle from its own rest —
                            // and re-apply it about the TARGET hand's own flexion
                            // axis, so the target finger curls in its anatomical
                            // plane by the same amount. Signed so an open frame
                            // (near rest) gives ~0 and never hyperextends.
                            float curl = std::acos(std::clamp(
                                dsrcRest.dotProduct(dsrc), -1.f, 1.f));
                            // sign: does dsrc fold to the SAME side of rest that
                            // curling (about the source's own bend) would? Use
                            // the source rest→frame axis vs the source flex proxy
                            // (mean finger dir change). Simple, robust: keep the
                            // fold as a positive curl (fingers only close here).
                            aimC = Ogre::Quaternion(Ogre::Radian(curl), fax);
                            // aimC rotates about fax in the +curl (closing) sense;
                            // applied to the target bind dir below.
                        } else if (useRel) {
                            const Ogre::Quaternion srcBend =
                                dsrcRest.getRotationTo(dsrc);
                            const Ogre::Vector3 dtarget =
                                (srcBend * dbind).normalisedCopy();
                            aimC = dbind.getRotationTo(dtarget);
                        } else {
                            aimC = dbind.getRotationTo(dsrc);
                        }
                        // canonical → world, apply on bind world, → local delta.
                        const Ogre::Quaternion aimW = CtInv * aimC * tb.Ct;
                        const Ogre::Quaternion newWorld = aimW * Wbind;
                        const Ogre::Quaternion newLocal =
                            Pbind.Inverse() * newWorld;
                        kf = Lbind.Inverse() * newLocal;
                    }  // else identity (no source dir this frame)
                    Ogre::TransformKeyFrame* nk =
                        trk->createNodeKeyFrame(f * dt);
                    nk->setRotation(kf);
                    nk->setTranslate(Ogre::Vector3::ZERO);
                    nk->setScale(Ogre::Vector3::UNIT_SCALE);
                }
                trk->_keyFrameDataChanged();
                ++animated;
            }
        }
    }
    return animated;
}

#ifdef ENABLE_MOCAP
AnimationMerger::FingerLiveDriveContext
AnimationMerger::buildFingerLiveDriveContext(Ogre::SkeletonInstance* skel)
{
    FingerLiveDriveContext ctx;
    if (!skel)
        return ctx;
    const int nBones = static_cast<int>(skel->getNumBones());
    std::vector<int> boneToCanon(static_cast<size_t>(nBones), -1);
    for (int i = 0; i < nBones; ++i)
        boneToCanon[static_cast<size_t>(i)] =
            MotionInbetween::canonicalIndexForBone(QString::fromStdString(
                skel->getBone(static_cast<unsigned short>(i))->getName()));
    const TargetBindFrame tb = readTargetBindFrame(skel, boneToCanon);
    ctx.CtInv = tb.Ct.Inverse();
    ctx.bindWorld.assign(static_cast<size_t>(nBones),
                         Ogre::Quaternion::IDENTITY);
    ctx.bindLocal.assign(static_cast<size_t>(nBones),
                         Ogre::Quaternion::IDENTITY);
    ctx.tgtBindDir.assign(static_cast<size_t>(nBones), Ogre::Vector3::ZERO);
    for (int i = 0; i < nBones; ++i) {
        Ogre::Bone* b = skel->getBone(static_cast<unsigned short>(i));
        ctx.bindWorld[static_cast<size_t>(i)] = b->_getDerivedOrientation();
        ctx.bindLocal[static_cast<size_t>(i)] = b->getOrientation();
    }
    ctx.fingerBones.assign(
        static_cast<size_t>(2 * MotionInbetween::kFingerCount), {});
    for (int i = 0; i < nBones; ++i) {
        Ogre::Bone* b = skel->getBone(static_cast<unsigned short>(i));
        const auto fr = MotionInbetween::fingerRoleForBone(
            QString::fromStdString(b->getName()));
        if (!fr.valid())
            continue;
        ctx.fingerBones[static_cast<size_t>(
            fr.side * MotionInbetween::kFingerCount + fr.finger)]
            .push_back({fr.segment, b->getHandle()});
    }
    for (auto& v : ctx.fingerBones)
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

    auto childOfTarget = [&](Ogre::Bone* b) -> Ogre::Bone* {
        for (unsigned short k = 0; k < b->numChildren(); ++k) {
            auto* c = dynamic_cast<Ogre::Bone*>(b->getChild(k));
            if (c && MotionInbetween::fingerRoleForBone(
                    QString::fromStdString(c->getName())).valid())
                return c;
        }
        if (b->numChildren() > 0)
            return dynamic_cast<Ogre::Bone*>(b->getChild(0));
        return nullptr;
    };
    for (int i = 0; i < nBones; ++i) {
        Ogre::Bone* b = skel->getBone(static_cast<unsigned short>(i));
        Ogre::Bone* c = childOfTarget(b);
        if (!c)
            continue;
        Ogre::Vector3 d = c->_getDerivedPosition() - b->_getDerivedPosition();
        if (d.squaredLength() < 1e-12f)
            continue;
        ctx.tgtBindDir[static_cast<size_t>(i)] =
            (tb.Ct * d).normalisedCopy();
    }
    for (int side = 0; side < 2; ++side) {
        Ogre::Vector3 meanDir = Ogre::Vector3::ZERO;
        std::vector<Ogre::Vector3> roots;
        for (int fgr = 1; fgr < MotionInbetween::kFingerCount; ++fgr) {
            auto& segs = ctx.fingerBones[static_cast<size_t>(
                side * MotionInbetween::kFingerCount + fgr)];
            if (segs.empty())
                continue;
            const unsigned short h = segs.front().second;
            if (ctx.tgtBindDir[static_cast<size_t>(h)].squaredLength() > 1e-9f) {
                meanDir += ctx.tgtBindDir[static_cast<size_t>(h)];
                roots.push_back(
                    tb.Ct * skel->getBone(h)->_getDerivedPosition());
            }
        }
        if (meanDir.squaredLength() < 1e-9f || roots.size() < 2)
            continue;
        meanDir.normalise();
        Ogre::Vector3 spread = roots.back() - roots.front();
        spread = spread - meanDir * spread.dotProduct(meanDir);
        if (spread.squaredLength() < 1e-9f)
            continue;
        Ogre::Vector3 ax = spread.normalisedCopy();
        Ogre::Vector3 palmN = meanDir.crossProduct(spread.normalisedCopy());
        if (palmN.squaredLength() > 1e-9f) {
            palmN.normalise();
            Ogre::Vector3 palmSide = Ogre::Vector3::ZERO;
            auto& thumbSegs = ctx.fingerBones[static_cast<size_t>(
                side * MotionInbetween::kFingerCount + 0)];
            if (!thumbSegs.empty() && !roots.empty()) {
                const Ogre::Vector3 thumbPos =
                    tb.Ct * skel->getBone(thumbSegs.front().second)
                               ->_getDerivedPosition();
                const Ogre::Vector3 off = thumbPos - roots.front();
                const float side_ = off.dotProduct(palmN);
                if (std::abs(side_) > 1e-5f)
                    palmSide = palmN * (side_ >= 0.f ? 1.f : -1.f);
            }
            if (palmSide.squaredLength() < 1e-9f)
                palmSide = palmN;
            const Ogre::Vector3 test =
                Ogre::Quaternion(Ogre::Radian(0.4f), ax) * meanDir;
            if ((test - meanDir).dotProduct(palmSide) < 0.f)
                ax = -ax;
        }
        ctx.flexAxis[static_cast<size_t>(side)] = ax;
    }
    ctx.valid = true;
    return ctx;
}

int AnimationMerger::driveFingersLive(
    Ogre::SkeletonInstance* skel,
    const std::array<std::array<float, 3>, kFingerSlots>& frameDirs,
    const FingerLiveDriveContext& ctx,
    const std::array<std::array<float, 3>, kFingerSlots>* neutralDirs)
{
    if (!skel || !ctx.valid || !neutralDirs)
        return 0;
    const int nBones = static_cast<int>(skel->getNumBones());
    auto parentBindWorld = [&](unsigned short handle) -> Ogre::Quaternion {
        Ogre::Bone* b = skel->getBone(handle);
        if (auto* p = dynamic_cast<Ogre::Bone*>(b->getParent()))
            return ctx.bindWorld[static_cast<size_t>(p->getHandle())];
        return Ogre::Quaternion::IDENTITY;
    };
    auto segArticWeight = [](int seg) -> float {
        switch (seg) {
        case 0: return 1.f;
        case 1: return 0.72f;
        case 2: return 0.45f;
        default: return 0.35f;
        }
    };

    int animated = 0;
    for (int side = 0; side < 2; ++side) {
        for (int fgr = 0; fgr < MotionInbetween::kFingerCount; ++fgr) {
            const auto& segs = ctx.fingerBones[static_cast<size_t>(
                side * MotionInbetween::kFingerCount + fgr)];
            if (segs.empty())
                continue;
            const int slot0 = fingerSlot(side, fgr, 0);
            const auto& n = (*neutralDirs)[static_cast<size_t>(slot0)];
            Ogre::Vector3 drest(n[0], n[1], n[2]);
            const bool haveNeutral = drest.squaredLength() > 1e-9f;
            const auto& q = frameDirs[static_cast<size_t>(slot0)];
            Ogre::Vector3 dsrc(q[0], q[1], q[2]);
            const bool haveLive = dsrc.squaredLength() > 1e-9f;
            if (!haveNeutral) {
                for (const auto& [seg, handle] : segs) {
                    (void)seg;
                    if (handle >= static_cast<unsigned short>(nBones))
                        continue;
                    Ogre::Bone* b = skel->getBone(handle);
                    b->setManuallyControlled(true);
                    b->setOrientation(ctx.bindLocal[static_cast<size_t>(handle)]);
                    b->needUpdate(true);
                }
                continue;
            }
            drest.normalise();
            if (!haveLive) {
                for (const auto& [seg, handle] : segs) {
                    (void)seg;
                    if (handle >= static_cast<unsigned short>(nBones))
                        continue;
                    Ogre::Bone* b = skel->getBone(handle);
                    b->setManuallyControlled(true);
                    b->setOrientation(ctx.bindLocal[static_cast<size_t>(handle)]);
                    b->needUpdate(true);
                }
                continue;
            }
            dsrc.normalise();
            Ogre::Quaternion srcBendFull = drest.getRotationTo(dsrc);
            for (const auto& [seg, handle] : segs) {
                if (handle >= static_cast<unsigned short>(nBones))
                    continue;
                const Ogre::Vector3 dbind =
                    ctx.tgtBindDir[static_cast<size_t>(handle)];
                if (dbind.squaredLength() < 1e-9f)
                    continue;
                const Ogre::Quaternion srcBend = Ogre::Quaternion::Slerp(
                    segArticWeight(seg), Ogre::Quaternion::IDENTITY,
                    srcBendFull, true);
                const Ogre::Vector3 dtarget =
                    (srcBend * dbind).normalisedCopy();
                const Ogre::Quaternion aimC = dbind.getRotationTo(dtarget);
                const Ogre::Quaternion Ct = ctx.CtInv.Inverse();
                const Ogre::Quaternion aimW = ctx.CtInv * aimC * Ct;
                const Ogre::Quaternion Wbind =
                    ctx.bindWorld[static_cast<size_t>(handle)];
                const Ogre::Quaternion newWorld = aimW * Wbind;
                const Ogre::Quaternion newLocalAtBind =
                    parentBindWorld(handle).Inverse() * newWorld;
                const Ogre::Quaternion kf =
                    ctx.bindLocal[static_cast<size_t>(handle)].Inverse()
                    * newLocalAtBind;
                Ogre::Bone* b = skel->getBone(handle);
                b->setManuallyControlled(true);
                // Hand-local delta (bindLocal * kf): follows wrist rotation
                // without needing derived transforms this frame.
                b->setOrientation(ctx.bindLocal[static_cast<size_t>(handle)]
                                  * kf);
                b->needUpdate(true);
                ++animated;
            }
        }
    }
    return animated;
}

void AnimationMerger::captureFingerNeutralScreenDirs(
    const float* screenCrop33x3,
    std::array<std::array<float, 2>, kFingerSlots>& outDir2d)
{
    outDir2d.fill({0.f, 0.f});
    if (!screenCrop33x3)
        return;
    struct FingerLm {
        int side;
        int finger;
        int wrist;
        int tip;
    };
    static const FingerLm map[] = {
        {0, 0, 16, 22}, {0, 1, 16, 20}, {0, 4, 16, 18},
        {1, 0, 15, 21}, {1, 1, 15, 19}, {1, 4, 15, 17},
    };
    for (const FingerLm& m : map) {
        float dx, dy, len2d;
        if (!MocapPoseIkFk::screenCropFingerDelta2D(
                screenCrop33x3, m.wrist, m.tip, dx, dy, len2d))
            continue;
        const int slot = fingerSlot(m.side, m.finger, 0);
        if (slot >= 0)
            outDir2d[static_cast<size_t>(slot)] = {dx, dy};
    }
}

int AnimationMerger::driveFingersLiveFromScreenCrop(
    Ogre::SkeletonInstance* skel,
    const std::array<std::array<float, 2>, kFingerSlots>& neutralDir2d,
    const std::array<std::array<float, 2>, kFingerSlots>& liveDir2d,
    const FingerLiveDriveContext& ctx,
    int sideMask)
{
    if (!skel || !ctx.valid)
        return 0;
    const int nBones = static_cast<int>(skel->getNumBones());
    const Ogre::Quaternion Ct = ctx.CtInv.Inverse();
    auto parentBindWorld = [&](unsigned short handle) -> Ogre::Quaternion {
        Ogre::Bone* b = skel->getBone(handle);
        if (auto* p = dynamic_cast<Ogre::Bone*>(b->getParent()))
            return ctx.bindWorld[static_cast<size_t>(p->getHandle())];
        return Ogre::Quaternion::IDENTITY;
    };
    auto segArticWeight = [](int seg) -> float {
        switch (seg) {
        case 0: return 1.f;
        case 1: return 0.72f;
        case 2: return 0.45f;
        default: return 0.35f;
        }
    };
    struct FingerLm {
        int side;
        int finger;
        int wrist;
        int tip;
    };
    static const FingerLm map[] = {
        {0, 0, 16, 22}, {0, 1, 16, 20}, {0, 4, 16, 18},
        {1, 0, 15, 21}, {1, 1, 15, 19}, {1, 4, 15, 17},
    };

    int animated = 0;
    auto resetToBind = [&](int side, int finger) {
        const auto& segs = ctx.fingerBones[static_cast<size_t>(
            side * MotionInbetween::kFingerCount + finger)];
        for (const auto& [seg, handle] : segs) {
            (void)seg;
            if (handle >= static_cast<unsigned short>(nBones))
                continue;
            Ogre::Bone* b = skel->getBone(handle);
            b->setManuallyControlled(true);
            b->setOrientation(ctx.bindLocal[static_cast<size_t>(handle)]);
            b->needUpdate(true);
            ++animated;
        }
    };
    auto applyCurl = [&](int side, int finger, float curl) {
        const auto& segs = ctx.fingerBones[static_cast<size_t>(
            side * MotionInbetween::kFingerCount + finger)];
        if (segs.empty())
            return;
        const Ogre::Vector3 fax = ctx.flexAxis[static_cast<size_t>(side)];
        if (fax.squaredLength() < 1e-9f)
            return;
        for (const auto& [seg, handle] : segs) {
            if (handle >= static_cast<unsigned short>(nBones))
                continue;
            const Ogre::Vector3 dbind =
                ctx.tgtBindDir[static_cast<size_t>(handle)];
            if (dbind.squaredLength() < 1e-9f)
                continue;
            const float segCurl = curl * segArticWeight(seg);
            const Ogre::Quaternion aimC(Ogre::Radian(segCurl), fax);
            const Ogre::Quaternion aimW = ctx.CtInv * aimC * Ct;
            const Ogre::Quaternion Wbind =
                ctx.bindWorld[static_cast<size_t>(handle)];
            const Ogre::Quaternion newWorld = aimW * Wbind;
            const Ogre::Quaternion newLocalAtBind =
                parentBindWorld(handle).Inverse() * newWorld;
            const Ogre::Quaternion kf =
                ctx.bindLocal[static_cast<size_t>(handle)].Inverse()
                * newLocalAtBind;
            Ogre::Bone* b = skel->getBone(handle);
            b->setManuallyControlled(true);
            b->setOrientation(ctx.bindLocal[static_cast<size_t>(handle)] * kf);
            b->needUpdate(true);
            ++animated;
        }
    };

    float curlByFinger[2][5];
    for (int s = 0; s < 2; ++s)
        for (int f = 0; f < 5; ++f)
            curlByFinger[s][f] = -1.f;

    for (const FingerLm& m : map) {
        if ((sideMask & (1 << m.side)) == 0)
            continue;
        const int slot0 = fingerSlot(m.side, m.finger, 0);
        if (slot0 < 0)
            continue;
        const auto& n2 = neutralDir2d[static_cast<size_t>(slot0)];
        const auto& l2 = liveDir2d[static_cast<size_t>(slot0)];
        const float nlen =
            std::sqrt(n2[0] * n2[0] + n2[1] * n2[1]);
        const float llen =
            std::sqrt(l2[0] * l2[0] + l2[1] * l2[1]);
        if (nlen < 1e-5f || llen < 1e-5f) {
            resetToBind(m.side, m.finger);
            continue;
        }
        // Facing the camera, curling shortens wrist→tip in 2D much more than
        // it rotates the 2D direction. Use length as the primary curl signal.
        const float shorten =
            std::clamp((nlen - llen) / (nlen * 0.70f), 0.f, 1.f);
        const float ndx = n2[0] / nlen, ndy = n2[1] / nlen;
        const float ldx = l2[0] / llen, ldy = l2[1] / llen;
        const float dot = ndx * ldx + ndy * ldy;
        const float cross = ndx * ldy - ndy * ldx;
        float rot = std::atan2(cross, dot);
        rot = std::clamp(rot, -0.6f, 0.6f);
        float curl = shorten * 1.45f + rot * 0.25f;
        curl = std::clamp(curl, 0.f, 1.55f);
        curlByFinger[m.side][m.finger] = curl;
        applyCurl(m.side, m.finger, curl);
    }
    for (int side = 0; side < 2; ++side) {
        if ((sideMask & (1 << side)) == 0)
            continue;
        const float idx = curlByFinger[side][1];
        const float pnk = curlByFinger[side][4];
        if (idx < 0.f || pnk < 0.f) {
            resetToBind(side, 2);
            resetToBind(side, 3);
            continue;
        }
        applyCurl(side, 2, 0.60f * idx + 0.40f * pnk);
        applyCurl(side, 3, 0.35f * idx + 0.65f * pnk);
    }
    return animated;
}

int AnimationMerger::driveFingersLiveFromFlex(
    Ogre::SkeletonInstance* skel,
    const std::array<float, kFingerSlots>& liveFlexRad,
    const std::array<float, kFingerSlots>& neutralFlexRad,
    const FingerLiveDriveContext& ctx)
{
    if (!skel || !ctx.valid)
        return 0;
    const int nBones = static_cast<int>(skel->getNumBones());
    const Ogre::Quaternion Ct = ctx.CtInv.Inverse();
    auto parentBindWorld = [&](unsigned short handle) -> Ogre::Quaternion {
        Ogre::Bone* b = skel->getBone(handle);
        if (auto* p = dynamic_cast<Ogre::Bone*>(b->getParent()))
            return ctx.bindWorld[static_cast<size_t>(p->getHandle())];
        return Ogre::Quaternion::IDENTITY;
    };

    int animated = 0;
    for (int side = 0; side < 2; ++side) {
        const Ogre::Vector3 fax = ctx.flexAxis[static_cast<size_t>(side)];
        if (fax.squaredLength() < 1e-9f)
            continue;
        for (int fgr = 0; fgr < MotionInbetween::kFingerCount; ++fgr) {
            const auto& segs = ctx.fingerBones[static_cast<size_t>(
                side * MotionInbetween::kFingerCount + fgr)];
            if (segs.empty())
                continue;
            for (const auto& [seg, handle] : segs) {
                if (handle >= static_cast<unsigned short>(nBones))
                    continue;
                const int slot = fingerSlot(side, fgr, std::min(seg, kFingerSegs - 1));
                if (slot < 0)
                    continue;
                const float live = liveFlexRad[static_cast<size_t>(slot)];
                const float rest = neutralFlexRad[static_cast<size_t>(slot)];
                if (live < 0.f || rest < 0.f)
                    continue;
                float delta = live - rest;
                delta = std::clamp(delta, -0.25f, 1.65f);
                const Ogre::Vector3 dbind =
                    ctx.tgtBindDir[static_cast<size_t>(handle)];
                if (dbind.squaredLength() < 1e-9f)
                    continue;
                const Ogre::Quaternion aimC(Ogre::Radian(delta), fax);
                const Ogre::Quaternion aimW = ctx.CtInv * aimC * Ct;
                const Ogre::Quaternion Wbind =
                    ctx.bindWorld[static_cast<size_t>(handle)];
                const Ogre::Quaternion newWorld = aimW * Wbind;
                const Ogre::Quaternion newLocalAtBind =
                    parentBindWorld(handle).Inverse() * newWorld;
                const Ogre::Quaternion kf =
                    ctx.bindLocal[static_cast<size_t>(handle)].Inverse()
                    * newLocalAtBind;
                Ogre::Bone* b = skel->getBone(handle);
                b->setManuallyControlled(true);
                b->setOrientation(ctx.bindLocal[static_cast<size_t>(handle)]
                                  * kf);
                b->needUpdate(true);
                ++animated;
            }
        }
    }
    return animated;
}
#endif

AnimationMerger::ApplyMotionResult AnimationMerger::applyMotionClip(
    Ogre::Skeleton* skel,
    const std::string& animName,
    const std::vector<std::vector<std::array<float, 4>>>& clipQuats,
    int fps,
    bool worldFrame,
    const std::vector<std::array<float, 4>>& cmuRestWorld,
    bool refineWithModel,
    int refineStride,
    bool yaw180,
    const std::vector<std::array<float, 3>>& clipRestDir,
    bool modelClip,
    const std::vector<float>& clipRootY,
    bool verticalDescent,
    bool cmuLibraryHandedness)
{
    ApplyMotionResult res;
    if (!skel) { res.error = QStringLiteral("no skeleton"); return res; }
    if (clipQuats.empty()) { res.error = QStringLiteral("empty motion clip"); return res; }
    if (fps <= 0) fps = 30;
    const int frames = static_cast<int>(clipQuats.size());
    // #838 V2: a 52-joint clip folds the fingers in as canonical joints 22..51,
    // so they retarget through the SAME body path (no separate applyFingerCurl).
    // Detect from the clip width; V1 (22-joint) is unchanged. The joint count
    // and the bone→role matcher both switch on this.
    const int canonN = (!clipQuats.empty()
        && static_cast<int>(clipQuats[0].size())
               >= MotionInbetween::canonicalJointCountV2())
        ? MotionInbetween::canonicalJointCountV2()   // 52 (V2)
        : MotionInbetween::canonicalJointCount();     // 22 (V1)
    const bool clipIsV2 = (canonN == MotionInbetween::canonicalJointCountV2());
    auto matchBoneToCanon = [clipIsV2](const QString& bn) {
        return clipIsV2 ? MotionInbetween::canonicalIndexForBoneV2(bn)
                        : MotionInbetween::canonicalIndexForBone(bn);
    };
    // Guard: every frame must carry all canonical joints — clipQ() indexes up to
    // canonN, and a malformed/downloaded/generated clip with a short frame would
    // otherwise crash during retargeting.
    {
        for (int f = 0; f < frames; ++f)
            if (static_cast<int>(clipQuats[f].size()) < canonN) {
                res.error = QStringLiteral("motion clip frame %1 has %2 joints; expected >= %3")
                    .arg(f).arg(clipQuats[f].size()).arg(canonN);
                return res;
            }
    }
    const float dt = 1.0f / static_cast<float>(fps);
    const float length = (frames - 1) * dt;
    res.frames = frames;
    res.length = length;

    // Map each skeleton bone -> canonical joint index (the #409 retargeting).
    const int nBones = static_cast<int>(skel->getNumBones());
    std::vector<int> boneToCanon(nBones, -1);
    std::vector<char> canonSeen(canonN, 0);
    int distinct = 0;
    for (int i = 0; i < nBones; ++i) {
        const QString bn = QString::fromStdString(skel->getBone(static_cast<unsigned short>(i))->getName());
        const int c = matchBoneToCanon(bn);
        if (c >= 0 && c < static_cast<int>(canonSeen.size())) {
            boneToCanon[i] = c;
            if (!canonSeen[c]) { canonSeen[c] = 1; ++distinct; }
        }
    }
    if (cmuLibraryHandedness)
        compensateCanonicalHandedness(skel, boneToCanon);

    res.canonicalJoints = distinct;
    // Bones-per-role: rigs segment chains differently (Mixamo has Spine AND
    // Spine1 in the canonical "abdomen" span; some rigs multi-segment arms).
    // Applying the full role delta to EVERY mapped bone would bend the chain
    // N times over — distribute it instead: each of the N bones gets the
    // N-th fractional rotation so the chain's total matches the clip.
    std::vector<int> canonDup(canonN, 0);   // 22 (V1) or 52 (V2)
    for (int i = 0; i < nBones; ++i)
        if (boneToCanon[i] >= 0) ++canonDup[boneToCanon[i]];
    // Need a humanoid-ish rig: require a reasonable share of the BODY roles. The
    // gate is always against the 22 body joints (fingers are a bonus in V2, and
    // many rigs have sparse/absent finger bones — they must not fail the gate).
    // `distinct` counts body+finger for V2, so cap it at the body count for the
    // threshold so a finger-rich rig can't mask a missing body.
    {
        int bodyDistinct = 0;
        std::vector<char> seenBody(MotionInbetween::canonicalJointCount(), 0);
        for (int i = 0; i < nBones; ++i) {
            const int c = boneToCanon[i];
            if (c >= 0 && c < MotionInbetween::canonicalJointCount()
                && !seenBody[c]) { seenBody[c] = 1; ++bodyDistinct; }
        }
        if (bodyDistinct < (MotionInbetween::canonicalJointCount() * 1) / 2) {
            res.error = QStringLiteral(
                "skeleton resolved only %1/%2 canonical BODY joints — not a humanoid rig the "
                "motion library can retarget onto")
                .arg(bodyDistinct).arg(MotionInbetween::canonicalJointCount());
            return res;
        }
    }

    if (skel->hasAnimation(animName))
        skel->removeAnimation(animName);
    // #854: the freshly (re)created clip has NO arm-space applied. A stale
    // stored angle from a prior generation of the same name would make a
    // re-request of that same angle a no-op (delta 0) on the new keyframes —
    // forget it.
    if (skel->getNumBones() > 0) {
        skel->getBone(0)->getUserObjectBindings().eraseUserAny(
            armSpaceKey(animName));
        // #856: same for the foot-pin marker — a regenerated clip is unpinned.
        skel->getBone(0)->getUserObjectBindings().eraseUserAny(
            "qtme.footpin." + animName);
    }
    Ogre::Animation* anim = skel->createAnimation(animName, length);
    anim->setInterpolationMode(Ogre::Animation::IM_LINEAR);
    anim->setRotationInterpolationMode(Ogre::Animation::RIM_LINEAR);

    // ===== BIND-REFERENCED WORLD RETARGET (preferred when available) =====
    // Clips extracted by --dump-canonical carry the SOURCE rig's bind-pose
    // world orientations (restWorld). Both rigs' binds are T-poses, so the
    // correspondence source-bind ↔ target-bind is exact:
    //     Δworld(f) = Ws(f) · Ws_bind⁻¹          (canonical world axes)
    //     Wt(f)     = Δworld(f) · Wt_bind
    //     local(f)  = Wt_parent(f)⁻¹ · Wt(f)     (hierarchy-ordered)
    // Generated clips therefore reference the TARGET BIND — no pose from any
    // other animation is involved (the standing-pose harvest below is only
    // the fallback for restWorld-less clips, e.g. the CMU-built libraries).
    // A reference orientation is PRESENT when it is a valid (unit-norm) quat;
    // unresolved roles in dumps are zero-filled. Identity counts — the v5
    // model's canonical triple (#858) is restWorld = identity ×22 by
    // construction, and treating it as "absent" would silently demote model
    // clips to the synthetic-standing-pose path.
    bool haveRestWorld = false;
    if (worldFrame
        && cmuRestWorld.size() == static_cast<size_t>(canonN)) {   // 22 or 52
        for (const auto& q : cmuRestWorld)
            if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] > 0.25f) {
                haveRestWorld = true;
                break;
            }
    }
    const int Jc = canonN;   // 22 (V1) or 52 (V2, fingers folded in)
    auto canonParentJc = [clipIsV2](int i) {
        return clipIsV2 ? MotionInbetween::canonicalParentOfV2(i)
                        : MotionInbetween::canonicalParentOf(i);
    };

    // The rig's STANDING pose the legacy transport composes onto — declared
    // here so the SYNTHETIC bind-referenced path below can fill it before
    // the (contaminating) animation harvest is even considered.
    struct StandXform { Ogre::Quaternion rot; Ogre::Vector3 pos; Ogre::Vector3 scale; bool has = false; };
    std::unordered_map<unsigned short, StandXform> standPose;
    bool synthStand = false;

    if (!clipRestDir.empty()) {
        const TargetBindFrame tb = readTargetBindFrame(skel, boneToCanon,
                                                       clipIsV2);
        const Ogre::Quaternion CtInv = tb.Ct.Inverse();

        if (haveRestWorld) {
            // PER-FRAME DIRECTION MATCHING. "Bind" poses are NOT trustworthy
            // T-poses across importers (Assimp's reset pose for Mixamo FBX is
            // whatever the file stored), so bind-to-bind delta transplant
            // breaks cross-rig. Each rig IS self-consistent though: the source
            // bone's constant LOCAL direction axis a_s = Ws_ref⁻¹·ds_ref
            // rotates to ds(f) = Ws(f)·a_s each frame, and the target bone is
            // aimed at exactly that world direction:
            //     Wt(f) = arc(dt_bind → ds(f)) · Wt_bind
            // hierarchy-ordered, twist about the bone intentionally not
            // transported (roll is the least visible DoF). The generated clip
            // therefore references the TARGET BIND — no pose from any other
            // animation is involved.
            std::vector<Ogre::Vector3> srcLocalAxis(
                static_cast<size_t>(Jc), Ogre::Vector3::ZERO);
            for (int c = 0; c < Jc; ++c) {
                const auto& sd = clipRestDir[static_cast<size_t>(c)];
                Ogre::Vector3 srcDir(sd[0], sd[1], sd[2]);
                if (srcDir.squaredLength() > 1e-8f
                    && tb.tgtBindDir[static_cast<size_t>(c)].squaredLength()
                           > 1e-8f) {
                    srcDir.normalise();
                    const auto& q = cmuRestWorld[static_cast<size_t>(c)];
                    const Ogre::Quaternion restQ(q[3], q[0], q[1], q[2]);
                    srcLocalAxis[static_cast<size_t>(c)] =
                        restQ.Inverse() * srcDir;
                }
            }
            auto clipQ = [&](int frame, int joint) -> Ogre::Quaternion {
                const auto& q = clipQuats[frame][joint];
                return Ogre::Quaternion(q[3], q[0], q[1], q[2]);   // (w,x,y,z)
            };
            // FINGER PLAUSIBILITY GATE (V2). Some source rigs carry garbage
            // finger data — e.g. "Animated Human Low Poly": the index chain
            // articulates up to 176° RELATIVE TO THE HAND (folds through the
            // palm; a full fist is ~100°), and the one-finger-drives-four
            // replication copies it to every finger. Transporting impossible
            // curls onto a realistic hand splays the fingers backwards, so
            // when ANY segment of a chain exceeds the anatomical limit at any
            // frame, HOLD THE WHOLE CHAIN AT BIND (neutral beats broken).
            // Per-chain: a rig with a good thumb but bad fingers keeps the
            // thumb animation.
            std::vector<char> fingerChainUntrusted(
                static_cast<size_t>(canonN), 0);
            if (clipIsV2 && haveRestWorld) {
                constexpr float kMaxFingerArticulationDeg = 120.0f;
                int droppedChains = 0;
                for (int side = 0; side < 2; ++side)
                    for (int fgr = 0; fgr < 5; ++fgr) {
                        float maxDeg = 0.0f;
                        for (int seg = 0; seg < 3; ++seg) {
                            const int c = MotionInbetween::fingerJointIndexV2(
                                side, fgr, seg);
                            const int pc =
                                MotionInbetween::canonicalParentOfV2(c);
                            if (c < 0 || c >= canonN || pc < 0) continue;
                            const auto& rq =
                                cmuRestWorld[static_cast<size_t>(c)];
                            const auto& prq =
                                cmuRestWorld[static_cast<size_t>(pc)];
                            if (rq[0]*rq[0] + rq[1]*rq[1] + rq[2]*rq[2]
                                    + rq[3]*rq[3] < 0.25f ||
                                prq[0]*prq[0] + prq[1]*prq[1] + prq[2]*prq[2]
                                    + prq[3]*prq[3] < 0.25f)
                                continue;   // unpopulated → holds bind anyway
                            const Ogre::Quaternion refQ(rq[3], rq[0], rq[1],
                                                        rq[2]);
                            const Ogre::Quaternion prefQ(prq[3], prq[0],
                                                         prq[1], prq[2]);
                            for (int f = 0; f < frames; ++f) {
                                const auto& cq =
                                    clipQuats[static_cast<size_t>(f)]
                                             [static_cast<size_t>(c)];
                                if (cq[0]*cq[0] + cq[1]*cq[1] + cq[2]*cq[2]
                                        + cq[3]*cq[3] < 0.25f)
                                    continue;
                                const Ogre::Quaternion Df =
                                    clipQ(f, c) * refQ.Inverse();
                                const Ogre::Quaternion Dp =
                                    clipQ(f, pc) * prefQ.Inverse();
                                const Ogre::Quaternion Drel =
                                    Dp.Inverse() * Df;
                                const float deg = 2.0f * Ogre::Math::ACos(
                                    std::min(1.0f, std::abs(Drel.w)))
                                        .valueDegrees();
                                if (deg > maxDeg) maxDeg = deg;
                            }
                        }
                        if (maxDeg > kMaxFingerArticulationDeg) {
                            ++droppedChains;
                            for (int seg = 0; seg < 3; ++seg) {
                                const int c =
                                    MotionInbetween::fingerJointIndexV2(
                                        side, fgr, seg);
                                if (c >= 0 && c < canonN)
                                    fingerChainUntrusted
                                        [static_cast<size_t>(c)] = 1;
                            }
                        }
                    }
                if (droppedChains > 0) {
                    Ogre::LogManager::getSingleton().logMessage(
                        "applyMotionClip: dropped " +
                        std::to_string(droppedChains) +
                        " implausible finger chain(s) (>120deg articulation)"
                        " — holding bind");
                    SentryReporter::addBreadcrumb(
                        QStringLiteral("ai.assist.text_to_motion"),
                        QStringLiteral("finger plausibility gate: %1 chain(s)"
                                       " held at bind")
                            .arg(droppedChains));
                }
            }
            // HAND-BASIS finger transport frames. The canonical-frame
            // conjugation (Ct⁻¹·Drel·Ct) assumes the source's extraction
            // frame C equals the target bind frame Ct; when they disagree
            // (the documented C≠Ct class) the curl AXIS lands wrong on the
            // target — fingers bend sideways/backward instead of toward the
            // palm ("Animated Human Low Poly" jump). Fingers articulate
            // relative to the HAND, so re-express the curl in each side's
            // hand basis instead: d = hand/finger direction, p = palmward
            // (thumb direction orthogonalized against d — the thumb is on
            // the palm side in any bind), b = d×p. B = R_target·R_source⁻¹
            // then replaces the Ct conjugation — no dependence on C at all.
            // Falls back to the Ct conjugation when either basis degenerates.
            Ogre::Quaternion fingerBasisMap[2] = {Ogre::Quaternion::IDENTITY,
                                                  Ogre::Quaternion::IDENTITY};
            bool fingerBasisOk[2] = {false, false};
            if (clipIsV2 && !clipRestDir.empty()
                && static_cast<int>(clipRestDir.size()) >= canonN) {
                // Basis from a hand direction + a flexion axis: x = hand
                // direction, y = flexion axis (orthogonalized), z = x×y.
                auto basisQuat = [](Ogre::Vector3 d, Ogre::Vector3 fl,
                                    Ogre::Quaternion& out) -> bool {
                    if (d.squaredLength() < 1e-8f || fl.squaredLength() < 1e-8f)
                        return false;
                    d.normalise();
                    fl = fl - d * fl.dotProduct(d);
                    if (fl.squaredLength() < 1e-6f)
                        return false;
                    fl.normalise();
                    Ogre::Matrix3 m;
                    m.SetColumn(0, d);
                    m.SetColumn(1, fl);
                    m.SetColumn(2, d.crossProduct(fl));
                    out = Ogre::Quaternion(m);
                    return true;
                };
                for (int side = 0; side < 2; ++side) {
                    const int hand = side == 0 ? 9 : 13;
                    const int thumb0 =
                        MotionInbetween::fingerJointIndexV2(side, 0, 0);
                    if (thumb0 < 0 || thumb0 >= canonN
                        || hand >= static_cast<int>(tb.tgtBindDir.size())
                        || thumb0 >= static_cast<int>(tb.tgtBindDir.size()))
                        continue;
                    const auto& hd = clipRestDir[static_cast<size_t>(hand)];
                    const Ogre::Vector3 dS(hd[0], hd[1], hd[2]);

                    // SOURCE flexion axis: SELF-CALIBRATED from the curl data
                    // itself — the dominant rotation axis of the non-thumb
                    // finger deltas (power-iterated axis covariance). Rigs
                    // whose thumb rest points oddly (block-hand exports) fool
                    // a thumb-based palmward guess, but the fingers' own curl
                    // axis is ground truth (measured 0.99+ concentration on
                    // such rigs). Sign: fingers FLEX far more than they
                    // extend, so the net signed curl points along +flexion.
                    Ogre::Matrix3 cov(0, 0, 0, 0, 0, 0, 0, 0, 0);
                    float totW = 0.0f;
                    for (int fgr = 1; fgr < 5; ++fgr)
                        for (int seg = 0; seg < 3; ++seg) {
                            const int c = MotionInbetween::fingerJointIndexV2(
                                side, fgr, seg);
                            const int pc =
                                MotionInbetween::canonicalParentOfV2(c);
                            if (c < 0 || c >= canonN || pc < 0) continue;
                            // Chains the plausibility gate rejected must not
                            // calibrate the axis either.
                            if (fingerChainUntrusted[static_cast<size_t>(c)])
                                continue;
                            const auto& rq =
                                cmuRestWorld[static_cast<size_t>(c)];
                            const auto& prq =
                                cmuRestWorld[static_cast<size_t>(pc)];
                            if (rq[0]*rq[0] + rq[1]*rq[1] + rq[2]*rq[2]
                                    + rq[3]*rq[3] < 0.25f ||
                                prq[0]*prq[0] + prq[1]*prq[1] + prq[2]*prq[2]
                                    + prq[3]*prq[3] < 0.25f) continue;
                            const Ogre::Quaternion refQ(rq[3], rq[0], rq[1],
                                                        rq[2]);
                            const Ogre::Quaternion prefQ(prq[3], prq[0],
                                                         prq[1], prq[2]);
                            for (int f = 0; f < frames; ++f) {
                                const Ogre::Quaternion Df =
                                    clipQ(f, c) * refQ.Inverse();
                                const Ogre::Quaternion Dp =
                                    clipQ(f, pc) * prefQ.Inverse();
                                Ogre::Quaternion Drel = Dp.Inverse() * Df;
                                if (Drel.w < 0) Drel = -Drel;
                                Ogre::Vector3 ax(Drel.x, Drel.y, Drel.z);
                                const float deg = 2.0f * Ogre::Math::ACos(
                                    std::min(1.0f, Drel.w)).valueDegrees();
                                if (deg < 10.0f
                                    || ax.squaredLength() < 1e-10f)
                                    continue;
                                ax.normalise();
                                for (int r = 0; r < 3; ++r)
                                    for (int cc = 0; cc < 3; ++cc)
                                        cov[r][cc] += deg * ax[r] * ax[cc];
                                totW += deg;
                            }
                        }
                    Ogre::Vector3 flexS = Ogre::Vector3::ZERO;
                    float concentration = 0.0f;
                    if (totW > 30.0f) {   // enough articulation to calibrate
                        Ogre::Vector3 v(1, 1, 1);
                        for (int it = 0; it < 40; ++it) {
                            v = cov * v;
                            if (v.squaredLength() < 1e-20f) break;
                            v.normalise();
                        }
                        // concentration + net sign in one pass
                        float aligned = 0.0f, net = 0.0f;
                        for (int fgr = 1; fgr < 5; ++fgr)
                            for (int seg = 0; seg < 3; ++seg) {
                                const int c =
                                    MotionInbetween::fingerJointIndexV2(
                                        side, fgr, seg);
                                const int pc =
                                    MotionInbetween::canonicalParentOfV2(c);
                                if (c < 0 || c >= canonN || pc < 0) continue;
                                if (fingerChainUntrusted
                                        [static_cast<size_t>(c)]) continue;
                                const auto& rq =
                                    cmuRestWorld[static_cast<size_t>(c)];
                                const auto& prq =
                                    cmuRestWorld[static_cast<size_t>(pc)];
                                if (rq[0]*rq[0] + rq[1]*rq[1] + rq[2]*rq[2]
                                        + rq[3]*rq[3] < 0.25f ||
                                    prq[0]*prq[0] + prq[1]*prq[1]
                                        + prq[2]*prq[2] + prq[3]*prq[3]
                                        < 0.25f) continue;
                                const Ogre::Quaternion refQ(rq[3], rq[0],
                                                            rq[1], rq[2]);
                                const Ogre::Quaternion prefQ(prq[3], prq[0],
                                                             prq[1], prq[2]);
                                for (int f = 0; f < frames; ++f) {
                                    const Ogre::Quaternion Df =
                                        clipQ(f, c) * refQ.Inverse();
                                    const Ogre::Quaternion Dp =
                                        clipQ(f, pc) * prefQ.Inverse();
                                    Ogre::Quaternion Drel =
                                        Dp.Inverse() * Df;
                                    if (Drel.w < 0) Drel = -Drel;
                                    Ogre::Vector3 ax(Drel.x, Drel.y, Drel.z);
                                    const float deg = 2.0f * Ogre::Math::ACos(
                                        std::min(1.0f, Drel.w))
                                            .valueDegrees();
                                    if (deg < 10.0f
                                        || ax.squaredLength() < 1e-10f)
                                        continue;
                                    ax.normalise();
                                    const float d = ax.dotProduct(v);
                                    aligned += deg * std::abs(d);
                                    net += deg * (d >= 0 ? 1.0f : -1.0f);
                                }
                            }
                        concentration = aligned / totW;
                        flexS = (net >= 0.0f) ? v : -v;
                    }

                    // TARGET flexion axis: the KNUCKLE LINE (seg0 bone
                    // positions of index..pinky) — anatomically the axis
                    // fingers flex about; sign chosen so +rotation curls
                    // toward the palm (thumb side).
                    Ogre::Vector3 flexT = Ogre::Vector3::ZERO;
                    {
                        std::vector<Ogre::Vector3> kn;
                        for (int fgr = 1; fgr < 5; ++fgr) {
                            const int c = MotionInbetween::fingerJointIndexV2(
                                side, fgr, 0);
                            if (c < 0 || c >= canonN) continue;
                            const int bi =
                                tb.roleBoneIdx[static_cast<size_t>(c)];
                            if (bi >= 0)
                                kn.push_back(
                                    tb.bindPos[static_cast<size_t>(bi)]);
                        }
                        const int hb =
                            tb.roleBoneIdx[static_cast<size_t>(hand)];
                        const int tbi =
                            tb.roleBoneIdx[static_cast<size_t>(thumb0)];
                        if (kn.size() >= 2 && hb >= 0 && tbi >= 0) {
                            Ogre::Vector3 line = kn.back() - kn.front();
                            const Ogre::Vector3 dT =
                                tb.tgtBindDir[static_cast<size_t>(hand)]
                                    .normalisedCopy();
                            line = line - dT * line.dotProduct(dT);
                            if (line.squaredLength() > 1e-10f) {
                                line.normalise();
                                // palm side = where the thumb sits
                                Ogre::Vector3 palm =
                                    tb.bindPos[static_cast<size_t>(tbi)]
                                    - tb.bindPos[static_cast<size_t>(hb)];
                                palm = palm - dT * palm.dotProduct(dT);
                                // +rotation about `line` moves fingertips
                                // toward line×dT — require that to be the
                                // palm side.
                                if (line.crossProduct(dT).dotProduct(palm)
                                        < 0.0f)
                                    line = -line;
                                flexT = line;
                            }
                        }
                    }

                    Ogre::Quaternion qs, qt;
                    bool okS = false, okT = false;
                    if (concentration > 0.8f
                        && flexS.squaredLength() > 0.5f)
                        okS = basisQuat(dS, flexS, qs);
                    if (flexT.squaredLength() > 0.5f)
                        okT = basisQuat(
                            tb.tgtBindDir[static_cast<size_t>(hand)], flexT,
                            qt);
                    if (!okS || !okT) {
                        // Fallback: thumb-direction palmward on both sides
                        // (the previous construction — fine for rigs with a
                        // sane thumb rest, e.g. Gregorio).
                        const auto& td =
                            clipRestDir[static_cast<size_t>(thumb0)];
                        Ogre::Vector3 tS(td[0], td[1], td[2]);
                        // flexion ≈ d × palmward: build via palmward and
                        // rotate: basis (d, palmward, d×palmward) is the same
                        // family — reuse it directly on both sides.
                        okS = basisQuat(dS, dS.crossProduct(
                                  tS - dS * tS.dotProduct(
                                      dS.normalisedCopy())), qs);
                        const Ogre::Vector3 dT =
                            tb.tgtBindDir[static_cast<size_t>(hand)];
                        const Ogre::Vector3 tT =
                            tb.tgtBindDir[static_cast<size_t>(thumb0)];
                        okT = basisQuat(dT, dT.crossProduct(
                                  tT - dT * tT.dotProduct(
                                      dT.normalisedCopy())), qt);
                    }
                    if (okS && okT) {
                        fingerBasisMap[side] = qt * qs.Inverse();
                        fingerBasisOk[side] = true;
                    }
                }
            }
            // #857: STABILIZED TWIST TRANSPORT. The aim above transports the
            // bone's DIRECTION but inherits the target bind's roll — gesture-
            // heavy clips (dance, wave) lose forearm/spine roll and read
            // flat. Decompose the source's frame rotation into swing (the
            // direction, already transported) + twist about the bone axis:
            //     Δ(f)   = Ws(f) · Ws_bind⁻¹              (canonical axes)
            //     aim(f) = arc(d_bind → d(f))
            //     θ(f)   = signed angle of aim(f)⁻¹·Δ(f) about d_bind
            // θ is wrapped to (−π,π] then UNWRAPPED across frames (a ±180°
            // pop near the shortest-arc degeneracy would otherwise flip the
            // roll direction mid-clip once a gain ≠ 1 scales it) and composed
            // on the target about the SAME pole the source decomposed about:
            //     Qbase  = arc(dt_bind → d_ref) · Wt_bind      (once per bone)
            //     Wt(f)  = twist(θ·gain, ds) · arc(d_ref → ds) · Qbase
            // Recomposing per-frame from the TARGET BIND direction instead
            // (the pre-#857 form) decomposes about a different pole than the
            // source did — the roll component "between" the two poles leaks
            // into swing and inflates amplitude (measured: Mixamo self-parity
            // arm amp +38%, elbow error 3.3°→6.3°). With matched poles the
            // per-frame relative motion is the source's world delta exactly,
            // conjugated into target axes — direction stays absolute, roll
            // transports losslessly, self-parity drops below the aim-only
            // baseline. Per-role gains: collars damped (they share the
            // shoulder line's roll), everything else transports fully.
            static const float kTwistGain[22] = {
                1.f, 1.f, 1.f, 1.f, 1.f, 1.f,   // spine chain + head
                0.5f, 1.f, 1.f, 1.f,            // rcollar damped, right arm
                0.5f, 1.f, 1.f, 1.f,            // lcollar damped, left arm
                1.f, 1.f, 1.f, 1.f,             // right leg + foot
                1.f, 1.f, 1.f, 1.f };           // left leg + foot
            // Per-role twist caps (radians). The single generous 150° cap
            // let noisy source roll through on spine/neck/head — takes whose
            // roll was invisible pre-#857 (dropped) came back with flailing
            // arms and a thrown-back head. Roll matters most on forearms;
            // the axial chain needs very little. Hip keeps a wide cap: it
            // carries genuine facing turns (salsa).
            // MODEL clips get tight per-role caps (the from-scratch model's
            // roll is noisy — uncapped it flails). AUTHORED/self-parity clips
            // carry legitimate large roll and must NOT be capped (measured:
            // caps collapse mouse elbow 180°→124°, parity 5.1°→3.1° off).
            static const float kTwistCapModel[22] = {
                2.62f, 0.79f, 0.79f, 0.52f, 0.52f, 0.52f,  // hip, spine 45°, neck/head 30°
                0.52f, 1.57f, 1.57f, 1.57f,                // rcollar 30°, right arm 90°
                0.52f, 1.57f, 1.57f, 1.57f,                // lcollar 30°, left arm 90°
                1.05f, 1.05f, 1.05f, 0.79f,                // right leg 60°, foot 45°
                1.05f, 1.05f, 1.05f, 0.79f };              // left leg 60°, foot 45°
            static const float kTwistCapOpen[22] = {
                3.15f, 3.15f, 3.15f, 3.15f, 3.15f, 3.15f,
                3.15f, 3.15f, 3.15f, 3.15f,
                3.15f, 3.15f, 3.15f, 3.15f,
                3.15f, 3.15f, 3.15f, 3.15f,
                3.15f, 3.15f, 3.15f, 3.15f };
            const float* kTwistCapRole = modelClip ? kTwistCapModel
                                                   : kTwistCapOpen;
            // Bounds-safe accessors: the cap/gain arrays are body-only (22). V2
            // finger joints (c>=22) use finger defaults — full gain (fingers
            // curl freely) and a 180° cap on template clips / 90° on model
            // clips (finger roll is noisy like the body's).
            auto twistCapAt = [&](int c) -> float {
                return (c < 22) ? kTwistCapRole[c] : (modelClip ? 1.57f : 3.15f);
            };
            // Finger joints (c>=22, V2): ZERO twist gain. A finger's flexion is a
            // SWING (its pointing direction changes), captured by the direction
            // aim below. The twist DoF about the finger axis is exactly the
            // rig-axis-mismatch garbage that over-bends / rotates the hand (the
            // same instability the V1 finger path dropped) — the source & target
            // finger frames don't correspond, so transporting finger twist adds
            // noise, not motion. Body joints keep their tuned gains.
            auto twistGainAt = [&](int c) -> float {
                return (c < 22) ? kTwistGain[c] : 0.0f;
            };
            std::vector<std::vector<float>> twistTheta(
                static_cast<size_t>(frames),
                std::vector<float>(static_cast<size_t>(Jc), 0.0f));
            {
                constexpr float kTau = 2.0f * static_cast<float>(M_PI);
                std::vector<float> prev(static_cast<size_t>(Jc), 0.0f);
                std::vector<char> has(static_cast<size_t>(Jc), 0);
                for (int f = 0; f < frames; ++f)
                    for (int c = 0; c < Jc; ++c) {
                        const Ogre::Vector3& as =
                            srcLocalAxis[static_cast<size_t>(c)];
                        if (as.squaredLength() <= 1e-8f) continue;
                        const auto& rq = cmuRestWorld[static_cast<size_t>(c)];
                        const Ogre::Quaternion restQ(rq[3], rq[0], rq[1], rq[2]);
                        const auto& sd = clipRestDir[static_cast<size_t>(c)];
                        Ogre::Vector3 dbind(sd[0], sd[1], sd[2]);
                        dbind.normalise();
                        const Ogre::Quaternion Wf = clipQ(f, c);
                        const Ogre::Vector3 d = Wf * as;
                        const Ogre::Quaternion delta = Wf * restQ.Inverse();
                        const Ogre::Quaternion aim = dbind.getRotationTo(d);
                        const Ogre::Quaternion tw = aim.Inverse() * delta;
                        float th = 2.0f * std::atan2(
                            tw.x * dbind.x + tw.y * dbind.y + tw.z * dbind.z,
                            tw.w);
                        th = std::remainder(th, kTau);
                        if (has[static_cast<size_t>(c)])
                            th += kTau * std::round(
                                (prev[static_cast<size_t>(c)] - th) / kTau);
                        prev[static_cast<size_t>(c)] = th;
                        has[static_cast<size_t>(c)] = 1;
                        twistTheta[static_cast<size_t>(f)]
                                  [static_cast<size_t>(c)] = std::clamp(
                            th, -twistCapAt(c), twistCapAt(c));
                    }
                // Low-pass the twist trajectories (5-tap binomial). θ comes
                // from per-frame shortest-arc decompositions and jitters near
                // degenerate aims — transporting it raw renders as trembling.
                // Directions are untouched; only the roll is smoothed.
                if (frames >= 5) {
                    for (int c = 0; c < Jc; ++c) {
                        std::vector<float> src(static_cast<size_t>(frames));
                        for (int f = 0; f < frames; ++f)
                            src[static_cast<size_t>(f)] =
                                twistTheta[static_cast<size_t>(f)]
                                          [static_cast<size_t>(c)];
                        static const float k[5] = {1.f, 4.f, 6.f, 4.f, 1.f};
                        for (int f = 0; f < frames; ++f) {
                            float acc = 0.0f, wsum = 0.0f;
                            for (int o = -2; o <= 2; ++o) {
                                const int j = f + o;
                                if (j < 0 || j >= frames) continue;
                                acc += k[o + 2] * src[static_cast<size_t>(j)];
                                wsum += k[o + 2];
                            }
                            twistTheta[static_cast<size_t>(f)]
                                      [static_cast<size_t>(c)] = acc / wsum;
                        }
                    }
                }
            }
            // Reference-aligned roll baseline per bone: aim the target bind
            // at the source's REFERENCE direction once, so every per-frame
            // swing below decomposes about the source's own pole.
            std::vector<Ogre::Vector3> dref(static_cast<size_t>(Jc),
                                            Ogre::Vector3::ZERO);
            for (int c = 0; c < Jc; ++c) {
                const auto& sd = clipRestDir[static_cast<size_t>(c)];
                Ogre::Vector3 v(sd[0], sd[1], sd[2]);
                if (v.squaredLength() > 1e-8f) {
                    v.normalise();
                    dref[static_cast<size_t>(c)] = CtInv * v;
                }
            }
            std::vector<Ogre::Quaternion> Qbase(static_cast<size_t>(nBones),
                                                Ogre::Quaternion::IDENTITY);
            for (int i = 0; i < nBones; ++i) {
                const int c = boneToCanon[i];
                if (c >= 0 && c < Jc
                    && srcLocalAxis[static_cast<size_t>(c)].squaredLength()
                           > 1e-8f) {
                    Qbase[static_cast<size_t>(i)] =
                        tb.tgtBindDir[static_cast<size_t>(c)]
                            .getRotationTo(dref[static_cast<size_t>(c)])
                        * tb.bindWorld[static_cast<size_t>(i)];
                    // NOTE (2026-08-02 investigation): this minimal-arc baseline
                    // keeps the TARGET-BIND's roll about the bone axis, dropping
                    // the roll of the source's bind→reference rotation — the
                    // measured 50-170° arm-chain loss on the Gregorio SELF-
                    // retarget (collar 49° → upper arm 165° → hand 132°; flared
                    // elbows, wrist/nail roll). A source-reference roll-
                    // correction here CANNOT be computed from the clip alone:
                    // stored orientations are conjugated in the EXTRACTION's
                    // frame C (per-clip reference + leveling) while the retarget
                    // bridges with the bind-derived Ct, and C≠Ct by up to ~57°
                    // per clip — orientation-level comparisons across that gap
                    // corrupted even the pelvis (94-173°) when attempted. Fixing
                    // the roll baseline requires the extraction to ALSO export
                    // its C↔bind frame link (schema v5 candidate `bindC`), so
                    // the retarget can reconstruct consistent frames. Direction-
                    // level bridging (this aim) is the only frame-safe transport
                    // without it.
                }
            }
            std::vector<Ogre::NodeAnimationTrack*> tracks(
                static_cast<size_t>(nBones), nullptr);
            for (int i = 0; i < nBones; ++i)
                if (boneToCanon[i] >= 0 && boneToCanon[i] < Jc) {
                    tracks[static_cast<size_t>(i)] = anim->createNodeTrack(
                        static_cast<unsigned short>(i),
                        skel->getBone(static_cast<unsigned short>(i)));
                    ++res.tracksWritten;
                }
            // #838 vertical descent (bind-referenced path): TARGET leg length
            // from the bind-pose derived positions (hip role 0 → foot role 17,
            // else 21), so the clip's leg-normalized rootY scales to this rig.
            const bool doVerticalDescentBR =
                verticalDescent && !clipRootY.empty()
                && static_cast<int>(clipRootY.size()) == frames;
            float tgtLegLenBR = 0.0f;
            if (doVerticalDescentBR) {
                const int hipB = tb.roleBoneIdx[0];
                int footB = tb.roleBoneIdx[17];
                if (footB < 0) footB = tb.roleBoneIdx[21];
                if (hipB >= 0 && footB >= 0)
                    tgtLegLenBR = (tb.bindPos[static_cast<size_t>(hipB)]
                                   - tb.bindPos[static_cast<size_t>(footB)])
                                      .length();
            }
            std::vector<Ogre::Quaternion> W(static_cast<size_t>(nBones));
            for (int f = 0; f < frames; ++f) {
                for (int i : tb.order) {
                    const int pi = tb.parentIdx[static_cast<size_t>(i)];
                    const Ogre::Quaternion Wp = (pi >= 0)
                        ? W[static_cast<size_t>(pi)]
                        : Ogre::Quaternion::IDENTITY;
                    const int c = boneToCanon[i];
                    Ogre::Quaternion local;
                    // ROOT/HIP full-orientation transport — ONLY for descent
                    // (crouch/ground-work) clips (#838). The hip is the FK-chain
                    // base, so its FULL 3-DOF orientation matters: a direction-
                    // aim (hip→abdomen) captures the spine lean but DROPS the
                    // pelvis PITCH, leaving a crouch torso-vertical when it
                    // should tip forward over the hands (the "won't rotate"
                    // bug). Transport the source hip's full rotation relative to
                    // its own reference onto the target bind hip. NOT done for
                    // locomotion — the full delta carries the source's whole-
                    // body FACING (yaw), which must stay locked so a walk
                    // doesn't spin; those clips keep the aim-only path below.
                    if (c == 0 && doVerticalDescentBR
                        && srcLocalAxis[0].squaredLength() > 1e-8f) {
                        const auto& rq = cmuRestWorld[0];
                        const Ogre::Quaternion refQ(rq[3], rq[0], rq[1], rq[2]);
                        const Ogre::Quaternion srcDelta =
                            clipQ(f, 0) * refQ.Inverse();     // canonical-frame
                        const Ogre::Quaternion Wt =
                            srcDelta * Qbase[static_cast<size_t>(i)];
                        local = Wp.Inverse() * Wt;   // Wp == identity (root)
                        W[static_cast<size_t>(i)] = Wt;
                    } else if (clipIsV2 && (c == 9 || c == 13)
                        && fingerBasisOk[c == 9 ? 0 : 1]
                        && haveRestWorld
                        && cmuRestWorld[static_cast<size_t>(c)][0]
                                * cmuRestWorld[static_cast<size_t>(c)][0]
                            + cmuRestWorld[static_cast<size_t>(c)][1]
                                * cmuRestWorld[static_cast<size_t>(c)][1]
                            + cmuRestWorld[static_cast<size_t>(c)][2]
                                * cmuRestWorld[static_cast<size_t>(c)][2]
                            + cmuRestWorld[static_cast<size_t>(c)][3]
                                * cmuRestWorld[static_cast<size_t>(c)][3]
                            > 0.25f) {
                        // HANDS via the self-calibrated hand-basis map. The
                        // aim+twist path transports the hand's DIRECTION but
                        // its ROLL rides the C≠Ct frame mismatch — the raised
                        // arm renders palm-out ("rotated arm"). Transport the
                        // hand's WRIST articulation (delta RELATIVE to the
                        // forearm) through the basis map M, riding the
                        // target's animated forearm — the same pattern the
                        // fingers use one level down:
                        //   Drel = Dp(forearm)⁻¹·Df(hand)     (source wrist)
                        //   Wt   = forearmDelta·(M·Drel·M⁻¹)·Wbind
                        // Parent-RELATIVE on purpose: an absolute palm pins
                        // the hand to the source's world orientation and
                        // fights the target's arm as it moves (broken wrists
                        // in mid-motion frames). Relative keeps the wrist
                        // faithful and smooth; fingers ride this hand.
                        const int side = (c == 9) ? 0 : 1;
                        const auto& rq = cmuRestWorld[static_cast<size_t>(c)];
                        const Ogre::Quaternion refQ(rq[3], rq[0], rq[1],
                                                    rq[2]);
                        const Ogre::Quaternion Df =
                            clipQ(f, c) * refQ.Inverse();
                        Ogre::Quaternion Dp = Ogre::Quaternion::IDENTITY;
                        const int pcArm = (c == 9) ? 8 : 12;   // elbow
                        const auto& prq =
                            cmuRestWorld[static_cast<size_t>(pcArm)];
                        if (prq[0]*prq[0] + prq[1]*prq[1] + prq[2]*prq[2]
                                + prq[3]*prq[3] > 0.25f) {
                            const Ogre::Quaternion prefQ(prq[3], prq[0],
                                                         prq[1], prq[2]);
                            Dp = clipQ(f, pcArm) * prefQ.Inverse();
                        }
                        const Ogre::Quaternion Drel = Dp.Inverse() * Df;
                        const Ogre::Quaternion WpBind = (pi >= 0)
                            ? tb.bindWorld[static_cast<size_t>(pi)]
                            : Ogre::Quaternion::IDENTITY;
                        const Ogre::Quaternion forearmDelta =
                            Wp * WpBind.Inverse();
                        const Ogre::Quaternion Wt =
                            forearmDelta
                            * (fingerBasisMap[side] * Drel
                               * fingerBasisMap[side].Inverse())
                            * tb.bindWorld[static_cast<size_t>(i)];
                        local = Wp.Inverse() * Wt;
                        W[static_cast<size_t>(i)] = Wt;
                    } else if (clipIsV2
                        && c >= MotionInbetween::canonicalJointCount()) {
                        // #838 V2 FINGERS: STANDARD HIERARCHICAL bind-referenced
                        // transport — the same change-of-basis pattern the #411
                        // body retarget documents (Wcmu·clip·Wcmu⁻¹), replacing
                        // the direction-aim + twist heuristics (which can't
                        // control twist: nails flipped sides, fingers rendered
                        // straight/extended). Transport the finger's curl
                        // RELATIVE to its parent so it rides the TARGET's hand
                        // and only adds the source's articulation:
                        //   Dp(f) = clipQ(f,pc)·restQ(pc)⁻¹      (parent delta)
                        //   Df(f) = clipQ(f,c) ·restQ(c)⁻¹       (finger delta)
                        //   Drel  = Dp⁻¹·Df                       (pure curl,
                        //           anchored to the source's REST axes)
                        //   Wt(f) = [Wp(f)·Wp_bind⁻¹] · (Ct⁻¹·Drel·Ct) · Wbind
                        // At the source's bind Drel = identity → the finger
                        // rests at ITS OWN bind carried rigidly by the target
                        // hand (no rest-convention over-bend); the full 3-DoF
                        // delta carries twist so the nail stays put. Unresolved
                        // slots hold identity → target holds bind.
                        const auto& rq = cmuRestWorld[static_cast<size_t>(c)];
                        const float rn = rq[0]*rq[0] + rq[1]*rq[1]
                                       + rq[2]*rq[2] + rq[3]*rq[3];
                        const auto& cq = clipQuats[static_cast<size_t>(f)]
                                                  [static_cast<size_t>(c)];
                        const float cn = cq[0]*cq[0] + cq[1]*cq[1]
                                       + cq[2]*cq[2] + cq[3]*cq[3];
                        // UNPOPULATED source slot (rig lacks this segment, e.g.
                        // a pruned Biped nub): extraction leaves rest AND quats
                        // exactly identity. It must HOLD BIND — transporting it
                        // would compute Df=identity against a curling parent
                        // (Drel=Dp⁻¹) and COUNTER-ROTATE the tip, straightening
                        // the finger (the "fingers look longer" artifact).
                        const bool populated =
                            std::abs(rq[0]) + std::abs(rq[1]) + std::abs(rq[2])
                                > 1e-4f
                            || std::abs(cq[0]) + std::abs(cq[1])
                                   + std::abs(cq[2]) > 1e-4f;
                        if (haveRestWorld && populated
                            && rn > 0.25f && cn > 0.25f
                            && !fingerChainUntrusted[static_cast<size_t>(c)]) {
                            const Ogre::Quaternion refQ(rq[3], rq[0], rq[1],
                                                        rq[2]);
                            const Ogre::Quaternion Df =
                                clipQ(f, c) * refQ.Inverse();
                            // parent delta on the SOURCE side (canonical parent:
                            // hand for seg0, previous segment deeper).
                            Ogre::Quaternion Dp = Ogre::Quaternion::IDENTITY;
                            const int pc =
                                MotionInbetween::canonicalParentOfV2(c);
                            if (pc >= 0 && pc < canonN) {
                                const auto& prq =
                                    cmuRestWorld[static_cast<size_t>(pc)];
                                const float prn = prq[0]*prq[0] + prq[1]*prq[1]
                                               + prq[2]*prq[2] + prq[3]*prq[3];
                                const auto& pcq =
                                    clipQuats[static_cast<size_t>(f)]
                                             [static_cast<size_t>(pc)];
                                const float pcn = pcq[0]*pcq[0] + pcq[1]*pcq[1]
                                               + pcq[2]*pcq[2] + pcq[3]*pcq[3];
                                if (prn > 0.25f && pcn > 0.25f) {
                                    const Ogre::Quaternion prefQ(
                                        prq[3], prq[0], prq[1], prq[2]);
                                    Dp = clipQ(f, pc) * prefQ.Inverse();
                                }
                            }
                            const Ogre::Quaternion Drel = Dp.Inverse() * Df;
                            // target parent's animated delta (world frame)
                            const Ogre::Quaternion WpBind = (pi >= 0)
                                ? tb.bindWorld[static_cast<size_t>(pi)]
                                : Ogre::Quaternion::IDENTITY;
                            const Ogre::Quaternion handDelta =
                                Wp * WpBind.Inverse();
                            // Re-express the curl in the target frame: prefer
                            // the HAND-BASIS map (frame-independent, see
                            // fingerBasisMap above); fall back to the
                            // canonical Ct conjugation.
                            const int fSide =
                                (c - MotionInbetween::canonicalJointCount())
                                    / 15;
                            const Ogre::Quaternion DrelT =
                                (fSide >= 0 && fSide < 2
                                 && fingerBasisOk[fSide])
                                    ? fingerBasisMap[fSide] * Drel
                                          * fingerBasisMap[fSide].Inverse()
                                    : CtInv * Drel * tb.Ct;
                            const Ogre::Quaternion Wt =
                                handDelta * DrelT
                                * tb.bindWorld[static_cast<size_t>(i)];
                            local = Wp.Inverse() * Wt;
                            W[static_cast<size_t>(i)] = Wt;
                        } else {
                            local = tb.bindLocal[static_cast<size_t>(i)];
                            W[static_cast<size_t>(i)] = Wp * local;
                        }
                    } else if (c >= 0 && c < Jc
                        && srcLocalAxis[static_cast<size_t>(c)]
                               .squaredLength() > 1e-8f) {
                        // NB: yaw180 is deliberately NOT applied here — this
                        // path anchors facing to the TARGET's own bind (the
                        // clip is canonical +Z-facing by construction), so a
                        // flip would swing every aim near-anti-parallel to
                        // its bind direction and destabilise getRotationTo.
                        // The flag only matters for the legacy standing-pose
                        // transport below.
                        const Ogre::Vector3 ds = CtInv *
                            (clipQ(f, c)
                             * srcLocalAxis[static_cast<size_t>(c)]);
                        const Ogre::Quaternion R =
                            dref[static_cast<size_t>(c)].getRotationTo(ds);
                        Ogre::Quaternion Wt =
                            R * Qbase[static_cast<size_t>(i)];
                        // #857: re-apply the source's roll about the aimed
                        // direction (the swing above deliberately dropped it).
                        // (A hierarchical "exact twist" hybrid and a source-
                        // reference roll-corrected Qbase were both attempted on
                        // 2026-08-02 and REVERTED: orientation-level transport
                        // requires the extraction's frame C and the retarget's
                        // Ct to agree, and they differ per clip by up to ~57° —
                        // see the Qbase note above. Only direction-level
                        // bridging is frame-safe here.)
                        const float th = twistTheta[static_cast<size_t>(f)]
                                                   [static_cast<size_t>(c)]
                                         * twistGainAt(c);
                        if (std::abs(th) > 1e-5f) {
                            Ogre::Vector3 axis = ds;
                            axis.normalise();
                            Wt = Ogre::Quaternion(Ogre::Radian(th), axis) * Wt;
                        }
                        local = Wp.Inverse() * Wt;
                        W[static_cast<size_t>(i)] = Wt;
                    } else {
                        local = tb.bindLocal[static_cast<size_t>(i)];
                        W[static_cast<size_t>(i)] = Wp * local;
                    }
                    if (auto* trk = tracks[static_cast<size_t>(i)]) {
                        // Ogre skeleton keyframes are DELTAS applied onto the
                        // binding pose (NodeAnimationTrack::applyToNode
                        // rotates the reset bone) — convert the absolute
                        // local target.
                        Ogre::TransformKeyFrame* kf =
                            trk->createNodeKeyFrame(f * dt);
                        kf->setRotation(
                            tb.bindLocal[static_cast<size_t>(i)].Inverse()
                            * local);
                        Ogre::Vector3 tdelta = Ogre::Vector3::ZERO;
                        // #838: lower the ROOT (canon 0) by the clip's descent
                        // (negative rootY only) × target leg length, along the
                        // canonical UP axis mapped into the rig's world frame.
                        // The keyframe is a parent-space translate delta; for a
                        // top-of-hierarchy root the parent frame is world, so
                        // the canonical-up vector (Ct⁻¹·+Y) is the drop axis.
                        if (c == 0 && doVerticalDescentBR && tgtLegLenBR > 1e-4f) {
                            const float drop =
                                std::min(0.0f, clipRootY[static_cast<size_t>(f)])
                                * tgtLegLenBR;
                            tdelta = tb.Ct.Inverse()
                                     * Ogre::Vector3(0.0f, drop, 0.0f);
                        }
                        kf->setTranslate(tdelta);
                        kf->setScale(Ogre::Vector3::UNIT_SCALE);
                    }
                }
            }
            res.ok = true;
            res.canonicalJoints = distinct;
            res.frames = frames;
            res.length = length;
            return res;
        }

        // MODEL clips carry no reference triple (their quats are learned,
        // mixed-convention deltas), so they ride the legacy transport below —
        // but its standing pose must NOT be harvested from the rig's other
        // animations (that bakes e.g. a dance stance into every generated
        // clip). Instead SYNTHESIZE the standing pose from a TEMPLATE clip's
        // canonical bone directions (passed via clipRestDir): aim each bone
        // from the target's BIND at the template's reference direction —
        // bind-referenced by construction, matching the direction path at
        // its reference frame.
        std::vector<Ogre::Quaternion> Wstand(static_cast<size_t>(nBones));
        int placedRoles = 0;
        for (int i : tb.order) {
            const int pi = tb.parentIdx[static_cast<size_t>(i)];
            const Ogre::Quaternion Wp = (pi >= 0)
                ? Wstand[static_cast<size_t>(pi)] : Ogre::Quaternion::IDENTITY;
            const int c = boneToCanon[i];
            bool aimed = false;
            if (c >= 0 && c < Jc
                && tb.tgtBindDir[static_cast<size_t>(c)].squaredLength()
                       > 1e-8f) {
                const auto& sd = clipRestDir[static_cast<size_t>(c)];
                Ogre::Vector3 srcDir(sd[0], sd[1], sd[2]);
                if (srcDir.squaredLength() > 1e-8f) {
                    srcDir.normalise();
                    const Ogre::Vector3 ds = CtInv * srcDir;
                    const Ogre::Quaternion R =
                        tb.tgtBindDir[static_cast<size_t>(c)]
                            .getRotationTo(ds);
                    const Ogre::Quaternion Wt =
                        R * tb.bindWorld[static_cast<size_t>(i)];
                    const Ogre::Quaternion local = Wp.Inverse() * Wt;
                    Wstand[static_cast<size_t>(i)] = Wt;
                    // Keyframe-delta form (relative to the bind local), the
                    // same convention the harvested standing pose arrives in.
                    standPose[static_cast<unsigned short>(i)] = {
                        tb.bindLocal[static_cast<size_t>(i)].Inverse() * local,
                        Ogre::Vector3::ZERO, Ogre::Vector3::UNIT_SCALE, true };
                    aimed = true;
                    ++placedRoles;
                }
            }
            if (!aimed)
                Wstand[static_cast<size_t>(i)] =
                    Wp * tb.bindLocal[static_cast<size_t>(i)];
        }
        synthStand = placedRoles > 0;
    }


    // The rig's natural STANDING pose. Mixamo skeletons have an identity bone
    // rest pose — the real standing orientation lives in frame 0 of their
    // existing animation, not in the bone transform. So harvest each bone's
    // orientation at t=0 of the rig's first existing animation; that is the
    // "bind" we compose our CMU motion onto (without it the body inverts, since
    // the bone rest is a meaningless identity). Falls back to the bone's own
    // rest if there's no prior animation. Skipped entirely when the synthetic
    // bind-referenced standing pose above was built — harvesting another
    // animation's pose is exactly the contamination that path eliminates.
    if (!synthStand && skel->getNumAnimations() > 0) {
        Ogre::Animation* ref = nullptr;
        for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
            Ogre::Animation* a = skel->getAnimation(ai);
            // Never harvest OUR OWN generated clips as the rig's standing
            // pose: on an auto-rigged skeleton the first --generate adds an
            // animation, and the next --generate would harvest it and switch
            // from the (correct) raw path to the standing-pose path — whose
            // Mc is bogus on non-identity bone rests. Only rig-authored
            // animations describe the true standing pose.
            if (a && a->getName() != animName
                && a->getName().rfind("generated_", 0) != 0) { ref = a; break; }
        }
        if (ref) {
            // Harvest at the reference animation's CALMEST frame, not
            // blindly frame 0 — authored clips often OPEN on a stylized
            // pose (a dance intro, a wind-up), and composing every
            // generated clip onto that bakes the style into the neutral
            // ("generations look based on the previous animation's first
            // frame"). The calmest frame is the closest thing the rig has
            // to a relaxed standing pose. Pure track math — nothing is
            // applied to the live skeleton.
            const float len = ref->getLength();
            const int samples = std::clamp(
                static_cast<int>(std::lround(len * 30.0f)) + 1, 2, 301);
            const auto& tracks = ref->_getNodeTrackList();
            std::vector<double> energy(static_cast<size_t>(samples), 0.0);
            for (const auto& [h, trk] : tracks) {
                if (!trk || trk->getNumKeyFrames() == 0) continue;
                Ogre::Quaternion prevQ;
                for (int f = 0; f < samples; ++f) {
                    const float t = len * static_cast<float>(f)
                                    / static_cast<float>(samples - 1);
                    Ogre::TransformKeyFrame kf(nullptr, 0.0f);
                    trk->getInterpolatedKeyFrame(ref->_getTimeIndex(t), &kf);
                    const Ogre::Quaternion q = kf.getRotation();
                    if (f > 0) {
                        const double d = std::min(1.0, std::abs(
                            static_cast<double>(q.Dot(prevQ))));
                        energy[static_cast<size_t>(f)] += 2.0 * std::acos(d);
                    }
                    prevQ = q;
                }
            }
            // energy[f] = motion between samples f-1 and f; energy[0] is 0
            // by construction, so start the argmin at frame 1.
            int calm = 1;
            for (int f = 2; f < samples; ++f)
                if (energy[static_cast<size_t>(f)]
                        < energy[static_cast<size_t>(calm)])
                    calm = f;
            const float tCalm = len * static_cast<float>(calm)
                                / static_cast<float>(samples - 1);
            for (const auto& [h, trk] : tracks) {
                if (!trk || trk->getNumKeyFrames() == 0) continue;
                Ogre::TransformKeyFrame f0(nullptr, 0.0f);
                trk->getInterpolatedKeyFrame(ref->_getTimeIndex(tCalm), &f0);
                standPose[h] = { f0.getRotation(), f0.getTranslate(), f0.getScale(), true };
            }
        }
    }

    const int J = MotionInbetween::canonicalJointCount();

    // ===== ROLL-CORRECTED PURE-LOCAL RETARGET =====
    // Apply each CMU joint's LOCAL (parent-relative) articulation delta onto the
    // rig's standing pose, conjugated by a CONSTANT per-joint frame map M_c so
    // the motion DIRECTION matches CMU while staying frame-coherent (smooth):
    //   local(f) = bind · (M_c⁻¹ · cmuLocalDelta[c][f] · M_c)
    // Conjugation by a constant preserves the rotation ANGLE (no jitter) while
    // redirecting its axis into the target bone's local frame. cmuLocalDelta is
    // derived from the v3 WORLD clip (Wparent⁻¹·Wjoint), or used directly for
    // v1/v2 local clips. Root (c==0) locked to standing (CMU bakes facing into
    // the hip). Robust to duplicate-canon bones (per-bone-local). Good-but-
    // imperfect: locomotion looks right; arm-precise gestures place the arm
    // approximately. A true IK/look-at retarget that handles the rig↔canonical
    // topology mismatch (multi-bone spines, canonical≠skeleton parents) is the
    // follow-up for exact gesture placement (see #411 retarget notes).
    static const int kParentCanon[22] = {
        -1, 0, 1, 2, 3, 4,   // hip, abdomen, chest, neck, neck1, head
         2, 6, 7, 8,          // rcollar, rshoulder, relbow, rhand
         2, 10, 11, 12,       // lcollar, lshoulder, lelbow, lhand
         0, 14, 15, 16,       // rbuttock, rhip, rknee, rfoot
         0, 18, 19, 20 };     // lbuttock, lhip, lknee, lfoot
    auto clipQ = [&](int frame, int joint) -> Ogre::Quaternion {
        const auto& q = clipQuats[frame][joint];
        return Ogre::Quaternion(q[3], q[0], q[1], q[2]);   // (w,x,y,z)
    };
    // REFERENCE FRAME: deltas (and the Mc frame map below) are taken against
    // the clip's CALMEST frame, not blindly frame 0. Template windows open on
    // a settled pose so this stays ~frame 0 for them; MODEL-generated clips
    // have noisy per-joint frame-0 orientations, and referencing them gave
    // every bone a slightly wrong constant offset (weird arm positions on
    // rigs that use the standing-pose path).
    int refFrame = 0;
    if (frames > 2) {
        double bestE = 1e30;
        for (int f = 0; f + 1 < frames; ++f) {
            double e = 0.0;
            for (int c = 1; c < J; ++c) {
                const Ogre::Quaternion d = clipQ(f, c).Inverse() * clipQ(f + 1, c);
                e += 2.0 * std::acos(std::min(1.0, std::abs(static_cast<double>(d.w))));
            }
            if (e < bestE) { bestE = e; refFrame = f; }
        }
    }
    std::vector<std::vector<Ogre::Quaternion>> cmuLocalDelta(
        J, std::vector<Ogre::Quaternion>(frames, Ogre::Quaternion::IDENTITY));
    for (int c = 0; c < J; ++c) {
        const int pc = kParentCanon[c];
        const Ogre::Quaternion localRef = (worldFrame && pc >= 0)
            ? clipQ(refFrame, pc).Inverse() * clipQ(refFrame, c)
            : clipQ(refFrame, c);
        for (int f = 0; f < frames; ++f) {
            Ogre::Quaternion local = (worldFrame && pc >= 0)
                ? clipQ(f, pc).Inverse() * clipQ(f, c)   // world→local
                : clipQ(f, c);                            // already local (or root)
            cmuLocalDelta[c][f] = localRef.Inverse() * local;
        }
    }
    // Standing-pose world orientation per bone (for the M_c roll correction).
    std::unordered_map<unsigned short, Ogre::Quaternion> standWorldCache;
    std::function<Ogre::Quaternion(Ogre::Bone*)> standWorldOf =
        [&](Ogre::Bone* b) -> Ogre::Quaternion {
        const unsigned short h = b->getHandle();
        auto it = standWorldCache.find(h);
        if (it != standWorldCache.end()) return it->second;
        auto spr = standPose.find(h);
        Ogre::Quaternion localRot = (spr != standPose.end() && spr->second.has)
            ? spr->second.rot : b->getInitialOrientation();
        Ogre::Quaternion w = localRot;
        if (b->getParent())
            w = standWorldOf(static_cast<Ogre::Bone*>(b->getParent())) * w;
        standWorldCache[h] = w;
        return w;
    };

    // The standing-pose change-of-basis (Mc) assumes Mixamo-style skeletons
    // whose bone rests are identity (the real pose lives in the animation).
    // On rigs with authored/non-identity rests (UniRig / template auto-rig)
    // standWorldOf accumulates the rests and Mc becomes a large bogus
    // rotation — use the raw path there instead.
    bool restsAreIdentity = true;
    for (int i = 0; i < nBones && restsAreIdentity; ++i) {
        Ogre::Bone* b = skel->getBone(static_cast<unsigned short>(i));
        if (!b->getInitialOrientation().equals(Ogre::Quaternion::IDENTITY,
                                               Ogre::Radian(0.02f)))
            restsAreIdentity = false;
    }

    // #838 vertical descent: TARGET rig hip→foot length (bind pose) so the
    // clip's source-normalized rootY (leg-lengths) scales to this rig's world
    // units. Only wired when the caller enabled verticalDescent AND the clip
    // carries a rootY track (non-locomotion actions). Uses the derived bind
    // positions of the hip (canon 0) and a foot (canon 17 = left, else 21).
    float targetLegLen = 0.0f;
    const bool doVerticalDescent =
        verticalDescent && !clipRootY.empty()
        && static_cast<int>(clipRootY.size()) == frames;
    if (doVerticalDescent) {
        // Measure the leg length from the BIND pose (readTargetBindFrame resets
        // + updates the skeleton, then reads derived positions), NOT the live
        // pose — otherwise a crouched runtime pose would skew the scale.
        const TargetBindFrame tbd = readTargetBindFrame(skel, boneToCanon);
        const int hipB  = tbd.roleBoneIdx[0];
        int footB = tbd.roleBoneIdx[17];
        if (footB < 0) footB = tbd.roleBoneIdx[21];
        if (hipB >= 0 && footB >= 0)
            targetLegLen = (tbd.bindPos[static_cast<size_t>(hipB)]
                            - tbd.bindPos[static_cast<size_t>(footB)]).length();
    }

    for (int i = 0; i < nBones; ++i) {
        const int c = boneToCanon[i];
        if (c < 0 || c >= J) continue;       // unmapped bone keeps its bind pose
        Ogre::Bone* bone = skel->getBone(static_cast<unsigned short>(i));
        Ogre::NodeAnimationTrack* track = anim->createNodeTrack(
            static_cast<unsigned short>(i), bone);

        auto sp = standPose.find(static_cast<unsigned short>(i));
        const bool haveStand = (sp != standPose.end() && sp->second.has);
        const Ogre::Quaternion bind = haveStand
            ? sp->second.rot
            : (bone->getOrientation().equals(Ogre::Quaternion::IDENTITY, Ogre::Radian(1e-4f))
                   ? bone->getInitialOrientation() : bone->getOrientation());
        const Ogre::Vector3 standPos = haveStand ? sp->second.pos : bone->getInitialPosition();
        const Ogre::Vector3 standScale = haveStand ? sp->second.scale : bone->getInitialScale();

        // Mc (roll correction) is a Mixamo-tuned heuristic: it is ≈identity when
        // the rig has IDENTITY bone rests (Mixamo — the standing pose lives in the
        // harvested animation, so standWorldOf≈I and Mc≈clip0≈I, harmless). But on
        // a rig with NON-IDENTITY bone rests AND no prior animation (UniRig auto-
        // rig), standWorldOf accumulates the real rest orientations and Mc becomes
        // a large bogus rotation that conjugates the motion into the wrong frame —
        // which inverted/splayed the UniRig animation. So only apply Mc when we
        // actually harvested a standing pose (have-anim rigs); otherwise use the
        // robust pure-local delta (bind · delta), which renders upright on UniRig.
        const bool haveAnyStand = !standPose.empty();
        Ogre::Quaternion Mc = Ogre::Quaternion::IDENTITY;
        if (worldFrame && c > 0 && haveAnyStand && restsAreIdentity)
            Mc = standWorldOf(bone).Inverse() * clipQ(refFrame, c);  // target-stand → clip rest
        // −Z-facing rig on the RAW path (no harvested stand → Mc == identity,
        // deltas act in ~world axes): view every delta in a 180°-yawed frame
        // so the sagittal swing matches the mesh's facing (else it walks
        // backward). Rigs WITH a stand pose are covered by Mc itself.
        if (yaw180 && !(worldFrame && haveAnyStand) && c > 0) {
            static const Ogre::Quaternion kYawPi(0.0f, 0.0f, 1.0f, 0.0f); // 180° about +Y
            Mc = Mc * kYawPi;
        }
        const Ogre::Quaternion McInv = Mc.Inverse();
        const int dup = std::max(1, canonDup[c]);
        for (int f = 0; f < frames; ++f) {
            Ogre::Quaternion artic = McInv * cmuLocalDelta[c][f] * Mc;
            if (dup > 1)   // share the role's rotation across its N bones
                artic = Ogre::Quaternion::Slerp(1.0f / static_cast<float>(dup),
                                                Ogre::Quaternion::IDENTITY,
                                                artic, /*shortestPath=*/true);
            Ogre::Quaternion local;
            if (c == 0) {
                // Root: CMU/scraped clips bake whole-body FACING into the hip,
                // so the yaw must stay locked to the standing pose — but the
                // pitch/roll component is the pelvic sway that makes walks
                // read as alive (measured 14.5° on a reference walk, 0° when
                // fully locked). Swing–twist split about canonical +Y: drop
                // the twist (facing), pre-multiply the swing in world axes.
                local = bind;
                if (worldFrame) {
                    const Ogre::Quaternion d = cmuLocalDelta[0][f];
                    Ogre::Quaternion twist(d.w, 0.0f, d.y, 0.0f);
                    const float n = std::sqrt(twist.w * twist.w
                                              + twist.y * twist.y);
                    if (n > 1e-6f) {
                        twist.w /= n; twist.y /= n;
                        Ogre::Quaternion swing = d * twist.Inverse();
                        swing.normalise();
                        local = swing * bind;
                    }
                }
            } else {
                local = bind * artic;
            }
            Ogre::TransformKeyFrame* kf = track->createNodeKeyFrame(f * dt);
            kf->setRotation(local);
            Ogre::Vector3 trans = standPos;
            // #838 vertical descent: lower the ROOT along its parent-local Y by
            // the clip's normalized hip drop × the target leg length. The hip's
            // parent-local +Y is the world up axis for an unrotated root, so
            // this reads as the body crouching straight down. Only the root
            // (c==0) moves; children follow through the hierarchy.
            //
            // DESCENT-ONLY: apply only the negative (lowering) component. Source
            // rigs vary wildly in whether/which-sign they bake hip translation
            // for crouch/pickup/sit (some author the whole squat in the KNEE/HIP
            // joint rotations we already retarget, leaving the hip pinned; a few
            // read spuriously POSITIVE). Clamping positive to 0 makes this strictly
            // safe — it can pull a floating crouch down but never lift a grounded
            // pose up. Genuine descents (working/crawl/death, measured ≤ ~1.8 leg)
            // still land.
            if (c == 0 && doVerticalDescent && targetLegLen > 1e-4f)
                trans.y += std::min(0.0f, clipRootY[f]) * targetLegLen;
            kf->setTranslate(trans);
            kf->setScale(standScale);
        }
        ++res.tracksWritten;
    }

    if (res.tracksWritten == 0) {
        skel->removeAnimation(animName);
        res.error = QStringLiteral("no bone tracks written");
        return res;
    }

    // RMIB REFINE PASS (#411). The raw retargeted keyframes are individually
    // plausible but temporally jittery / slightly off the natural-motion
    // manifold (the per-frame bind-compose accumulates small errors). The RMIB
    // in-between model (#409) was trained on CMU motion — the SAME source as
    // these clips — so re-predicting the interior frames between sparse
    // keyframes acts as a learned motion smoother that pulls the poses back
    // toward plausible CMU-style motion. (User-validated: decimate→in-between
    // markedly cleans up the result.) We decimate to every `refineStride`-th
    // keyframe, then RMIB-fill each gap. Best-effort: any failure leaves the
    // dense keyframes intact.
    if (refineWithModel && refineStride > 0 && frames > refineStride * 2) {
        const QString modelPath = MotionInbetween::ensureModelBlocking();
        // Decimate every track to keep every refineStride-th key (+ the last),
        // then in-between-fill the whole clip so RMIB regenerates the interior.
        decimateAnimation(skel, animName, refineStride);
        const auto fill = inbetweenAnimation(
            skel, animName, 0.0f, length, refineStride - 1, modelPath,
            /*forceFallback=*/modelPath.isEmpty());
        res.refined = fill.ok;
        res.usedModel = fill.ok && fill.usedModel;
    }

    res.ok = true;
    return res;
}

void AnimationMerger::analyzeRedundantKeyframes(const Ogre::Animation* anim,
                                                const SimplifyTolerances& tol,
                                                int* outOriginal,
                                                int* outRedundant)
{
    int original = 0;
    int redundant = 0;

    if (anim) {
        for (const auto& [handle, track] : anim->_getNodeTrackList()) {
            unsigned short numKf = track->getNumKeyFrames();
            original += numKf;
            if (numKf < 3) continue;

            std::vector<SimpleKey> keys;
            keys.reserve(numKf);
            for (unsigned short k = 0; k < numKf; ++k) {
                const auto* kf = track->getNodeKeyFrame(k);
                keys.push_back({kf->getTime(), kf->getTranslate(), kf->getRotation(), kf->getScale()});
            }
            redundant += simplifyTrackKeys(keys, tol);
        }
    }

    if (outOriginal)  *outOriginal  = original;
    if (outRedundant) *outRedundant = redundant;
}

AnimationMerger::SimplifyTolerances AnimationMerger::tolerancesForPreset(
    const std::string& preset, bool* outOk)
{
    SimplifyTolerances tol; // conservative default (since simplify is destructive)

    // Lowercase comparison without bringing in QString — keeps this header
    // safe to call from any TU that already pulls in AnimationMerger.h.
    std::string p = preset;
    for (auto& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    bool ok = true;
    if (p.empty() || p == "conservative") {
        // tol already holds the conservative defaults from SimplifyTolerances{}.
    } else if (p == "balanced") {
        tol.translation = 1e-3f;
        tol.rotationDeg = 0.5f;
        tol.scale       = 1e-3f;
    } else if (p == "aggressive") {
        tol.translation = 1e-2f;
        tol.rotationDeg = 1.0f;
        tol.scale       = 1e-2f;
    } else {
        ok = false; // tol stays at conservative so callers that ignore outOk still get something usable
    }

    if (outOk) *outOk = ok;
    return tol;
}

bool AnimationMerger::areSkeletonsCompatible(const Ogre::SkeletonPtr& a, const Ogre::SkeletonPtr& b)
{
    if (!a || !b)
        return false;

    // Check that every bone in 'b' (source/animation) has a matching bone in 'a' (base/mesh).
    // The base is allowed to have extra bones (e.g. IK targets) that the animation doesn't touch.
    Ogre::Skeleton::BoneHandleMap boneHandleMap;
    b->_buildMapBoneByName(a.get(), boneHandleMap);

    // The map has one entry per bone in 'b'.
    // An entry equal to a->getNumBones() means "no match found" for that source bone.
    unsigned short numBaseBones = a->getNumBones();
    for (auto handle : boneHandleMap)
    {
        if (handle == numBaseBones)
            return false;
    }
    return true;
}

Ogre::Entity* AnimationMerger::mergeAnimations(
    Ogre::Entity* baseEntity,
    const QList<Ogre::Entity*>& sourceEntities,
    QString& errorMsg)
{
    return mergeAnimations(baseEntity, sourceEntities, {}, errorMsg);
}

Ogre::Entity* AnimationMerger::mergeAnimations(
    Ogre::Entity* baseEntity,
    const QList<Ogre::Entity*>& sourceEntities,
    const QList<Ogre::SkeletonPtr>& sourceSkeletons,
    QString& errorMsg)
{
    if (!baseEntity || !baseEntity->hasSkeleton())
    {
        errorMsg = "Base entity has no skeleton";
        return nullptr;
    }

    Ogre::SkeletonPtr baseSkel = baseEntity->getMesh()->getSkeleton();
    if (!baseSkel)
    {
        errorMsg = "Base entity's mesh has no skeleton";
        return nullptr;
    }

    // Record which animations belong to the base (before merge)
    QSet<QString> baseAnimNames;
    for (unsigned short i = 0; i < baseSkel->getNumAnimations(); ++i)
        baseAnimNames.insert(QString::fromStdString(baseSkel->getAnimation(i)->getName()));

    // Collect names to detect collisions during source merging
    QSet<QString> existingNames = baseAnimNames;

    // --- Step 1: Merge source animations with prefix + cleanup ---
    int mergedCount = 0;

    for (Ogre::Entity* srcEntity : sourceEntities)
    {
        if (srcEntity == baseEntity)
            continue;

        if (!srcEntity || !srcEntity->hasSkeleton())
            continue;

        Ogre::SkeletonPtr srcSkel = srcEntity->getMesh()->getSkeleton();
        if (!srcSkel)
            continue;

        if (srcSkel.get() == baseSkel.get())
            continue;

        if (!areSkeletonsCompatible(baseSkel, srcSkel))
        {
            errorMsg = QString("Skeleton of '%1' is incompatible with base skeleton")
                .arg(srcEntity->getName().c_str());
            return nullptr;
        }

        QString rawName;
        if (auto* parentNode = srcEntity->getParentSceneNode())
            rawName = QString::fromStdString(parentNode->getName());
        else
            rawName = QString::fromStdString(srcEntity->getName());

        unsigned short numAnims = srcSkel->getNumAnimations();

        // Rename on source skeleton before Ogre copies them across
        QList<std::pair<std::string, std::string>> renameList;
        for (unsigned short i = 0; i < numAnims; ++i)
        {
            Ogre::Animation* anim = srcSkel->getAnimation(i);
            std::string origName = anim->getName();
            QString desired = buildAnimName(rawName, QString::fromStdString(origName));
            QString finalName = deduplicateName(desired, existingNames);
            renameList.append({origName, finalName.toStdString()});
        }

        // Two-pass rename to avoid old↔new name collisions on source
        QList<std::pair<std::string, std::string>> srcTempToFinal;
        for (int i = 0; i < renameList.size(); ++i)
        {
            std::string tempName = "__merge_temp_src_" + std::to_string(i);
            while (srcSkel->hasAnimation(tempName))
                tempName += "_x";
            renameAnimation(srcSkel.get(), renameList[i].first, tempName);
            srcTempToFinal.append({tempName, renameList[i].second});
        }
        for (const auto& [tempName, finalName] : srcTempToFinal)
            renameAnimation(srcSkel.get(), tempName, finalName);

        mergeAnimationsByName(baseSkel.get(), srcSkel.get(),
                              lookupUpAxis(srcSkel->getName()),
                              lookupUpAxis(baseSkel->getName()));
        mergedCount += numAnims;
    }

    // --- Step 1b: Merge from standalone skeletons (animation-only files) ---
    for (const Ogre::SkeletonPtr& srcSkel : sourceSkeletons)
    {
        if (!srcSkel || srcSkel.get() == baseSkel.get())
            continue;

        if (!areSkeletonsCompatible(baseSkel, srcSkel))
        {
            errorMsg = QString("Skeleton '%1' is incompatible with base skeleton")
                .arg(srcSkel->getName().c_str());
            return nullptr;
        }

        // Use skeleton name (strip ".skeleton" suffix) as the naming prefix
        QString rawName = QString::fromStdString(srcSkel->getName());
        if (rawName.endsWith(".skeleton", Qt::CaseInsensitive))
            rawName.chop(9);

        unsigned short numAnims = srcSkel->getNumAnimations();

        QList<std::pair<std::string, std::string>> renameList;
        for (unsigned short i = 0; i < numAnims; ++i)
        {
            Ogre::Animation* anim = srcSkel->getAnimation(i);
            std::string origName = anim->getName();
            QString desired = buildAnimName(rawName, QString::fromStdString(origName));
            QString finalName = deduplicateName(desired, existingNames);
            renameList.append({origName, finalName.toStdString()});
        }

        QList<std::pair<std::string, std::string>> srcTempToFinal;
        for (int i = 0; i < renameList.size(); ++i)
        {
            std::string tempName = "__merge_temp_src_" + std::to_string(i);
            while (srcSkel->hasAnimation(tempName))
                tempName += "_x";
            renameAnimation(srcSkel.get(), renameList[i].first, tempName);
            srcTempToFinal.append({tempName, renameList[i].second});
        }
        for (const auto& [tempName, finalName] : srcTempToFinal)
            renameAnimation(srcSkel.get(), tempName, finalName);

        mergeAnimationsByName(baseSkel.get(), srcSkel.get(),
                              lookupUpAxis(srcSkel->getName()),
                              lookupUpAxis(baseSkel->getName()));
        mergedCount += numAnims;
    }

    if (mergedCount == 0)
    {
        errorMsg = "No animations were merged (no valid source entities or skeletons found)";
        return nullptr;
    }

    // --- Step 2: Post-process ALL animations (base + merged) ---
    // Rename base animations with their prefix, clean Mixamo noise from everything.
    // This happens AFTER merge so we never conflict with Ogre's merge operation.
    {
        QString baseRawName;
        if (auto* parentNode = baseEntity->getParentSceneNode())
            baseRawName = QString::fromStdString(parentNode->getName());
        else
            baseRawName = QString::fromStdString(baseEntity->getName());
        QString baseSlug = slugify(baseRawName);

        // Collect all current animation names and compute final names
        QSet<QString> finalNames;
        QList<std::pair<std::string, std::string>> renames;

        for (unsigned short i = 0; i < baseSkel->getNumAnimations(); ++i)
        {
            std::string origName = baseSkel->getAnimation(i)->getName();
            QString origQName = QString::fromStdString(origName);

            QString desired;
            if (baseAnimNames.contains(origQName)) {
                // This was a base animation — prefix it (unless already prefixed)
                QString origSlug = slugify(origQName);
                if (origSlug.startsWith(baseSlug + "_") || origSlug == baseSlug)
                    desired = slugify(cleanAnimNoise(origQName)); // already prefixed
                else
                    desired = buildAnimName(baseRawName, origQName); // needs prefix
            } else {
                // This was merged from a source — already renamed in Step 1b, just clean.
                desired = slugify(cleanAnimNoise(origQName));
                // If cleaning removes the entire name (e.g. "unreal_take" → ""),
                // keep the already-processed name from Step 1b as-is.
                if (desired.isEmpty())
                    desired = slugify(origQName);
            }

            QString finalName = deduplicateName(desired, finalNames);
            if (finalName.toStdString() != origName)
                renames.append({origName, finalName.toStdString()});
            // If same name, already in finalNames via deduplicateName
        }

        // Two-pass rename to avoid old↔new name collisions
        QList<std::pair<std::string, std::string>> tempToFinal;
        for (int i = 0; i < renames.size(); ++i)
        {
            std::string tempName = "__merge_temp_" + std::to_string(i);
            while (baseSkel->hasAnimation(tempName))
                tempName += "_x";
            renameAnimation(baseSkel.get(), renames[i].first, tempName);
            tempToFinal.append({tempName, renames[i].second});
        }
        for (const auto& [tempName, finalName] : tempToFinal)
            renameAnimation(baseSkel.get(), tempName, finalName);
    }

    // Rebuild animation states from scratch. refreshAvailableAnimationState() only
    // adds new states but doesn't remove stale ones from renamed/removed animations.
    {
        auto* stateSet = baseEntity->getAllAnimationStates();
        if (stateSet) {
            // Collect stale state names (present in entity but not in skeleton)
            std::vector<std::string> staleNames;
            for (const auto& [name, state] : stateSet->getAnimationStates()) {
                if (!baseSkel->hasAnimation(name))
                    staleNames.push_back(name);
            }
            for (const auto& name : staleNames)
                stateSet->removeAnimationState(name);
        }
        // Add any new animations from the skeleton
        baseEntity->refreshAvailableAnimationState();
    }

    return baseEntity;
}
