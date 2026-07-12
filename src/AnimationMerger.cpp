#include "AnimationMerger.h"
#include "AutoRig.h"
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
                                       const QString& onlyAnimation)
{
    std::vector<CanonicalClip> out;
    if (!entity || !entity->hasSkeleton() || fps <= 0)
        return out;
    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    if (!skel)
        return out;

    const int J = MotionInbetween::canonicalJointCount();

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

        // Pass 1: sample RAW world orientations per frame per role.
        std::vector<std::vector<Ogre::Quaternion>> raw(
            static_cast<size_t>(frames),
            std::vector<Ogre::Quaternion>(static_cast<size_t>(J)));
        for (int f = 0; f < frames; ++f) {
            skel->reset(true);
            anim->apply(skel, std::min(length,
                static_cast<float>(f) / static_cast<float>(fps)));
            skel->_updateTransforms();
            for (int j = 0; j < J; ++j)
                if (Ogre::Bone* b = roleBone[static_cast<size_t>(j)])
                    raw[static_cast<size_t>(f)][static_cast<size_t>(j)] =
                        b->_getDerivedOrientation();
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

        // Measure the reference triple at f*: canonical frame C, reference
        // world orientation and bone directions — all from the SAME applied
        // pose, so they share one frame with the sampled quats.
        skel->reset(true);
        anim->apply(skel, std::min(length,
            static_cast<float>(fStar) / static_cast<float>(fps)));
        skel->_updateTransforms();
        const Ogre::Quaternion C = deriveFrame();
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
        out.push_back(std::move(clip));
    }

    // Restore the bind pose so the on-screen entity isn't left mid-clip.
    skel->reset(true);
    skel->_updateTransforms();
    return out;
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
                                    const std::vector<int>& boneToCanon)
{
    const int nBones = static_cast<int>(skel->getNumBones());
    const int Jc = MotionInbetween::canonicalJointCount();
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
        const int child = MotionInbetween::canonicalChildOf(role);
        const int parent = MotionInbetween::canonicalParentOf(role);
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
} // namespace

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
    const std::vector<std::array<float, 3>>& clipRestDir)
{
    ApplyMotionResult res;
    if (!skel) { res.error = QStringLiteral("no skeleton"); return res; }
    if (clipQuats.empty()) { res.error = QStringLiteral("empty motion clip"); return res; }
    if (fps <= 0) fps = 30;
    const int frames = static_cast<int>(clipQuats.size());
    // Guard: every frame must carry all canonical joints — clipQ() indexes up to
    // canonicalJointCount(), and a malformed/downloaded/generated clip with a
    // short frame would otherwise crash during retargeting.
    {
        const int need = MotionInbetween::canonicalJointCount();
        for (int f = 0; f < frames; ++f)
            if (static_cast<int>(clipQuats[f].size()) < need) {
                res.error = QStringLiteral("motion clip frame %1 has %2 joints; expected >= %3")
                    .arg(f).arg(clipQuats[f].size()).arg(need);
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
    std::vector<char> canonSeen(MotionInbetween::canonicalJointCount(), 0);
    int distinct = 0;
    for (int i = 0; i < nBones; ++i) {
        const QString bn = QString::fromStdString(skel->getBone(static_cast<unsigned short>(i))->getName());
        const int c = MotionInbetween::canonicalIndexForBone(bn);
        if (c >= 0 && c < static_cast<int>(canonSeen.size())) {
            boneToCanon[i] = c;
            if (!canonSeen[c]) { canonSeen[c] = 1; ++distinct; }
        }
    }
    // HANDEDNESS COMPENSATION. The bone NAMES are anatomically correct for the
    // mesh, but a mesh may face opposite to the CMU data — then the rig's "Left"
    // bones sit on the −X side while CMU's left canonical joints are at +X. If we
    // mapped name→canon directly the motion would play MIRRORED. So detect the
    // rig's handedness from the actual world-X of a left vs right bone and, when
    // it's opposite CMU's (+X = left), SWAP the canonical L/R indices in the
    // mapping — labels stay correct, motion stays correct. Uses the upper-arm
    // pair (canon 11 = left, 7 = right) with a fallback to the leg pair (19/15).
    {
        auto worldXForCanon = [&](int canon) -> double {
            for (int i = 0; i < nBones; ++i)
                if (boneToCanon[i] == canon)
                    return skel->getBone(static_cast<unsigned short>(i))->_getDerivedPosition().x;
            return 0.0;
        };
        double lx = worldXForCanon(11), rx = worldXForCanon(7);   // arms
        if (std::abs(lx - rx) < 1e-4) { lx = worldXForCanon(19); rx = worldXForCanon(15); }  // legs
        // CMU: left at +X. If the rig's "left" bone is more −X than its "right",
        // the rig is mirrored vs CMU → swap L/R canonical targets.
        if (lx < rx - 1e-5) {
            static const int kLR[][2] = {{6,10},{7,11},{8,12},{9,13},{14,18},{15,19},{16,20},{17,21}};
            auto swapCanon = [&](int& c) {
                for (auto& p : kLR) { if (c == p[0]) { c = p[1]; return; } if (c == p[1]) { c = p[0]; return; } }
            };
            for (int i = 0; i < nBones; ++i)
                if (boneToCanon[i] >= 0) swapCanon(boneToCanon[i]);
        }
    }

    res.canonicalJoints = distinct;
    // Bones-per-role: rigs segment chains differently (Mixamo has Spine AND
    // Spine1 in the canonical "abdomen" span; some rigs multi-segment arms).
    // Applying the full role delta to EVERY mapped bone would bend the chain
    // N times over — distribute it instead: each of the N bones gets the
    // N-th fractional rotation so the chain's total matches the clip.
    std::vector<int> canonDup(MotionInbetween::canonicalJointCount(), 0);
    for (int i = 0; i < nBones; ++i)
        if (boneToCanon[i] >= 0) ++canonDup[boneToCanon[i]];
    // Need a humanoid-ish rig: require a reasonable share of the 22 roles.
    if (distinct < (MotionInbetween::canonicalJointCount() * 1) / 2) {
        res.error = QStringLiteral(
            "skeleton resolved only %1/%2 canonical joints — not a humanoid rig the "
            "motion library can retarget onto")
            .arg(distinct).arg(MotionInbetween::canonicalJointCount());
        return res;
    }

    if (skel->hasAnimation(animName))
        skel->removeAnimation(animName);
    // #854: the freshly (re)created clip has NO arm-space applied. A stale
    // stored angle from a prior generation of the same name would make a
    // re-request of that same angle a no-op (delta 0) on the new keyframes —
    // forget it.
    if (skel->getNumBones() > 0)
        skel->getBone(0)->getUserObjectBindings().eraseUserAny(
            armSpaceKey(animName));
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
    bool haveRestWorld = false;
    if (worldFrame
        && cmuRestWorld.size() == static_cast<size_t>(
               MotionInbetween::canonicalJointCount())) {
        for (const auto& q : cmuRestWorld)
            if (std::abs(q[0]) > 1e-5f || std::abs(q[1]) > 1e-5f
                || std::abs(q[2]) > 1e-5f || std::abs(1.f - std::abs(q[3])) > 1e-5f) {
                haveRestWorld = true;
                break;
            }
    }
    const int Jc = MotionInbetween::canonicalJointCount();

    // The rig's STANDING pose the legacy transport composes onto — declared
    // here so the SYNTHETIC bind-referenced path below can fill it before
    // the (contaminating) animation harvest is even considered.
    struct StandXform { Ogre::Quaternion rot; Ogre::Vector3 pos; Ogre::Vector3 scale; bool has = false; };
    std::unordered_map<unsigned short, StandXform> standPose;
    bool synthStand = false;

    if (!clipRestDir.empty()) {
        const TargetBindFrame tb = readTargetBindFrame(skel, boneToCanon);
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
            std::vector<Ogre::NodeAnimationTrack*> tracks(
                static_cast<size_t>(nBones), nullptr);
            for (int i = 0; i < nBones; ++i)
                if (boneToCanon[i] >= 0 && boneToCanon[i] < Jc) {
                    tracks[static_cast<size_t>(i)] = anim->createNodeTrack(
                        static_cast<unsigned short>(i),
                        skel->getBone(static_cast<unsigned short>(i)));
                    ++res.tracksWritten;
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
                    if (c >= 0 && c < Jc
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
                            tb.tgtBindDir[static_cast<size_t>(c)]
                                .getRotationTo(ds);
                        const Ogre::Quaternion Wt =
                            R * tb.bindWorld[static_cast<size_t>(i)];
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
                        kf->setTranslate(Ogre::Vector3::ZERO);
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
            kf->setTranslate(standPos);
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
