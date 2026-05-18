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
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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

bool PoseLibrary::applyPoseMasked(Ogre::Entity* entity,
                                  const QString& name,
                                  const QSet<QString>& boneFilter)
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
    int filteredOut = 0;
    int missingBones = 0;
    for (auto it = poseIt->cbegin(); it != poseIt->cend(); ++it) {
        // Filter check first — names not in the mask are skipped
        // without consulting the skeleton, so "applies only to the
        // jaw" is a single hash lookup per pose entry.
        if (!boneFilter.contains(it.key())) { ++filteredOut; continue; }
        const std::string boneName = it.key().toStdString();
        if (!skel->hasBone(boneName)) { ++missingBones; continue; }
        Ogre::Bone* bone = skel->getBone(boneName);
        if (!bone) { ++missingBones; continue; }
        bone->setPosition(it.value().translate);
        bone->setOrientation(it.value().rotation);
        bone->setScale(it.value().scale);
        ++boneCount;
    }

    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("apply pose '%1' masked (%2 bones, %3 filtered, %4 missing)")
            .arg(name).arg(boneCount).arg(filteredOut).arg(missingBones));
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

namespace {

// Schema string written into / verified out of the sidecar JSON.
// Bump when the format changes incompatibly so loadPoseLibrary can
// reject older files cleanly.
constexpr const char* kPoseLibSchemaV1 = "qtmesheditor.poselib.v1";

} // namespace

bool PoseLibrary::savePoseLibrary(Ogre::Entity* entity, const QString& filePath) const
{
    assertMainThread();
    if (!entity || filePath.isEmpty()) return false;
    auto entIt = m_byEntity.constFind(entity);
    if (entIt == m_byEntity.constEnd()) return false;
    if (entIt->order.isEmpty()) return false;

    QJsonArray poses;
    for (const QString& name : entIt->order) {
        auto poseIt = entIt->byName.constFind(name);
        if (poseIt == entIt->byName.constEnd()) continue;
        QJsonObject bones;
        for (auto bIt = poseIt->cbegin(); bIt != poseIt->cend(); ++bIt) {
            const auto& trs = bIt.value();
            QJsonObject bone;
            bone["t"] = QJsonArray{ trs.translate.x, trs.translate.y, trs.translate.z };
            bone["r"] = QJsonArray{ trs.rotation.w, trs.rotation.x,
                                     trs.rotation.y, trs.rotation.z };
            bone["s"] = QJsonArray{ trs.scale.x, trs.scale.y, trs.scale.z };
            bones[bIt.key()] = bone;
        }
        QJsonObject poseObj;
        poseObj["name"] = name;
        poseObj["bones"] = bones;
        poses.append(poseObj);
    }

    QJsonObject root;
    root["schema"] = kPoseLibSchemaV1;
    root["poses"] = poses;

    // QSaveFile gives atomic write: temp file + rename on commit, so
    // a power loss / kill -9 mid-write doesn't leave a half-written
    // sidecar that loadPoseLibrary later rejects.
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) return false;

    SentryReporter::addBreadcrumb("file.export",
        QStringLiteral("save library to '%1' (%2 poses)")
            .arg(filePath).arg(entIt->order.size()));
    return true;
}

bool PoseLibrary::loadPoseLibrary(Ogre::Entity* entity, const QString& filePath)
{
    assertMainThread();
    if (!entity || filePath.isEmpty()) return false;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    file.close();
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) return false;
    if (!doc.isObject()) return false;
    const QJsonObject root = doc.object();
    if (root.value("schema").toString() != QString::fromLatin1(kPoseLibSchemaV1))
        return false;
    // Codex P1 on PR #602: validate the payload shape BEFORE wiping
    // the in-memory library. A schema-matching file with `poses`
    // missing or non-array would otherwise silently drop the user's
    // existing data and return success.
    const QJsonValue posesV = root.value("poses");
    if (!posesV.isArray()) return false;
    const QJsonArray poses = posesV.toArray();

    auto readVec3 = [](const QJsonArray& a, const Ogre::Vector3& def) -> Ogre::Vector3 {
        if (a.size() != 3) return def;
        return Ogre::Vector3(static_cast<Ogre::Real>(a[0].toDouble()),
                             static_cast<Ogre::Real>(a[1].toDouble()),
                             static_cast<Ogre::Real>(a[2].toDouble()));
    };
    auto readQuat = [](const QJsonArray& a, const Ogre::Quaternion& def) -> Ogre::Quaternion {
        if (a.size() != 4) return def;
        return Ogre::Quaternion(static_cast<Ogre::Real>(a[0].toDouble()),
                                static_cast<Ogre::Real>(a[1].toDouble()),
                                static_cast<Ogre::Real>(a[2].toDouble()),
                                static_cast<Ogre::Real>(a[3].toDouble()));
    };

    // Build the new library entry off to the side so any parse
    // failure leaves the in-memory store untouched.
    EntityPoses staging;
    for (const QJsonValue& p : poses) {
        if (!p.isObject()) continue;
        const QJsonObject pObj = p.toObject();
        const QString name = pObj.value("name").toString();
        if (name.isEmpty()) continue;
        PoseSnapshot snapshot;
        const QJsonObject bones = pObj.value("bones").toObject();
        for (auto it = bones.constBegin(); it != bones.constEnd(); ++it) {
            const QJsonObject boneObj = it.value().toObject();
            BonePoseSnapshot trs;
            trs.translate = readVec3(boneObj.value("t").toArray(),
                                      Ogre::Vector3::ZERO);
            trs.rotation = readQuat(boneObj.value("r").toArray(),
                                     Ogre::Quaternion::IDENTITY);
            trs.scale = readVec3(boneObj.value("s").toArray(),
                                  Ogre::Vector3(1, 1, 1));
            snapshot.insert(it.key(), trs);
        }
        // Codex P2 on PR #602: only append `order` on first sighting
        // of the name so duplicate entries in the file don't leave
        // `order` with phantom names that survive a `deletePose`.
        // Later occurrences of the same name overwrite the snapshot
        // (last-write-wins) like a regular `savePose` does.
        const bool isFirstSighting = !staging.byName.contains(name);
        staging.byName.insert(name, snapshot);
        if (isFirstSighting) staging.order.append(name);
    }

    // All-or-nothing replacement: now we know the file parsed
    // cleanly, swap the per-entity entry. Partial overlay would be
    // confusing UX (which pose wins on name collision?), so we go
    // with replacement.
    m_byEntity.insert(entity, staging);

    SentryReporter::addBreadcrumb("file.import",
        QStringLiteral("load library from '%1' (%2 poses)")
            .arg(filePath).arg(staging.order.size()));
    emit posesChanged(entity);
    return true;
}

bool PoseLibrary::savePoseLibraryForSelection(const QString& filePath) const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return false;
    return savePoseLibrary(ents.first(), filePath);
}

bool PoseLibrary::loadPoseLibraryForSelection(const QString& filePath)
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return false;
    return loadPoseLibrary(ents.first(), filePath);
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
