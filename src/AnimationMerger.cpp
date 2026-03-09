#include "AnimationMerger.h"
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <QSet>
#include <QRegularExpression>

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

    // Use Ogre's built-in bone name mapping to check compatibility
    Ogre::Skeleton::BoneHandleMap boneHandleMap;
    a->_buildMapBoneByName(b.get(), boneHandleMap);

    // The map has one entry per bone in skeleton 'a'.
    // An entry equal to b->getNumBones() means "no match found" for that bone.
    unsigned short numBonesSrc = b->getNumBones();
    for (auto handle : boneHandleMap)
    {
        if (handle == numBonesSrc)
            return false;
    }
    return true;
}

Ogre::Entity* AnimationMerger::mergeAnimations(
    Ogre::Entity* baseEntity,
    const QList<Ogre::Entity*>& sourceEntities,
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

    // Collect existing animation names to detect collisions
    QSet<QString> existingNames;
    for (unsigned short i = 0; i < baseSkel->getNumAnimations(); ++i)
        existingNames.insert(QString::fromStdString(baseSkel->getAnimation(i)->getName()));

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

        // Skip if source shares the same skeleton resource as base
        // (merging a skeleton with itself is undefined behavior)
        if (srcSkel.get() == baseSkel.get())
            continue;

        // Check compatibility
        if (!areSkeletonsCompatible(baseSkel, srcSkel))
        {
            errorMsg = QString("Skeleton of '%1' is incompatible with base skeleton")
                .arg(srcEntity->getName().c_str());
            return nullptr;
        }

        // Build bone handle map for the merge
        Ogre::Skeleton::BoneHandleMap boneHandleMap;
        baseSkel->_buildMapBoneByName(srcSkel.get(), boneHandleMap);

        // Determine slug from node name (e.g. "Hip Hop Dancing" → "hip_hop_dancing")
        QString rawName;
        if (auto* parentNode = srcEntity->getParentSceneNode())
            rawName = QString::fromStdString(parentNode->getName());
        else
            rawName = QString::fromStdString(srcEntity->getName());
        QString slug = slugify(rawName);

        unsigned short numAnims = srcSkel->getNumAnimations();

        // Rename animations on the source skeleton BEFORE merging.
        // Format: {slug}_{originalAnimName}
        // Ogre's _mergeSkeletonAnimations copies source animation names as-is,
        // and Animation has no setName(), so we must rename on the source first.
        // This is safe because the source entities will be destroyed after merge.
        QList<std::pair<std::string, std::string>> renameList;
        for (unsigned short i = 0; i < numAnims; ++i)
        {
            Ogre::Animation* anim = srcSkel->getAnimation(i);
            std::string originalName = anim->getName();

            QString desiredName = slug + "_" + QString::fromStdString(originalName);

            // Handle name collisions by appending _2, _3, etc.
            QString finalName = desiredName;
            int suffix = 2;
            while (existingNames.contains(finalName))
            {
                finalName = desiredName + "_" + QString::number(suffix);
                ++suffix;
            }

            existingNames.insert(finalName);
            renameList.append({originalName, finalName.toStdString()});
        }

        // Apply renames on the source skeleton
        for (const auto& [oldName, newName] : renameList)
            renameAnimation(srcSkel.get(), oldName, newName);

        // Merge all animations (empty vector = all). Ogre handles track/keyframe
        // copying, bone handle remapping, and binding pose differences.
        baseSkel->_mergeSkeletonAnimations(srcSkel.get(), boneHandleMap);
        mergedCount += numAnims;
    }

    // Refresh the entity's animation states to pick up new animations
    baseEntity->refreshAvailableAnimationState();

    if (mergedCount == 0)
    {
        errorMsg = "No animations were merged (no valid source entities found)";
        return nullptr;
    }

    return baseEntity;
}
