#ifndef ANIMATIONCONTROLCONTROLLER_H
#define ANIMATIONCONTROLCONTROLLER_H

#include <QObject>
#include <QVariantList>
#include <QStringList>
#include <QColor>
#include <QTimer>
#include <QQmlEngine>
#include <string>

namespace Ogre {
    class Entity;
    class NodeAnimationTrack;
    class SkeletonInstance;
    class TransformKeyFrame;
}

class AnimationControlController : public QObject
{
    Q_OBJECT

    // Theme colors (same QPalette derivation as PropertiesPanelController)
    Q_PROPERTY(QColor panelColor     READ panelColor     NOTIFY themeChanged)
    Q_PROPERTY(QColor headerColor    READ headerColor    NOTIFY themeChanged)
    Q_PROPERTY(QColor textColor      READ textColor      NOTIFY themeChanged)
    Q_PROPERTY(QColor borderColor    READ borderColor    NOTIFY themeChanged)
    Q_PROPERTY(QColor inputColor     READ inputColor     NOTIFY themeChanged)
    Q_PROPERTY(QColor highlightColor READ highlightColor NOTIFY themeChanged)
    Q_PROPERTY(QColor buttonColor    READ buttonColor    NOTIFY themeChanged)
    Q_PROPERTY(QColor buttonTextColor READ buttonTextColor NOTIFY themeChanged)
    Q_PROPERTY(QColor disabledTextColor READ disabledTextColor NOTIFY themeChanged)

    // Animation / bone selection
    Q_PROPERTY(QVariantList animationTree   READ animationTree   NOTIFY animationTreeChanged)
    Q_PROPERTY(QString selectedEntityName   READ selectedEntityName   NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedAnimation    READ selectedAnimation    NOTIFY selectionChanged)
    Q_PROPERTY(QStringList boneNames        READ boneNames        NOTIFY boneListChanged)
    Q_PROPERTY(QString selectedBone         READ selectedBone         NOTIFY boneListChanged)

    // Timeline
    Q_PROPERTY(int    sliderValue     READ sliderValue     WRITE setSliderValue     NOTIFY sliderValueChanged)
    Q_PROPERTY(int    sliderMaximum   READ sliderMaximum   NOTIFY animationLengthChanged)
    Q_PROPERTY(double animationLength READ animationLength WRITE setAnimationLength NOTIFY animationLengthChanged)

    // Playback controls (speed multiplier + loop in/out region)
    Q_PROPERTY(double playbackSpeed   READ playbackSpeed   WRITE setPlaybackSpeed   NOTIFY playbackSpeedChanged)
    Q_PROPERTY(double loopStart       READ loopStart       WRITE setLoopStart       NOTIFY loopRegionChanged)
    Q_PROPERTY(double loopEnd         READ loopEnd         WRITE setLoopEnd         NOTIFY loopRegionChanged)
    Q_PROPERTY(bool   loopRegionActive READ loopRegionActive WRITE setLoopRegionActive NOTIFY loopRegionChanged)

    // Keyframe tick marks on the timeline (list of ms positions)
    Q_PROPERTY(QVariantList keyframeTicks READ keyframeTicks NOTIFY keyframeTicksChanged)
    Q_PROPERTY(int selectedTick           READ selectedTick  NOTIFY keyframeTicksChanged)

    // Keyframe editing state
    Q_PROPERTY(bool onKeyframe        READ onKeyframe        NOTIFY currentKeyframeChanged)
    Q_PROPERTY(bool canDeleteKeyframe READ canDeleteKeyframe NOTIFY currentKeyframeChanged)
    Q_PROPERTY(bool hasPrevKeyframe   READ hasPrevKeyframe   NOTIFY keyframeTicksChanged)
    Q_PROPERTY(bool hasNextKeyframe   READ hasNextKeyframe   NOTIFY keyframeTicksChanged)
    Q_PROPERTY(bool hasAnimation      READ hasAnimation      NOTIFY selectionChanged)

    // Keyframe values (T/R/S for the closest keyframe to current time)
    Q_PROPERTY(double kfTransX READ kfTransX NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfTransY READ kfTransY NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfTransZ READ kfTransZ NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfScaleX READ kfScaleX NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfScaleY READ kfScaleY NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfScaleZ READ kfScaleZ NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfRotW   READ kfRotW   NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfRotX   READ kfRotX   NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfRotY   READ kfRotY   NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfRotZ   READ kfRotZ   NOTIFY currentKeyframeChanged)

public:
    static AnimationControlController* instance();
    static AnimationControlController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    // Theme colors
    QColor panelColor()        const;
    QColor headerColor()       const;
    QColor textColor()         const;
    QColor borderColor()       const;
    QColor inputColor()        const;
    QColor highlightColor()    const;
    QColor buttonColor()       const;
    QColor buttonTextColor()   const;
    QColor disabledTextColor() const;

    // Animation tree
    QVariantList animationTree()     const { return m_animationTree; }
    QString      selectedEntityName() const { return QString::fromStdString(m_selectedEntityName); }
    QString      selectedAnimation()  const { return QString::fromStdString(m_selectedAnimation); }

    // Bone list
    QStringList boneNames()   const { return m_boneNames; }
    QString     selectedBone() const { return QString::fromStdString(m_selectedBone); }

    // Timeline
    int    sliderValue()     const { return m_sliderValue; }
    int    sliderMaximum()   const { return m_sliderMaximum; }
    double animationLength() const { return m_sliderMaximum / 1000.0; }
    void   setSliderValue(int ms);
    void   setAnimationLength(double length);

    // Playback speed / loop region
    double playbackSpeed()    const { return m_playbackSpeed; }
    double loopStart()        const { return m_loopStart; }
    double loopEnd()          const { return m_loopEnd; }
    bool   loopRegionActive() const { return m_loopRegionActive; }
    void   setPlaybackSpeed(double s);
    void   setLoopStart(double s);
    void   setLoopEnd(double s);
    void   setLoopRegionActive(bool on);

    // Compute the time after applying speed scaling and (optional) loop wrap.
    // Used by MainWindow::frameRenderingQueued. `currentTime` and `dt` are
    // in seconds; returns the new time position to assign back to the state.
    double advanceTime(double currentTime, double dt) const;

    // Keyframe ticks
    QVariantList keyframeTicks() const { return m_keyframeTicks; }
    int          selectedTick()  const { return m_selectedTick; }

    // Keyframe state
    bool onKeyframe()        const { return m_currentKeyframe != nullptr; }
    bool canDeleteKeyframe() const { return m_currentKeyframe != nullptr; }
    bool hasPrevKeyframe()   const;
    bool hasNextKeyframe()   const;
    bool hasAnimation()      const { return !m_selectedAnimation.empty(); }

    // Keyframe values
    double kfTransX() const { return m_kfTransX; }
    double kfTransY() const { return m_kfTransY; }
    double kfTransZ() const { return m_kfTransZ; }
    double kfScaleX() const { return m_kfScaleX; }
    double kfScaleY() const { return m_kfScaleY; }
    double kfScaleZ() const { return m_kfScaleZ; }
    double kfRotW()   const { return m_kfRotW; }
    double kfRotX()   const { return m_kfRotX; }
    double kfRotY()   const { return m_kfRotY; }
    double kfRotZ()   const { return m_kfRotZ; }

    // QML-invokable actions
    Q_INVOKABLE void selectAnimation(const QString& entityName, const QString& animName);
    Q_INVOKABLE void selectBone(const QString& boneName);
    Q_INVOKABLE void addKeyframe();
    Q_INVOKABLE void deleteKeyframe();
    Q_INVOKABLE void prevKeyframe();
    Q_INVOKABLE void nextKeyframe();
    Q_INVOKABLE void setKfTransX(double v);
    Q_INVOKABLE void setKfTransY(double v);
    Q_INVOKABLE void setKfTransZ(double v);
    Q_INVOKABLE void setKfScaleX(double v);
    Q_INVOKABLE void setKfScaleY(double v);
    Q_INVOKABLE void setKfScaleZ(double v);
    Q_INVOKABLE void setKfRotW(double v);
    Q_INVOKABLE void setKfRotX(double v);
    Q_INVOKABLE void setKfRotY(double v);
    Q_INVOKABLE void setKfRotZ(double v);

public slots:
    void updateAnimationTree();

signals:
    void themeChanged();
    void animationTreeChanged();
    void selectionChanged();
    void boneListChanged();
    void sliderValueChanged();
    void animationLengthChanged();
    void keyframeTicksChanged();
    void currentKeyframeChanged();
    void playbackSpeedChanged();
    void loopRegionChanged();

private:
    AnimationControlController();
    ~AnimationControlController() override = default;

    void setAnimationFrame(int ms);
    void refreshBoneList();
    void refreshSliderTicks();
    void pushKeyframeValues();
    void notifyOgreUpdate();

    static AnimationControlController* m_pSingleton;

    QTimer* m_pollTimer = nullptr;

    Ogre::Entity*             m_selectedEntity   = nullptr;
    Ogre::SkeletonInstance*   m_selectedSkeleton = nullptr;
    Ogre::NodeAnimationTrack* m_selectedTrack    = nullptr;
    Ogre::TransformKeyFrame*  m_currentKeyframe  = nullptr;
    std::string               m_selectedEntityName;
    std::string               m_selectedAnimation;
    std::string               m_selectedBone;

    int    m_sliderValue   = 0;
    int    m_sliderMaximum = 0;
    int    m_selectedTick  = -1;

    QVariantList m_animationTree;
    QStringList  m_boneNames;
    QVariantList m_keyframeTicks;

    bool   m_updatingValues = false;
    double m_kfTransX = 0, m_kfTransY = 0, m_kfTransZ = 0;
    double m_kfScaleX = 1, m_kfScaleY = 1, m_kfScaleZ = 1;
    double m_kfRotW   = 1, m_kfRotX   = 0, m_kfRotY   = 0, m_kfRotZ = 0;

    double m_playbackSpeed    = 1.0;
    double m_loopStart        = 0.0;
    double m_loopEnd          = 0.0;
    bool   m_loopRegionActive = false;
};

#endif // ANIMATIONCONTROLCONTROLLER_H
