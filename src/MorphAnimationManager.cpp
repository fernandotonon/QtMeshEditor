/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "MorphAnimationManager.h"

#include "EditModeController.h"
#include "EditableMesh.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/MorphCommands.h"

#include <QCoreApplication>
#include <QThread>

#include <OgreAnimationState.h>
#include <OgreEntity.h>
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

MorphAnimationManager::MorphAnimationManager(QObject* parent) : QObject(parent)
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
        slice.submeshHandle = static_cast<unsigned short>(s + 1);
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
