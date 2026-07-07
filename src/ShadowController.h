#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QSet>

namespace Ogre
{
class SceneManager;
class Viewport;
}

class OgreWidget;

/// Slice F (#488): global shadow technique + per-viewport enablement.
class ShadowController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int qualityPreset READ qualityPreset WRITE setQualityPreset NOTIFY settingsChanged)
    Q_PROPERTY(int cascadeCount READ cascadeCount WRITE setCascadeCount NOTIFY settingsChanged)
    Q_PROPERTY(double splitLambda READ splitLambda WRITE setSplitLambda NOTIFY settingsChanged)
    Q_PROPERTY(int spotShadowResolution READ spotShadowResolution WRITE setSpotShadowResolution
                   NOTIFY settingsChanged)
    Q_PROPERTY(QStringList qualityPresetNames READ qualityPresetNames CONSTANT)
    Q_PROPERTY(QStringList spotShadowResolutionChoices READ spotShadowResolutionChoices CONSTANT)
    Q_PROPERTY(QStringList cascadeCountChoices READ cascadeCountChoices CONSTANT)

public:
    enum class QualityPreset
    {
        Off = 0,
        Low,
        Medium,
        High
    };
    Q_ENUM(QualityPreset)

    static ShadowController* instance();
    static ShadowController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    int qualityPreset() const { return static_cast<int>(m_qualityPreset); }
    void setQualityPreset(int preset);

    int cascadeCount() const { return m_cascadeCount; }
    void setCascadeCount(int count);

    double splitLambda() const { return m_splitLambda; }
    void setSplitLambda(double lambda);

    int spotShadowResolution() const { return m_spotShadowResolution; }
    void setSpotShadowResolution(int pixels);

    QStringList qualityPresetNames() const;
    QStringList spotShadowResolutionChoices() const;
    QStringList cascadeCountChoices() const;

    bool shadowsActive() const { return m_shadowsActive; }

    void registerViewport(OgreWidget* widget);
    void unregisterViewport(OgreWidget* widget);

    /// Re-evaluate caster lights and install/uninstall the scene shadow technique.
    void syncFromScene();

    /// Default depth/slope bias used when a light has no override.
    static constexpr float kDefaultDepthBias = 0.00005f;
    static constexpr float kDefaultSlopeBias = 1.0f;

signals:
    void settingsChanged();

private:
    explicit ShadowController(QObject* parent = nullptr);
    ~ShadowController() override;

    struct QualityProfile
    {
        int textureSize = 1024;
        int cascades = 3;
        float splitLambda = 0.85f;
        float shadowFarDistance = 50.0f;
    };

    QualityProfile profileForPreset(QualityPreset preset) const;
    void applyPresetDefaults(QualityPreset preset);
    void refreshViewports(bool enabled);
    void installSceneShadows(Ogre::SceneManager* sceneMgr);
    void uninstallSceneShadows(Ogre::SceneManager* sceneMgr);
    bool anyUserLightCastsShadows() const;
    int requiredShadowTextureCount(int directionalCasters) const;

    static ShadowController* s_singleton;

    QualityPreset m_qualityPreset = QualityPreset::Medium;
    int m_cascadeCount = 3;
    double m_splitLambda = 0.85;
    int m_spotShadowResolution = 1024;
    bool m_shadowsActive = false;
    QSet<OgreWidget*> m_viewports;
};
