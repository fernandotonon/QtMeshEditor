#ifndef ANIMATIONBLENDER_H
#define ANIMATIONBLENDER_H

#include <QObject>
#include <QStringList>
#include <QQmlEngine>
#include <string>

namespace Ogre {
    class Entity;
    class AnimationState;
}

/**
 * Live two-way animation blender for the entity that is currently selected
 * in the Animation Control panel.
 *
 *   Mix       — both states enabled, weights = (1-w, w)
 *   Additive  — skeleton blend mode set to ANIMBLEND_CUMULATIVE; same weights
 *   Override  — only state B is enabled when w >= 0.5, else state A
 *
 * The blender does not own a frame loop; weights are applied lazily via
 * apply() (called from MainWindow::frameRenderingQueued) so we can reuse the
 * existing render-tick path. When inactive, apply() is a no-op.
 *
 * Bake samples the live blend at a fixed frame rate over [0, max(lengthA,
 * lengthB)] seconds and writes a new Ogre::Animation onto the entity's
 * skeleton. Existing tracks per bone are extended; existing animations with
 * the same name are removed first.
 */
class AnimationBlender : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool        active   READ active   WRITE setActive   NOTIFY activeChanged)
    Q_PROPERTY(QString     animA    READ animA    WRITE setAnimA    NOTIFY selectionChanged)
    Q_PROPERTY(QString     animB    READ animB    WRITE setAnimB    NOTIFY selectionChanged)
    Q_PROPERTY(double      weight   READ weight   WRITE setWeight   NOTIFY weightChanged)
    Q_PROPERTY(int         mode     READ mode     WRITE setMode     NOTIFY modeChanged)
    Q_PROPERTY(QStringList animations READ animations NOTIFY animationsChanged)

public:
    enum Mode { ModeMix = 0, ModeAdditive = 1, ModeOverride = 2 };
    Q_ENUM(Mode)

    static AnimationBlender* instance();
    static AnimationBlender* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool        active()     const { return m_active; }
    QString     animA()      const;
    QString     animB()      const;
    double      weight()     const { return m_weight; }
    int         mode()       const { return static_cast<int>(m_mode); }
    QStringList animations() const { return m_animations; }

    void setActive(bool on);
    void setAnimA(const QString& name);
    void setAnimB(const QString& name);
    void setWeight(double w);
    void setMode(int m);

    /// Apply the current blend to the active entity's animation states.
    /// Called from MainWindow's render tick; safe to call when inactive.
    /// Returns true if the blender consumed the frame (caller should skip
    /// its own per-state advance for the active entity), false otherwise.
    bool apply(Ogre::Entity* entity, double dt);

    /// Bake the live blend into a new Ogre::Animation on the entity's
    /// skeleton. fps controls the sample rate (default 30). Returns the
    /// new clip name on success or an empty string on failure (errorMsg
    /// populated). If a clip with `clipName` already exists it is replaced.
    Q_INVOKABLE QString bake(const QString& clipName, int fps = 30);

public slots:
    /// Repopulate `animations` from the entity currently selected in
    /// AnimationControlController, plus reset blend state if the active
    /// entity changed.
    void refreshFromSelection();

signals:
    void activeChanged();
    void selectionChanged();
    void weightChanged();
    void modeChanged();
    void animationsChanged();
    /// Emitted after a successful bake. `name` is the new clip name.
    void clipBaked(const QString& name);

private:
    AnimationBlender();
    ~AnimationBlender() override = default;

    Ogre::Entity* resolveActiveEntity() const;

    static AnimationBlender* m_pSingleton;

    bool        m_active = false;
    std::string m_animA;
    std::string m_animB;
    double      m_weight = 0.5;
    Mode        m_mode   = ModeMix;
    std::string m_activeEntityName;
    QStringList m_animations;
};

#endif // ANIMATIONBLENDER_H
