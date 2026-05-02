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
    if (!m_pSingleton) m_pSingleton = new AnimationBlender();
    return m_pSingleton;
}

AnimationBlender* AnimationBlender::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine); Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AnimationBlender::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

AnimationBlender::AnimationBlender()
    : QObject(nullptr)
{
    auto* ctrl = AnimationControlController::instance();
    connect(ctrl, &AnimationControlController::selectionChanged,
            this, &AnimationBlender::refreshFromSelection);
}

QString AnimationBlender::animA() const { return QString::fromStdString(m_animA); }
QString AnimationBlender::animB() const { return QString::fromStdString(m_animB); }

void AnimationBlender::setActive(bool on)
{
    if (on == m_active) return;
    m_active = on;
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
    if (qFuzzyCompare(w + 1.0, m_weight + 1.0)) return; // qFuzzyCompare is iffy near zero
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
    auto* ctrl = AnimationControlController::instance();
    const std::string activeEntity = ctrl->selectedEntityName().toStdString();

    // If the active entity changed, reset selection — clip names from the
    // previous entity may not exist on the new one.
    if (activeEntity != m_activeEntityName) {
        m_activeEntityName = activeEntity;
        if (!m_animA.empty() || !m_animB.empty()) {
            m_animA.clear();
            m_animB.clear();
            emit selectionChanged();
        }
    }

    QStringList anims;
    Ogre::Entity* entity = resolveActiveEntity();
    if (entity) {
        if (auto* set = entity->getAllAnimationStates()) {
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

    // Set blend mode on the skeleton up-front. We don't restore the previous
    // mode on toggle-off — projects that need that can read the saved mode
    // before activating the blender.
    if (auto* skel = entity->getSkeleton()) {
        skel->setBlendMode(m_mode == ModeAdditive
                           ? Ogre::ANIMBLEND_CUMULATIVE
                           : Ogre::ANIMBLEND_AVERAGE);
    }

    if (m_mode == ModeOverride) {
        const bool useB = (m_weight >= 0.5);
        a->setEnabled(!useB);
        b->setEnabled(useB);
        a->setWeight(useB ? 0.0f : 1.0f);
        b->setWeight(useB ? 1.0f : 0.0f);
        (useB ? b : a)->addTime(static_cast<float>(dt));
    } else {
        a->setEnabled(true);
        b->setEnabled(true);
        a->setWeight(static_cast<float>(1.0 - m_weight));
        b->setWeight(static_cast<float>(m_weight));
        a->addTime(static_cast<float>(dt));
        b->addTime(static_cast<float>(dt));
    }
    return true;
}

QString AnimationBlender::bake(const QString& clipName, int fps)
{
    if (clipName.isEmpty()) return {};
    if (fps <= 0) fps = 30;
    if (m_animA.empty() || m_animB.empty()) return {};

    Ogre::Entity* entity = resolveActiveEntity();
    if (!entity) return {};
    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    if (!skel) return {};
    if (!entity->hasAnimationState(m_animA) || !entity->hasAnimationState(m_animB)) {
        return {};
    }

    Ogre::AnimationState* sa = entity->getAnimationState(m_animA);
    Ogre::AnimationState* sb = entity->getAnimationState(m_animB);

    // Bake length = max of the two clips, so neither side gets clipped.
    const float length = std::max(sa->getLength(), sb->getLength());
    if (length <= 0.0f) return {};
    const int   sampleCount = std::max(2, static_cast<int>(std::ceil(length * fps)) + 1);
    const float step        = length / static_cast<float>(sampleCount - 1);

    // Replace any pre-existing clip with the same name.
    const std::string clipStd = clipName.toStdString();
    if (skel->hasAnimation(clipStd)) {
        skel->removeAnimation(clipStd);
    }

    Ogre::Animation* anim = skel->createAnimation(clipStd, length);
    anim->setInterpolationMode(Ogre::Animation::IM_LINEAR);

    const unsigned short numBones = skel->getNumBones();
    // One track per bone, even unanimated bones — keeps bake reproducible
    // and avoids surprises when a downstream system iterates tracks.
    for (unsigned short i = 0; i < numBones; ++i) {
        Ogre::Bone* bone = skel->getBone(i);
        if (!bone) continue;
        // Use the bone's handle as track handle (Ogre convention).
        anim->createNodeTrack(bone->getHandle(), bone);
    }

    // Save the current state so the live preview isn't disturbed by the bake.
    const float saTime = sa->getTimePosition();
    const float sbTime = sb->getTimePosition();
    const bool  saOn   = sa->getEnabled();
    const bool  sbOn   = sb->getEnabled();
    const float saW    = sa->getWeight();
    const float sbW    = sb->getWeight();
    const Ogre::SkeletonAnimationBlendMode prevBlend = skel->getBlendMode();

    // Disable everything else so the sampled pose is purely the blend of A+B.
    if (auto* set = entity->getAllAnimationStates()) {
        for (const auto& [name, state] : set->getAnimationStates()) {
            if (name != m_animA && name != m_animB) state->setEnabled(false);
        }
    }
    skel->setBlendMode(m_mode == ModeAdditive
                       ? Ogre::ANIMBLEND_CUMULATIVE
                       : Ogre::ANIMBLEND_AVERAGE);

    for (int s = 0; s < sampleCount; ++s) {
        const float t = (s == sampleCount - 1) ? length : (s * step);
        // Sample both clips at the same t (looping is naturally handled by
        // Ogre when t > length of either side; we explicitly modulo to keep
        // the bake deterministic regardless of state's loop flag).
        const float ta = (sa->getLength() > 0.0f) ? std::fmod(t, sa->getLength()) : 0.0f;
        const float tb = (sb->getLength() > 0.0f) ? std::fmod(t, sb->getLength()) : 0.0f;
        sa->setTimePosition(ta);
        sb->setTimePosition(tb);

        if (m_mode == ModeOverride) {
            const bool useB = (m_weight >= 0.5);
            sa->setEnabled(!useB);
            sb->setEnabled(useB);
            sa->setWeight(useB ? 0.0f : 1.0f);
            sb->setWeight(useB ? 1.0f : 0.0f);
        } else {
            sa->setEnabled(true);
            sb->setEnabled(true);
            sa->setWeight(static_cast<float>(1.0 - m_weight));
            sb->setWeight(static_cast<float>(m_weight));
        }

        // Force the skeleton to evaluate the blended pose at this sample.
        skel->setAnimationState(*entity->getAllAnimationStates());
        skel->_updateTransforms();

        // Capture each bone's local TRS (relative to its parent's bind pose),
        // which is what TransformKeyFrame stores.
        for (unsigned short i = 0; i < numBones; ++i) {
            Ogre::Bone*  bone  = skel->getBone(i);
            if (!bone) continue;
            auto* track = anim->getNodeTrack(bone->getHandle());
            if (!track) continue;
            auto* kf = track->createNodeKeyFrame(t);
            kf->setTranslate(bone->getPosition() - bone->getInitialPosition());
            kf->setRotation(bone->getInitialOrientation().Inverse() * bone->getOrientation());
            kf->setScale(bone->getScale() / bone->getInitialScale());
        }
    }

    // Restore previous live state.
    sa->setTimePosition(saTime);
    sb->setTimePosition(sbTime);
    sa->setEnabled(saOn);
    sb->setEnabled(sbOn);
    sa->setWeight(saW);
    sb->setWeight(sbW);
    skel->setBlendMode(prevBlend);

    // Make sure the new animation is exposed via the entity's state set.
    entity->refreshAvailableAnimationState();

    refreshFromSelection();
    emit clipBaked(clipName);
    return clipName;
}
