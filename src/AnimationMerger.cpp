#include "AnimationMerger.h"
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
#include <vector>
#include <cmath>

// Registry: skeleton name → up-axis (1=Y-up, 2=Z-up).
// Populated by AnimationMerger::registerSkeletonUpAxis() at import time.
static QMap<QString, int> s_skeletonUpAxis;

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

    skel->removeAnimation(oldName);
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

AnimationMerger::ApplyMotionResult AnimationMerger::applyMotionClip(
    Ogre::Skeleton* skel,
    const std::string& animName,
    const std::vector<std::vector<std::array<float, 4>>>& clipQuats,
    int fps,
    bool worldFrame,
    const std::vector<std::array<float, 4>>& cmuRestWorld,
    bool refineWithModel,
    int refineStride)
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
    Ogre::Animation* anim = skel->createAnimation(animName, length);
    anim->setInterpolationMode(Ogre::Animation::IM_LINEAR);
    anim->setRotationInterpolationMode(Ogre::Animation::RIM_LINEAR);

    // The rig's natural STANDING pose. Mixamo skeletons have an identity bone
    // rest pose — the real standing orientation lives in frame 0 of their
    // existing animation, not in the bone transform. So harvest each bone's
    // orientation at t=0 of the rig's first existing animation; that is the
    // "bind" we compose our CMU motion onto (without it the body inverts, since
    // the bone rest is a meaningless identity). Falls back to the bone's own
    // rest if there's no prior animation.
    struct StandXform { Ogre::Quaternion rot; Ogre::Vector3 pos; Ogre::Vector3 scale; bool has = false; };
    std::unordered_map<unsigned short, StandXform> standPose;
    if (skel->getNumAnimations() > 0) {
        Ogre::Animation* ref = nullptr;
        for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
            Ogre::Animation* a = skel->getAnimation(ai);
            if (a && a->getName() != animName) { ref = a; break; }
        }
        if (ref) {
            for (const auto& [h, trk] : ref->_getNodeTrackList()) {
                if (!trk || trk->getNumKeyFrames() == 0) continue;
                Ogre::TransformKeyFrame f0(nullptr, 0.0f);
                trk->getInterpolatedKeyFrame(ref->_getTimeIndex(0.0f), &f0);
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
    std::vector<std::vector<Ogre::Quaternion>> cmuLocalDelta(
        J, std::vector<Ogre::Quaternion>(frames, Ogre::Quaternion::IDENTITY));
    for (int c = 0; c < J; ++c) {
        const int pc = kParentCanon[c];
        Ogre::Quaternion local0;
        for (int f = 0; f < frames; ++f) {
            Ogre::Quaternion local = (worldFrame && pc >= 0)
                ? clipQ(f, pc).Inverse() * clipQ(f, c)   // world→local
                : clipQ(f, c);                            // already local (or root)
            if (f == 0) local0 = local;
            cmuLocalDelta[c][f] = local0.Inverse() * local;
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
        if (worldFrame && c > 0 && haveAnyStand)
            Mc = standWorldOf(bone).Inverse() * clipQ(0, c);  // target-stand → CMU-rest
        const Ogre::Quaternion McInv = Mc.Inverse();
        for (int f = 0; f < frames; ++f) {
            const Ogre::Quaternion local = (c == 0)
                ? bind
                : (bind * (McInv * cmuLocalDelta[c][f] * Mc));
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
