#include "AnimationMerger.h"
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <QSet>
#include <QRegularExpression>

// Remove common Mixamo noise from animation names before slugifying.
// e.g. "Armature|mixamo.com|Layer0" → "Armature|Layer0"
static QString cleanMixamoNoise(const QString& name)
{
    QString s = name;
    s.replace(QRegularExpression("\\|?mixamo\\.com\\|?"), "|");
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

// Build a clean animation name: slugify the prefix and the animation name separately,
// then combine. Removes duplicated prefix (e.g. "idle" + "idle" → "idle", not "idle_idle").
static QString buildAnimName(const QString& prefix, const QString& animName)
{
    QString cleanAnim = cleanMixamoNoise(animName);
    QString slugAnim = slugify(cleanAnim);
    QString slugPrefix = slugify(prefix);

    // If the animation name equals the prefix, just use the prefix once
    // e.g. node="idle", anim="idle" → "idle" (not "idle_idle")
    if (slugAnim == slugPrefix || slugAnim.isEmpty())
        return slugPrefix;

    return slugPrefix + "_" + slugAnim;
}

// Deduplicate a name against an existing set, appending _2, _3, etc. if needed.
static QString deduplicateName(const QString& desired, QSet<QString>& existingNames)
{
    if (!existingNames.contains(desired)) {
        existingNames.insert(desired);
        return desired;
    }
    int suffix = 2;
    QString candidate;
    do {
        candidate = desired + "_" + QString::number(suffix++);
    } while (existingNames.contains(candidate));
    existingNames.insert(candidate);
    return candidate;
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

    // --- Step 1: Rename base entity animations with prefix + cleanup ---
    // Only prefix base animations that don't already start with the base slug
    // (prevents re-prefixing on repeated merges: "test_jump" won't become "test_test_jump")
    QSet<QString> existingNames;
    {
        QString baseRawName;
        if (auto* parentNode = baseEntity->getParentSceneNode())
            baseRawName = QString::fromStdString(parentNode->getName());
        else
            baseRawName = QString::fromStdString(baseEntity->getName());
        QString baseSlug = slugify(baseRawName);

        // Collect renames needed (skip animations already prefixed)
        QList<std::pair<std::string, std::string>> baseRenames;
        for (unsigned short i = 0; i < baseSkel->getNumAnimations(); ++i)
        {
            std::string origName = baseSkel->getAnimation(i)->getName();
            QString origSlug = slugify(QString::fromStdString(origName));

            // Skip if already prefixed with the base slug (from a previous merge)
            if (origSlug.startsWith(baseSlug + "_") || origSlug == baseSlug) {
                // Keep as-is, just clean Mixamo noise if present
                QString cleaned = QString::fromStdString(origName);
                if (cleaned.contains("mixamo.com", Qt::CaseInsensitive)) {
                    QString desired = slugify(cleanMixamoNoise(cleaned));
                    QString finalName = deduplicateName(desired, existingNames);
                    baseRenames.append({origName, finalName.toStdString()});
                } else {
                    existingNames.insert(QString::fromStdString(origName));
                }
                continue;
            }

            QString desired = buildAnimName(baseRawName, QString::fromStdString(origName));
            QString finalName = deduplicateName(desired, existingNames);
            baseRenames.append({origName, finalName.toStdString()});
        }

        // Two-pass rename to avoid collisions: a new name might equal an old name
        // that hasn't been renamed yet.
        QList<std::pair<std::string, std::string>> tempToFinal;
        for (int i = 0; i < baseRenames.size(); ++i)
        {
            std::string tempName = "__merge_temp_" + std::to_string(i);
            renameAnimation(baseSkel.get(), baseRenames[i].first, tempName);
            tempToFinal.append({tempName, baseRenames[i].second});
        }
        for (const auto& [tempName, finalName] : tempToFinal)
            renameAnimation(baseSkel.get(), tempName, finalName);

        // If no renames were needed, populate existingNames from current state
        if (baseRenames.isEmpty()) {
            for (unsigned short i = 0; i < baseSkel->getNumAnimations(); ++i)
                existingNames.insert(QString::fromStdString(baseSkel->getAnimation(i)->getName()));
        }
    }

    // --- Step 2: Merge source animations with prefix + cleanup ---
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

        Ogre::Skeleton::BoneHandleMap boneHandleMap;
        baseSkel->_buildMapBoneByName(srcSkel.get(), boneHandleMap);

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

        for (const auto& [oldName, newName] : renameList)
            renameAnimation(srcSkel.get(), oldName, newName);

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
