/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "PoseLibrary.h"

#include "GamificationManager.h"

#include "ModelTurntableRenderer.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/PoseLibraryCommands.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>

#include <algorithm>

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

PoseLibrary::PoseSnapshot PoseLibrary::captureLive(Ogre::Entity* entity)
{
    PoseSnapshot snapshot;
    auto* skel = skeletonOf(entity);
    if (!skel) return snapshot;
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
    return snapshot;
}

int PoseLibrary::applySnapshot(Ogre::Entity* entity, const PoseSnapshot& snapshot)
{
    auto* skel = skeletonOf(entity);
    if (!skel) return 0;
    int written = 0;
    for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it) {
        const std::string boneName = it.key().toStdString();
        // Bone present at save time but missing now (e.g. LOD change,
        // skeleton swap). Skip silently — a partial apply is more
        // useful than refusing the whole pose.
        if (!skel->hasBone(boneName)) continue;
        Ogre::Bone* bone = skel->getBone(boneName);
        if (!bone) continue;
        bone->setPosition(it.value().translate);
        bone->setOrientation(it.value().rotation);
        bone->setScale(it.value().scale);
        ++written;
    }
    return written;
}

QString PoseLibrary::thumbKey(Ogre::Entity* entity, const QString& name)
{
    return QStringLiteral("%1/%2")
        .arg(reinterpret_cast<quintptr>(entity)).arg(name);
}

void PoseLibrary::invalidateThumbnail(Ogre::Entity* entity, const QString& name)
{
    m_thumbCache.remove(thumbKey(entity, name));
}

bool PoseLibrary::storePose(Ogre::Entity* entity,
                            const QString& name,
                            const PoseSnapshot& snapshot)
{
    auto& store = m_byEntity[entity];
    const bool isOverwrite = store.byName.contains(name);
    store.byName.insert(name, snapshot);
    if (!isOverwrite) store.order.append(name);
    // The pose's CONTENT changed, so any cached render of it is stale.
    invalidateThumbnail(entity, name);
    emit posesChanged(entity);
    return isOverwrite;
}

bool PoseLibrary::savePose(Ogre::Entity* entity, const QString& name)
{
    assertMainThread();
    if (!entity || name.isEmpty()) return false;
    if (!skeletonOf(entity)) return false;

    const PoseSnapshot snapshot = captureLive(entity);
    const bool isOverwrite = storePose(entity, name, snapshot);

    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("save pose '%1' (%2 bones%3)")
            .arg(name).arg(snapshot.size())
            .arg(isOverwrite ? ", overwrite" : ""));
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

    if (!skeletonOf(entity)) return false;

    const int total = poseIt->size();
    const int boneCount = applySnapshot(entity, *poseIt);
    const int skipped = total - boneCount;

    // A snap-apply supersedes any in-flight time blend on this entity —
    // otherwise the next tick would drag the skeleton back toward the
    // old target and visibly fight the pose the user just applied.
    cancelBlend(entity);

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
    invalidateThumbnail(entity, name);
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
    const bool isOverwrite = storePose(entity, dstName, mirrored);

    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("mirror '%1' -> '%2' (%3 bones%4)")
            .arg(srcName, dstName).arg(mirrored.size())
            .arg(isOverwrite ? QStringLiteral(", overwrite") : QString()));
    return true;
}

bool PoseLibrary::blendPoses(Ogre::Entity* entity,
                             const QString& aName,
                             const QString& bName,
                             float weight,
                             const QString& dstName)
{
    assertMainThread();
    if (!entity || aName.isEmpty() || bName.isEmpty() || dstName.isEmpty())
        return false;

    auto entIt = m_byEntity.constFind(entity);
    if (entIt == m_byEntity.constEnd()) return false;
    auto aIt = entIt->byName.constFind(aName);
    if (aIt == entIt->byName.constEnd()) return false;
    auto bIt = entIt->byName.constFind(bName);
    if (bIt == entIt->byName.constEnd()) return false;

    // Copy the sources before we touch m_byEntity — `storePose` below
    // may rehash the container (dstName can be a brand-new key), which
    // would invalidate `aIt`/`bIt` and leave us reading freed memory.
    // This also makes dstName == aName / bName safe.
    const PoseSnapshot a = *aIt;
    const PoseSnapshot b = *bIt;

    // Clamp rather than extrapolate: past the endpoints slerp keeps
    // rotating and real rigs fold in on themselves, which reads as a
    // bug rather than a feature.
    const float w = std::clamp(weight, 0.0f, 1.0f);

    PoseSnapshot blended;
    blended.reserve(a.size() + b.size());

    for (auto it = a.cbegin(); it != a.cend(); ++it) {
        auto bBone = b.constFind(it.key());
        if (bBone == b.constEnd()) {
            // Only in A — nothing to interpolate toward. Take it
            // verbatim; falling back to the live skeleton would make
            // the result depend on the current pose.
            blended.insert(it.key(), it.value());
            continue;
        }
        const BonePoseSnapshot& sa = it.value();
        const BonePoseSnapshot& sb = bBone.value();
        BonePoseSnapshot out;
        out.translate = sa.translate + (sb.translate - sa.translate) * w;
        out.scale     = sa.scale     + (sb.scale     - sa.scale)     * w;
        // shortestPath=true is the "dual-quat-correct" behaviour the
        // issue asks for: without it a blend between orientations more
        // than 180° apart takes the long way round and the limb spins.
        out.rotation  = Ogre::Quaternion::Slerp(w, sa.rotation, sb.rotation,
                                                /*shortestPath*/ true);
        blended.insert(it.key(), out);
    }
    // Bones only in B.
    for (auto it = b.cbegin(); it != b.cend(); ++it) {
        if (!a.contains(it.key())) blended.insert(it.key(), it.value());
    }

    const bool isOverwrite = storePose(entity, dstName, blended);

    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("blend '%1'+'%2' @%3 -> '%4' (%5 bones%6)")
            .arg(aName, bName)
            .arg(static_cast<double>(w), 0, 'f', 3)
            .arg(dstName).arg(blended.size())
            .arg(isOverwrite ? QStringLiteral(", overwrite") : QString()));
    return true;
}

bool PoseLibrary::applyPoseBlended(Ogre::Entity* entity,
                                   const QString& name,
                                   float durationSeconds)
{
    assertMainThread();
    if (!entity || name.isEmpty()) return false;
    auto entIt = m_byEntity.constFind(entity);
    if (entIt == m_byEntity.constEnd()) return false;
    auto poseIt = entIt->byName.constFind(name);
    if (poseIt == entIt->byName.constEnd()) return false;
    if (!skeletonOf(entity)) return false;

    // Non-positive duration means "snap" — same contract as applyPose,
    // and it keeps callers from having to special-case a 0 slider.
    if (durationSeconds <= 0.0f) {
        m_blends.remove(entity);
        return applyPose(entity, name);
    }

    ActiveBlend blend;
    blend.name = name;
    blend.from = captureLive(entity);
    // Copy (not reference) the target so deleting / overwriting the
    // source pose mid-transition can't dangle.
    blend.to = *poseIt;
    blend.elapsed = 0.0f;
    blend.duration = durationSeconds;
    // Replaces any in-flight blend on this entity.
    m_blends.insert(entity, blend);

    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("apply pose '%1' blended over %2s (%3 bones)")
            .arg(name)
            .arg(static_cast<double>(durationSeconds), 0, 'f', 2)
            .arg(blend.to.size()));
    return true;
}

int PoseLibrary::tickBlend(float dt)
{
    assertMainThread();
    if (m_blends.isEmpty()) return 0;

    // Collect finished blends and emit AFTER the loop — a slot on
    // blendFinished could call back into the library (e.g. start
    // another blend) and mutate m_blends while we're iterating it.
    QList<QPair<Ogre::Entity*, QString>> finished;

    for (auto it = m_blends.begin(); it != m_blends.end(); ) {
        Ogre::Entity* entity = it.key();
        ActiveBlend& blend = it.value();
        blend.elapsed += dt;

        // Smoothstep easing so the transition eases in AND out — a
        // linear ramp reads as mechanical on a character.
        const float linear = blend.duration > 0.0f
            ? std::clamp(blend.elapsed / blend.duration, 0.0f, 1.0f)
            : 1.0f;
        const float t = linear * linear * (3.0f - 2.0f * linear);
        const bool done = linear >= 1.0f;

        auto* skel = skeletonOf(entity);
        if (!skel) {
            // Entity lost its skeleton mid-blend — drop the transition
            // rather than dereferencing a stale instance every frame.
            finished.append({entity, blend.name});
            it = m_blends.erase(it);
            continue;
        }

        for (auto b = blend.to.cbegin(); b != blend.to.cend(); ++b) {
            const std::string boneName = b.key().toStdString();
            if (!skel->hasBone(boneName)) continue;
            Ogre::Bone* bone = skel->getBone(boneName);
            if (!bone) continue;
            auto fromIt = blend.from.constFind(b.key());
            // A bone with no captured start (present in the target but
            // not in the live capture) has nothing to ease from — snap
            // it to the target rather than blending from garbage.
            if (fromIt == blend.from.constEnd()) {
                bone->setPosition(b.value().translate);
                bone->setOrientation(b.value().rotation);
                bone->setScale(b.value().scale);
                continue;
            }
            const BonePoseSnapshot& f = fromIt.value();
            const BonePoseSnapshot& to = b.value();
            bone->setPosition(f.translate + (to.translate - f.translate) * t);
            bone->setScale(f.scale + (to.scale - f.scale) * t);
            bone->setOrientation(
                Ogre::Quaternion::Slerp(t, f.rotation, to.rotation, true));
        }

        if (done) {
            finished.append({entity, blend.name});
            it = m_blends.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& [entity, name] : finished) {
        SentryReporter::addBreadcrumb("scene.anim.pose",
            QStringLiteral("blend to '%1' finished").arg(name));
        emit blendFinished(entity, name);
    }
    return static_cast<int>(m_blends.size());
}

bool PoseLibrary::isBlending(Ogre::Entity* entity) const
{
    assertMainThread();
    if (!entity) return false;
    return m_blends.contains(entity);
}

bool PoseLibrary::cancelBlend(Ogre::Entity* entity)
{
    assertMainThread();
    if (!entity) return false;
    auto it = m_blends.constFind(entity);
    if (it == m_blends.constEnd()) return false;
    const QString name = it->name;
    m_blends.remove(entity);
    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("blend to '%1' cancelled").arg(name));
    emit blendFinished(entity, name);
    return true;
}

bool PoseLibrary::forgetEntity(Ogre::Entity* entity)
{
    assertMainThread();
    if (!entity) return false;
    // An in-flight blend holds a raw Entity* we'd otherwise keep
    // ticking after teardown — drop it even if the entity had no
    // saved poses.
    const bool hadBlend = m_blends.remove(entity) > 0;
    // Thumbnails are keyed by entity pointer; a later entity could
    // land on the same address, so purge this entity's entries.
    const QString prefix = QStringLiteral("%1/")
        .arg(reinterpret_cast<quintptr>(entity));
    for (auto it = m_thumbCache.begin(); it != m_thumbCache.end(); ) {
        if (it.key().startsWith(prefix)) it = m_thumbCache.erase(it);
        else ++it;
    }
    if (!m_byEntity.contains(entity)) return hadBlend;
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
    m_blends.clear();
    m_thumbCache.clear();
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
    // Every pose on this entity just changed content, so every
    // cached render of it is stale.
    const QString thumbPrefix = QStringLiteral("%1/")
        .arg(reinterpret_cast<quintptr>(entity));
    for (auto it = m_thumbCache.begin(); it != m_thumbCache.end(); ) {
        if (it.key().startsWith(thumbPrefix)) it = m_thumbCache.erase(it);
        else ++it;
    }
    // A blend targeting the old library's pose is meaningless now.
    m_blends.remove(entity);

    SentryReporter::addBreadcrumb("file.import",
        QStringLiteral("load library from '%1' (%2 poses)")
            .arg(filePath).arg(staging.order.size()));
    emit posesChanged(entity);
    return true;
}

namespace {

// Shared "first selected entity" resolution for every ForSelection
// wrapper. Returns null when nothing usable is selected.
Ogre::Entity* firstSelectedEntity()
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return nullptr;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return nullptr;
    return ents.first();
}

} // namespace

bool PoseLibrary::savePoseLibraryForSelection(const QString& filePath) const
{
    auto* entity = firstSelectedEntity();
    if (!entity) return false;
    return savePoseLibrary(entity, filePath);
}

bool PoseLibrary::loadPoseLibraryForSelection(const QString& filePath)
{
    auto* entity = firstSelectedEntity();
    if (!entity) return false;
    return loadPoseLibrary(entity, filePath);
}

bool PoseLibrary::savePoseForSelection(const QString& name)
{
    auto* entity = firstSelectedEntity();
    if (!entity) return false;
    GamificationManager::noteFeature(QStringLiteral("pose_library"));
    return savePose(entity, name);
}

bool PoseLibrary::applyPoseForSelection(const QString& name)
{
    auto* entity = firstSelectedEntity();
    if (!entity) return false;
    return applyPose(entity, name);
}

bool PoseLibrary::applyPoseMaskedForSelection(const QString& name,
                                              const QStringList& boneNames)
{
    auto* entity = firstSelectedEntity();
    if (!entity) return false;
    QSet<QString> mask(boneNames.cbegin(), boneNames.cend());
    return applyPoseMasked(entity, name, mask);
}

bool PoseLibrary::deletePoseForSelection(const QString& name)
{
    auto* entity = firstSelectedEntity();
    if (!entity) return false;
    return deletePose(entity, name);
}

bool PoseLibrary::mirrorPoseForSelection(const QString& srcName,
                                          const QString& dstName)
{
    auto* entity = firstSelectedEntity();
    if (!entity) return false;
    return mirrorPose(entity, srcName, dstName);
}

QStringList PoseLibrary::listPosesForSelection() const
{
    auto* entity = firstSelectedEntity();
    if (!entity) return {};
    return listPoses(entity);
}

void PoseLibrary::requestExportLibrary()
{
    assertMainThread();
    emit exportLibraryRequested();
}

void PoseLibrary::requestImportLibrary()
{
    assertMainThread();
    emit importLibraryRequested();
}

bool PoseLibrary::blendPosesForSelection(const QString& aName,
                                         const QString& bName,
                                         double weight,
                                         const QString& dstName)
{
    auto* entity = firstSelectedEntity();
    if (!entity) return false;
    GamificationManager::noteFeature(QStringLiteral("pose_library"));
    return blendPoses(entity, aName, bName,
                      static_cast<float>(weight), dstName);
}

bool PoseLibrary::applyPoseBlendedForSelection(const QString& name,
                                               double durationSeconds)
{
    auto* entity = firstSelectedEntity();
    if (!entity) return false;
    return applyPoseBlended(entity, name, static_cast<float>(durationSeconds));
}

QStringList PoseLibrary::boneNamesForSelection() const
{
    assertMainThread();
    auto* entity = firstSelectedEntity();
    auto* skel = skeletonOf(entity);
    if (!skel) return {};
    QStringList names;
    names.reserve(skel->getNumBones());
    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        if (Ogre::Bone* bone = skel->getBone(i))
            names.append(QString::fromStdString(bone->getName()));
    }
    return names;
}

// ── Undoable GUI entry points ───────────────────────────────────────
// Every one of these pushes a command onto the shared UndoManager so
// the acceptance criterion "all operations are undoable" holds for the
// panel. They mirror the non-undoable variants' preconditions and
// return false without touching the stack when those don't hold —
// otherwise Ctrl+Z would step over a no-op command.

bool PoseLibrary::savePoseUndoable(const QString& name)
{
    auto* entity = firstSelectedEntity();
    if (!entity || name.isEmpty() || !skeletonOf(entity)) return false;
    auto* undo = UndoManager::getSingleton();
    if (!undo) return savePoseForSelection(name);
    GamificationManager::noteFeature(QStringLiteral("pose_library"));
    undo->push(new SavePoseCommand(entity, name));
    return true;
}

bool PoseLibrary::applyPoseUndoable(const QString& name)
{
    auto* entity = firstSelectedEntity();
    if (!entity || !hasPose(entity, name)) return false;
    auto* undo = UndoManager::getSingleton();
    if (!undo) return applyPose(entity, name);
    undo->push(new ApplyPoseCommand(entity, name));
    return true;
}

bool PoseLibrary::applyPoseBlendedUndoable(const QString& name,
                                           double durationSeconds)
{
    auto* entity = firstSelectedEntity();
    if (!entity || !hasPose(entity, name)) return false;
    auto* undo = UndoManager::getSingleton();
    if (!undo) return applyPoseBlended(entity, name,
                                       static_cast<float>(durationSeconds));
    undo->push(new ApplyPoseBlendedCommand(entity, name,
                                           static_cast<float>(durationSeconds)));
    return true;
}

bool PoseLibrary::deletePoseUndoable(const QString& name)
{
    auto* entity = firstSelectedEntity();
    if (!entity || !hasPose(entity, name)) return false;
    auto* undo = UndoManager::getSingleton();
    if (!undo) return deletePose(entity, name);
    undo->push(new DeletePoseCommand(entity, name));
    return true;
}

bool PoseLibrary::mirrorPoseUndoable(const QString& srcName,
                                     const QString& dstName)
{
    auto* entity = firstSelectedEntity();
    if (!entity || dstName.isEmpty() || !hasPose(entity, srcName)) return false;
    auto* undo = UndoManager::getSingleton();
    if (!undo) return mirrorPose(entity, srcName, dstName);
    GamificationManager::noteFeature(QStringLiteral("pose_library"));
    undo->push(new MirrorPoseCommand(entity, srcName, dstName));
    return true;
}

bool PoseLibrary::blendPosesUndoable(const QString& aName,
                                     const QString& bName,
                                     double weight,
                                     const QString& dstName)
{
    auto* entity = firstSelectedEntity();
    if (!entity || dstName.isEmpty()) return false;
    if (!hasPose(entity, aName) || !hasPose(entity, bName)) return false;
    auto* undo = UndoManager::getSingleton();
    if (!undo) return blendPosesForSelection(aName, bName, weight, dstName);
    GamificationManager::noteFeature(QStringLiteral("pose_library"));
    undo->push(new BlendPosesCommand(entity, aName, bName,
                                     static_cast<float>(weight), dstName));
    return true;
}

bool PoseLibrary::applyPoseMaskedUndoable(const QString& name,
                                          const QStringList& boneNames)
{
    auto* entity = firstSelectedEntity();
    if (!entity || !hasPose(entity, name)) return false;
    auto* undo = UndoManager::getSingleton();
    if (!undo) return applyPoseMaskedForSelection(name, boneNames);
    undo->push(new ApplyPoseMaskedCommand(entity, name, boneNames));
    return true;
}

QString PoseLibrary::poseThumbnailForSelection(const QString& name)
{
    assertMainThread();
    auto* entity = firstSelectedEntity();
    if (!entity || name.isEmpty()) return {};

    const QString key = thumbKey(entity, name);
    auto cached = m_thumbCache.constFind(key);
    if (cached != m_thumbCache.constEnd()) return *cached;

    auto entIt = m_byEntity.constFind(entity);
    if (entIt == m_byEntity.constEnd()) return {};
    auto poseIt = entIt->byName.constFind(name);
    if (poseIt == entIt->byName.constEnd()) return {};
    if (!skeletonOf(entity)) return {};

    // Pose the entity to the snapshot, grab one frame, then put the
    // skeleton back exactly as we found it — asking for a thumbnail
    // must never disturb what the author is looking at. Copy the
    // target snapshot first: the render can re-enter Ogre and we do
    // not want an iterator into m_byEntity live across that.
    const PoseSnapshot target = *poseIt;
    const PoseSnapshot before = captureLive(entity);
    applySnapshot(entity, target);

    TurntableOptions opts;
    opts.width = kThumbnailSize;
    opts.height = kThumbnailSize;
    opts.frameCount = 1;
    // Shaded key+fill so an untextured rig reads as a shape rather
    // than a flat silhouette (#933).
    opts.studio = true;

    QList<QImage> frames;
    QString renderError;
    const bool ok = ModelTurntableRenderer::renderToImages(
        {entity}, opts, &frames, &renderError);

    // Restore BEFORE inspecting the result so an early return can't
    // leave the character stuck in the thumbnail pose.
    applySnapshot(entity, before);

    if (!ok || frames.isEmpty() || frames.first().isNull()) {
        // Headless / no-GL environments land here. Returning empty is
        // the documented contract; the panel just shows no image.
        SentryReporter::addBreadcrumb("scene.anim.pose",
            QStringLiteral("thumbnail '%1' unavailable: %2")
                .arg(name, renderError.isEmpty() ? QStringLiteral("no frame")
                                                 : renderError));
        return {};
    }

    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly)) return {};
    if (!frames.first().save(&buffer, "PNG")) return {};
    buffer.close();

    const QString uri = QStringLiteral("data:image/png;base64,")
                        + QString::fromLatin1(png.toBase64());
    m_thumbCache.insert(key, uri);
    SentryReporter::addBreadcrumb("scene.anim.pose",
        QStringLiteral("thumbnail rendered for '%1'").arg(name));
    return uri;
}
