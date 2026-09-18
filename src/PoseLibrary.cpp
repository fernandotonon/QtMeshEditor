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

#include <OgreAnimationState.h>
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
    QList<unsigned short> posedHandles;
    posedHandles.reserve(snapshot.size());
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
        posedHandles.append(bone->getHandle());
        ++written;
    }
    if (written > 0) {
        // Hold the bones against reset() + running tracks, or the pose is
        // gone before the next frame renders.
        holdPosedBones(entity, posedHandles);
        flushSkeletonPose(entity, skel);
    }
    return written;
}

int PoseLibrary::holdPosedBones(Ogre::Entity* entity,
                                const QList<unsigned short>& boneHandles)
{
    // Making an applied pose STICK needs both halves of the recipe the
    // bone-drag gizmo uses (TransformOperator::mousePressEvent):
    //
    //   1. setManuallyControlled(true) — excludes the bone from
    //      Skeleton::reset(), which would otherwise snap it back to the
    //      BIND pose (that is the "everything goes T-pose" symptom).
    //   2. a per-state blend mask entry of 0 — manual control alone does
    //      NOT stop animation TRACKS applying to the bone each frame
    //      (Ogre's own docs warn about this), which is the "pose flashes
    //      then reverts" symptom.
    //   3. setSkipAnimationStateUpdate(true) — Entity::cacheBoneMatrices
    //      otherwise calls setAnimationState() (reset + re-apply) once per
    //      frame, so whether the pose survived depended on where the click
    //      landed in the frame (Apply appeared to work only randomly).
    //
    // Doing only (1) breaks reset semantics: reset(false) skips manual
    // bones, so a re-enabled clip lands on an unreset bone and the error
    // compounds on every toggle.
    //
    // Everything overwritten here is recorded in m_holds so release can put
    // back EXACTLY that — other systems (the gizmo, mocap) own blend masks
    // of their own and must not be trampled.
    if (!entity || boneHandles.isEmpty()) return 0;
    auto* skel = skeletonOf(entity);
    if (!skel) return 0;

    const bool firstHold = !m_holds.contains(entity);
    PoseHold& hold = m_holds[entity];
    if (firstHold)
        hold.priorSkipAnimStateUpdate = entity->getSkipAnimationStateUpdate();

    for (unsigned short handle : boneHandles) {
        if (Ogre::Bone* b = skel->getBone(handle)) {
            b->setManuallyControlled(true);
            if (!hold.handles.contains(handle)) hold.handles.append(handle);
        }
    }

    entity->setSkipAnimationStateUpdate(true);

    if (auto* states = entity->getAllAnimationStates()) {
        const auto numBones = static_cast<size_t>(skel->getNumBones());
        for (const auto& [name, st] : states->getAnimationStates()) {
            if (!st || !st->getEnabled()) continue;
            const QString clip = QString::fromStdString(name);
            if (!st->hasBlendMask()) {
                st->createBlendMask(numBones, 1.0f);
                hold.maskCreatedByUs.insert(clip);
            }
            auto& prior = hold.priorMaskWeights[clip];
            for (unsigned short handle : boneHandles) {
                // Record the pre-hold weight ONCE per handle, so a repeated
                // apply doesn't overwrite the original with our own 0.
                if (!prior.contains(handle))
                    prior.insert(handle, st->getBlendMaskEntry(handle));
                st->setBlendMaskEntry(handle, 0.0f);
            }
        }
    }
    return boneHandles.size();
}

bool PoseLibrary::hasHeldBones(Ogre::Entity* entity) const
{
    return entity && m_holds.contains(entity);
}

int PoseLibrary::releasePosedBones(Ogre::Entity* entity)
{
    // Hand the skeleton back to the animation system, restoring exactly what
    // the hold overwrote. Deliberately does NOT call Skeleton::reset(true):
    // release runs right after a caller has written the TRS it wants kept
    // (e.g. the thumbnail path restoring the pre-render pose), and a reset
    // would discard it and snap the rig to bind.
    if (!entity) return 0;
    auto it = m_holds.find(entity);
    if (it == m_holds.end()) return 0;
    PoseHold hold = *it;
    m_holds.erase(it);

    auto* skel = skeletonOf(entity);
    if (!skel) return 0;

    entity->setSkipAnimationStateUpdate(hold.priorSkipAnimStateUpdate);

    int released = 0;
    for (unsigned short handle : hold.handles) {
        if (Ogre::Bone* b = skel->getBone(handle)) {
            if (!b->isManuallyControlled()) continue;
            b->setManuallyControlled(false);
            ++released;
        }
    }

    if (auto* states = entity->getAllAnimationStates()) {
        for (const auto& [name, st] : states->getAnimationStates()) {
            if (!st) continue;
            const QString clip = QString::fromStdString(name);
            auto priorIt = hold.priorMaskWeights.constFind(clip);
            if (priorIt == hold.priorMaskWeights.constEnd()) continue;
            if (hold.maskCreatedByUs.contains(clip)) {
                // We created this mask purely to hold the pose — drop it so
                // the clip goes back to unmasked rather than carrying a
                // full-weight mask it never had.
                st->destroyBlendMask();
                continue;
            }
            if (!st->hasBlendMask()) continue;
            for (auto p = priorIt->cbegin(); p != priorIt->cend(); ++p)
                st->setBlendMaskEntry(p.key(), p.value());
        }
    }

    // The bones are no longer manual, so the next animation update owns
    // them again; make sure that update actually happens.
    skel->_notifyManualBonesDirty();
    entity->_updateAnimation();
    return released;
}

void PoseLibrary::flushSkeletonPose(Ogre::Entity* entity,
                                    Ogre::SkeletonInstance* skel)
{
    if (!skel) return;
    // Push the new locals into derived transforms so the skin, the
    // SkeletonDebug visuals and any TagPoint-attached entities all see
    // the pose in the frame it was applied — writing bone TRS alone
    // leaves everything downstream on the previous pose.
    for (Ogre::Bone* root : skel->getRootBones())
        if (root) root->_update(true, true);

    // ...but derived transforms are not what the GPU samples. The skin reads
    // Entity::mBoneMatrices, refreshed only by cacheBoneMatrices(), which is
    // gated on "frame number changed OR Skeleton::getManualBonesDirty()".
    // With setSkipAnimationStateUpdate(true) the per-frame re-apply is off, so
    // the manual-bones-dirty flag is the ONLY thing that re-uploads the pose.
    // Order matters: Skeleton::_updateTransforms() CLEARS that flag, so it has
    // to be set here, AFTER the _update() calls above, and consumed by
    // _updateAnimation() below. Without this the bones hold the right values
    // (the debug log proved they do) while the mesh keeps rendering the stale
    // pose — the "pose applied but nothing visibly happens" bug.
    skel->_notifyManualBonesDirty();
    if (entity) entity->_updateAnimation();
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
    QList<unsigned short> maskedHandles;
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
        maskedHandles.append(bone->getHandle());
        ++boneCount;
    }
    if (boneCount > 0) {
        holdPosedBones(entity, maskedHandles);
        flushSkeletonPose(entity, skel);
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

        QList<unsigned short> blendedHandles;
        blendedHandles.reserve(blend.to.size());
        for (auto b = blend.to.cbegin(); b != blend.to.cend(); ++b) {
            const std::string boneName = b.key().toStdString();
            if (!skel->hasBone(boneName)) continue;
            Ogre::Bone* bone = skel->getBone(boneName);
            if (!bone) continue;
            blendedHandles.append(bone->getHandle());
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

        // Same contract as a snap apply: hold the blended bones against
        // Skeleton::reset + running tracks, and re-upload the bone matrices,
        // or each eased frame is written but never rendered.
        if (!blendedHandles.isEmpty()) {
            holdPosedBones(entity, blendedHandles);
            flushSkeletonPose(entity, skel);
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
    // Same rationale for the pose hold: m_holds is keyed on the Entity*,
    // and a stale entry would make hasHeldBones() true for a dead pointer
    // (and could be "released" against freed memory later).
    m_holds.remove(entity);
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

QJsonArray PoseLibrary::posesToJson(Ogre::Entity* entity) const
{
    QJsonArray poses;
    auto entIt = m_byEntity.constFind(entity);
    if (entIt == m_byEntity.constEnd()) return poses;
    // Iterate `order`, not `byName`, so the file preserves the
    // author's save order rather than hash-bucket order.
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
    return poses;
}

PoseLibrary::EntityPoses PoseLibrary::posesFromJson(const QJsonArray& poses)
{
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
            trs.translate = readVec3(boneObj.value("t").toArray(), Ogre::Vector3::ZERO);
            trs.rotation = readQuat(boneObj.value("r").toArray(), Ogre::Quaternion::IDENTITY);
            trs.scale = readVec3(boneObj.value("s").toArray(), Ogre::Vector3(1, 1, 1));
            snapshot.insert(it.key(), trs);
        }
        // Only append `order` on first sighting so duplicate entries in
        // the file don't leave phantom names that survive a deletePose
        // (Codex P2 on PR #602). Later duplicates overwrite the snapshot.
        const bool isFirstSighting = !staging.byName.contains(name);
        staging.byName.insert(name, snapshot);
        if (isFirstSighting) staging.order.append(name);
    }
    return staging;
}

bool PoseLibrary::savePoseLibrary(Ogre::Entity* entity, const QString& filePath) const
{
    assertMainThread();
    if (!entity || filePath.isEmpty()) return false;
    auto entIt = m_byEntity.constFind(entity);
    if (entIt == m_byEntity.constEnd()) return false;
    if (entIt->order.isEmpty()) return false;

    const QJsonArray poses = posesToJson(entity);

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

void PoseLibrary::commitLoadedLibrary(Ogre::Entity* entity,
                                      const EntityPoses& staging)
{
    // All-or-nothing replacement: the caller has already parsed
    // successfully. Partial overlay would be confusing UX (which pose
    // wins on name collision?), so we replace.
    m_byEntity.insert(entity, staging);
    // Every pose on this entity just changed content, so every cached
    // render of it is stale.
    const QString thumbPrefix = QStringLiteral("%1/")
        .arg(reinterpret_cast<quintptr>(entity));
    for (auto it = m_thumbCache.begin(); it != m_thumbCache.end(); ) {
        if (it.key().startsWith(thumbPrefix)) it = m_thumbCache.erase(it);
        else ++it;
    }
    // A blend targeting the old library's pose is meaningless now.
    m_blends.remove(entity);
    emit posesChanged(entity);
}

// Read + parse a sidecar down to its root object. Returns false (and
// leaves `root` untouched) on any read / parse / schema failure, so
// callers can bail BEFORE mutating in-memory state.
bool PoseLibrary::readSidecarRoot(const QString& filePath, QJsonObject& root)
{
    if (filePath.isEmpty()) return false;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    file.close();
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) return false;
    if (!doc.isObject()) return false;
    const QJsonObject obj = doc.object();
    if (obj.value("schema").toString() != QString::fromLatin1(kPoseLibSchemaV1))
        return false;
    root = obj;
    return true;
}

bool PoseLibrary::loadPoseLibrary(Ogre::Entity* entity, const QString& filePath)
{
    assertMainThread();
    if (!entity) return false;
    QJsonObject root;
    if (!readSidecarRoot(filePath, root)) return false;
    // Codex P1 on PR #602: validate the payload shape BEFORE wiping the
    // in-memory library. A schema-matching file with `poses` missing or
    // non-array would otherwise silently drop the user's existing data
    // and return success.
    const QJsonValue posesV = root.value("poses");
    if (!posesV.isArray()) return false;

    const EntityPoses staging = posesFromJson(posesV.toArray());
    commitLoadedLibrary(entity, staging);

    SentryReporter::addBreadcrumb("file.import",
        QStringLiteral("load library from '%1' (%2 poses)")
            .arg(filePath).arg(staging.order.size()));
    return true;
}

bool PoseLibrary::saveSceneLibraries(const QHash<QString, Ogre::Entity*>& nodesToEntities,
                                     const QString& filePath) const
{
    assertMainThread();
    if (filePath.isEmpty()) return false;

    // Sort the node names so the file is byte-stable across runs —
    // QHash iteration order is randomised per process, and an
    // export that reshuffles its own sidecar every time is hostile
    // to version control.
    QStringList nodeNames = nodesToEntities.keys();
    nodeNames.sort();

    QJsonArray entitiesJson;
    int totalPoses = 0;
    for (const QString& nodeName : nodeNames) {
        Ogre::Entity* entity = nodesToEntities.value(nodeName, nullptr);
        if (!entity) continue;
        const QJsonArray poses = posesToJson(entity);
        if (poses.isEmpty()) continue;   // nothing saved on this entity
        QJsonObject entry;
        entry["node"] = nodeName;
        entry["poses"] = poses;
        entitiesJson.append(entry);
        totalPoses += poses.size();
    }
    // Nothing to persist — report failure so the caller can remove a
    // stale sidecar rather than writing an empty one.
    if (entitiesJson.isEmpty()) return false;

    QJsonObject root;
    root["schema"] = kPoseLibSchemaV1;
    root["entities"] = entitiesJson;

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) return false;

    SentryReporter::addBreadcrumb("file.export",
        QStringLiteral("save scene libraries to '%1' (%2 entities, %3 poses)")
            .arg(filePath).arg(entitiesJson.size()).arg(totalPoses));
    return true;
}

int PoseLibrary::loadSceneLibraries(const QHash<QString, Ogre::Entity*>& nodesToEntities,
                                    const QString& filePath)
{
    assertMainThread();
    QJsonObject root;
    if (!readSidecarRoot(filePath, root)) return -1;
    const QJsonValue entitiesV = root.value("entities");
    if (!entitiesV.isArray()) return -1;
    const QJsonArray entityEntries = entitiesV.toArray();

    // Parse EVERYTHING first, then commit. A malformed entry halfway
    // through must not leave some entities restored and others not.
    QList<QPair<Ogre::Entity*, EntityPoses>> pending;
    for (const QJsonValue& ev : entityEntries) {
        if (!ev.isObject()) continue;
        const QJsonObject entry = ev.toObject();
        const QString nodeName = entry.value("node").toString();
        if (nodeName.isEmpty()) continue;
        Ogre::Entity* entity = nodesToEntities.value(nodeName, nullptr);
        // Node in the file but not in this scene — the scene changed
        // since export. Skip rather than fail the whole load.
        if (!entity) continue;
        const QJsonValue posesV = entry.value("poses");
        if (!posesV.isArray()) continue;
        pending.append({entity, posesFromJson(posesV.toArray())});
    }

    for (const auto& [entity, staging] : pending)
        commitLoadedLibrary(entity, staging);

    SentryReporter::addBreadcrumb("file.import",
        QStringLiteral("load scene libraries from '%1' (%2 of %3 entities matched)")
            .arg(filePath).arg(pending.size()).arg(entityEntries.size()));
    return static_cast<int>(pending.size());
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
    // A thumbnail must leave the skeleton EXACTLY as it found it — including
    // whether an applied pose was being held. Rendering one is triggered by
    // QML (row creation / thumbGeneration bump / cache miss), so it lands at
    // an unpredictable moment relative to the user's click: blanket-releasing
    // here is what made Apply work only after a random number of clicks.
    const bool wasHeld = hasHeldBones(entity);
    // applySnapshot disables enabled clips (they would overwrite the pose).
    // For a THUMBNAIL that must not stick: remember what was enabled and put
    // it back below, alongside the bone restore.
    QStringList reEnable;
    if (auto* states = entity->getAllAnimationStates())
        for (const auto& [nm, st] : states->getAnimationStates())
            if (st && st->getEnabled())
                reEnable << QString::fromStdString(nm);
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
    // Order matters: drop the hold the thumbnail's own applySnapshot took
    // BEFORE writing the pose we want to keep. Releasing afterwards would
    // hand the skeleton back to the animation system and discard the very
    // TRS we just restored (a non-animated hand-posed rig would snap to
    // bind every time a thumbnail rendered). Only release a hold this
    // function created — never cancel one the user's Apply established.
    if (!wasHeld)
        releasePosedBones(entity);
    applySnapshot(entity, before);
    if (!wasHeld)
        releasePosedBones(entity);
    if (auto* states = entity->getAllAnimationStates()) {
        for (const QString& nm : reEnable) {
            const std::string key = nm.toStdString();
            if (states->hasAnimationState(key))
                states->getAnimationState(key)->setEnabled(true);
        }
    }

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
