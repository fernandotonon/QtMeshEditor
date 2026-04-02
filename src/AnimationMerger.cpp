#include "AnimationMerger.h"
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <QSet>
#include <QMap>
#include <QRegularExpression>
#include <unordered_map>

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
// identity root while a mesh FBX has R_x(-90°) root — the stored delta must be
// re-expressed in the target bone's local space via:
//   correction = q_dst.Inverse() * q_src
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
