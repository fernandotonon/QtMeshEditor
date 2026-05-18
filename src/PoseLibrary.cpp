/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "PoseLibrary.h"

#include "SelectionSet.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QThread>

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>

namespace {

// Singletons run on the main thread by project convention (CLAUDE.md).
// Assert at the lifecycle entry points so a regression surfaces
// loudly in debug builds.
inline void assertMainThread()
{
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
}

// Both `Entity::hasSkeleton` and SkeletonInstance access are guarded
// here so the rest of the manager can assume non-null. Returns null
// when the entity has no skeleton attached (a static prop, a
// non-skinned scene-node child, etc.).
Ogre::SkeletonInstance* skeletonOf(Ogre::Entity* entity)
{
    if (!entity) return nullptr;
    if (!entity->hasSkeleton()) return nullptr;
    return entity->getSkeleton();
}

} // namespace

PoseLibrary* PoseLibrary::s_instance = nullptr;

PoseLibrary* PoseLibrary::instance()
{
    assertMainThread();
    if (!s_instance) s_instance = new PoseLibrary();
    return s_instance;
}

PoseLibrary* PoseLibrary::qmlInstance(QQmlEngine*, QJSEngine*)
{
    assertMainThread();
    return instance();
}

void PoseLibrary::kill()
{
    assertMainThread();
    if (!s_instance) return;
    delete s_instance;
    s_instance = nullptr;
}

PoseLibrary::PoseLibrary(QObject* parent) : QObject(parent) {}
PoseLibrary::~PoseLibrary() = default;

bool PoseLibrary::savePose(Ogre::Entity* entity, const QString& name)
{
    assertMainThread();
    if (!entity || name.isEmpty()) return false;
    auto* skel = skeletonOf(entity);
    if (!skel) return false;

    PoseSnapshot snapshot;
    snapshot.reserve(skel->getNumBones());
    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        Ogre::Bone* bone = skel->getBone(i);
        if (!bone) continue;
        BonePoseSnapshot bs;
        bs.translate = bone->getPosition();
        bs.rotation = bone->getOrientation();
        bs.scale = bone->getScale();
        snapshot.insert(QString::fromStdString(bone->getName()), bs);
    }

    auto& store = m_byEntity[entity];
    const bool isOverwrite = store.byName.contains(name);
    store.byName.insert(name, snapshot);
    if (!isOverwrite) store.order.append(name);

    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("save pose '%1' (%2 bones%3)")
            .arg(name).arg(snapshot.size())
            .arg(isOverwrite ? ", overwrite" : ""));

    emit posesChanged(entity);
    return true;
}

bool PoseLibrary::applyPose(Ogre::Entity* entity, const QString& name)
{
    assertMainThread();
    if (!entity || name.isEmpty()) return false;
    auto storeIt = m_byEntity.constFind(entity);
    if (storeIt == m_byEntity.constEnd()) return false;
    auto poseIt = storeIt->byName.constFind(name);
    if (poseIt == storeIt->byName.constEnd()) return false;

    auto* skel = skeletonOf(entity);
    if (!skel) return false;

    int boneCount = 0;
    int skipped = 0;
    for (auto it = poseIt->cbegin(); it != poseIt->cend(); ++it) {
        const std::string boneName = it.key().toStdString();
        if (!skel->hasBone(boneName)) {
            // Bone present at save time but missing now (e.g. LOD
            // change, skeleton swap). Skip silently — partial apply
            // is more useful than refusing the whole pose.
            ++skipped;
            continue;
        }
        Ogre::Bone* bone = skel->getBone(boneName);
        if (!bone) { ++skipped; continue; }
        bone->setPosition(it.value().translate);
        bone->setOrientation(it.value().rotation);
        bone->setScale(it.value().scale);
        ++boneCount;
    }

    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("apply pose '%1' (%2 bones%3)")
            .arg(name).arg(boneCount)
            .arg(skipped > 0 ? QStringLiteral(", %1 skipped").arg(skipped)
                             : QString()));
    return true;
}

bool PoseLibrary::deletePose(Ogre::Entity* entity, const QString& name)
{
    assertMainThread();
    if (!entity || name.isEmpty()) return false;
    auto storeIt = m_byEntity.find(entity);
    if (storeIt == m_byEntity.end()) return false;
    if (!storeIt->byName.contains(name)) return false;
    storeIt->byName.remove(name);
    storeIt->order.removeOne(name);
    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("delete pose '%1'").arg(name));
    emit posesChanged(entity);
    return true;
}

bool PoseLibrary::hasPose(Ogre::Entity* entity, const QString& name) const
{
    if (!entity || name.isEmpty()) return false;
    auto it = m_byEntity.constFind(entity);
    if (it == m_byEntity.constEnd()) return false;
    return it->byName.contains(name);
}

QStringList PoseLibrary::listPoses(Ogre::Entity* entity) const
{
    if (!entity) return {};
    auto it = m_byEntity.constFind(entity);
    if (it == m_byEntity.constEnd()) return {};
    return it->order;
}

bool PoseLibrary::savePoseForSelection(const QString& name)
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return false;
    return savePose(ents.first(), name);
}

bool PoseLibrary::applyPoseForSelection(const QString& name)
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return false;
    return applyPose(ents.first(), name);
}

bool PoseLibrary::deletePoseForSelection(const QString& name)
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return false;
    return deletePose(ents.first(), name);
}

QStringList PoseLibrary::listPosesForSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return {};
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return {};
    return listPoses(ents.first());
}
