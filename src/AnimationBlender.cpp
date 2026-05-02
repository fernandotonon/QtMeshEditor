#include "AnimationBlender.h"
#include "AnimationControlController.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

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
    auto* ctrl = AnimationControlController::instance();
    connect(ctrl, &AnimationControlController::selectionChanged,
            this, &AnimationBlender::refreshFromSelection);
    // Tell the Animation Control panel about newly baked clips so its
    // animation tree picks them up without requiring a re-select. Inspector
    // is wired separately in PropertiesPanelController.
    connect(this, &AnimationBlender::clipBaked,
            ctrl, &AnimationControlController::updateAnimationTree);
}

QString AnimationBlender::animA() const { return QString::fromStdString(m_animA); }
QString AnimationBlender::animB() const { return QString::fromStdString(m_animB); }

namespace {

// Disable every animation state on `entity`. Pre-condition: caller already
// captured a PreviewSnapshot so deactivate can restore the original flags.
void disableAllStates(Ogre::Entity* entity)
{
    if (!entity) return;
    auto* set = entity->getAllAnimationStates();
    if (!set) return;
    for (const auto& [name, state] : set->getAnimationStates()) {
        state->setEnabled(false);
    }
}

const char* modeName(int mode)
{
    switch (mode) {
    case AnimationBlender::ModeMix:      return "mix";
    case AnimationBlender::ModeAdditive: return "additive";
    case AnimationBlender::ModeOverride: return "override";
    default:                             return "mix";
    }
}

} // namespace

void AnimationBlender::setActive(bool on)
{
    if (on == m_active) return;

    // Refuse activation unless A/B are both set and distinct. apply() bails
    // out for the same reasons, but bailing there alone leaves the entity
    // with all-states-off until the user picks valid clips. Rejecting here
    // keeps the toggle truthful: "Active" only sticks when it's meaningful.
    if (on && (m_animA.empty() || m_animB.empty() || m_animA == m_animB)) {
        return;
    }

    m_active = on;
    if (on) {
        // Snapshot the entity's pre-blend state so deactivation can restore
        // it, then turn off every animation layer immediately. apply() on
        // the next frame re-enables A and B with the right weights;
        // doing it here prevents a one-frame flash where a stale checked
        // animation continues to play before the blend kicks in.
        Ogre::Entity* entity = resolveActiveEntity();
        captureSnapshot(entity);
        disableAllStates(entity);
    } else {
        // Toggling off: roll the entity's animation states back to whatever
        // they looked like before the blender started writing to them.
        restoreSnapshot();
    }
    // Inspector listens to activeChanged and refreshes its animation list,
    // which keeps its per-anim Enable/Loop checkboxes in sync.
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

    // Active entity changed — anything we'd cached or written to the previous
    // entity's states needs to be undone before we point at the new one.
    if (const std::string activeEntity = ctrl->selectedEntityName().toStdString();
        activeEntity != m_activeEntityName) {
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
    if (const Ogre::Entity* entity = resolveActiveEntity()) {
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
    // findEntityByName lives in the anonymous namespace below; forward-declare
    // here to keep the function above the helper without reordering.
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

namespace {

Ogre::Entity* findEntityByName(const std::string& name) {
    for (Ogre::Entity* ent : SelectionSet::getSingleton()->getResolvedEntities()) {
        if (ent && ent->getName() == name) return ent;
    }
    return nullptr;
}

} // namespace

void AnimationBlender::restoreSnapshot()
{
    if (!m_snapshot.valid) return;
    // Resolve by the snapshot's recorded entity name in case the active
    // selection has already moved on.
    Ogre::Entity* entity = findEntityByName(m_snapshot.entityName);
    if (!entity) {
        m_snapshot = {};
        return;
    }
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
    m_snapshot = {};
}

// ── Live blend ────────────────────────────────────────────────────────────────

namespace {

// Apply weights + enabled flags for a single sample of the given mode.
// Used by both live apply() and bake() to keep the math in one place.
void configureBlend(Ogre::AnimationState* a, Ogre::AnimationState* b,
                    int mode, double weight)
{
    if (mode == AnimationBlender::ModeOverride) {
        const bool useB = (weight >= 0.5);
        a->setEnabled(!useB);
        b->setEnabled(useB);
        a->setWeight(useB ? 0.0f : 1.0f);
        b->setWeight(useB ? 1.0f : 0.0f);
    } else {
        a->setEnabled(true);
        b->setEnabled(true);
        a->setWeight(static_cast<float>(1.0 - weight));
        b->setWeight(static_cast<float>(weight));
    }
}

// Disable any layer that isn't A or B so the entity's pose is exactly A+B.
// Pre-blend layer enabled-flags are restored by the snapshot pathway.
void muteOtherLayers(Ogre::Entity* entity,
                     const std::string& nameA, const std::string& nameB)
{
    auto* set = entity->getAllAnimationStates();
    if (!set) return;
    for (const auto& [name, state] : set->getAnimationStates()) {
        if (name != nameA && name != nameB) state->setEnabled(false);
    }
}

// Advance a single state, routing through advanceTime() if it's the selected
// clip (for slice-A loop region) and using speed-scaled addTime() otherwise.
void advanceState(Ogre::AnimationState* s, const std::string& name,
                  const std::string& activeAnim,
                  AnimationControlController* ctrl,
                  double dt, double scaledDt)
{
    if (name == activeAnim) {
        const auto   now  = static_cast<double>(s->getTimePosition());
        const double next = ctrl->advanceTime(now, dt);
        s->setTimePosition(static_cast<float>(next));
    } else {
        s->addTime(static_cast<float>(scaledDt));
    }
}

} // namespace

bool AnimationBlender::apply(Ogre::Entity* entity, double dt) // NOSONAR — mutates state via entity*; not const
{
    if (!m_active || !entity) return false;
    if (m_animA.empty() || m_animB.empty() || m_animA == m_animB) return false;
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
    muteOtherLayers(entity, m_animA, m_animB);
    configureBlend(a, b, m_mode, m_weight);

    auto* ctrl = AnimationControlController::instance();
    const std::string activeAnim = ctrl->selectedAnimation().toStdString();
    const double      scaledDt   = dt * ctrl->playbackSpeed();
    if (m_mode == ModeOverride) {
        const bool useB = (m_weight >= 0.5);
        advanceState(useB ? b : a, useB ? m_animB : m_animA,
                     activeAnim, ctrl, dt, scaledDt);
    } else {
        advanceState(a, m_animA, activeAnim, ctrl, dt, scaledDt);
        advanceState(b, m_animB, activeAnim, ctrl, dt, scaledDt);
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
void writeAllBoneKeyframes(Ogre::Animation* anim, const Ogre::Skeleton* skel, float t)
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

// Capture every state's enabled/weight/time into a flat map keyed by name.
std::unordered_map<std::string, StateRestore> captureAllStates(const Ogre::Entity* entity)
{
    std::unordered_map<std::string, StateRestore> out;
    if (const auto* set = entity->getAllAnimationStates()) {
        for (const auto& [name, state] : set->getAnimationStates()) {
            out[name] = captureState(state);
        }
    }
    return out;
}

// Inverse of captureAllStates — re-applies whatever was recorded earlier so
// the entity's animation states look identical to before bake() ran.
void restoreAllStates(Ogre::Entity* entity,
                      const std::unordered_map<std::string, StateRestore>& saved)
{
    auto* set = entity->getAllAnimationStates();
    if (!set) return;
    for (const auto& [name, state] : set->getAnimationStates()) {
        auto it = saved.find(name);
        if (it != saved.end()) restoreState(state, it->second);
    }
}

void disableNonAB(Ogre::Entity* entity, const std::string& a, const std::string& b)
{
    auto* set = entity->getAllAnimationStates();
    if (!set) return;
    for (const auto& [name, state] : set->getAnimationStates()) {
        if (name != a && name != b) state->setEnabled(false);
    }
}

void createBoneTracks(Ogre::Animation* anim, Ogre::Skeleton* skel)
{
    const unsigned short numBones = skel->getNumBones();
    for (unsigned short i = 0; i < numBones; ++i) {
        if (Ogre::Bone* bone = skel->getBone(i)) {
            anim->createNodeTrack(bone->getHandle(), bone);
        }
    }
}

} // namespace

QString AnimationBlender::bake(const QString& clipName, int fps)
{
    if (clipName.isEmpty()) return {};
    if (fps <= 0) fps = 30;
    if (m_animA.empty() || m_animB.empty() || m_animA == m_animB) return {};

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
    createBoneTracks(anim, skel);

    // Preserve every state's pre-bake config so the live preview is unaffected.
    const auto live      = captureAllStates(entity);
    const auto prevBlend = skel->getBlendMode();

    disableNonAB(entity, m_animA, m_animB);
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

    restoreAllStates(entity, live);
    skel->setBlendMode(prevBlend);

    entity->refreshAvailableAnimationState();

    // Deactivate so the per-frame apply() stops re-imposing blender weights
    // on top of the restored state. Without this, the next render tick would
    // overwrite the pre-bake configuration we just restored. setActive(false)
    // also calls restoreSnapshot(), which is a no-op if Active was off.
    setActive(false);

    refreshFromSelection();
    SentryReporter::addBreadcrumb(
        "ui.action",
        QString("Bake blended animation '%1' (mode=%2, weight=%3, fps=%4, length=%5s, samples=%6)")
            .arg(clipName)
            .arg(modeName(m_mode))
            .arg(m_weight, 0, 'f', 2)
            .arg(fps)
            .arg(length, 0, 'f', 3)
            .arg(sampleCount));
    emit clipBaked(clipName);
    return clipName;
}
