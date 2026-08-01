#include "AnimationControlController.h"
#include "GamificationManager.h"
#include "PropertiesPanelController.h"
#include "MorphAnimationManager.h"
#include "NodeAnimationManager.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/MoveKeyframeCommand.h"
#include "commands/BulkKeyframeCommands.h"
#include "commands/SetKeyframeValueCommand.h"
#include "commands/ResampleCurveCommand.h"
#include "commands/CurveEditModelChangeCommand.h"
#include "commands/DecimateTrackCommand.h"
#include "CurveEditModel.h"
#include "AnimationMerger.h"
#include "MotionInbetween.h"
#include "MotionLibrary.h"
#include "MotionGenerator.h"
#include "commands/AddKeyframeCommand.h"
#include "commands/DeleteKeyframeCommand.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QSet>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <Ogre.h>

AnimationControlController* AnimationControlController::m_pSingleton = nullptr;

AnimationControlController* AnimationControlController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new AnimationControlController();
    return m_pSingleton;
}

AnimationControlController* AnimationControlController::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine); Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AnimationControlController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

AnimationControlController::AnimationControlController()
    : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &AnimationControlController::updateAnimationTree);

    // Adding a morph target calls Entity::_initialise(true) (via
    // EditableMesh::commitToEntity), which DESTROYS and recreates the entity's
    // SkeletonInstance — leaving our cached m_selectedSkeleton dangling and
    // crashing the next bone-list / gizmo access. Rebind it (and refresh the
    // bone list) whenever the morph set changes so the cache stays valid.
    connect(MorphAnimationManager::instance(), &MorphAnimationManager::morphTargetsChanged,
            this, [this]() {
                if (!m_selectedEntity) return;
                rebindSelectedSkeleton();
                refreshBoneList(QString::fromStdString(m_selectedBone));
            });

    connect(qApp, &QApplication::paletteChanged, this, [this]() {
        emit themeChanged();
    });

    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        if (!m_selectedEntity || m_selectedAnimation.empty()) return;
        if (!m_selectedEntity->hasAnimationState(m_selectedAnimation)) return;

        Ogre::AnimationState* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
        int newMs = static_cast<int>(state->getTimePosition() * 1000);
        if (newMs != m_sliderValue) {
            m_sliderValue = newMs;
            emit sliderValueChanged();
            setAnimationFrame(newMs);
        }
    });
    m_pollTimer->start(16);
}

void AnimationControlController::suspendPollTimer()
{
    if (m_pollTimer && m_pollTimer->isActive()) {
        SentryReporter::addBreadcrumb(
            "ui.action", "Animation poll timer suspended (export safety)");
        m_pollTimer->stop();
        m_pollSuspended = true;
    }
}

void AnimationControlController::resumePollTimer()
{
    if (m_pollSuspended && m_pollTimer) {
        SentryReporter::addBreadcrumb(
            "ui.action", "Animation poll timer resumed");
        m_pollTimer->start(16);
        m_pollSuspended = false;
    }
}

// ── Theme colors ──────────────────────────────────────────────────────────────

QColor AnimationControlController::panelColor() const
    { return QApplication::palette().color(QPalette::Window); }

QColor AnimationControlController::headerColor() const
    { return QApplication::palette().color(QPalette::Window).darker(110); }

QColor AnimationControlController::textColor() const
    { return QApplication::palette().color(QPalette::WindowText); }

QColor AnimationControlController::borderColor() const
    { return QApplication::palette().color(QPalette::Mid); }

QColor AnimationControlController::inputColor() const
    { return QApplication::palette().color(QPalette::Base); }

QColor AnimationControlController::highlightColor() const
    { return QApplication::palette().color(QPalette::Highlight); }

QColor AnimationControlController::buttonColor() const
    { return QApplication::palette().color(QPalette::Button); }

QColor AnimationControlController::buttonTextColor() const
    { return QApplication::palette().color(QPalette::ButtonText); }

QColor AnimationControlController::disabledTextColor() const
    { return QApplication::palette().color(QPalette::Disabled, QPalette::WindowText); }

// ── Animation tree ────────────────────────────────────────────────────────────

void AnimationControlController::updateAnimationTree()
{
    // Save current selection to restore after rebuild
    QString prevEntity = QString::fromStdString(m_selectedEntityName);
    QString prevAnim   = QString::fromStdString(m_selectedAnimation);

    // Node-transform clips (#517) are SceneManager-level and animate a
    // SceneNode by name. createEntity() names each entity after its parent
    // SceneNode, so an entity named "Door" owns the node clips whose animated
    // node is "Door" — list them in that entity's group alongside skeletal /
    // vertex clips so the main Play button + timeline drive them uniformly.
    auto* nodeAnimMgr = NodeAnimationManager::instance();
    const QStringList allNodeClips = nodeAnimMgr ? nodeAnimMgr->listClips() : QStringList();

    QVariantList newTree;
    for (Ogre::Entity* entity : SelectionSet::getSingleton()->getResolvedEntities()) {
        const QString entityName = QString::fromStdString(entity->getName());

        QStringList animNames;
        if (Ogre::AnimationStateSet* set = entity->getAllAnimationStates()) {
            for (const auto& pair : set->getAnimationStates())
                animNames << QString::fromStdString(pair.first);
        }

        // Append node clips that animate THIS entity's scene node (skip drafts
        // in an open edit session — they surface after "Done editing").
        for (const QString& clip : allNodeClips) {
            if (nodeAnimMgr->isEditing(clip)) continue;
            if (nodeAnimMgr->animatedNodes(clip).contains(entityName))
                animNames << clip;
        }

        if (animNames.isEmpty()) continue;

        QVariantMap group;
        group["entity"]     = entityName;
        group["animations"] = animNames;
        newTree.append(group);
    }

    // Early-out when the tree hasn't actually changed AFTER it's been
    // built once: the connected signal (SelectionSet::selectionChanged)
    // also fires whenever undo commands are pushed (mainwindow re-emits
    // it on QUndoStack indexChanged). Without this guard, every
    // BoneTransformCommand push would call selectAnimation() and reset
    // slider+bone selection. We always run the first call so QML
    // listeners get an initial signal, even if the tree is empty.
    if (m_animationTreeBuilt && newTree == m_animationTree) return;

    m_animationTree = newTree;
    m_animationTreeBuilt = true;
    emit animationTreeChanged();

    // Try to restore selection
    if (!prevEntity.isEmpty() && !prevAnim.isEmpty()) {
        selectAnimation(prevEntity, prevAnim);
    } else if (!m_animationTree.isEmpty()) {
        // Auto-select first animation
        auto first = m_animationTree.first().toMap();
        auto anims = first["animations"].toStringList();
        if (!anims.isEmpty())
            selectAnimation(first["entity"].toString(), anims.first());
        else
            selectAnimation("", "");
    } else {
        selectAnimation("", "");
    }
}

void AnimationControlController::selectAnimation(const QString& entityName, const QString& animName)
{
    // Reset state
    m_selectedEntity    = nullptr;
    m_selectedSkeleton  = nullptr;
    m_selectedTrack     = nullptr;
    m_currentKeyframe   = nullptr;
    m_selectedEntityName.clear();
    m_selectedAnimation.clear();
    m_selectedBone.clear();
    m_sliderValue   = 0;
    m_sliderMaximum = 0;
    m_selectedTick  = -1;
    m_boneNames.clear();
    m_keyframeTicks.clear();
    // Cleared up front so an empty selection or a switch to a skeletal/vertex
    // clip always drops the node-clip flag (the dope sheet's band gating binds
    // to it) — it's re-set below only when the resolved clip is a node clip.
    m_selectedIsNodeClip = false;

    if (entityName.isEmpty() || animName.isEmpty()) {
        emit selectionChanged();
        emit boneListChanged();
        emit sliderValueChanged();
        emit animationLengthChanged();
        emit keyframeTicksChanged();
        emit currentKeyframeChanged();
        return;
    }

    // Find the entity
    for (Ogre::Entity* entity : SelectionSet::getSingleton()->getResolvedEntities()) {
        if (entity->getName() == entityName.toStdString()) {
            m_selectedEntity = entity;
            break;
        }
    }
    if (!m_selectedEntity) {
        emit selectionChanged();
        return;
    }

    m_selectedEntityName = entityName.toStdString();
    m_selectedAnimation  = animName.toStdString();
    m_selectedSkeleton   = m_selectedEntity->getSkeleton();

    if (m_selectedSkeleton && m_selectedSkeleton->hasAnimation(m_selectedAnimation)) {
        Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
        m_sliderMaximum = static_cast<int>(anim->getLength() * 1000);
    } else if (Ogre::MeshPtr mesh = m_selectedEntity->getMesh();
               mesh && mesh->hasAnimation(m_selectedAnimation)) {
        // Mesh-level (VAT_POSE) clip — morph-weight animation or Alembic vertex
        // cache. These have no skeleton; take the length off the mesh Animation
        // so the timeline scrubs (skeleton-only derivation left it at 0).
        m_sliderMaximum = static_cast<int>(
            mesh->getAnimation(m_selectedAnimation)->getLength() * 1000);
    } else if (auto* nam = NodeAnimationManager::instance();
               nam && nam->listClips().contains(animName)
               && nam->animatedNodes(animName).contains(entityName)) {
        // SceneManager-level node-transform clip (#517). No skeleton/mesh anim;
        // take the length off the node clip so the timeline scrubs it, and flag
        // it so setAnimationFrame + playback drive the SceneManager state.
        m_selectedIsNodeClip = true;
        m_sliderMaximum = static_cast<int>(nam->clipLength(animName) * 1000);
        // Point the dope-sheet "Node Transforms" band at this clip so selecting
        // it in the animation list also shows its keyframes.
        nam->setActiveClip(animName);
    }

    // Reset loop region to span the whole animation whenever a new clip is
    // selected — users typically want fresh in/out points per clip.
    m_loopStart        = 0.0;
    m_loopEnd          = m_sliderMaximum / 1000.0;
    m_loopRegionActive = false;
    emit loopRegionChanged();

    emit selectionChanged();
    emit animationLengthChanged();
    emit sliderValueChanged();

    refreshBoneList();
}

// ── Bone list ─────────────────────────────────────────────────────────────────

void AnimationControlController::rebindSelectedSkeleton()
{
    if (!m_selectedEntity) return;
    m_selectedSkeleton = m_selectedEntity->getSkeleton();
}

void AnimationControlController::bindSkeletonForEntity(const QString& entityName)
{
    if (entityName.isEmpty()) return;
    for (Ogre::Entity* entity : SelectionSet::getSingleton()->getResolvedEntities()) {
        if (entity->getName() != entityName.toStdString()) continue;
        if (!entity->hasSkeleton()) return;
        const QString keepBone = selectedBone();
        m_selectedEntity = entity;
        m_selectedEntityName = entity->getName();
        m_selectedSkeleton = entity->getSkeleton();
        refreshBoneList(keepBone);
        return;
    }
}

void AnimationControlController::refreshBoneList(const QString& preferSelectBone)
{
    const QString previousBone = QString::fromStdString(m_selectedBone);

    m_boneNames.clear();
    m_selectedTrack     = nullptr;
    m_currentKeyframe   = nullptr;
    // Keep m_selectedBone until we pick the replacement below so QML dropdown
    // never flashes "(none)" during list rebuilds (bone CRUD / undo).

    if (!m_selectedSkeleton || m_selectedAnimation.empty()) {
        if (m_selectedSkeleton) {
            for (unsigned short i = 0; i < m_selectedSkeleton->getNumBones(); ++i)
                m_boneNames << QString::fromStdString(m_selectedSkeleton->getBone(i)->getName());
        }
        emit boneListChanged();
        QString toSelect = !preferSelectBone.isEmpty() ? preferSelectBone : previousBone;
        if (!toSelect.isEmpty() && m_boneNames.contains(toSelect))
            selectBone(toSelect);
        else if (!m_boneNames.isEmpty())
            selectBone(m_boneNames.first());
        else {
            m_selectedBone.clear();
            emit boneListChanged();
            emit keyframeTicksChanged();
            emit currentKeyframeChanged();
        }
        return;
    }
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) {
        for (unsigned short i = 0; i < m_selectedSkeleton->getNumBones(); ++i)
            m_boneNames << QString::fromStdString(m_selectedSkeleton->getBone(i)->getName());
        emit boneListChanged();
        QString toSelect = !preferSelectBone.isEmpty() ? preferSelectBone : previousBone;
        if (!toSelect.isEmpty() && m_boneNames.contains(toSelect))
            selectBone(toSelect);
        else if (!m_boneNames.isEmpty())
            selectBone(m_boneNames.first());
        else {
            m_selectedBone.clear();
            emit boneListChanged();
            emit keyframeTicksChanged();
            emit currentKeyframeChanged();
        }
        return;
    }

    // Show every bone in the skeleton — not just the ones that already
    // have a track in this animation. Tracks for picked-but-untracked
    // bones get created lazily when the user adds a keyframe. Order:
    // tracked bones first (so users see what's already animated at the
    // top), then the rest in skeleton index order.
    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    QSet<QString> trackedNames;
    for (const auto& pair : anim->_getNodeTrackList()) {
        Ogre::Node* node = pair.second->getAssociatedNode();
        if (!node) continue;
        QString name = QString::fromStdString(node->getName());
        m_boneNames << name;
        trackedNames.insert(name);
    }
    for (unsigned short i = 0; i < m_selectedSkeleton->getNumBones(); ++i) {
        QString name = QString::fromStdString(m_selectedSkeleton->getBone(i)->getName());
        if (!trackedNames.contains(name))
            m_boneNames << name;
    }

    emit boneListChanged();

    QString toSelect = !preferSelectBone.isEmpty() ? preferSelectBone : previousBone;
    if (!toSelect.isEmpty() && !m_boneNames.contains(toSelect))
        toSelect.clear();
    if (toSelect.isEmpty() && !m_boneNames.isEmpty())
        toSelect = m_boneNames.first();

    if (!toSelect.isEmpty())
        selectBone(toSelect);
    else {
        m_selectedBone.clear();
        emit boneListChanged();
        emit keyframeTicksChanged();
        emit currentKeyframeChanged();
    }
}

Ogre::Bone* AnimationControlController::selectedBonePtr() const
{
    if (m_selectedBone.empty()) return nullptr;

    // Re-resolve the skeleton LIVE from a currently-selected entity rather than
    // trusting the cached m_selectedSkeleton — operations like adding a morph
    // target call Entity::_initialise(true) (EditableMesh::commitToEntity),
    // which DESTROYS and recreates the entity's SkeletonInstance, leaving
    // m_selectedSkeleton dangling. The gizmo update path then called hasBone()
    // on freed memory and crashed. Cross-check the cached entity is still in the
    // selection before dereferencing it.
    Ogre::Skeleton* skel = nullptr;
    const auto ents = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* e : ents) {
        if (!e || !e->hasSkeleton()) continue;
        if (e == m_selectedEntity) { skel = e->getSkeleton(); break; }
    }
    // Fall back to the first skinned selection if the cached entity is gone.
    if (!skel) {
        for (Ogre::Entity* e : ents) {
            if (e && e->hasSkeleton()) { skel = e->getSkeleton(); break; }
        }
    }
    if (!skel) return nullptr;
    if (!skel->hasBone(m_selectedBone)) return nullptr;
    return skel->getBone(m_selectedBone);
}

bool AnimationControlController::boneCanTranslate(const Ogre::Bone* bone) const
{
    if (!bone) return true;
    // Skeleton roots → always translatable (these are the "this is the
    // character" bones that move the whole rig around — Hips, Pelvis,
    // Armature, etc. — and may also have skin weights). Some importers
    // wrap the actual root in an additional parent bone, so we check
    // both: parentless OR present in Skeleton::getRootBones().
    if (!bone->getParent()) return true;
    if (m_selectedSkeleton) {
        for (Ogre::Bone* root : m_selectedSkeleton->getRootBones())
            if (root == bone) return true;
    }
    if (!m_selectedEntity) return true;
    Ogre::MeshPtr mesh = m_selectedEntity->getMesh();
    if (!mesh) return true;

    // Walk every submesh's vertex-bone-assignment list. If any vertex
    // is weighted to this bone's handle, translating it would tear the
    // mesh away from the rig.
    const auto handle = bone->getHandle();
    auto referencesBone = [&](const Ogre::SubMesh::VertexBoneAssignmentList& list) {
        for (auto it = list.begin(); it != list.end(); ++it)
            if (it->second.boneIndex == handle) return true;
        return false;
    };
    if (referencesBone(mesh->getBoneAssignments())) return false;
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* sub = mesh->getSubMesh(i);
        if (!sub) continue;
        if (referencesBone(sub->getBoneAssignments())) return false;
    }
    return true;
}

void AnimationControlController::selectBone(const QString& boneName)
{
    const bool changedSelection = m_selectedBone != boneName.toStdString();
    m_selectedTrack   = nullptr;
    m_currentKeyframe = nullptr;
    m_selectedBone    = boneName.toStdString();

    // Clear all bone selection highlights
    if (m_selectedSkeleton) {
        for (unsigned short i = 0; i < m_selectedSkeleton->getNumBones(); ++i)
            m_selectedSkeleton->getBone(i)->getUserObjectBindings()
                .setUserAny("selected", Ogre::Any(false));
    }

    // Highlight the picked bone first — independent of whether a track
    // exists for it in the current animation. Bones that aren't yet
    // rigged into the active clip (no NodeAnimationTrack) should still
    // visually select; the keyframe-editing path then no-ops cleanly
    // since m_selectedTrack stays null until the user actually adds a
    // keyframe (which lazily creates the track).
    if (m_selectedSkeleton && !m_selectedBone.empty()
        && m_selectedSkeleton->hasBone(m_selectedBone))
    {
        m_selectedSkeleton->getBone(m_selectedBone)
            ->getUserObjectBindings().setUserAny("selected", Ogre::Any(true));
    }

    // Bind the existing track for this bone, if the animation has one.
    if (m_selectedSkeleton && !m_selectedAnimation.empty()
        && m_selectedSkeleton->hasAnimation(m_selectedAnimation))
    {
        Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
        for (const auto& pair : anim->_getNodeTrackList()) {
            Ogre::Node* node = pair.second->getAssociatedNode();
            if (node && node->getName() == m_selectedBone) {
                m_selectedTrack = pair.second;
                break;
            }
        }
    }

    if (changedSelection) {
        SentryReporter::captureTelemetryEvent(QStringLiteral("selection.bone"),
            QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("gui")},
                        {QStringLiteral("target_kind"), QStringLiteral("bone")}});
    }
    emit boneListChanged();
    refreshSliderTicks();
    setAnimationFrame(m_sliderValue);
}

// ── Timeline / slider ─────────────────────────────────────────────────────────

void AnimationControlController::setSliderValue(int ms)
{
    if (ms == m_sliderValue && m_selectedEntity) {
        // Still call setAnimationFrame to keep Ogre in sync on explicit user drags
    }
    m_sliderValue = ms;
    emit sliderValueChanged();
    setAnimationFrame(ms);
}

void AnimationControlController::setAnimationLength(double length)
{
    if (m_selectedAnimation.empty()) return;

    // Resolve the Animation from the skeleton (skeletal clip) OR the mesh
    // (VAT_POSE morph-weight / vertex-cache clip). Length is settable for both.
    Ogre::Animation* anim = nullptr;
    if (m_selectedSkeleton && m_selectedSkeleton->hasAnimation(m_selectedAnimation)) {
        anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    } else if (m_selectedEntity) {
        if (Ogre::MeshPtr mesh = m_selectedEntity->getMesh();
            mesh && mesh->hasAnimation(m_selectedAnimation))
            anim = mesh->getAnimation(m_selectedAnimation);
    }
    if (!anim) return;

    anim->setLength(static_cast<float>(length));
    m_sliderMaximum = static_cast<int>(length * 1000);

    if (m_sliderValue > m_sliderMaximum) {
        m_sliderValue = m_sliderMaximum;
        emit sliderValueChanged();
    }

    if (m_selectedEntity && m_selectedEntity->hasAnimationState(m_selectedAnimation)) {
        Ogre::AnimationState* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
        state->setLength(static_cast<float>(length));
        state->setTimePosition(std::min(state->getTimePosition(), static_cast<float>(length)));
        m_selectedEntity->getAllAnimationStates()->_notifyDirty();
    }

    refreshSliderTicks();
    emit animationLengthChanged();
}

// ── Playback speed / loop region ──────────────────────────────────────────────

void AnimationControlController::setPlaybackSpeed(double s)
{
    if (s < 0.0) s = 0.0;
    if (qFuzzyCompare(s, m_playbackSpeed)) return;
    m_playbackSpeed = s;
    emit playbackSpeedChanged();
}

void AnimationControlController::setLoopStart(double s)
{
    if (s < 0.0) s = 0.0;
    // Clamp first, then bail out if nothing actually changed — avoids
    // emitting loopRegionChanged when the request collapses to the
    // existing value after clamping.
    if (m_loopEnd > 0.0 && s > m_loopEnd) {
        s = m_loopEnd;
    }
    if (qFuzzyCompare(s, m_loopStart)) return;
    m_loopStart = s;
    emit loopRegionChanged();
}

void AnimationControlController::setLoopEnd(double s)
{
    if (s < 0.0) s = 0.0;
    if (qFuzzyCompare(s, m_loopEnd)) return;
    m_loopEnd = s;
    if (m_loopEnd > 0.0 && m_loopStart > m_loopEnd) {
        m_loopStart = m_loopEnd;
    }
    emit loopRegionChanged();
}

void AnimationControlController::setLoopRegionActive(bool on)
{
    if (on == m_loopRegionActive) return;
    m_loopRegionActive = on;
    emit loopRegionChanged();
}

void AnimationControlController::setAutoKey(bool on)
{
    if (on == m_autoKey) return;
    m_autoKey = on;
    SentryReporter::addBreadcrumb("ui.action",
        QString("AutoKey toggled %1").arg(on ? "on" : "off"));
    emit autoKeyChanged();
}

void AnimationControlController::autoKeyOnTransform()
{
    if (!m_autoKey) return;
    // Don't require m_selectedTrack here — addKeyframe lazily creates
    // the track for non-rigged bones. Just ensure the bone-level
    // identity is set so addKeyframe has something to write into.
    if (!m_selectedEntity || m_selectedAnimation.empty()) return;
    if (!m_selectedSkeleton || m_selectedBone.empty()) return;
    SentryReporter::addBreadcrumb("ui.action", "AutoKey applied keyframe");
    addKeyframe();
}

double AnimationControlController::advanceTime(double currentTime, double dt) const
{
    double next = currentTime + dt * m_playbackSpeed;

    // Apply loop region only when active and the region is well-formed.
    if (m_loopRegionActive && m_loopEnd > m_loopStart) {
        if (next > m_loopEnd) {
            const double span = m_loopEnd - m_loopStart;
            double over = next - m_loopEnd;
            // Wrap any number of full passes back into the region.
            if (span > 0.0) over = std::fmod(over, span);
            next = m_loopStart + over;
        } else if (next < m_loopStart) {
            // Reverse wrap (negative speed); not exposed via UI today but kept
            // symmetrical so callers don't trip if they ever get here.
            const double span = m_loopEnd - m_loopStart;
            double under = m_loopStart - next;
            if (span > 0.0) under = std::fmod(under, span);
            next = m_loopEnd - under;
        }
    }
    return next;
}

void AnimationControlController::setAnimationFrame(int ms)
{
    if (m_selectedAnimation.empty()) return;

    // Node-transform clip (#517): scrub the SceneManager-level AnimationState.
    // It's enabled here so setTimePosition actually poses the node on the next
    // _applySceneAnimations (the play path enables it too; scrubbing a paused
    // clip needs it enabled to show the pose). Node clips carry no bone track,
    // so there is no keyframe-value panel to update — clear and return.
    if (m_selectedIsNodeClip) {
        auto* mgr = Manager::getSingletonPtr();
        auto* scene = mgr ? mgr->getSceneMgr() : nullptr;
        if (scene && scene->hasAnimationState(m_selectedAnimation)) {
            auto* nstate = scene->getAnimationState(m_selectedAnimation);
            nstate->setEnabled(true);
            nstate->setTimePosition(ms / 1000.0f);
        }
        if (m_currentKeyframe) {
            m_currentKeyframe = nullptr;
            m_selectedTick = -1;
            emit currentKeyframeChanged();
        }
        return;
    }

    if (!m_selectedEntity || m_selectedAnimation.empty()) return;
    if (!m_selectedEntity->hasAnimationState(m_selectedAnimation)) return;

    Ogre::AnimationState* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
    state->setTimePosition(ms / 1000.0f);

    if (!m_selectedTrack || m_selectedTrack->getNumKeyFrames() == 0) {
        if (m_currentKeyframe) {
            m_updatingValues = true;
            m_currentKeyframe = nullptr;
            m_selectedTick = -1;
            m_updatingValues = false;
            emit keyframeTicksChanged();
            emit currentKeyframeChanged();
        }
        return;
    }

    Ogre::KeyFrame* kf1 = nullptr;
    Ogre::KeyFrame* kf2 = nullptr;
    m_selectedTrack->getKeyFramesAtTime(ms / 1000.0f, &kf1, &kf2);

    Ogre::TransformKeyFrame* closest = nullptr;
    if (kf1 && kf2) {
        float d1 = std::fabs(kf1->getTime() - ms / 1000.0f);
        float d2 = std::fabs(kf2->getTime() - ms / 1000.0f);
        closest = static_cast<Ogre::TransformKeyFrame*>(d1 <= d2 ? kf1 : kf2);
    } else if (kf1) {
        closest = static_cast<Ogre::TransformKeyFrame*>(kf1);
    } else if (kf2) {
        closest = static_cast<Ogre::TransformKeyFrame*>(kf2);
    }

    bool tickChanged = (closest != m_currentKeyframe);
    m_currentKeyframe = closest;

    if (m_currentKeyframe) {
        int newTick = static_cast<int>(m_currentKeyframe->getTime() * 1000);
        if (newTick != m_selectedTick) {
            m_selectedTick = newTick;
            tickChanged = true;
        }
    } else {
        m_selectedTick = -1;
        tickChanged = true;
    }

    if (tickChanged) emit keyframeTicksChanged();
    if (m_currentKeyframe) pushKeyframeValues();
}

void AnimationControlController::refreshSliderTicks()
{
    m_keyframeTicks.clear();
    m_selectedTick = -1;

    if (m_selectedTrack) {
        for (unsigned short i = 0; i < m_selectedTrack->getNumKeyFrames(); ++i)
            m_keyframeTicks.append(static_cast<int>(m_selectedTrack->getKeyFrame(i)->getTime() * 1000));
        if (m_currentKeyframe)
            m_selectedTick = static_cast<int>(m_currentKeyframe->getTime() * 1000);
    }

    emit keyframeTicksChanged();
    // Dope-sheet view re-queries rows on the same signals as track-level
    // changes (add/delete/move/select).
    emit boneRowsChanged();
}

void AnimationControlController::onUndoRedoCommandApplied()
{
    // Structural undo/redo (track destroy, keyframe add/remove) can
    // invalidate cached pointers we hold. Drop them and re-resolve
    // against the current skeleton state — but preserve the user's
    // current bone selection (refreshBoneList would reset to the
    // first bone, which is jarring).
    m_selectedTrack   = nullptr;
    m_currentKeyframe = nullptr;
    m_selectedTick    = -1;

    // Re-resolve m_selectedTrack from the active animation + bone, if
    // both are still valid (the track may have been destroyed by an
    // AddKeyframeCommand undo on a lazy-created track).
    if (m_selectedSkeleton && !m_selectedAnimation.empty()
        && m_selectedSkeleton->hasAnimation(m_selectedAnimation)
        && !m_selectedBone.empty()
        && m_selectedSkeleton->hasBone(m_selectedBone))
    {
        Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
        for (const auto& pair : anim->_getNodeTrackList()) {
            Ogre::Node* node = pair.second->getAssociatedNode();
            if (node && node->getName() == m_selectedBone) {
                m_selectedTrack = pair.second;
                break;
            }
        }
    }

    refreshSliderTicks();
    setAnimationFrame(m_sliderValue);
    // The dope sheet + curve editor read keyTimes from allBoneRows(),
    // which they refresh on boneRowsChanged. Without this emit, undo
    // of a Bake leaves the QML views showing stale dense keyframes
    // even though the underlying track has been reverted.
    emit boneRowsChanged();
}

// ── Keyframe editing ──────────────────────────────────────────────────────────

bool AnimationControlController::hasPrevKeyframe() const
{
    if (!m_selectedTrack || m_selectedTrack->getNumKeyFrames() == 0) return false;
    float time = m_sliderValue / 1000.0f;
    for (int i = static_cast<int>(m_selectedTrack->getNumKeyFrames()) - 1; i >= 0; --i) {
        if (m_selectedTrack->getKeyFrame(i)->getTime() < time - 0.001f) return true;
    }
    return false;
}

bool AnimationControlController::hasNextKeyframe() const
{
    if (!m_selectedTrack || m_selectedTrack->getNumKeyFrames() == 0) return false;
    float time = m_sliderValue / 1000.0f;
    for (unsigned short i = 0; i < m_selectedTrack->getNumKeyFrames(); ++i) {
        if (m_selectedTrack->getKeyFrame(i)->getTime() > time + 0.001f) return true;
    }
    return false;
}

void AnimationControlController::prevKeyframe()
{
    if (!m_selectedTrack || m_selectedTrack->getNumKeyFrames() == 0) return;
    float time = m_sliderValue / 1000.0f;
    Ogre::TransformKeyFrame* target = nullptr;
    for (int i = static_cast<int>(m_selectedTrack->getNumKeyFrames()) - 1; i >= 0; --i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(m_selectedTrack->getKeyFrame(i));
        if (kf->getTime() < time - 0.001f) { target = kf; break; }
    }
    if (!target)
        target = static_cast<Ogre::TransformKeyFrame*>(m_selectedTrack->getKeyFrame(0));
    setSliderValue(static_cast<int>(target->getTime() * 1000));
}

void AnimationControlController::nextKeyframe()
{
    if (!m_selectedTrack || m_selectedTrack->getNumKeyFrames() == 0) return;
    float time = m_sliderValue / 1000.0f;
    Ogre::TransformKeyFrame* target = nullptr;
    for (unsigned short i = 0; i < m_selectedTrack->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(m_selectedTrack->getKeyFrame(i));
        if (kf->getTime() > time + 0.001f) { target = kf; break; }
    }
    if (!target)
        target = static_cast<Ogre::TransformKeyFrame*>(
            m_selectedTrack->getKeyFrame(m_selectedTrack->getNumKeyFrames() - 1));
    setSliderValue(static_cast<int>(target->getTime() * 1000));
}

void AnimationControlController::addKeyframe()
{
    if (!m_selectedEntity || m_selectedAnimation.empty()) return;
    if (!m_selectedSkeleton || m_selectedBone.empty()) return;
    if (!m_selectedSkeleton->hasBone(m_selectedBone)) return;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return;

    // Determine the operation mode for undo: track-create vs.
    // keyframe-create vs. keyframe-update.
    const bool trackExisted = (m_selectedTrack != nullptr);
    AddKeyframeCommand::Mode mode = AddKeyframeCommand::Mode::TrackCreated;

    // Lazily create an animation track for this bone if it doesn't have
    // one yet (non-rigged bones, or bones the imported animation didn't
    // touch). Without this, the user's edit would be a runtime-only
    // pose that vanishes on export.
    if (!m_selectedTrack) {
        Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
        Ogre::Bone* bone = m_selectedSkeleton->getBone(m_selectedBone);
        m_selectedTrack = anim->createNodeTrack(bone->getHandle(), bone);
        // Re-include this bone in the bone-list and rebuild the panel
        // so subsequent +KF / dope-sheet operations see the new track.
        QString boneNameStr = QString::fromStdString(m_selectedBone);
        if (!m_boneNames.contains(boneNameStr))
            m_boneNames << boneNameStr;
        emit boneListChanged();
    }
    if (!m_selectedTrack) return;

    const float time = m_sliderValue / 1000.0f;
    // Reuse a keyframe at the same time if one already exists — the rest of
    // this controller treats same-time collisions as invalid, and auto-key
    // would otherwise stack duplicates on every drag-end at the same scrub
    // time. Match the same epsilon used by deleteKeyframe (1 ms).
    Ogre::TransformKeyFrame* existingKf = nullptr;
    constexpr float kKeyframeEpsilon = 0.001f;
    for (unsigned short i = 0; i < m_selectedTrack->getNumKeyFrames(); ++i) {
        auto* existing = static_cast<Ogre::TransformKeyFrame*>(m_selectedTrack->getKeyFrame(i));
        if (std::fabs(existing->getTime() - time) <= kKeyframeEpsilon) {
            existingKf = existing;
            break;
        }
    }

    // Capture before-TRS so undo restores the keyframe's pre-edit state.
    // Defaults are identity for the create paths (the keyframe didn't
    // exist; undo will remove it instead of restoring values).
    Ogre::Vector3    beforeT = Ogre::Vector3::ZERO;
    Ogre::Quaternion beforeR = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3    beforeS = Ogre::Vector3::UNIT_SCALE;
    if (existingKf) {
        beforeT = existingKf->getTranslate();
        beforeR = existingKf->getRotation();
        beforeS = existingKf->getScale();
        mode = AddKeyframeCommand::Mode::KeyframeUpdated;
    } else if (trackExisted) {
        mode = AddKeyframeCommand::Mode::KeyframeCreated;
    }
    // else: trackExisted == false → Mode::TrackCreated (already set above).

    // Compute the after-TRS from the bone's current local pose
    // (relative to its initial bind pose, the format
    // TransformKeyFrame stores). Previously this used
    // getInterpolatedKeyFrame, which samples the existing animation
    // curve at `time` and produces an identity-ish keyframe whenever
    // the curve is flat there — the user-visible "blank registry"
    // bug. With this change, hitting +KF after dragging the scene
    // node (or, via the bone gizmo, the bone directly) captures the
    // actual pose under the cursor at that scrub time.
    const Ogre::Bone* bone = m_selectedSkeleton->getBone(m_selectedBone);
    const Ogre::Vector3    afterT = bone->getPosition() - bone->getInitialPosition();
    const Ogre::Quaternion afterR = bone->getInitialOrientation().Inverse() * bone->getOrientation();
    const Ogre::Vector3    afterS = bone->getScale() / bone->getInitialScale();

    // QUndoStack::push() executes redo() immediately, which performs the
    // actual write. Pushing the command both creates the keyframe and
    // makes it undoable.
    auto* cmd = new AddKeyframeCommand(  // NOSONAR — QUndoStack owns
        m_selectedEntityName,
        m_selectedAnimation,
        m_selectedBone,
        time,
        mode,
        beforeT, beforeR, beforeS,
        afterT,  afterR,  afterS);
    UndoManager::getSingleton()->push(cmd);

    refreshSliderTicks();
    setAnimationFrame(m_sliderValue);
}

void AnimationControlController::deleteKeyframe()
{
    if (!m_selectedTrack || !m_currentKeyframe) return;

    // Capture the keyframe's TRS so the undo path can restore it.
    const float            t = m_currentKeyframe->getTime();
    const Ogre::Vector3    keyT = m_currentKeyframe->getTranslate();
    const Ogre::Quaternion keyR = m_currentKeyframe->getRotation();
    const Ogre::Vector3    keyS = m_currentKeyframe->getScale();

    // Push the command — its redo() removes the keyframe. Drop our
    // cached pointer first since the command will invalidate it.
    m_currentKeyframe = nullptr;
    m_selectedTick    = -1;
    auto* cmd = new DeleteKeyframeCommand(  // NOSONAR — QUndoStack owns
        m_selectedEntityName,
        m_selectedAnimation,
        m_selectedBone,
        t, keyT, keyR, keyS);
    UndoManager::getSingleton()->push(cmd);

    refreshSliderTicks();
    setAnimationFrame(m_sliderValue);
}

void AnimationControlController::pushKeyframeValues()
{
    if (!m_currentKeyframe) return;

    m_updatingValues = true;
    Ogre::Vector3    t = m_currentKeyframe->getTranslate();
    Ogre::Vector3    s = m_currentKeyframe->getScale();
    Ogre::Quaternion r = m_currentKeyframe->getRotation();
    m_kfTransX = t.x; m_kfTransY = t.y; m_kfTransZ = t.z;
    m_kfScaleX = s.x; m_kfScaleY = s.y; m_kfScaleZ = s.z;
    m_kfRotW   = r.w; m_kfRotX   = r.x; m_kfRotY   = r.y; m_kfRotZ = r.z;
    m_updatingValues = false;
    emit currentKeyframeChanged();
}

void AnimationControlController::notifyOgreUpdate()
{
    if (!m_selectedEntity || m_selectedAnimation.empty()) return;
    m_selectedEntity->getAllAnimationStates()->_notifyDirty();
    auto* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
    state->setTimePosition(state->getTimePosition());
}

// ── Keyframe value setters ────────────────────────────────────────────────────

#define KF_SET_TRANS(AXIS, FIELD) \
void AnimationControlController::setKfTrans##AXIS(double v) { \
    if (m_updatingValues || !m_currentKeyframe) return; \
    Ogre::Vector3 t = m_currentKeyframe->getTranslate(); \
    t.FIELD = static_cast<float>(v); \
    m_currentKeyframe->setTranslate(t); \
    m_kfTrans##AXIS = v; \
    notifyOgreUpdate(); \
}

#define KF_SET_SCALE(AXIS, FIELD) \
void AnimationControlController::setKfScale##AXIS(double v) { \
    if (m_updatingValues || !m_currentKeyframe) return; \
    Ogre::Vector3 s = m_currentKeyframe->getScale(); \
    s.FIELD = static_cast<float>(v); \
    m_currentKeyframe->setScale(s); \
    m_kfScale##AXIS = v; \
    notifyOgreUpdate(); \
}

#define KF_SET_ROT(AXIS, FIELD) \
void AnimationControlController::setKfRot##AXIS(double v) { \
    if (m_updatingValues || !m_currentKeyframe) return; \
    Ogre::Quaternion r = m_currentKeyframe->getRotation(); \
    r.FIELD = static_cast<float>(v); \
    m_currentKeyframe->setRotation(r); \
    m_kfRot##AXIS = v; \
    notifyOgreUpdate(); \
}

KF_SET_TRANS(X, x)
KF_SET_TRANS(Y, y)
KF_SET_TRANS(Z, z)
KF_SET_SCALE(X, x)
KF_SET_SCALE(Y, y)
KF_SET_SCALE(Z, z)
KF_SET_ROT(W, w)
KF_SET_ROT(X, x)
KF_SET_ROT(Y, y)
KF_SET_ROT(Z, z)

// ── Dope sheet API (slice C) ──────────────────────────────────────────────────

namespace {

// A channel is "active" on a track if any keyframe's value differs from the
// channel's identity (translate.x = 0, rotation = 1+0i+0j+0k, scale = 1) by
// more than this epsilon. Tighter than kBulkEpsilon since these are values,
// not times — a 1mm translate is meaningful.
constexpr float kChannelEpsilon = 1e-4f;

// Returns the 9 boolean channel flags for a track in TRS order:
// {tx, ty, tz, rw, rx, ry, rz, sx, sy, sz}.
// Only flags whose values deviate from identity are set.
//
// Rotation identity covers BOTH (+1, 0, 0, 0) AND (-1, 0, 0, 0) — a
// quaternion and its negative encode the same rotation. Naive component-
// wise comparison would flag rw as active for a sign-flipped identity,
// producing bogus chevrons. We compare against the absolute values
// instead: |w| ≈ 1, |x| ≈ |y| ≈ |z| ≈ 0 means identity regardless of sign.
QVariantMap collectActiveChannels(const Ogre::NodeAnimationTrack* track)
{
    bool tx = false, ty = false, tz = false;
    bool rw = false, rx = false, ry = false, rz = false;
    bool sx = false, sy = false, sz = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const auto* kf = static_cast<const Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        const Ogre::Vector3    t = kf->getTranslate();
        const Ogre::Quaternion r = kf->getRotation();
        const Ogre::Vector3    s = kf->getScale();
        if (std::fabs(t.x) > kChannelEpsilon) tx = true;
        if (std::fabs(t.y) > kChannelEpsilon) ty = true;
        if (std::fabs(t.z) > kChannelEpsilon) tz = true;
        // Sign-agnostic rotation identity check — see header comment.
        if (std::fabs(std::fabs(r.w) - 1.0f) > kChannelEpsilon) rw = true;
        if (std::fabs(r.x) > kChannelEpsilon) rx = true;
        if (std::fabs(r.y) > kChannelEpsilon) ry = true;
        if (std::fabs(r.z) > kChannelEpsilon) rz = true;
        // Scale identity = (1, 1, 1).
        if (std::fabs(s.x - 1.0f) > kChannelEpsilon) sx = true;
        if (std::fabs(s.y - 1.0f) > kChannelEpsilon) sy = true;
        if (std::fabs(s.z - 1.0f) > kChannelEpsilon) sz = true;
    }
    QVariantMap m;
    m[QStringLiteral("tx")] = tx; m[QStringLiteral("ty")] = ty; m[QStringLiteral("tz")] = tz;
    m[QStringLiteral("rw")] = rw; m[QStringLiteral("rx")] = rx;
    m[QStringLiteral("ry")] = ry; m[QStringLiteral("rz")] = rz;
    m[QStringLiteral("sx")] = sx; m[QStringLiteral("sy")] = sy; m[QStringLiteral("sz")] = sz;
    return m;
}

} // namespace

QVariantList AnimationControlController::allBoneRows() const
{
    QVariantList rows;

    // Resolve the skeleton to show. Normally it's m_selectedSkeleton (set when a
    // skeletal clip is the selected animation). But the dope sheet shows ALL of
    // a mesh's animation types together (skeletal + morph + node): when a NODE
    // clip is selected/edited, no skeletal clip is selected and
    // m_selectedSkeleton is null — so fall back to the first selected entity's
    // skeleton so its bones still show alongside the node band. (#517)
    Ogre::Skeleton* skel = m_selectedSkeleton;
    if (!skel) {
        const auto ents = SelectionSet::getSingleton()->getResolvedEntities();
        for (Ogre::Entity* e : ents) {
            if (e && e->hasSkeleton()) { skel = e->getSkeleton(); break; }
        }
    }
    if (!skel) return rows;

    // Which skeletal animation to read bone tracks from — the selected one, or
    // the skeleton's first animation when a node/morph clip is what's selected.
    std::string animName = m_selectedAnimation;
    if (animName.empty() || !skel->hasAnimation(animName)) {
        if (skel->getNumAnimations() == 0) return rows;
        animName = skel->getAnimation(static_cast<unsigned short>(0))->getName();
    }

    Ogre::Animation* anim = skel->getAnimation(animName);
    for (const auto& [handle, track] : anim->_getNodeTrackList()) {
        Ogre::Node* node = track->getAssociatedNode();
        if (!node) continue;

        QVariantList keyTimes;
        keyTimes.reserve(static_cast<int>(track->getNumKeyFrames()));
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            keyTimes.append(static_cast<double>(track->getKeyFrame(i)->getTime()));
        }

        QVariantMap row;
        row[QStringLiteral("bone")]     = QString::fromStdString(node->getName());
        row[QStringLiteral("keyTimes")] = keyTimes;
        row[QStringLiteral("channels")] = collectActiveChannels(track);
        rows.append(row);
    }
    return rows;
}

QVariantList AnimationControlController::allMorphRows() const
{
    QVariantList rows;
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return rows;
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty() || !entities.first()) return rows;
    Ogre::Entity* entity = entities.first();
    if (!entity) return rows;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return rows;
    const Ogre::PoseList& poseList = mesh->getPoseList();
    if (poseList.empty()) return rows;

    // Each pose has a matching `Ogre::Animation` (see MeshProcessor:
    // import-time one-animation-per-pose pattern). Read the keyframe
    // times off the animation's VAT_POSE track; in A1 every animation
    // carries a single t=0 keyframe, but future slices may add more.
    for (const Ogre::Pose* pose : poseList) {
        if (!pose) continue;
        const Ogre::String poseName = pose->getName();
        if (poseName.empty()) continue;

        QVariantList keyTimes;
        const unsigned short handle = pose->getTarget();

        // Prefer the animated-weight track on the shared "MorphAnim" clip
        // (Slice 2 #519 — multiple keyframes at varying influence). Fall back
        // to the static per-target Animation (single shape keyframe at t=0)
        // when the target has no weight animation yet.
        auto appendTrackTimes = [&](const std::string& clipName) -> bool {
            if (!mesh->hasAnimation(clipName)) return false;
            Ogre::Animation* anim = mesh->getAnimation(clipName);
            if (!anim->hasVertexTrack(handle)) return false;
            Ogre::VertexAnimationTrack* track = anim->getVertexTrack(handle);
            if (!track || track->getAnimationType() != Ogre::VAT_POSE) return false;
            // A MorphAnim track carries keyframes for every target on this
            // handle; keep only the ones that reference THIS pose so each row
            // shows its own diamonds.
            bool any = false;
            for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
                auto* kf = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(i));
                bool refsThisPose = false;
                for (const auto& ref : kf->getPoseReferences()) {
                    const Ogre::Pose* rp =
                        (ref.poseIndex < mesh->getPoseList().size())
                            ? mesh->getPoseList()[ref.poseIndex] : nullptr;
                    if (rp && rp->getName() == poseName) { refsThisPose = true; break; }
                }
                if (refsThisPose) {
                    keyTimes.append(static_cast<double>(kf->getTime()));
                    any = true;
                }
            }
            return any;
        };

        // Read the ACTIVE morph clip's keys (the one the user is editing), so
        // switching clips in the dropdown shows that clip's diamonds. If the
        // active clip isn't on this mesh (e.g. right after IMPORTING a model
        // whose clip is "Sniff" while the app default is still "MorphAnim", and
        // the auto-adopt signal hasn't landed yet), resolve the first real morph
        // clip on the mesh directly — don't depend on active-clip timing. Fall
        // back to the static shape-only clip when neither has a key here.
        std::string clipToRead =
            MorphAnimationManager::instance()->activeMorphClip().toStdString();
        if (!mesh->hasAnimation(clipToRead)) {
            const QStringList clips = MorphAnimationManager::instance()->morphClips();
            if (!clips.isEmpty())
                clipToRead = clips.first().toStdString();
        }
        if (!appendTrackTimes(clipToRead))
            appendTrackTimes(poseName);  // static shape-only clip

        QVariantMap row;
        row[QStringLiteral("name")]     = QString::fromStdString(poseName);
        row[QStringLiteral("keyTimes")] = keyTimes;
        rows.append(row);
    }
    return rows;
}

bool AnimationControlController::moveKeyframe(const QString& boneName,
                                              double oldTime, double newTime)
{
    if (boneName.isEmpty()) return false;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return false;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return false;
    if (qFuzzyCompare(oldTime + 1.0, newTime + 1.0)) return false;

    // Validate up-front so we never push a no-op onto the undo stack.
    // The command's internal validation is the source of truth, but
    // duplicating the find-source-keyframe + collision-check here lets
    // us return false without polluting the undo history.
    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    const std::string boneStd = boneName.toStdString();
    if (!m_selectedSkeleton->hasBone(boneStd)) return false;
    Ogre::Bone* bone = m_selectedSkeleton->getBone(boneStd);
    if (!bone || !anim->hasNodeTrack(bone->getHandle())) return false;
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());

    constexpr float kEpsilon = 0.001f;
    int sourceIdx = -1;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::fabs(track->getKeyFrame(i)->getTime() - static_cast<float>(oldTime)) <= kEpsilon) {
            sourceIdx = static_cast<int>(i);
            break;
        }
    }
    if (sourceIdx < 0) return false; // no keyframe at oldTime

    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (static_cast<int>(i) == sourceIdx) continue;
        if (std::fabs(track->getKeyFrame(i)->getTime() - static_cast<float>(newTime)) <= kEpsilon) {
            return false; // collision with another existing keyframe
        }
    }

    // QUndoStack::push() takes ownership of the command — this raw new is
    // the standard QUndoCommand idiom (mirrors TransformCommands callers).
    auto* cmd = new MoveKeyframeCommand(m_selectedEntityName, // NOSONAR — QUndoStack owns
                                        m_selectedAnimation,
                                        boneStd,
                                        static_cast<float>(oldTime),
                                        static_cast<float>(newTime));
    UndoManager::getSingleton()->push(cmd);
    // The command's redo() ran inside push(); refresh the slider ticks for
    // the currently-edited bone and signal QML views to re-read rows.
    refreshSliderTicks();
    emit boneRowsChanged();
    return true;
}

bool AnimationControlController::moveKeyframePreview(const QString& boneName,
                                                      double oldTime, double newTime)
{
    if (boneName.isEmpty()) return false;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return false;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return false;
    if (qFuzzyCompare(oldTime + 1.0, newTime + 1.0)) return false;

    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    const std::string boneStd = boneName.toStdString();
    if (!m_selectedSkeleton->hasBone(boneStd)) return false;
    Ogre::Bone* bone = m_selectedSkeleton->getBone(boneStd);
    if (!bone || !anim->hasNodeTrack(bone->getHandle())) return false;
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());

    constexpr float kEpsilon = 0.001f;
    int sourceIdx = -1;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::fabs(track->getKeyFrame(i)->getTime() - static_cast<float>(oldTime)) <= kEpsilon) {
            sourceIdx = static_cast<int>(i);
            break;
        }
    }
    if (sourceIdx < 0) return false;

    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (static_cast<int>(i) == sourceIdx) continue;
        if (std::fabs(track->getKeyFrame(i)->getTime() - static_cast<float>(newTime)) <= kEpsilon) {
            return false;
        }
    }

    auto* oldKf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(sourceIdx));
    const Ogre::Vector3    t = oldKf->getTranslate();
    const Ogre::Quaternion r = oldKf->getRotation();
    const Ogre::Vector3    s = oldKf->getScale();
    track->removeKeyFrame(static_cast<unsigned short>(sourceIdx));
    auto* newKf = track->createNodeKeyFrame(static_cast<float>(newTime));
    newKf->setTranslate(t);
    newKf->setRotation(r);
    newKf->setScale(s);
    track->_keyFrameDataChanged();

    // Skip refreshSliderTicks + boneRowsChanged: the release-time
    // MoveKeyframeCommand re-emits both.
    notifyOgreUpdate();
    return true;
}

// ── Bulk keyframe ops (slice D1) ──────────────────────────────────────────────

namespace {

constexpr float kBulkEpsilon = 0.001f;

// Resolve an arbitrary bone's track on the controller's current animation.
// Returns nullptr if any link in the chain is missing.
Ogre::NodeAnimationTrack* resolveTrackByBone(Ogre::Skeleton* skel,
                                             const std::string& animName,
                                             const std::string& boneName)
{
    if (!skel || animName.empty() || boneName.empty()) return nullptr;
    if (!skel->hasAnimation(animName)) return nullptr;
    if (!skel->hasBone(boneName)) return nullptr;
    Ogre::Animation* anim = skel->getAnimation(animName);
    Ogre::Bone* bone = skel->getBone(boneName);
    if (!anim || !bone) return nullptr;
    if (!anim->hasNodeTrack(bone->getHandle())) return nullptr;
    return anim->getNodeTrack(bone->getHandle());
}

// Convert a QML-side selection list ({bone, time} maps) into the flat
// command-side list of items, preserving caller-supplied order.
QVector<MoveKeyframesCommand::Item> selectionToItems(const QVariantList& sel)
{
    QVector<MoveKeyframesCommand::Item> out;
    out.reserve(sel.size());
    for (const QVariant& v : sel) {
        const QVariantMap m = v.toMap();
        const QString bone = m.value(QStringLiteral("bone")).toString();
        const double  t    = m.value(QStringLiteral("time")).toDouble();
        if (bone.isEmpty()) continue;
        out.append({ bone.toStdString(), static_cast<float>(t) });
    }
    return out;
}

} // namespace

namespace {

// Clamp the batch dt so no item's destination falls outside [0, length].
// Returns the largest legal magnitude that still has the requested sign.
float clampBatchDt(const QVector<MoveKeyframesCommand::Item>& items,
                   float requestedDt, float length)
{
    if (items.isEmpty()) return 0.0f;
    float lo = -std::numeric_limits<float>::infinity();
    float hi =  std::numeric_limits<float>::infinity();
    for (const auto& it : items) {
        // Each item's destination must satisfy 0 <= origT + dt <= length.
        lo = std::max(lo, -it.originalTime);
        hi = std::min(hi,  length - it.originalTime);
    }
    if (lo > hi) return 0.0f; // shouldn't happen for non-empty range
    return std::clamp(requestedDt, lo, hi);
}

// Returns true if the selection's destinations would collide with any
// non-selected keyframe on its track.
bool batchCollides(Ogre::Skeleton* skel, const std::string& animName,
                   const QVector<MoveKeyframesCommand::Item>& items, float dt)
{
    auto isMember = [&](const std::string& bone, float time) {
        for (const auto& it : items) {
            if (it.boneName == bone &&
                std::fabs(it.originalTime - time) <= kBulkEpsilon) return true;
        }
        return false;
    };
    for (const auto& it : items) {
        const float dst = it.originalTime + dt;
        auto* track = resolveTrackByBone(skel, animName, it.boneName);
        if (!track) return true; // missing track = treat as collision; bail
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            const float t = track->getKeyFrame(i)->getTime();
            if (std::fabs(t - it.originalTime) <= kBulkEpsilon) continue;
            if (std::fabs(t - dst) <= kBulkEpsilon &&
                !isMember(it.boneName, t)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool AnimationControlController::moveKeyframes(const QVariantList& selection,
                                                double dt)
{
    if (selection.isEmpty()) return false;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return false;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return false;
    if (qFuzzyCompare(dt + 1.0, 1.0)) return false;

    QVector<MoveKeyframesCommand::Item> items = selectionToItems(selection);
    if (items.isEmpty()) return false;

    const auto length = static_cast<float>(animationLength());
    // Clamp the requested delta so a multi-selection sliding into a clip
    // boundary lands on the nearest legal time instead of snapping back.
    const float dtf = clampBatchDt(items, static_cast<float>(dt), length);
    if (qFuzzyIsNull(dtf)) return false;

    // Refuse if the (now-clamped) shift would collide with any non-selected
    // keyframe on the same track.
    if (batchCollides(m_selectedSkeleton, m_selectedAnimation, items, dtf)) {
        return false;
    }

    auto* cmd = new MoveKeyframesCommand(m_selectedEntityName, // NOSONAR — QUndoStack owns
                                          m_selectedAnimation,
                                          items, dtf);
    UndoManager::getSingleton()->push(cmd);
    SentryReporter::addBreadcrumb(
        "ui.action",
        QString("Dope Sheet: bulk-move %1 keyframe(s) by %2s")
            .arg(static_cast<int>(items.size()))
            .arg(static_cast<double>(dtf), 0, 'f', 3));
    refreshSliderTicks();
    emit boneRowsChanged();
    return true;
}

QString AnimationControlController::serializeKeyframes(
        const QVariantList& selection) const
{
    if (selection.isEmpty()) return {};
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return {};

    // Find the earliest selected time so paste stays relative.
    double t0 = std::numeric_limits<double>::infinity();
    for (const QVariant& v : selection) {
        const double t = v.toMap().value(QStringLiteral("time")).toDouble();
        if (t < t0) t0 = t;
    }
    if (!std::isfinite(t0)) return {};

    QJsonArray arr;
    for (const QVariant& v : selection) {
        const QVariantMap m = v.toMap();
        const QString bone = m.value(QStringLiteral("bone")).toString();
        const auto    tabs = static_cast<float>(m.value(QStringLiteral("time")).toDouble());
        if (bone.isEmpty()) continue;
        auto* track = resolveTrackByBone(m_selectedSkeleton, m_selectedAnimation,
                                          bone.toStdString());
        if (!track) continue;
        // Find the actual keyframe to capture its TRS values.
        Ogre::TransformKeyFrame* kf = nullptr;
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            auto* candidate = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
            if (std::fabs(candidate->getTime() - tabs) <= kBulkEpsilon) {
                kf = candidate;
                break;
            }
        }
        if (!kf) continue;

        const Ogre::Vector3    t = kf->getTranslate();
        const Ogre::Quaternion r = kf->getRotation();
        const Ogre::Vector3    s = kf->getScale();
        QJsonObject e;
        e[QStringLiteral("bone")] = bone;
        e[QStringLiteral("dt")]   = static_cast<double>(tabs) - t0;
        e[QStringLiteral("tx")] = t.x; e[QStringLiteral("ty")] = t.y; e[QStringLiteral("tz")] = t.z;
        e[QStringLiteral("rw")] = r.w; e[QStringLiteral("rx")] = r.x;
        e[QStringLiteral("ry")] = r.y; e[QStringLiteral("rz")] = r.z;
        e[QStringLiteral("sx")] = s.x; e[QStringLiteral("sy")] = s.y; e[QStringLiteral("sz")] = s.z;
        arr.append(e);
    }
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("kind")]    = QStringLiteral("qtmesh.dopesheet.keyframes");
    root[QStringLiteral("entries")] = arr;
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

namespace {

// All numeric fields that must be present on a clipboard entry. Missing or
// non-numeric values reject the entry entirely (rather than silently
// coercing to identity, which would paste corrupted transforms).
const QStringList& pasteRequiredFields() {
    static const QStringList kFields = {
        QStringLiteral("tx"), QStringLiteral("ty"), QStringLiteral("tz"),
        QStringLiteral("rw"), QStringLiteral("rx"), QStringLiteral("ry"),
        QStringLiteral("rz"),
        QStringLiteral("sx"), QStringLiteral("sy"), QStringLiteral("sz"),
    };
    return kFields;
}

bool entryHasAllFields(const QJsonObject& e) {
    for (const QString& key : pasteRequiredFields()) {
        if (!e.contains(key) || !e.value(key).isDouble()) return false;
    }
    return true;
}

// Parse one clipboard entry into a PasteKeyframesCommand::Entry. Returns
// nullopt when the entry is missing required fields or its destination
// time falls outside [0, length].
std::optional<PasteKeyframesCommand::Entry>
parsePasteEntry(const QJsonObject& e, double atTime, float length)
{
    const QString bone = e.value(QStringLiteral("bone")).toString();
    if (bone.isEmpty()) return std::nullopt;
    if (!entryHasAllFields(e)) return std::nullopt;
    const auto dst = static_cast<float>(atTime + e.value(QStringLiteral("dt")).toDouble());
    if (dst < 0.0f || dst > length + kBulkEpsilon) return std::nullopt;
    PasteKeyframesCommand::Entry pe;
    pe.boneName = bone.toStdString();
    pe.time = dst;
    pe.tx = static_cast<float>(e.value(QStringLiteral("tx")).toDouble());
    pe.ty = static_cast<float>(e.value(QStringLiteral("ty")).toDouble());
    pe.tz = static_cast<float>(e.value(QStringLiteral("tz")).toDouble());
    pe.rw = static_cast<float>(e.value(QStringLiteral("rw")).toDouble());
    pe.rx = static_cast<float>(e.value(QStringLiteral("rx")).toDouble());
    pe.ry = static_cast<float>(e.value(QStringLiteral("ry")).toDouble());
    pe.rz = static_cast<float>(e.value(QStringLiteral("rz")).toDouble());
    pe.sx = static_cast<float>(e.value(QStringLiteral("sx")).toDouble());
    pe.sy = static_cast<float>(e.value(QStringLiteral("sy")).toDouble());
    pe.sz = static_cast<float>(e.value(QStringLiteral("sz")).toDouble());
    return pe;
}

// Counts how many entries would actually paste — i.e. don't collide with
// an existing keyframe on their target track.
int countNonColliding(Ogre::Skeleton* skel, const std::string& animName,
                      const QVector<PasteKeyframesCommand::Entry>& entries)
{
    int n = 0;
    for (const auto& e : entries) {
        auto* track = resolveTrackByBone(skel, animName, e.boneName);
        if (!track) continue;
        bool collides = false;
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            if (std::fabs(track->getKeyFrame(i)->getTime() - e.time) <= kBulkEpsilon) {
                collides = true; break;
            }
        }
        if (!collides) ++n;
    }
    return n;
}

} // namespace

int AnimationControlController::pasteKeyframesAt(const QString& json,
                                                  double atTime)
{
    if (json.isEmpty()) return 0;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return 0;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return 0;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return 0;
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("kind")).toString() !=
        QStringLiteral("qtmesh.dopesheet.keyframes")) return 0;
    if (root.value(QStringLiteral("version")).toInt() != 1) return 0;

    const auto length = static_cast<float>(animationLength());
    QVector<PasteKeyframesCommand::Entry> entries;
    for (const QJsonValue& v : root.value(QStringLiteral("entries")).toArray()) {
        if (auto pe = parsePasteEntry(v.toObject(), atTime, length)) {
            entries.append(*pe);
        }
    }
    if (entries.isEmpty()) return 0;

    // Skip pushing the command if every entry would collide — keeps the
    // undo history clean. The command's own redo() does the authoritative
    // collision check; this is purely a tidiness pre-filter.
    if (countNonColliding(m_selectedSkeleton, m_selectedAnimation, entries) == 0) {
        return 0;
    }

    auto* cmd = new PasteKeyframesCommand(m_selectedEntityName, // NOSONAR — QUndoStack owns
                                           m_selectedAnimation,
                                           entries);
    UndoManager::getSingleton()->push(cmd);
    const int n = cmd->pastedCount();
    const int skipped = static_cast<int>(entries.size()) - n;
    SentryReporter::addBreadcrumb(
        "ui.action",
        QString("Dope Sheet: paste %1 keyframe(s) at t=%2s (skipped %3 collision(s))")
            .arg(n).arg(atTime, 0, 'f', 3).arg(skipped));
    refreshSliderTicks();
    emit boneRowsChanged();
    return n;
}

// ── Curve editor API (slice D3b) ──────────────────────────────────────────────

namespace {

// Resolve channel id → scalar reader on a TransformKeyFrame. Mirrors
// SetKeyframeValueCommand's accessor without pulling that header in.
double readChannel(const Ogre::TransformKeyFrame* kf, const QString& ch) {
    const QString c = ch.toLower();
    if (c == "tx") return kf->getTranslate().x;
    if (c == "ty") return kf->getTranslate().y;
    if (c == "tz") return kf->getTranslate().z;
    if (c == "rw") return kf->getRotation().w;
    if (c == "rx") return kf->getRotation().x;
    if (c == "ry") return kf->getRotation().y;
    if (c == "rz") return kf->getRotation().z;
    if (c == "sx") return kf->getScale().x;
    if (c == "sy") return kf->getScale().y;
    if (c == "sz") return kf->getScale().z;
    return 0.0;
}

bool isKnownChannel(const QString& ch) {
    static const QStringList kKnown = {
        QStringLiteral("tx"), QStringLiteral("ty"), QStringLiteral("tz"),
        QStringLiteral("rw"), QStringLiteral("rx"),
        QStringLiteral("ry"), QStringLiteral("rz"),
        QStringLiteral("sx"), QStringLiteral("sy"), QStringLiteral("sz"),
    };
    return kKnown.contains(ch.toLower());
}

// Symmetric writer to readChannel — writes the requested scalar onto
// the keyframe's TRS without touching the other 9 components.
void writeChannel(Ogre::TransformKeyFrame* kf, const QString& ch, double v) {
    const QString c = ch.toLower();
    const float fv = static_cast<float>(v);
    if (c == "tx") { auto t = kf->getTranslate(); t.x = fv; kf->setTranslate(t); return; }
    if (c == "ty") { auto t = kf->getTranslate(); t.y = fv; kf->setTranslate(t); return; }
    if (c == "tz") { auto t = kf->getTranslate(); t.z = fv; kf->setTranslate(t); return; }
    if (c == "rw") { auto r = kf->getRotation();  r.w = fv; kf->setRotation(r);  return; }
    if (c == "rx") { auto r = kf->getRotation();  r.x = fv; kf->setRotation(r);  return; }
    if (c == "ry") { auto r = kf->getRotation();  r.y = fv; kf->setRotation(r);  return; }
    if (c == "rz") { auto r = kf->getRotation();  r.z = fv; kf->setRotation(r);  return; }
    if (c == "sx") { auto s = kf->getScale();     s.x = fv; kf->setScale(s);     return; }
    if (c == "sy") { auto s = kf->getScale();     s.y = fv; kf->setScale(s);     return; }
    if (c == "sz") { auto s = kf->getScale();     s.z = fv; kf->setScale(s);     return; }
}

} // namespace

QVariantList AnimationControlController::channelValuesAt(
        const QString& boneName, const QString& channel) const
{
    QVariantList out;
    if (boneName.isEmpty() || !isKnownChannel(channel)) return out;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return out;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return out;
    if (!m_selectedSkeleton->hasBone(boneName.toStdString())) return out;

    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    Ogre::Bone* bone = m_selectedSkeleton->getBone(boneName.toStdString());
    if (!anim->hasNodeTrack(bone->getHandle())) return out;
    auto* track = anim->getNodeTrack(bone->getHandle());

    out.reserve(static_cast<int>(track->getNumKeyFrames()));
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const auto* kf = static_cast<const Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        out.append(readChannel(kf, channel));
    }
    return out;
}

bool AnimationControlController::setKeyframeValue(const QString& boneName,
                                                   const QString& channel,
                                                   double time, double value)
{
    if (boneName.isEmpty() || !isKnownChannel(channel)) return false;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return false;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return false;
    if (!m_selectedSkeleton->hasBone(boneName.toStdString())) return false;

    // Pre-check that there's actually a keyframe at `time` — otherwise the
    // command would push a no-op onto the undo stack. Mirrors the same
    // tolerance the command itself uses (1 ms).
    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    Ogre::Bone* bone = m_selectedSkeleton->getBone(boneName.toStdString());
    if (!anim->hasNodeTrack(bone->getHandle())) return false;
    auto* track = anim->getNodeTrack(bone->getHandle());
    bool found = false;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        if (std::fabs(track->getKeyFrame(i)->getTime() - static_cast<float>(time))
                <= 0.001f) {
            found = true; break;
        }
    }
    if (!found) return false;

    auto* cmd = new SetKeyframeValueCommand(m_selectedEntityName, // NOSONAR — QUndoStack owns
                                             m_selectedAnimation,
                                             boneName.toStdString(),
                                             channel.toLower().toStdString(),
                                             static_cast<float>(time),
                                             value);
    UndoManager::getSingleton()->push(cmd);
    refreshSliderTicks();
    emit boneRowsChanged();
    return true;
}

bool AnimationControlController::setKeyframeValuePreview(const QString& boneName,
                                                          const QString& channel,
                                                          double time, double value)
{
    if (boneName.isEmpty() || !isKnownChannel(channel)) return false;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return false;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return false;
    if (!m_selectedSkeleton->hasBone(boneName.toStdString())) return false;
    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    Ogre::Bone* bone = m_selectedSkeleton->getBone(boneName.toStdString());
    if (!anim->hasNodeTrack(bone->getHandle())) return false;
    auto* track = anim->getNodeTrack(bone->getHandle());
    Ogre::TransformKeyFrame* target = nullptr;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - static_cast<float>(time)) <= 0.001f) {
            target = kf; break;
        }
    }
    if (!target) return false;
    writeChannel(target, channel, value);
    notifyOgreUpdate();
    return true;
}

namespace {

// Find the immediate predecessor and successor of `keyTime` in
// `anchorTimes`. anchors is the AUTHORED key list (without resampled
// frames) — passing the dense post-resample list here would expand the
// segment by one synthetic neighbor on each pass and the resampler
// would never close on a stable shape.
bool neighborAnchors(double keyTime,
                     const QVariantList& anchorTimes,
                     double& prevOut, double& nextOut)
{
    constexpr double kEps = 0.001;
    double prev = -1.0, next = -1.0;
    bool havePrev = false, haveNext = false;
    for (const QVariant& v : anchorTimes) {
        const double t = v.toDouble();
        if (t < keyTime - kEps) {
            if (!havePrev || t > prev) { prev = t; havePrev = true; }
        } else if (t > keyTime + kEps) {
            if (!haveNext || t < next) { next = t; haveNext = true; }
        }
    }
    prevOut = prev;
    nextOut = next;
    return havePrev || haveNext;
}

} // namespace

bool AnimationControlController::resampleCurveSegment(const QString& boneName,
                                                      const QString& channel,
                                                      double t0, double t1,
                                                      double toleranceMul,
                                                      int fixedFps)
{
    if (boneName.isEmpty() || !isKnownChannel(channel)) return false;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return false;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return false;
    const std::string boneStd = boneName.toStdString();
    if (!m_selectedSkeleton->hasBone(boneStd)) return false;
    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    Ogre::Bone* bone = m_selectedSkeleton->getBone(boneStd);
    if (!bone || !anim->hasNodeTrack(bone->getHandle())) return false;
    if (t1 <= t0) return false;

    // Pre-check: both endpoints must be near existing keyframes so the
    // command's interior-overwrite math is well defined.
    auto* track = anim->getNodeTrack(bone->getHandle());
    bool foundT0 = false, foundT1 = false;
    constexpr float kEps = 0.001f;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        const float kt = track->getKeyFrame(i)->getTime();
        if (std::fabs(kt - static_cast<float>(t0)) <= kEps) foundT0 = true;
        if (std::fabs(kt - static_cast<float>(t1)) <= kEps) foundT1 = true;
    }
    if (!foundT0 || !foundT1) return false;

    auto* cmd = new ResampleCurveCommand(m_selectedEntityName, // NOSONAR — QUndoStack owns
                                          m_selectedAnimation,
                                          boneStd,
                                          channel.toLower().toStdString(),
                                          static_cast<float>(t0),
                                          static_cast<float>(t1),
                                          toleranceMul,
                                          fixedFps);
    UndoManager::getSingleton()->push(cmd);
    SentryReporter::addBreadcrumb("ui.action", "Resampled curve segment");
    if (!m_suspendRowsRefresh) {
        refreshSliderTicks();
        emit boneRowsChanged();
    }
    return true;
}

void AnimationControlController::notifyExternalAnimationEdit()
{
    // A keyframe insert/remove elsewhere may have reallocated a track's
    // keyframe vector — drop the cached pointers and re-resolve the view.
    m_selectedTrack   = nullptr;
    m_currentKeyframe = nullptr;
    refreshSliderTicks();
    emit boneRowsChanged();
    emit keyframeTicksChanged();
    emit currentKeyframeChanged();
}

QVariantMap AnimationControlController::inbetweenWindow(double t0, double t1,
                                                        int gapFrames,
                                                        bool noModel)
{
    QVariantMap out;
    out["ok"] = false;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()
        || !m_selectedSkeleton->hasAnimation(m_selectedAnimation)) {
        out["error"] = QStringLiteral("No animation selected.");
        emit inbetweenStatus(out["error"].toString(), true);
        return out;
    }
    if (gapFrames < 1) {
        out["error"] = QStringLiteral("Gap frames must be >= 1.");
        emit inbetweenStatus(out["error"].toString(), true);
        return out;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.in_between"),
        QStringLiteral("GUI in-between gap=%1").arg(gapFrames));

    // Resolve / download the model on the GUI thread (it has an event loop).
    QString modelPath;
    if (!noModel)
        modelPath = MotionInbetween::ensureModelBlocking();

    const auto r = AnimationMerger::inbetweenAnimation(
        m_selectedSkeleton, m_selectedAnimation,
        static_cast<float>(t0), static_cast<float>(t1), gapFrames,
        modelPath, noModel);

    out["ok"] = r.ok;
    out["keyframesInserted"] = r.keyframesInserted;
    out["tracksAffected"] = r.tracksAffected;
    out["usedModel"] = r.usedModel;
    out["fallbackReason"] = r.fallbackReason;
    if (!r.ok) {
        out["error"] = r.error;
        emit inbetweenStatus(r.error, true);
        return out;
    }

    // Inserting keyframes is a STRUCTURAL edit: it can reallocate a track's
    // internal keyframe vector, dangling the cached m_selectedTrack /
    // m_currentKeyframe pointers. Drop them before refreshing the view (the
    // dope sheet / slider re-resolve from the skeleton on the next emit).
    m_selectedTrack   = nullptr;
    m_currentKeyframe = nullptr;
    refreshSliderTicks();
    emit boneRowsChanged();
    emit keyframeTicksChanged();
    emit currentKeyframeChanged();

    GamificationManager::noteOperation(
        QStringLiteral("motion_inbetween"),
        {{QStringLiteral("keyframes_inserted"), r.keyframesInserted},
         {QStringLiteral("tracks_affected"), r.tracksAffected}});

    QString msg = QStringLiteral("Inserted %1 keyframes across %2 track(s) via %3")
        .arg(r.keyframesInserted).arg(r.tracksAffected)
        .arg(r.usedModel ? QStringLiteral("RMIB model")
                         : QStringLiteral("spline fallback"));
    if (!r.usedModel && !r.fallbackReason.isEmpty())
        msg += QStringLiteral(" — %1").arg(r.fallbackReason);
    emit inbetweenStatus(msg, false);
    return out;
}

bool AnimationControlController::adjustArmSpace(const QString& animName,
                                                double degrees,
                                                const QString& entityName)
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return false;
    const std::string want = entityName.isEmpty()
        ? m_selectedEntityName : entityName.toStdString();
    Ogre::Entity* entity = nullptr;
    for (auto* e : mgr->getEntities()) {
        if (!e || e->getMovableType() != "Entity" || !e->hasSkeleton()) continue;
        if (want.empty() || e->getName() == want) { entity = e; break; }
    }
    if (!entity) return false;
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    const std::string an = animName.toStdString();
    if (!skel || !skel->hasAnimation(an)) return false;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("GUI arm_space %1 deg").arg(degrees));
    if (!AnimationMerger::adjustArmSpace(skel.get(), an,
                                         static_cast<float>(degrees)))
        return false;

    // Re-pose the mesh NOW, even when the clip is paused. Rewriting keyframes
    // doesn't move the bones until the animation state re-applies; when
    // playback is stopped nothing ticks it, so the viewport would stay frozen
    // on the pre-edit pose until the user hits play. Mark the state dirty and
    // re-stamp its current time to force an immediate re-evaluation (the same
    // idiom as notifyOgreUpdate, but targeting THIS entity + clip so it works
    // for any animation, not just the selected one).
    entity->refreshAvailableAnimationState();
    if (auto* states = entity->getAllAnimationStates()) {
        states->_notifyDirty();
        if (states->hasAnimationState(an)) {
            auto* st = states->getAnimationState(an);
            if (st->getEnabled())
                st->setTimePosition(st->getTimePosition());
        }
    }
    notifyExternalAnimationEdit();
    return true;
}

double AnimationControlController::currentArmSpace(const QString& animName,
                                                   const QString& entityName)
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return 0.0;
    const std::string want = entityName.isEmpty()
        ? m_selectedEntityName : entityName.toStdString();
    for (auto* e : mgr->getEntities()) {
        if (!e || e->getMovableType() != "Entity" || !e->hasSkeleton()) continue;
        if (want.empty() || e->getName() == want) {
            Ogre::SkeletonPtr skel = e->getMesh()->getSkeleton();
            return skel ? AnimationMerger::currentArmSpace(
                              skel.get(), animName.toStdString()) : 0.0;
        }
    }
    return 0.0;
}

QVariantMap AnimationControlController::generateMotion(const QString& prompt,
                                                       double duration, bool useModel,
                                                       double armSpaceDeg, bool footPin)
{
    QVariantMap out;
    out["ok"] = false;
    auto fail = [&](const QString& e) { out["error"] = e; emit generateMotionStatus(e, true); return out; };

    if (prompt.trimmed().isEmpty())
        return fail(QStringLiteral("Enter a motion prompt (e.g. \"walking\")."));

    // Resolve a rigged entity: the selected one, else the first skinned mesh.
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return fail(QStringLiteral("Scene not available."));
    Ogre::Entity* entity = nullptr;
    for (auto* e : mgr->getEntities()) {
        if (!e || e->getMovableType() != "Entity" || !e->hasSkeleton()) continue;
        if (m_selectedEntityName.empty()
            || e->getName() == m_selectedEntityName) { entity = e; break; }
    }
    if (!entity)
        return fail(QStringLiteral("Select a rigged mesh first — text-to-motion needs a skeleton."));
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.text_to_motion"),
        QStringLiteral("GUI generate_motion"));
    GamificationManager::noteFeature(QStringLiteral("animation_blend"));

    // Acquire the canonical clip: EXPERIMENTAL trained model first (when opted in),
    // else the reliable TEMPLATE library — which is also the automatic fallback.
    QString action, clipSource;
    std::vector<std::vector<std::array<float, 4>>> quats;
    int fps = 30;
    bool worldFrame = false;
    std::vector<std::array<float, 4>> cmuRest;
    std::vector<std::array<float, 3>> clipDirs;
    bool gotClip = false;

    if (useModel) {
        const QString mp = MotionGenerator::ensureModelBlocking();
        if (!mp.isEmpty()) {
            const auto mr = MotionGenerator::generate(prompt, mp,
                                                      MotionGenerator::vocabPath(), duration);
            if (mr.ok) {
                action = mr.matchedAction; quats = mr.clip.quats; fps = mr.clip.fps;
                worldFrame = mr.worldFrame; clipSource = QStringLiteral("model"); gotClip = true;
                if (!mr.clip.restWorld.empty() && !mr.clip.restDir.empty()) {
                    // v5 models (#858) ship their canonical reference triple
                    // — same bind-referenced retarget as template clips.
                    cmuRest = mr.clip.restWorld;
                    clipDirs = mr.clip.restDir;
                } else {
                    // Legacy v4: borrow a template clip's reference
                    // directions so the retarget synthesizes a BIND-
                    // referenced base pose (no harvest from the rig's
                    // other animations).
                    clipDirs = MotionLibrary::referenceDirsForPrompt(prompt);
                }
            }
        }
        if (!gotClip)
            emit generateMotionStatus(
                QStringLiteral("Model unavailable for this prompt — using template library."), false);
    }

    if (!gotClip) {
        const QString libPath = MotionLibrary::ensureLibraryBlocking();
        if (libPath.isEmpty())
            return fail(QStringLiteral("Motion library unavailable (offline?)."));
        MotionLibrary lib;
        if (!lib.loadFromFile(libPath))
            return fail(lib.error());
        const int idx = lib.matchPrompt(prompt, &action);
        if (idx < 0) {
            QString known; for (const QString& a : lib.actions()) known += " " + a;
            return fail(QStringLiteral("No motion matched \"%1\". Try:%2").arg(prompt, known));
        }
        const MotionLibrary::Clip& clip = lib.clip(idx);
        quats = clip.quats; fps = clip.fps;
        worldFrame = lib.isWorldFrame();
        // Prefer the clip's own source-bind orientations (bind-referenced
        // retarget); the library-level block is the v2 CMU legacy.
        cmuRest = clip.restWorld.empty() ? lib.cmuRestWorld()
                                         : clip.restWorld;
        clipDirs = clip.restDir;
        clipSource = QStringLiteral("template");
        if (duration > 0.05) {
            const int want = std::max(2, int(duration * clip.fps));
            std::vector<std::vector<std::array<float, 4>>> retimed(want);
            for (int f = 0; f < want; ++f) {
                const float src = (clip.frames - 1) * (float(f) / float(want - 1));
                retimed[f] = quats[std::min(clip.frames - 1, int(src + 0.5f))];
            }
            quats.swap(retimed);
        }
    }

    const std::string animName = ("generated_" + action).toStdString();
    // Auto-rigged (no prior animation) meshes that face −Z would walk
    // backward — detect facing from the mesh's foot region.
    const bool yaw180 = AnimationMerger::detectBackwardFacing(entity);
    const auto res = AnimationMerger::applyMotionClip(skel.get(), animName, quats, fps,
                                                      worldFrame, cmuRest,
                                                      /*refineWithModel=*/false,
                                                      /*refineStride=*/8, yaw180,
                                                      clipDirs,
                                                      clipSource == QStringLiteral("model"));
    if (!res.ok) return fail(res.error);
    out["source"] = clipSource;

    // #837 quality post-pass: sparse-bake temporal low-pass (removes
    // retarget trembling). Before arm-space/foot-pin so pins stay exact.
    AnimationMerger::smoothBakeAnimation(skel.get(), animName, 12, fps);

    // #854: optional Mixamo-style arm-space post-process.
    if (std::abs(armSpaceDeg) > 1e-4)
        AnimationMerger::adjustArmSpace(skel.get(), animName,
                                        static_cast<float>(armSpaceDeg));

    // #856: foot-contact cleanup — ON by default (checkbox opts out).
    if (footPin) {
        const auto fp = AnimationMerger::pinFeet(skel.get(), animName);
        if (fp.ok && fp.spans > 0)
            out["footPinSpans"] = fp.spans;
        else if (!fp.ok && !fp.error.isEmpty())
            out["footPinError"] = fp.error;   // surface why (no leg tracks, etc.)
    }

    entity->refreshAvailableAnimationState();
    // Make the generated clip the ONLY enabled animation. Ogre AVERAGES all
    // enabled animation states, so leaving the import's auto-enabled clip (or
    // previously generated ones) on blends them into a shaking mid-pose —
    // "generate walk" must visibly walk.
    if (auto* animSet = entity->getAllAnimationStates()) {
        for (const auto& [key, state] : animSet->getAnimationStates())
            state->setEnabled(false);
        if (animSet->hasAnimationState(animName)) {
            auto* gen = animSet->getAnimationState(animName);
            gen->setEnabled(true);
            gen->setLoop(true);
            gen->setTimePosition(0.0f);
        }
    }
    // The Inspector's Animations checkboxes read enabled flags through
    // PropertiesPanelController — notify it, or the panel shows STALE state
    // (old clip still checked) and the user's next toggle crosses the
    // enables back into a blended, shaking pose.
    if (auto* ppc = PropertiesPanelController::instance())
        emit ppc->animationStateChanged();
    // Adding an animation can reallocate skeleton state — drop cached pointers
    // and refresh the dope sheet, like the in-between path.
    m_selectedTrack   = nullptr;
    m_currentKeyframe = nullptr;
    updateAnimationTree();
    refreshSliderTicks();
    emit boneRowsChanged();
    emit keyframeTicksChanged();
    emit animationTreeChanged();

    out["ok"] = true;
    out["action"] = action;
    out["source"] = clipSource;
    out["animation"] = QString::fromStdString(animName);
    out["entity"] = QString::fromStdString(entity->getName());
    out["frames"] = res.frames;
    out["length"] = res.length;
    out["tracksWritten"] = res.tracksWritten;
    const QString msg = QStringLiteral("Generated '%1' (%2) — %3 bones, %4 frames (%5s)")
        .arg(action, clipSource).arg(res.tracksWritten).arg(res.frames)
        .arg(res.length, 0, 'f', 1);
    emit generateMotionStatus(msg, false);
    return out;
}

bool AnimationControlController::setCurveHandle(const QString& boneName,
                                                 const QString& channel,
                                                 double keyTime,
                                                 double newInTangent,
                                                 double newOutTangent,
                                                 int newMode)
{
    if (boneName.isEmpty() || !isKnownChannel(channel)) return false;
    if (m_selectedEntityName.empty() || m_selectedAnimation.empty()) return false;

    auto* m = CurveEditModel::instance();
    const QString skel = QString::fromStdString(m_selectedEntityName);
    const QString anim = QString::fromStdString(m_selectedAnimation);
    const QVariantList prev = m->tangentsAt(skel, anim, boneName, channel, keyTime);
    const double oldIn   = prev.size() >= 1 ? prev[0].toDouble() : 0.0;
    const double oldOut  = prev.size() >= 2 ? prev[1].toDouble() : 0.0;
    const int    oldMode = prev.size() >= 3 ? prev[2].toInt()    : 0;
    const int    finalMode = (newMode < 0) ? oldMode : newMode;

    auto* cmd = new CurveEditModelChangeCommand( // NOSONAR — stack owns
            m_selectedEntityName, m_selectedAnimation,
            boneName.toStdString(), channel.toLower().toStdString(),
            keyTime,
            oldIn, oldOut, oldMode,
            newInTangent, newOutTangent, finalMode);
    UndoManager::getSingleton()->push(cmd);
    SentryReporter::addBreadcrumb("ui.action", "Edited curve handle");
    // We don't touch Ogre's per-Animation interp mode here. The
    // Animation::setInterpolationMode setter is animation-wide, not
    // per-track, so flipping it for one bone's curve change visibly
    // distorts every other bone's track in the same animation. The
    // curve editor canvas shows the authored shape; the user clicks
    // Bake to commit the curve into dense TransformKeyFrames when
    // playback needs to match exactly.
    return true;
}

int AnimationControlController::resampleAllSegmentsForBone(const QString& boneName,
                                                            const QString& channel,
                                                            int density)
{
    if (boneName.isEmpty() || !isKnownChannel(channel)) return 0;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return 0;
    const std::string boneStd = boneName.toStdString();
    if (!m_selectedSkeleton->hasBone(boneStd)) return 0;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return 0;

    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    Ogre::Bone* bone = m_selectedSkeleton->getBone(boneStd);
    if (!bone || !anim->hasNodeTrack(bone->getHandle())) return 0;
    auto* track = anim->getNodeTrack(bone->getHandle());
    if (track->getNumKeyFrames() < 2) return 0;

    // Map density level → (toleranceMul, baselineFps, fixedFps).
    // Fixed-FPS modes lock the track to that exact rate. Adaptive
    // modes pre-decimate to a baseline so repeated bakes at the same
    // density CONVERGE to a stable keyframe count (without the
    // pre-decimate, on an already-dense track each anchor pair is
    // already smaller than the simplifier's source rate, and the
    // bake becomes a no-op).
    double toleranceMul   = 1.0;
    int    fixedFps       = 0;  // exact-rate modes
    int    baselineFps    = 0;  // adaptive pre-decimate target
    switch (density) {
        case 6:  fixedFps = 60; break;          // 60 FPS exact
        case 5:  fixedFps = 30; break;          // 30 FPS exact
        case 4:  fixedFps = 15; break;          // 15 FPS exact
        case 3:  fixedFps = 10; break;          // 10 FPS exact
        case 2:  toleranceMul = 1.0;  baselineFps = 30; break;  // Dense
        case 1:  toleranceMul = 4.0;  baselineFps = 15; break;  // Medium
        default: toleranceMul = 12.0; baselineFps = 5;  break;  // Sparse
    }

    std::vector<double> anchors;
    anchors.reserve(track->getNumKeyFrames());
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        anchors.push_back(track->getKeyFrame(i)->getTime());
    }

    auto* stack = UndoManager::getSingleton()->stack();
    stack->beginMacro(QObject::tr("Resample bone curve"));
    // Suspend per-segment QML refresh — the dope sheet + curve editor
    // would otherwise rebuild thousands of times during the macro.
    const bool prevSuspend = m_suspendRowsRefresh;
    m_suspendRowsRefresh = true;

    int count = 0;

    // Fixed-FPS bake: collapse the track to first+last anchor, then
    // resample that single segment at exactly N FPS. Net effect is a
    // single uniform N-FPS grid regardless of starting density —
    // works for source-already-at-N-FPS (re-grids non-uniform
    // spacing) AND source-much-denser-than-N (drops to N) AND
    // source-sparser-than-N (densifies to N).
    if (fixedFps > 0 && anchors.size() >= 2) {
        // Treat the whole clip as a single segment so the per-pair
        // loop's "1/N source duration → 0 new samples" issue
        // disappears. ResampleCurveCommand snapshots kfTimes/kfValues
        // from the live track BEFORE stripping the interior, so the
        // curve evaluator still has the full original data to
        // interpolate from when generating the dense samples — even
        // though the strip+insert phase replaces every interior
        // keyframe with the uniform N-FPS grid.
        const int beforeKeys = static_cast<int>(track->getNumKeyFrames());
        if (resampleCurveSegment(boneName, channel,
                                  anchors.front(), anchors.back(),
                                  1.0, fixedFps)) {
            ++count;
            const int afterKeys = static_cast<int>(track->getNumKeyFrames());
            SentryReporter::addBreadcrumb("ui.action",
                QString("Bake @ %1 FPS: %2 → %3 keys")
                    .arg(fixedFps).arg(beforeKeys).arg(afterKeys));
        }
    } else {
        // Adaptive modes use a coarser baselineFps as a pre-decimate
        // so the simplifier has consistent input regardless of
        // starting density. Repeated Sparse/Medium/Dense bakes
        // converge to stable counts (without pre-decimation, on an
        // already-dense track each anchor pair was already smaller
        // than the simplifier's source sample interval, and the
        // bake silently no-op'd).
        if (baselineFps > 0) {
            reduceTrackToFps(boneName, baselineFps);
            anchors.clear();
            anchors.reserve(track->getNumKeyFrames());
            for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
                anchors.push_back(track->getKeyFrame(i)->getTime());
            }
        }
        for (size_t i = 1; i < anchors.size(); ++i) {
            if (resampleCurveSegment(boneName, channel,
                                      anchors[i-1], anchors[i],
                                      toleranceMul, fixedFps)) {
                ++count;
            }
        }
    }
    m_suspendRowsRefresh = prevSuspend;
    stack->endMacro();
    if (!m_suspendRowsRefresh) {
        refreshSliderTicks();
        emit boneRowsChanged();
    }
    return count;
}

void AnimationControlController::refreshAfterBulkResample()
{
    refreshSliderTicks();
    emit boneRowsChanged();
}

int AnimationControlController::reduceTrackToFps(const QString& boneName,
                                                  int targetFps)
{
    if (boneName.isEmpty() || targetFps <= 0) return 0;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return 0;
    const std::string boneStd = boneName.toStdString();
    if (!m_selectedSkeleton->hasBone(boneStd)) return 0;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return 0;
    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    Ogre::Bone* bone = m_selectedSkeleton->getBone(boneStd);
    if (!bone || !anim->hasNodeTrack(bone->getHandle())) return 0;
    auto* track = anim->getNodeTrack(bone->getHandle());
    const int beforeCount = static_cast<int>(track->getNumKeyFrames());

    auto* cmd = new DecimateTrackCommand( // NOSONAR — QUndoStack owns
        m_selectedEntityName, m_selectedAnimation, boneStd, targetFps);
    UndoManager::getSingleton()->push(cmd);
    SentryReporter::addBreadcrumb("ui.action", "Reduced keyframes to target FPS");
    refreshSliderTicks();
    emit boneRowsChanged();
    const int afterCount = static_cast<int>(track->getNumKeyFrames());
    return std::max(0, beforeCount - afterCount);
}

