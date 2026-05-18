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
    assertMainThread();
    if (!entity || name.isEmpty()) return false;
    auto it = m_byEntity.constFind(entity);
    if (it == m_byEntity.constEnd()) return false;
    return it->byName.contains(name);
}

QStringList PoseLibrary::listPoses(Ogre::Entity* entity) const
{
    assertMainThread();
    if (!entity) return {};
    auto it = m_byEntity.constFind(entity);
    if (it == m_byEntity.constEnd()) return {};
    return it->order;
}

QString PoseLibrary::flipBoneName(const QString& boneName)
{
    // Mixamo / generic _l ↔ _r suffix. Case-preserving on the
    // suffix letter so "BoneL" stays uppercase, "bone_l" stays
    // lowercase, etc.
    if (boneName.endsWith(QStringLiteral("_l"))) {
        return boneName.left(boneName.size() - 2) + QStringLiteral("_r");
    }
    if (boneName.endsWith(QStringLiteral("_r"))) {
        return boneName.left(boneName.size() - 2) + QStringLiteral("_l");
    }
    if (boneName.endsWith(QStringLiteral("_L"))) {
        return boneName.left(boneName.size() - 2) + QStringLiteral("_R");
    }
    if (boneName.endsWith(QStringLiteral("_R"))) {
        return boneName.left(boneName.size() - 2) + QStringLiteral("_L");
    }
    // Blender .L / .R convention.
    if (boneName.endsWith(QStringLiteral(".L"))) {
        return boneName.left(boneName.size() - 2) + QStringLiteral(".R");
    }
    if (boneName.endsWith(QStringLiteral(".R"))) {
        return boneName.left(boneName.size() - 2) + QStringLiteral(".L");
    }
    // Maya Left / Right prefix. We require the prefix to be
    // followed by an uppercase letter so "Lefty" isn't flipped
    // to "Righty"; the convention is "LeftHand", "RightArm" etc.
    auto hasPrefixWord = [&](const QString& prefix) {
        if (!boneName.startsWith(prefix)) return false;
        if (boneName.size() == prefix.size()) return true;
        const QChar next = boneName.at(prefix.size());
        return next.isUpper() || next == QLatin1Char('_');
    };
    if (hasPrefixWord(QStringLiteral("Left"))) {
        return QStringLiteral("Right") + boneName.mid(4);
    }
    if (hasPrefixWord(QStringLiteral("Right"))) {
        return QStringLiteral("Left") + boneName.mid(5);
    }
    return boneName;
}

bool PoseLibrary::mirrorPose(Ogre::Entity* entity,
                             const QString& srcName,
                             const QString& dstName)
{
    assertMainThread();
    if (!entity || srcName.isEmpty() || dstName.isEmpty()) return false;
    auto* skel = skeletonOf(entity);
    if (!skel) return false;

    // Read the source pose directly from m_byEntity. Codex P2 on
    // PR #597: an earlier draft re-captured the entire live
    // skeleton after applyPose, which silently pulled in live
    // bone values for any bone NOT in the saved pose (LOD diff,
    // partial save). Reading from the stored snapshot keeps mirror
    // deterministic — only the bones that were actually saved
    // contribute to the result.
    auto entIt = m_byEntity.constFind(entity);
    if (entIt == m_byEntity.constEnd()) return false;
    auto poseIt = entIt->byName.constFind(srcName);
    if (poseIt == entIt->byName.constEnd()) return false;
    const PoseSnapshot& src = *poseIt;

    // Build the mirrored snapshot: for each source bone, look up
    // its mirrored counterpart's name and write the X-flipped TRS
    // under that key. Bones whose flipped name is the same as the
    // original (centre-line: Spine, Hips, Head, etc.) get the
    // reflected TRS in place.
    PoseSnapshot mirrored;
    mirrored.reserve(src.size());
    for (auto it = src.cbegin(); it != src.cend(); ++it) {
        const QString flipped = flipBoneName(it.key());
        BonePoseSnapshot bs;
        const auto& s = it.value();
        bs.translate = Ogre::Vector3(-s.translate.x, s.translate.y, s.translate.z);
        // X-symmetric reflection of a quaternion: keep w, x; flip y, z.
        // Equivalent to conjugating by the reflection-X transform.
        bs.rotation = Ogre::Quaternion(s.rotation.w, s.rotation.x,
                                       -s.rotation.y, -s.rotation.z);
        // Negative scale.x is the standard mirror trick — preserves
        // volume and produces the correct mirrored orientation when
        // the renderer applies the bone transform.
        bs.scale = Ogre::Vector3(-s.scale.x, s.scale.y, s.scale.z);
        mirrored.insert(flipped, bs);
    }

    // Write the mirrored snapshot directly into m_byEntity. We're
    // already inside the class, and the live skeleton isn't a
    // necessary intermediate — bypassing `savePose` here also
    // avoids the round-trip-apply that the public surface would
    // require. Preserves the byName + order parallel storage and
    // emits posesChanged just like a normal save would.
    auto& store = m_byEntity[entity];
    const bool isOverwrite = store.byName.contains(dstName);
    store.byName.insert(dstName, mirrored);
    if (!isOverwrite) store.order.append(dstName);

    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("mirror '%1' -> '%2' (%3 bones%4)")
            .arg(srcName, dstName).arg(mirrored.size())
            .arg(isOverwrite ? QStringLiteral(", overwrite") : QString()));
    emit posesChanged(entity);
    return true;
}

bool PoseLibrary::forgetEntity(Ogre::Entity* entity)
{
    assertMainThread();
    if (!entity) return false;
    if (!m_byEntity.contains(entity)) return false;
    m_byEntity.remove(entity);
    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("forget entity (entity-side teardown)"));
    return true;
}

void PoseLibrary::clearAll()
{
    assertMainThread();
    const int n = m_byEntity.size();
    m_byEntity.clear();
    if (n > 0) {
        SentryReporter::addBreadcrumb("scene.anim.pose",
            QStringLiteral("clear all (%1 entities)").arg(n));
    }
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

bool PoseLibrary::mirrorPoseForSelection(const QString& srcName,
                                          const QString& dstName)
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return false;
    return mirrorPose(ents.first(), srcName, dstName);
}

QStringList PoseLibrary::listPosesForSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return {};
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return {};
    return listPoses(ents.first());
}
