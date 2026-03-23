#include "AnimationTimelineController.h"
#include "SelectionSet.h"
#include <Ogre.h>

AnimationTimelineController* AnimationTimelineController::m_pSingleton = nullptr;

AnimationTimelineController* AnimationTimelineController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new AnimationTimelineController();
    return m_pSingleton;
}

AnimationTimelineController* AnimationTimelineController::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AnimationTimelineController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

AnimationTimelineController::AnimationTimelineController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &AnimationTimelineController::onSelectionChanged);
}

Ogre::Entity* AnimationTimelineController::getSelectedAnimatedEntity() const
{
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities)
    {
        if (ent->getAllAnimationStates() && !ent->getAllAnimationStates()->getAnimationStates().empty())
            return ent;
    }
    return nullptr;
}

Ogre::AnimationState* AnimationTimelineController::getCurrentAnimState() const
{
    Ogre::Entity* ent = getSelectedAnimatedEntity();
    if (!ent || mCurrentAnimation.isEmpty()) return nullptr;

    auto* states = ent->getAllAnimationStates();
    if (!states) return nullptr;

    std::string name = mCurrentAnimation.toStdString();
    if (states->hasAnimationState(name))
        return states->getAnimationState(name);
    return nullptr;
}

bool AnimationTimelineController::hasAnimations() const
{
    return getSelectedAnimatedEntity() != nullptr;
}

QStringList AnimationTimelineController::animationNames() const
{
    QStringList names;
    Ogre::Entity* ent = getSelectedAnimatedEntity();
    if (!ent) return names;

    auto* states = ent->getAllAnimationStates();
    if (!states) return names;

    for (const auto& [key, value] : states->getAnimationStates())
        names.append(QString::fromStdString(key));
    return names;
}

QString AnimationTimelineController::currentAnimation() const
{
    return mCurrentAnimation;
}

bool AnimationTimelineController::isPlaying() const
{
    return mPlaying;
}

double AnimationTimelineController::progress() const
{
    auto* state = getCurrentAnimState();
    if (!state) return 0;
    return state->getTimePosition();
}

double AnimationTimelineController::duration() const
{
    auto* state = getCurrentAnimState();
    if (!state) return 0;
    return state->getLength();
}

void AnimationTimelineController::setCurrentAnimation(const QString& name)
{
    if (mCurrentAnimation != name)
    {
        // Disable previous animation
        auto* prevState = getCurrentAnimState();
        if (prevState)
            prevState->setEnabled(false);

        mCurrentAnimation = name;

        // Enable new animation
        auto* newState = getCurrentAnimState();
        if (newState)
        {
            newState->setEnabled(true);
            newState->setLoop(true);
            newState->setTimePosition(0);
        }

        emit currentAnimationChanged();
        emit progressChanged();
    }
}

void AnimationTimelineController::setPlaying(bool playing)
{
    if (mPlaying != playing)
    {
        mPlaying = playing;
        emit playingChanged();
    }
}

void AnimationTimelineController::setProgress(double p)
{
    auto* state = getCurrentAnimState();
    if (state)
    {
        state->setTimePosition(static_cast<Ogre::Real>(p));
        emit progressChanged();
    }
}

void AnimationTimelineController::play()
{
    setPlaying(true);
}

void AnimationTimelineController::pause()
{
    setPlaying(false);
}

void AnimationTimelineController::stop()
{
    setPlaying(false);
    setProgress(0);
}

void AnimationTimelineController::onSelectionChanged()
{
    mCurrentAnimation.clear();

    // Auto-select first animation if available
    auto names = animationNames();
    if (!names.isEmpty())
        mCurrentAnimation = names.first();

    emit animationsChanged();
    emit currentAnimationChanged();
    emit progressChanged();
}

void AnimationTimelineController::updateProgress()
{
    if (mPlaying)
        emit progressChanged();
}
