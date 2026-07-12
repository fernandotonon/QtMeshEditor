/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "MorphAnimationManager.h"

#include "AnimationControlController.h"
#include "PropertiesPanelController.h"
#include "GamificationManager.h"
#include "EditModeController.h"
#include "EditableMesh.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/MorphCommands.h"

#include <QCoreApplication>
#include <QThread>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreLogManager.h>
#include <OgreMesh.h>
#include <OgrePose.h>

#include <algorithm>

namespace {

// Per the project's singleton-on-main-thread convention (CLAUDE.md:
// "All run on the main thread."), assert any cross-thread access at
// the lifecycle entry points so a regression surfaces loudly in
// debug builds.
inline void assertMainThread()
{
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
}

} // namespace

MorphAnimationManager* MorphAnimationManager::s_instance = nullptr;

MorphAnimationManager* MorphAnimationManager::instance()
{
    assertMainThread();
    if (!s_instance) s_instance = new MorphAnimationManager();
    return s_instance;
}

MorphAnimationManager* MorphAnimationManager::qmlInstance(QQmlEngine*, QJSEngine*)
{
    assertMainThread();
    return instance();
}

void MorphAnimationManager::kill()
{
    assertMainThread();
    if (!s_instance) return;
    delete s_instance;
    s_instance = nullptr;
}

MorphAnimationManager::MorphAnimationManager(QObject* parent)
    : QObject(parent),
      m_activeMorphClip(QString::fromUtf8(kWeightClipName))
{
    if (auto* sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged,
                this, &MorphAnimationManager::morphTargetsChanged);
    }
}

MorphAnimationManager::~MorphAnimationManager() = default;

QStringList MorphAnimationManager::morphTargetsFor(Ogre::Entity* entity) const
{
    QStringList out;
    if (!entity) return out;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return out;
    const auto& poseList = mesh->getPoseList();
    for (const Ogre::Pose* p : poseList) {
        if (!p) continue;
        const Ogre::String n = p->getName();
        if (!n.empty()) out << QString::fromStdString(n);
    }
    return out;
}

float MorphAnimationManager::weight(Ogre::Entity* entity, const QString& name) const
{
    if (!entity || name.isEmpty()) return 0.0f;
    auto* states = entity->getAllAnimationStates();
    if (!states) return 0.0f;
    const std::string sn = name.toStdString();
    if (!states->hasAnimationState(sn)) return 0.0f;
    auto* state = states->getAnimationState(sn);
    if (!state) return 0.0f;
    // Ogre's AnimationState defaults to weight=1.0 but is disabled
    // until first use. From a user-facing perspective, "this morph
    // target hasn't been touched yet" should read as 0.0, not 1.0 —
    // the slider in the Inspector starts at 0, not at full influence.
    // We only surface the live weight once the state is enabled (which
    // setWeight() flips), so the contract is: disabled ⇒ 0; enabled ⇒
    // whatever we wrote.
    return state->getEnabled() ? state->getWeight() : 0.0f;
}

bool MorphAnimationManager::setWeight(Ogre::Entity* entity, const QString& name, float w)
{
    if (!entity || name.isEmpty()) return false;
    auto* states = entity->getAllAnimationStates();
    if (!states) return false;
    const std::string sn = name.toStdString();
    if (!states->hasAnimationState(sn)) return false;
    auto* state = states->getAnimationState(sn);
    if (!state) return false;

    const float clamped = std::clamp(w, 0.0f, 1.0f);
    const float current = state->getWeight();
    const bool wasEnabled = state->getEnabled();
    if (std::abs(clamped - current) < 1e-6f && wasEnabled) return true;

    state->setEnabled(true);
    state->setWeight(clamped);
    // The pose track has its only keyframe at t=0; pin the state
    // there so the weight actually drives the pose.
    state->setTimePosition(0.0f);

    SentryReporter::addBreadcrumb("scene.anim.morph",
        QStringLiteral("set '%1' weight = %2").arg(name).arg(clamped, 0, 'f', 3));

    emit morphWeightChanged(entity, name, static_cast<double>(clamped));
    return true;
}

QStringList MorphAnimationManager::morphTargetsForSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return {};
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return {};
    return morphTargetsFor(ents.first());
}

double MorphAnimationManager::weightForSelection(const QString& name) const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return 0.0;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return 0.0;
    return static_cast<double>(weight(ents.first(), name));
}

bool MorphAnimationManager::setWeightForSelection(const QString& name, double w)
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return false;
    return setWeight(ents.first(), name, static_cast<float>(w));
}

const char* MorphAnimationManager::kWeightClipName = "MorphAnim";

namespace {

// Find the pose index (into mesh->getPoseList()) for the first pose named
// `name`. -1 if none. Weight keyframing references the pose by this index.
int poseIndexForName(Ogre::Mesh* mesh, const std::string& name)
{
    const auto& poses = mesh->getPoseList();
    for (unsigned short i = 0; i < poses.size(); ++i)
        if (poses[i] && poses[i]->getName() == name) return i;
    return -1;
}

// The weight-animation track for a target lives on the named `clip`, keyed on
// the pose's target submesh handle. Fetch-or-create the clip + track. Returns
// nullptr if the pose doesn't exist.
Ogre::VertexAnimationTrack* weightTrackFor(Ogre::Mesh* mesh, const std::string& name,
                                           const std::string& clip, bool create)
{
    const int pi = poseIndexForName(mesh, name);
    if (pi < 0) return nullptr;
    const unsigned short handle = mesh->getPoseList()[pi]->getTarget();

    Ogre::Animation* anim = mesh->hasAnimation(clip)
        ? mesh->getAnimation(clip)
        : (create ? mesh->createAnimation(clip, 0.0f) : nullptr);
    if (!anim) return nullptr;

    if (anim->hasVertexTrack(handle))
        return anim->getVertexTrack(handle);
    if (!create) return nullptr;
    return anim->createVertexTrack(handle, Ogre::VAT_POSE);
}

// Is `animName` a per-target SHAPE clip (named exactly a pose name)? Those are
// not user-facing "morph clips"; the weight clips are everything else that
// carries a VAT_POSE track and isn't a pose name.
bool isPoseShapeClip(Ogre::Mesh* mesh, const std::string& animName)
{
    for (const Ogre::Pose* p : mesh->getPoseList())
        if (p && p->getName() == animName) return true;
    return false;
}

} // namespace

bool MorphAnimationManager::setMorphWeightKeyframe(const QString& name,
                                                   double time, double weight)
{
    assertMainThread();
    if (name.isEmpty() || time < 0.0) return false;

    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return false;

    const std::string clip = m_activeMorphClip.toStdString();
    const int pi = poseIndexForName(mesh.get(), name.toStdString());
    if (pi < 0) return false;
    Ogre::VertexAnimationTrack* track =
        weightTrackFor(mesh.get(), name.toStdString(), clip, true);
    if (!track) return false;

    const float t = static_cast<float>(time);
    const float w = std::clamp(static_cast<float>(weight), 0.0f, 1.0f);

    // Update in place if a keyframe already exists at ~t, else create one.
    Ogre::VertexPoseKeyFrame* kf = nullptr;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* k = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(i));
        if (std::abs(k->getTime() - t) < 1e-4f) { kf = k; break; }
    }
    if (!kf) kf = track->createVertexPoseKeyFrame(t);

    // A VAT_POSE keyframe holds a list of pose references; for a weight track
    // each keyframe references exactly this target's pose at influence = weight.
    kf->removeAllPoseReferences();
    kf->addPoseReference(static_cast<unsigned short>(pi), w);

    // Extend the clip length to cover the new time.
    Ogre::Animation* anim = mesh->getAnimation(clip);
    if (anim && t > anim->getLength())
        anim->setLength(t);

    // Refresh the entity's animation-state mirror so the new clip is playable.
    entity->refreshAvailableAnimationState();
    emit morphTargetsChanged();
    // The clip only becomes a playable AnimationState once it has a track
    // (created on the first key here) — signal the clip list so the Animation
    // Mode list picks it up now, not just on create/delete/rename.
    emit morphClipsChanged();
    SentryReporter::addBreadcrumb("scene.anim.morph",
        QStringLiteral("key weight '%1' @%2 = %3").arg(name).arg(t).arg(w));
    return true;
}

bool MorphAnimationManager::clearMorphWeightKeyframe(const QString& name, double time)
{
    assertMainThread();
    if (name.isEmpty()) return false;

    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::MeshPtr mesh = ents.first()->getMesh();
    if (!mesh) return false;

    Ogre::VertexAnimationTrack* track =
        weightTrackFor(mesh.get(), name.toStdString(), m_activeMorphClip.toStdString(), false);
    if (!track) return false;
    const float t = static_cast<float>(time);
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::abs(track->getKeyFrame(i)->getTime() - t) < 1e-4f) {
            track->removeKeyFrame(i);
            emit morphTargetsChanged();
            return true;
        }
    }
    return false;
}

double MorphAnimationManager::morphWeightAt(const QString& name, double time) const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return -1.0;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return -1.0;
    Ogre::MeshPtr mesh = ents.first()->getMesh();
    if (!mesh) return -1.0;

    const int pi = poseIndexForName(mesh.get(), name.toStdString());
    if (pi < 0) return -1.0;
    Ogre::VertexAnimationTrack* track =
        weightTrackFor(mesh.get(), name.toStdString(), m_activeMorphClip.toStdString(), false);
    if (!track) return -1.0;
    const float t = static_cast<float>(time);
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(i));
        if (std::abs(kf->getTime() - t) >= 1e-4f) continue;
        for (const auto& ref : kf->getPoseReferences())
            if (ref.poseIndex == static_cast<unsigned short>(pi))
                return static_cast<double>(ref.influence);
    }
    return -1.0;
}

bool MorphAnimationManager::moveMorphWeightKeyframe(const QString& name,
                                                    double oldTime, double newTime)
{
    assertMainThread();
    if (name.isEmpty()) return false;
    if (std::abs(oldTime - newTime) < 1e-4) return false;
    if (newTime < 0.0) return false;

    // Preserve the weight from the source key; reject if there's already a key
    // at the destination (avoid silently merging two keys).
    const double w = morphWeightAt(name, oldTime);
    if (w < 0.0) return false;                 // no source key
    if (morphWeightAt(name, newTime) >= 0.0) return false;  // destination occupied

    if (!clearMorphWeightKeyframe(name, oldTime)) return false;
    return setMorphWeightKeyframe(name, newTime, w);
}

bool MorphAnimationManager::activateWeightClip()
{
    assertMainThread();
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();
    Ogre::MeshPtr mesh = entity->getMesh();
    const std::string clip = m_activeMorphClip.toStdString();
    if (!mesh || !mesh->hasAnimation(clip)) return false;

    // Make sure the entity mirrors the clip as a playable AnimationState and
    // enable it (weight tracks only apply while the state is enabled). Disable
    // any OTHER morph clip so two emotion clips don't fight over the same poses.
    entity->refreshAvailableAnimationState();
    if (auto* states = entity->getAllAnimationStates()) {
        for (const auto& [nm, st] : states->getAnimationStates())
            if (!isPoseShapeClip(mesh.get(), nm) && nm != clip
                && mesh->hasAnimation(nm))
                st->setEnabled(false);
    }
    if (entity->hasAnimationState(clip)) {
        Ogre::AnimationState* st = entity->getAnimationState(clip);
        st->setEnabled(true);
        st->setLoop(true);
    }

    // Select it in the Animation Control panel so the timeline slider scrubs it
    // (its length + slider maximum come from the mesh Animation now).
    if (auto* acc = AnimationControlController::instance())
        acc->selectAnimation(QString::fromStdString(entity->getName()),
                             m_activeMorphClip);
    return true;
}

QVariantList MorphAnimationManager::morphWeightKeyframeTimes(const QString& name) const
{
    QVariantList out;
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return out;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return out;
    Ogre::MeshPtr mesh = ents.first()->getMesh();
    if (!mesh) return out;

    const int pi = poseIndexForName(mesh.get(), name.toStdString());
    if (pi < 0) return out;
    Ogre::VertexAnimationTrack* track =
        weightTrackFor(mesh.get(), name.toStdString(), m_activeMorphClip.toStdString(), false);
    if (!track) return out;
    // Targets on the same submesh SHARE one VAT_POSE track, so a keyframe on the
    // track may belong to a DIFFERENT target. Only report times of keyframes
    // that actually reference THIS pose — otherwise "JawOpen" would report the
    // "Smile" keys on the shared track (matches the dope-sheet's per-pose rows).
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(i));
        bool refsThisPose = false;
        for (const auto& ref : kf->getPoseReferences())
            if (ref.poseIndex == static_cast<unsigned short>(pi)) { refsThisPose = true; break; }
        if (refsThisPose)
            out.append(static_cast<double>(kf->getTime()));
    }
    return out;
}

// ── Morph clip management (multiple named weight clips) ──────────────────────

void MorphAnimationManager::setActiveMorphClip(const QString& name)
{
    if (name.isEmpty() || name == m_activeMorphClip) return;
    m_activeMorphClip = name;
    emit morphClipsChanged();
    emit morphTargetsChanged();  // dope sheet reads the active clip's keys
}

QStringList MorphAnimationManager::morphClips() const
{
    QStringList out;
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return out;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return out;
    Ogre::MeshPtr mesh = ents.first()->getMesh();
    if (!mesh) return out;

    // A morph (weight) clip = any MESH-level Animation that is NOT a per-target
    // shape clip (those are named exactly a pose name). Skeletal clips live on
    // the skeleton, not the mesh, so they never appear here. We deliberately do
    // NOT require a VAT_POSE track: a freshly-created clip has no tracks until
    // its first keyframe, and it must still show in the dropdown.
    for (unsigned short i = 0; i < mesh->getNumAnimations(); ++i) {
        Ogre::Animation* a = mesh->getAnimation(i);
        if (!a) continue;
        const std::string nm = a->getName();
        if (isPoseShapeClip(mesh.get(), nm)) continue;
        out << QString::fromStdString(nm);
    }

    // Auto-adopt an existing clip as active when the current active clip isn't
    // on this mesh (e.g. right after IMPORTING a model whose clip is named
    // "Sniff" while the app default is "MorphAnim"). Without this the dope sheet
    // + keying would target a non-existent clip and show nothing until the user
    // manually picked the imported clip from the dropdown. const_cast is safe:
    // this only mutates the transient active-clip selector, not scene data.
    if (!out.isEmpty() && !out.contains(m_activeMorphClip)) {
        auto* self = const_cast<MorphAnimationManager*>(this);
        self->m_activeMorphClip = out.first();
        // Defer the signal so we don't emit inside a const getter the QML may be
        // mid-binding on; a queued emit refreshes the dropdown + dope sheet.
        QMetaObject::invokeMethod(self, [self]() {
            emit self->morphClipsChanged();
            emit self->morphTargetsChanged();
        }, Qt::QueuedConnection);
    }
    return out;
}

bool MorphAnimationManager::createMorphClip(const QString& name)
{
    assertMainThread();
    if (name.trimmed().isEmpty()) return false;
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return false;
    // A morph clip animates existing morph TARGETS. With no poses there is
    // nothing to key, and creating an empty VAT-less animation on a plain mesh
    // (e.g. an unmorphed OBJ) then refreshing/rendering its AnimationState is
    // what crashed. Require at least one target first — the user sculpts +Add
    // before making clips. (m_activeMorphClip still tracks the intended name.)
    if (mesh->getPoseCount() == 0)
        return false;
    const std::string nm = name.trimmed().toStdString();
    if (mesh->hasAnimation(nm)) return false;   // name collision

    mesh->createAnimation(nm, 0.0f);            // empty; tracks added on first key
    entity->refreshAvailableAnimationState();
    m_activeMorphClip = name.trimmed();
    emit morphClipsChanged();
    emit morphTargetsChanged();
    SentryReporter::addBreadcrumb("scene.anim.morph",
        QStringLiteral("create morph clip '%1'").arg(name.trimmed()));
    return true;
}

bool MorphAnimationManager::deleteMorphClip(const QString& name)
{
    assertMainThread();
    if (name.isEmpty()) return false;
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh || !mesh->hasAnimation(name.toStdString())) return false;
    if (isPoseShapeClip(mesh.get(), name.toStdString())) return false;  // that's a target

    // Stop playback and drop the state before removing the animation.
    if (auto* ppc = PropertiesPanelController::instance()) ppc->setPlaying(false);
    if (auto* states = entity->getAllAnimationStates()) {
        if (states->hasAnimationState(name.toStdString()))
            states->getAnimationState(name.toStdString())->setEnabled(false);
    }
    mesh->removeAnimation(name.toStdString());
    if (auto* states = entity->getAllAnimationStates()) {
        if (states->hasAnimationState(name.toStdString()))
            states->removeAnimationState(name.toStdString());
    }
    entity->refreshAvailableAnimationState();

    // If the active clip was deleted, fall back to another (or the default).
    if (m_activeMorphClip == name) {
        const QStringList remaining = morphClips();
        m_activeMorphClip = remaining.isEmpty()
            ? QString::fromUtf8(kWeightClipName) : remaining.first();
    }
    emit morphClipsChanged();
    emit morphTargetsChanged();
    return true;
}

bool MorphAnimationManager::renameMorphClip(const QString& oldName, const QString& newName)
{
    assertMainThread();
    if (oldName.isEmpty() || newName.trimmed().isEmpty() || oldName == newName)
        return false;
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();
    Ogre::MeshPtr mesh = entity->getMesh();
    const std::string on = oldName.toStdString();
    const std::string nn = newName.trimmed().toStdString();
    if (!mesh || !mesh->hasAnimation(on) || mesh->hasAnimation(nn)) return false;
    if (isPoseShapeClip(mesh.get(), on)) return false;  // that's a target, not a clip

    // Rebuild the Animation under the new name copying every VAT_POSE track +
    // keyframe + pose reference, then drop the old one. (Ogre has no rename.)
    Ogre::Animation* src = mesh->getAnimation(on);
    Ogre::Animation* dst = mesh->createAnimation(nn, src->getLength());
    for (const auto& [handle, srcTrack] : src->_getVertexTrackList()) {
        if (!srcTrack || srcTrack->getAnimationType() != Ogre::VAT_POSE) continue;
        Ogre::VertexAnimationTrack* dstTrack =
            dst->createVertexTrack(handle, Ogre::VAT_POSE);
        for (unsigned short k = 0; k < srcTrack->getNumKeyFrames(); ++k) {
            auto* skf = static_cast<Ogre::VertexPoseKeyFrame*>(srcTrack->getKeyFrame(k));
            auto* dkf = dstTrack->createVertexPoseKeyFrame(skf->getTime());
            for (const auto& ref : skf->getPoseReferences())
                dkf->addPoseReference(ref.poseIndex, ref.influence);
        }
    }
    if (auto* states = entity->getAllAnimationStates()) {
        if (states->hasAnimationState(on))
            states->removeAnimationState(on);
    }
    mesh->removeAnimation(on);
    entity->refreshAvailableAnimationState();
    if (m_activeMorphClip == oldName) m_activeMorphClip = newName.trimmed();
    emit morphClipsChanged();
    emit morphTargetsChanged();
    return true;
}

namespace {

// Walk the per-submesh edited positions on the EditableMesh and diff
// against the bind-position snapshot taken at `loadFromOgreMesh` time
// (`EditableSubMesh::originalPositions`). Returns the sparse delta
// map per submesh. Submeshes that didn't change at all get no slice
// entry, mirroring the importer's "lazy createPose" rule.
//
// We diff against the captured snapshot rather than re-reading the
// live mesh vertex buffer because edit-mode ops continuously
// `commitToEntity` — by the time the user clicks "save morph", the
// GPU buffer already holds the edited positions, so a re-read would
// produce a zero diff for every vertex. The snapshot was captured
// once at edit-mode entry, before any mutation, so it remains a
// valid baseline regardless of how many edit ops ran.
//
// Submeshes with `useSharedVertices=true` share a single vertex
// pool. EditableMesh::loadFromOgreMesh handles that by copying the
// shared positions into each affected submesh's `vertices` and
// `originalPositions`, so this code path doesn't need to special-
// case it — every submesh carries its own baseline.
std::vector<MorphPoseSlice> capturePoseSlicesFromEdit(
    Ogre::Entity* entity, const EditableMesh* edit)
{
    std::vector<MorphPoseSlice> slices;
    if (!entity || !edit) return slices;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return slices;

    const auto& subs = edit->subMeshes();
    const size_t meshSubCount = mesh->getNumSubMeshes();
    const size_t n = std::min(subs.size(), meshSubCount);

    for (size_t s = 0; s < n; ++s) {
        const auto& bindPositions = subs[s].originalPositions;
        // Mismatch typically means the submesh was modified
        // topologically (insert/delete vertex) — we can't meaningfully
        // diff against a different-shaped baseline, so skip it.
        if (bindPositions.size() != subs[s].vertices.size()) continue;

        MorphPoseSlice slice;
        // Shared geometry poses target handle 0; per-submesh use 1-based.
        slice.submeshHandle = subs[s].usesSharedVertices
            ? 0 : static_cast<unsigned short>(s + 1);
        for (size_t vi = 0; vi < bindPositions.size(); ++vi) {
            const Ogre::Vector3 delta =
                subs[s].vertices[vi].position - bindPositions[vi];
            if (delta.squaredLength() <= 1e-12f) continue;
            slice.offsets[static_cast<unsigned int>(vi)] =
                Ogre::Vector3f(delta.x, delta.y, delta.z);
        }
        if (!slice.offsets.empty()) slices.push_back(std::move(slice));
    }
    return slices;
}

// Reject names that would collide with an existing pose on the mesh.
// Same-named poses across submeshes are allowed by Ogre, but for
// authoring we treat "name already in use" as a no-op so the UI
// can show "rename existing" instead of silently shadowing.
bool nameAlreadyInUse(Ogre::Mesh* mesh, const QString& name)
{
    if (!mesh) return false;
    const std::string sn = name.toStdString();
    const auto& poseList = mesh->getPoseList();
    for (const Ogre::Pose* p : poseList) {
        if (p && p->getName() == sn) return true;
    }
    return false;
}

} // namespace

bool MorphAnimationManager::addMorphTargetFromCurrentEdit(const QString& name)
{
    assertMainThread();
    if (name.trimmed().isEmpty()) return false;

    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return false;

    if (nameAlreadyInUse(mesh.get(), name)) return false;

    auto* edit = EditModeController::instance();
    if (!edit) return false;
    EditableMesh* editable = edit->currentMesh();
    if (!editable) return false;

    auto slices = capturePoseSlicesFromEdit(entity, editable);
    if (slices.empty()) return false;

    auto* undo = UndoManager::getSingleton();
    if (!undo) return false;
    undo->push(new AddMorphTargetCommand(entity, name, slices));

    GamificationManager::noteOperation(
        QStringLiteral("morph"),
        {{QStringLiteral("targets_count"), morphTargetsForSelection().size()}});

    emit morphTargetsChanged();
    return true;
}

bool MorphAnimationManager::renameMorphTarget(const QString& oldName,
                                              const QString& newName)
{
    assertMainThread();
    const QString trimmedNew = newName.trimmed();
    if (oldName.isEmpty() || trimmedNew.isEmpty() || oldName == trimmedNew)
        return false;

    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return false;
    if (!nameAlreadyInUse(mesh.get(), oldName)) return false;
    if (nameAlreadyInUse(mesh.get(), trimmedNew)) return false;

    auto* undo = UndoManager::getSingleton();
    if (!undo) return false;
    undo->push(new RenameMorphTargetCommand(entity, oldName, trimmedNew));

    emit morphTargetsChanged();
    return true;
}

bool MorphAnimationManager::deleteMorphTarget(const QString& name)
{
    assertMainThread();
    if (name.isEmpty()) return false;

    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return false;
    if (!nameAlreadyInUse(mesh.get(), name)) return false;

    auto* undo = UndoManager::getSingleton();
    if (!undo) return false;
    undo->push(new DeleteMorphTargetCommand(entity, name));

    emit morphTargetsChanged();
    return true;
}

bool MorphAnimationManager::moveMorphTarget(const QString& name, int delta)
{
    assertMainThread();
    if (name.isEmpty() || delta == 0) return false;

    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();

    const QStringList order = morphTargetsFor(entity);
    const int from = order.indexOf(name);
    if (from < 0) return false;
    int to = from + delta;
    if (to < 0) to = 0;
    if (to > order.size() - 1) to = order.size() - 1;
    if (to == from) return false;   // already at the edge → no-op

    return moveMorphTargetToIndex(name, to);
}

bool MorphAnimationManager::moveMorphTargetToIndex(const QString& name, int toIndex)
{
    assertMainThread();
    if (name.isEmpty()) return false;

    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty() || !ents.first()) return false;
    Ogre::Entity* entity = ents.first();

    const QStringList oldOrder = morphTargetsFor(entity);
    const int from = oldOrder.indexOf(name);
    if (from < 0) return false;
    int to = toIndex;
    if (to < 0) to = 0;
    if (to > oldOrder.size() - 1) to = oldOrder.size() - 1;
    if (to == from) return false;

    QStringList newOrder = oldOrder;
    newOrder.move(from, to);
    if (newOrder == oldOrder) return false;

    auto* undo = UndoManager::getSingleton();
    if (!undo) return false;
    undo->push(new ReorderMorphTargetsCommand(entity, oldOrder, newOrder));

    emit morphTargetsChanged();
    return true;
}
