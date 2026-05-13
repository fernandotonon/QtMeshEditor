#include "AnimationMerger.h"
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreKeyFrame.h>
#include <QSet>
#include <QMap>
#include <QRegularExpression>
#include <cctype>
#include <unordered_map>
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
