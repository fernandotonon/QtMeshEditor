#ifndef ANIMATION_TIMELINE_CONTROLLER_H
#define ANIMATION_TIMELINE_CONTROLLER_H

#include <QObject>
#include <QStringList>
#include <QQmlEngine>

namespace Ogre {
    class Entity;
    class AnimationState;
}

class AnimationTimelineController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasAnimations READ hasAnimations NOTIFY animationsChanged)
    Q_PROPERTY(QStringList animationNames READ animationNames NOTIFY animationsChanged)
    Q_PROPERTY(QString currentAnimation READ currentAnimation WRITE setCurrentAnimation NOTIFY currentAnimationChanged)
    Q_PROPERTY(bool playing READ isPlaying WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(double progress READ progress WRITE setProgress NOTIFY progressChanged)
    Q_PROPERTY(double duration READ duration NOTIFY currentAnimationChanged)

public:
    static AnimationTimelineController* instance();
    static AnimationTimelineController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasAnimations() const;
    QStringList animationNames() const;
    QString currentAnimation() const;
    bool isPlaying() const;
    double progress() const;
    double duration() const;

    void setCurrentAnimation(const QString& name);
    void setPlaying(bool playing);
    void setProgress(double p);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();

public slots:
    void onSelectionChanged();
    void updateProgress();

signals:
    void animationsChanged();
    void currentAnimationChanged();
    void playingChanged();
    void progressChanged();

private:
    AnimationTimelineController();
    ~AnimationTimelineController() override = default;

    Ogre::Entity* getSelectedAnimatedEntity() const;
    Ogre::AnimationState* getCurrentAnimState() const;

    static AnimationTimelineController* m_pSingleton;
    QString mCurrentAnimation;
    bool mPlaying = false;
};

#endif // ANIMATION_TIMELINE_CONTROLLER_H
