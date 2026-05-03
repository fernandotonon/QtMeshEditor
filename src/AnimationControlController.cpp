#include "AnimationControlController.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/MoveKeyframeCommand.h"
#include "commands/BulkKeyframeCommands.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <limits>

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

    m_animationTree.clear();
    for (Ogre::Entity* entity : SelectionSet::getSingleton()->getResolvedEntities()) {
        Ogre::AnimationStateSet* set = entity->getAllAnimationStates();
        if (!set) continue;

        QStringList animNames;
        for (const auto& pair : set->getAnimationStates())
            animNames << QString::fromStdString(pair.first);

        if (animNames.isEmpty()) continue;

        QVariantMap group;
        group["entity"]     = QString::fromStdString(entity->getName());
        group["animations"] = animNames;
        m_animationTree.append(group);
    }
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

void AnimationControlController::refreshBoneList()
{
    m_boneNames.clear();
    m_selectedTrack   = nullptr;
    m_currentKeyframe = nullptr;
    m_selectedBone.clear();

    if (!m_selectedSkeleton || m_selectedAnimation.empty()) {
        emit boneListChanged();
        emit keyframeTicksChanged();
        emit currentKeyframeChanged();
        return;
    }
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) {
        emit boneListChanged();
        emit keyframeTicksChanged();
        emit currentKeyframeChanged();
        return;
    }

    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    for (const auto& pair : anim->_getNodeTrackList())
        m_boneNames << QString::fromStdString(pair.second->getAssociatedNode()->getName());

    emit boneListChanged();

    if (!m_boneNames.isEmpty())
        selectBone(m_boneNames.first());
    else {
        emit keyframeTicksChanged();
        emit currentKeyframeChanged();
    }
}

void AnimationControlController::selectBone(const QString& boneName)
{
    m_selectedTrack   = nullptr;
    m_currentKeyframe = nullptr;
    m_selectedBone    = boneName.toStdString();

    // Clear all bone selection highlights
    if (m_selectedSkeleton) {
        for (unsigned short i = 0; i < m_selectedSkeleton->getNumBones(); ++i)
            m_selectedSkeleton->getBone(i)->getUserObjectBindings()
                .setUserAny("selected", Ogre::Any(false));
    }

    if (m_selectedSkeleton && !m_selectedAnimation.empty()
        && m_selectedSkeleton->hasAnimation(m_selectedAnimation))
    {
        Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
        for (const auto& pair : anim->_getNodeTrackList()) {
            if (pair.second->getAssociatedNode()->getName() == m_selectedBone) {
                m_selectedTrack = pair.second;
                m_selectedSkeleton->getBone(m_selectedBone)
                    ->getUserObjectBindings().setUserAny("selected", Ogre::Any(true));
                break;
            }
        }
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
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return;

    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
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
    if (!m_selectedTrack || !m_selectedEntity || m_selectedAnimation.empty()) return;

    float time = m_sliderValue / 1000.0f;
    Ogre::TransformKeyFrame* newKf = m_selectedTrack->createNodeKeyFrame(time);

    Ogre::TransformKeyFrame interpKf(nullptr, time);
    m_selectedTrack->getInterpolatedKeyFrame(
        m_selectedEntity->getAnimationState(m_selectedAnimation)->getTimePosition(), &interpKf);
    newKf->setTranslate(interpKf.getTranslate());
    newKf->setRotation(interpKf.getRotation());
    newKf->setScale(interpKf.getScale());

    refreshSliderTicks();
    setAnimationFrame(m_sliderValue);
}

void AnimationControlController::deleteKeyframe()
{
    if (!m_selectedTrack || !m_currentKeyframe) return;

    float t = m_currentKeyframe->getTime();
    for (unsigned short i = 0; i < m_selectedTrack->getNumKeyFrames(); ++i) {
        if (std::fabs(m_selectedTrack->getKeyFrame(i)->getTime() - t) < 0.001f) {
            m_selectedTrack->removeKeyFrame(i);
            break;
        }
    }
    m_currentKeyframe = nullptr;
    m_selectedTick    = -1;
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

QVariantList AnimationControlController::allBoneRows() const
{
    QVariantList rows;
    if (!m_selectedSkeleton || m_selectedAnimation.empty()) return rows;
    if (!m_selectedSkeleton->hasAnimation(m_selectedAnimation)) return rows;

    Ogre::Animation* anim = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    for (const auto& [handle, track] : anim->_getNodeTrackList()) {
        Ogre::Node* node = track->getAssociatedNode();
        if (!node) continue;

        QVariantList keyTimes;
        keyTimes.reserve(static_cast<int>(track->getNumKeyFrames()));
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            keyTimes.append(static_cast<double>(track->getKeyFrame(i)->getTime()));
        }

        QVariantMap row;
        row[QStringLiteral("bone")] = QString::fromStdString(node->getName());
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
    auto* cmd = new MoveKeyframeCommand(m_selectedSkeleton, // NOSONAR — QUndoStack owns
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

    auto* cmd = new MoveKeyframesCommand(m_selectedSkeleton, // NOSONAR — QUndoStack owns
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
        const float   tabs = static_cast<float>(m.value(QStringLiteral("time")).toDouble());
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
    // Reject unknown clipboard versions — keeps stale clipboard payloads
    // from a different schema out of the undo stack.
    if (root.value(QStringLiteral("version")).toInt() != 1) return 0;

    const auto length = static_cast<float>(animationLength());
    QVector<PasteKeyframesCommand::Entry> entries;
    for (const QJsonValue& v : root.value(QStringLiteral("entries")).toArray()) {
        const QJsonObject e = v.toObject();
        const QString bone = e.value(QStringLiteral("bone")).toString();
        const double  dt   = e.value(QStringLiteral("dt")).toDouble();
        if (bone.isEmpty()) continue;
        // Require all the numeric fields to be present so we don't silently
        // paste a partial/corrupted entry as identity TRS.
        const QStringList required{ QStringLiteral("tx"), QStringLiteral("ty"),
                                    QStringLiteral("tz"), QStringLiteral("rw"),
                                    QStringLiteral("rx"), QStringLiteral("ry"),
                                    QStringLiteral("rz"), QStringLiteral("sx"),
                                    QStringLiteral("sy"), QStringLiteral("sz") };
        bool ok = true;
        for (const QString& key : required) {
            if (!e.contains(key) || !e.value(key).isDouble()) { ok = false; break; }
        }
        if (!ok) continue;

        const auto dst = static_cast<float>(atTime + dt);
        if (dst < 0.0f || dst > length + kBulkEpsilon) continue; // out of range — skip
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
        entries.append(pe);
    }
    if (entries.isEmpty()) return 0;

    // Pre-check collisions so we don't push a no-op command onto the undo
    // stack when every entry would skip. The command's own redo() repeats
    // the check authoritatively — this is purely to avoid polluting undo.
    int wouldPaste = 0;
    for (const auto& e : entries) {
        auto* track = resolveTrackByBone(m_selectedSkeleton, m_selectedAnimation,
                                          e.boneName);
        if (!track) continue;
        bool collides = false;
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            if (std::fabs(track->getKeyFrame(i)->getTime() - e.time) <= kBulkEpsilon) {
                collides = true; break;
            }
        }
        if (!collides) ++wouldPaste;
    }
    if (wouldPaste == 0) return 0;

    auto* cmd = new PasteKeyframesCommand(m_selectedSkeleton, // NOSONAR — QUndoStack owns
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
