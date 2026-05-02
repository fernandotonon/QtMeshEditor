#include "AnimationBlender.h"
#include "AnimationControlController.h"
#include "SelectionSet.h"

#include <Ogre.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreKeyFrame.h>

#include <algorithm>
#include <cmath>

AnimationBlender* AnimationBlender::m_pSingleton = nullptr;

AnimationBlender* AnimationBlender::instance()
{
    if (!m_pSingleton) m_pSingleton = new AnimationBlender(); // NOSONAR — see kill()
    return m_pSingleton;
}

AnimationBlender* AnimationBlender::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AnimationBlender::kill()
{
    delete m_pSingleton; // NOSONAR — manual lifetime mirrors AnimationControlController
    m_pSingleton = nullptr;
}

AnimationBlender::AnimationBlender()
    : QObject(nullptr)
{
    const auto* ctrl = AnimationControlController::instance();
    connect(ctrl, &AnimationControlController::selectionChanged,
            this, &AnimationBlender::refreshFromSelection);
}

QString AnimationBlender::animA() const { return QString::fromStdString(m_animA); }
QString AnimationBlender::animB() const { return QString::fromStdString(m_animB); }

void AnimationBlender::setActive(bool on)
{
    if (on == m_active) return;
    m_active = on;
    if (!on) {
        // Toggling off: roll the entity's animation states back to whatever
        // they looked like before the blender started writing to them.
        restoreSnapshot();
    } else {
        captureSnapshot(resolveActiveEntity());
    }
    emit activeChanged();
}

void AnimationBlender::setAnimA(const QString& name)
{
    const std::string s = name.toStdString();
    if (s == m_animA) return;
    m_animA = s;
    emit selectionChanged();
}

void AnimationBlender::setAnimB(const QString& name)
{
    const std::string s = name.toStdString();
    if (s == m_animB) return;
    m_animB = s;
    emit selectionChanged();
}

void AnimationBlender::setWeight(double w)
{
    if (w < 0.0) w = 0.0;
    if (w > 1.0) w = 1.0;
    if (qFuzzyCompare(w + 1.0, m_weight + 1.0)) return;
    m_weight = w;
    emit weightChanged();
}

void AnimationBlender::setMode(int m)
{
    Mode newMode = ModeMix;
    if (m == ModeMix || m == ModeAdditive || m == ModeOverride) {
        newMode = static_cast<Mode>(m);
    }
    if (newMode == m_mode) return;
    m_mode = newMode;
    emit modeChanged();
}

void AnimationBlender::refreshFromSelection()
{
    const auto* ctrl = AnimationControlController::instance();
    const std::string activeEntity = ctrl->selectedEntityName().toStdString();

    // Active entity changed — anything we'd cached or written to the previous
    // entity's states needs to be undone before we point at the new one.
    if (activeEntity != m_activeEntityName) {
        if (m_active) restoreSnapshot();
        m_activeEntityName = activeEntity;
        if (!m_animA.empty() || !m_animB.empty()) {
            m_animA.clear();
            m_animB.clear();
            emit selectionChanged();
        }
        if (m_active) captureSnapshot(resolveActiveEntity());
    }

    QStringList anims;
    if (Ogre::Entity* entity = resolveActiveEntity()) {
        if (const auto* set = entity->getAllAnimationStates()) {
            for (const auto& [name, state] : set->getAnimationStates()) {
                anims << QString::fromStdString(name);
            }
        }
    }
    if (anims != m_animations) {
        m_animations = anims;
        emit animationsChanged();
    }
}

Ogre::Entity* AnimationBlender::resolveActiveEntity() const
{
    if (m_activeEntityName.empty()) return nullptr;
    for (Ogre::Entity* ent : SelectionSet::getSingleton()->getResolvedEntities()) {
        if (ent && ent->getName() == m_activeEntityName) return ent;
    }
    return nullptr;
}

// ── Snapshot helpers ──────────────────────────────────────────────────────────

void AnimationBlender::captureSnapshot(Ogre::Entity* entity)
{
    m_snapshot = {};
    if (!entity) return;
    m_snapshot.entityName = entity->getName();
    if (const auto* set = entity->getAllAnimationStates()) {
        for (const auto& [name, state] : set->getAnimationStates()) {
            StateSnapshot s;
            s.enabled = state->getEnabled();
            s.weight  = state->getWeight();
            m_snapshot.states[name] = s;
        }
    }
    if (auto* skel = entity->getSkeleton()) {
        m_snapshot.blendMode = skel->getBlendMode();
    }
    m_snapshot.valid = true;
}

void AnimationBlender::restoreSnapshot()
{
    if (!m_snapshot.valid) return;
    // Resolve by the snapshot's recorded entity name in case the active
    // selection has already moved on.
    Ogre::Entity* entity = nullptr;
    for (Ogre::Entity* ent : SelectionSet::getSingleton()->getResolvedEntities()) {
        if (ent && ent->getName() == m_snapshot.entityName) { entity = ent; break; }
    }
    if (entity) {
        if (auto* set = entity->getAllAnimationStates()) {
            for (const auto& [name, state] : set->getAnimationStates()) {
                auto it = m_snapshot.states.find(name);
                if (it == m_snapshot.states.end()) continue;
                state->setEnabled(it->second.enabled);
                state->setWeight(it->second.weight);
            }
        }
        if (auto* skel = entity->getSkeleton()) {
            skel->setBlendMode(m_snapshot.blendMode);
        }
    }
    m_snapshot = {};
}

// ── Live blend ────────────────────────────────────────────────────────────────

bool AnimationBlender::apply(Ogre::Entity* entity, double dt)
{
    if (!m_active || !entity) return false;
    if (m_animA.empty() || m_animB.empty()) return false;
    if (!entity->hasAnimationState(m_animA) || !entity->hasAnimationState(m_animB)) {
        return false;
    }

    Ogre::AnimationState* a = entity->getAnimationState(m_animA);
    Ogre::AnimationState* b = entity->getAnimationState(m_animB);
    if (!a || !b) return false;

    if (auto* skel = entity->getSkeleton()) {
        skel->setBlendMode(m_mode == ModeAdditive
                           ? Ogre::ANIMBLEND_CUMULATIVE
                           : Ogre::ANIMBLEND_AVERAGE);
    }

    // Disable any non-A/B states so the blended pose is purely A+B. Snapshot
    // taken in setActive() will reinstate them when the blender is toggled
    // off, so this is a non-destructive override.
    if (auto* set = entity->getAllAnimationStates()) {
        for (const auto& [name, state] : set->getAnimationStates()) {
            if (name != m_animA && name != m_animB) state->setEnabled(false);
        }
    }

    // Use the controller's advance helper so the slice-A loop region still
    // wraps the *selected* clip (which is what the user is scrubbing).
    // `dt` here is the raw frame delta from MainWindow; advanceTime() applies
    // playbackSpeed itself, while the non-active clip uses scaled dt directly.
    auto* ctrl = AnimationControlController::instance();
    const std::string activeAnim = ctrl->selectedAnimation().toStdString();
    const double scaledDt = dt * ctrl->playbackSpeed();
    auto advance = [&](Ogre::AnimationState* s, const std::string& name) {
        if (name == activeAnim) {
            const double now  = static_cast<double>(s->getTimePosition());
            const double next = ctrl->advanceTime(now, dt);
            s->setTimePosition(static_cast<float>(next));
        } else {
            s->addTime(static_cast<float>(scaledDt));
        }
    };

    if (m_mode == ModeOverride) {
        const bool useB = (m_weight >= 0.5);
        a->setEnabled(!useB);
        b->setEnabled(useB);
        a->setWeight(useB ? 0.0f : 1.0f);
        b->setWeight(useB ? 1.0f : 0.0f);
        advance(useB ? b : a, useB ? m_animB : m_animA);
    } else {
        a->setEnabled(true);
        b->setEnabled(true);
        a->setWeight(static_cast<float>(1.0 - m_weight));
        b->setWeight(static_cast<float>(m_weight));
        advance(a, m_animA);
        advance(b, m_animB);
    }
    return true;
}

// ── Bake ──────────────────────────────────────────────────────────────────────

namespace {

// Per-bake snapshot of one A or B state. Smaller than PreviewSnapshot since
// bake() also restores the per-state-set's other layers via the entity-level
// snapshot from setActive().
struct StateRestore {
    float time   = 0.0f;
    bool  on     = false;
    float weight = 0.0f;
};

StateRestore captureState(const Ogre::AnimationState* s) {
    return { s->getTimePosition(), s->getEnabled(), s->getWeight() };
}

void restoreState(Ogre::AnimationState* s, const StateRestore& r) {
    s->setTimePosition(r.time);
    s->setEnabled(r.on);
    s->setWeight(r.weight);
}

void writeBoneKeyframe(Ogre::NodeAnimationTrack* track, float t, const Ogre::Bone* bone) {
    auto* kf = track->createNodeKeyFrame(t);
    kf->setTranslate(bone->getPosition() - bone->getInitialPosition());
    kf->setRotation(bone->getInitialOrientation().Inverse() * bone->getOrientation());
    kf->setScale(bone->getScale() / bone->getInitialScale());
}

// Set up sa/sb state for a single bake sample at time `t` according to
// `mode` and `weight`. Wraps t around each state's own clip length so a
// shorter clip loops within the bake range.
void positionForSample(Ogre::AnimationState* sa, Ogre::AnimationState* sb,
                       float t, int mode, double weight)
{
    const float ta = (sa->getLength() > 0.0f) ? std::fmod(t, sa->getLength()) : 0.0f;
    const float tb = (sb->getLength() > 0.0f) ? std::fmod(t, sb->getLength()) : 0.0f;
    sa->setTimePosition(ta);
    sb->setTimePosition(tb);

    if (mode == AnimationBlender::ModeOverride) {
        const bool useB = (weight >= 0.5);
        sa->setEnabled(!useB);
        sb->setEnabled(useB);
        sa->setWeight(useB ? 0.0f : 1.0f);
        sb->setWeight(useB ? 1.0f : 0.0f);
    } else {
        sa->setEnabled(true);
        sb->setEnabled(true);
        sa->setWeight(static_cast<float>(1.0 - weight));
        sb->setWeight(static_cast<float>(weight));
    }
}

// Sample every bone's local TRS at the current skeleton state and emit a
// keyframe at time `t` on each node track owned by `anim`.
void writeAllBoneKeyframes(Ogre::Animation* anim, Ogre::Skeleton* skel, float t)
{
    const unsigned short numBones = skel->getNumBones();
    for (unsigned short i = 0; i < numBones; ++i) {
        Ogre::Bone* bone = skel->getBone(i);
        if (!bone) continue;
        if (auto* track = anim->getNodeTrack(bone->getHandle())) {
            writeBoneKeyframe(track, t, bone);
        }
    }
}

} // namespace

QString AnimationBlender::bake(const QString& clipName, int fps)
{
    if (clipName.isEmpty()) return {};
    if (fps <= 0) fps = 30;
    if (m_animA.empty() || m_animB.empty()) return {};

    const std::string clipStd = clipName.toStdString();
    // Refuse to bake over one of the source clips — sa/sb resolve to those
    // states, and removeAnimation() would invalidate them mid-bake.
    if (clipStd == m_animA || clipStd == m_animB) return {};

    Ogre::Entity* entity = resolveActiveEntity();
    if (!entity) return {};
    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    if (!skel) return {};
    if (!entity->hasAnimationState(m_animA) || !entity->hasAnimationState(m_animB)) {
        return {};
    }

    Ogre::AnimationState* sa = entity->getAnimationState(m_animA);
    Ogre::AnimationState* sb = entity->getAnimationState(m_animB);
    const float length = std::max(sa->getLength(), sb->getLength());
    if (length <= 0.0f) return {};
    const int   sampleCount = std::max(2,
        static_cast<int>(std::ceil(length * static_cast<float>(fps))) + 1);
    const float step        = length / static_cast<float>(sampleCount - 1);

    if (skel->hasAnimation(clipStd)) skel->removeAnimation(clipStd);
    Ogre::Animation* anim = skel->createAnimation(clipStd, length);
    anim->setInterpolationMode(Ogre::Animation::IM_LINEAR);

    // Track per bone — keeps the bake reproducible even for unanimated bones.
    const unsigned short numBones = skel->getNumBones();
    for (unsigned short i = 0; i < numBones; ++i) {
        if (Ogre::Bone* bone = skel->getBone(i)) {
            anim->createNodeTrack(bone->getHandle(), bone);
        }
    }

    // Preserve every state's pre-bake config — A, B, and any third-party
    // layers the entity had enabled — so the live preview is unaffected.
    std::unordered_map<std::string, StateRestore> live;
    if (auto* set = entity->getAllAnimationStates()) {
        for (const auto& [name, state] : set->getAnimationStates()) {
            live[name] = captureState(state);
        }
    }
    const Ogre::SkeletonAnimationBlendMode prevBlend = skel->getBlendMode();

    // Disable everything except A and B so each sampled pose is purely A+B.
    if (auto* set = entity->getAllAnimationStates()) {
        for (const auto& [name, state] : set->getAnimationStates()) {
            if (name != m_animA && name != m_animB) state->setEnabled(false);
        }
    }
    skel->setBlendMode(m_mode == ModeAdditive
                       ? Ogre::ANIMBLEND_CUMULATIVE
                       : Ogre::ANIMBLEND_AVERAGE);

    for (int s = 0; s < sampleCount; ++s) {
        const float t = (s == sampleCount - 1) ? length : (static_cast<float>(s) * step);
        positionForSample(sa, sb, t, m_mode, m_weight);
        skel->setAnimationState(*entity->getAllAnimationStates());
        skel->_updateTransforms();
        writeAllBoneKeyframes(anim, skel, t);
    }

    // Restore live state for every layer we touched.
    if (auto* set = entity->getAllAnimationStates()) {
        for (const auto& [name, state] : set->getAnimationStates()) {
            auto it = live.find(name);
            if (it != live.end()) restoreState(state, it->second);
        }
    }
    skel->setBlendMode(prevBlend);

    entity->refreshAvailableAnimationState();
    refreshFromSelection();
    emit clipBaked(clipName);
    return clipName;
}
