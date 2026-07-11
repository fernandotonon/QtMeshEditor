#pragma once

#include "commands/LightCommands.h"

#include <QColor>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>

class LightPropertiesController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasLightSelection READ hasLightSelection NOTIFY propertiesChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY propertiesChanged)
    Q_PROPERTY(int lightType READ lightType WRITE setLightType NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedLightType READ mixedLightType NOTIFY propertiesChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedEnabled READ mixedEnabled NOTIFY propertiesChanged)
    Q_PROPERTY(QColor diffuseColor READ diffuseColor WRITE setDiffuseColor NOTIFY propertiesChanged)
    Q_PROPERTY(QColor specularColor READ specularColor WRITE setSpecularColor NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedDiffuseColor READ mixedDiffuseColor NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedSpecularColor READ mixedSpecularColor NOTIFY propertiesChanged)
    Q_PROPERTY(bool colorsLinked READ colorsLinked WRITE setColorsLinked NOTIFY propertiesChanged)
    Q_PROPERTY(double intensity READ intensity WRITE setIntensity NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedIntensity READ mixedIntensity NOTIFY propertiesChanged)
    Q_PROPERTY(double range READ range WRITE setRange NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedRange READ mixedRange NOTIFY propertiesChanged)
    Q_PROPERTY(int attenuationPreset READ attenuationPreset WRITE setAttenuationPreset
                   NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedAttenuationPreset READ mixedAttenuationPreset NOTIFY propertiesChanged)
    Q_PROPERTY(double attenuationConstant READ attenuationConstant WRITE setAttenuationConstant
                   NOTIFY propertiesChanged)
    Q_PROPERTY(double attenuationLinear READ attenuationLinear WRITE setAttenuationLinear
                   NOTIFY propertiesChanged)
    Q_PROPERTY(double attenuationQuadratic READ attenuationQuadratic WRITE setAttenuationQuadratic
                   NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedAttenuationConstant READ mixedAttenuationConstant NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedAttenuationLinear READ mixedAttenuationLinear NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedAttenuationQuadratic READ mixedAttenuationQuadratic NOTIFY propertiesChanged)
    Q_PROPERTY(double spotInnerAngle READ spotInnerAngle WRITE setSpotInnerAngle
                   NOTIFY propertiesChanged)
    Q_PROPERTY(double spotOuterAngle READ spotOuterAngle WRITE setSpotOuterAngle
                   NOTIFY propertiesChanged)
    Q_PROPERTY(double spotFalloff READ spotFalloff WRITE setSpotFalloff NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedSpotInnerAngle READ mixedSpotInnerAngle NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedSpotOuterAngle READ mixedSpotOuterAngle NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedSpotFalloff READ mixedSpotFalloff NOTIFY propertiesChanged)
    Q_PROPERTY(bool isPointOrSpot READ isPointOrSpot NOTIFY propertiesChanged)
    Q_PROPERTY(bool isSpot READ isSpot NOTIFY propertiesChanged)
    Q_PROPERTY(bool isDirectional READ isDirectional NOTIFY propertiesChanged)
    Q_PROPERTY(bool castShadows READ castShadows WRITE setCastShadows NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedCastShadows READ mixedCastShadows NOTIFY propertiesChanged)
    Q_PROPERTY(double shadowDepthBias READ shadowDepthBias WRITE setShadowDepthBias
                   NOTIFY propertiesChanged)
    Q_PROPERTY(double shadowSlopeBias READ shadowSlopeBias WRITE setShadowSlopeBias
                   NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedShadowDepthBias READ mixedShadowDepthBias NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedShadowSlopeBias READ mixedShadowSlopeBias NOTIFY propertiesChanged)
    Q_PROPERTY(QStringList lightTypeChoices READ lightTypeChoices CONSTANT)
    Q_PROPERTY(QStringList attenuationPresetChoices READ attenuationPresetChoices CONSTANT)
    Q_PROPERTY(int linkMode READ linkMode WRITE setLinkMode NOTIFY propertiesChanged)
    Q_PROPERTY(bool mixedLinkMode READ mixedLinkMode NOTIFY propertiesChanged)
    Q_PROPERTY(QStringList linkedEntityNames READ linkedEntityNames NOTIFY propertiesChanged)
    Q_PROPERTY(QStringList linkModeChoices READ linkModeChoices CONSTANT)
    Q_PROPERTY(QStringList availableLinkTargets READ availableLinkTargets NOTIFY propertiesChanged)

public:
    static LightPropertiesController* instance();
    static LightPropertiesController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasLightSelection() const;
    int selectionCount() const;

    int lightType() const;
    void setLightType(int type);
    bool mixedLightType() const;

    bool enabled() const;
    void setEnabled(bool value);
    bool mixedEnabled() const;

    QColor diffuseColor() const;
    void setDiffuseColor(const QColor& color);
    bool mixedDiffuseColor() const;

    QColor specularColor() const;
    void setSpecularColor(const QColor& color);
    bool mixedSpecularColor() const;

    bool colorsLinked() const;
    void setColorsLinked(bool linked);

    double intensity() const;
    void setIntensity(double value);
    bool mixedIntensity() const;

    double range() const;
    void setRange(double value);
    bool mixedRange() const;

    int attenuationPreset() const;
    void setAttenuationPreset(int preset);
    bool mixedAttenuationPreset() const;

    double attenuationConstant() const;
    void setAttenuationConstant(double value);
    bool mixedAttenuationConstant() const;

    double attenuationLinear() const;
    void setAttenuationLinear(double value);
    bool mixedAttenuationLinear() const;

    double attenuationQuadratic() const;
    void setAttenuationQuadratic(double value);
    bool mixedAttenuationQuadratic() const;

    double spotInnerAngle() const;
    void setSpotInnerAngle(double degrees);
    bool mixedSpotInnerAngle() const;

    double spotOuterAngle() const;
    void setSpotOuterAngle(double degrees);
    bool mixedSpotOuterAngle() const;

    double spotFalloff() const;
    void setSpotFalloff(double value);
    bool mixedSpotFalloff() const;

    bool isPointOrSpot() const;
    bool isSpot() const;
    bool isDirectional() const;

    bool castShadows() const;
    void setCastShadows(bool value);
    bool mixedCastShadows() const;

    double shadowDepthBias() const;
    void setShadowDepthBias(double value);
    bool mixedShadowDepthBias() const;

    double shadowSlopeBias() const;
    void setShadowSlopeBias(double value);
    bool mixedShadowSlopeBias() const;

    QStringList lightTypeChoices() const;
    QStringList attenuationPresetChoices() const;

    int linkMode() const;
    void setLinkMode(int mode);
    bool mixedLinkMode() const;
    QStringList linkedEntityNames() const;
    QStringList linkModeChoices() const;
    QStringList availableLinkTargets() const;

    Q_INVOKABLE void addLinkedEntity(const QString& entityName);
    Q_INVOKABLE void removeLinkedEntity(const QString& entityName);

    Q_INVOKABLE void beginSliderEdit(int propertyClass);
    Q_INVOKABLE void endSliderEdit(int propertyClass);
    Q_INVOKABLE void pickDiffuseColor();
    Q_INVOKABLE void pickSpecularColor();
    Q_INVOKABLE void refreshFromSelection();

signals:
    void propertiesChanged();

private:
    explicit LightPropertiesController(QObject* parent = nullptr);

    QList<LightSnapshot> selectedSnapshots() const;
    void applyToSelection(const std::function<void(LightSnapshot&)>& mutator, bool emitChanged);
    void commitEdit(LightPropertyClass propertyClass,
                    const QList<LightSnapshot>& before,
                    const QList<LightSnapshot>& after);
    void pushImmediateEdit(LightPropertyClass propertyClass,
                           const std::function<void(LightSnapshot&)>& mutator);
    void clampSpotAngles(LightSnapshot& snapshot) const;
    int detectAttenuationPreset(const LightSnapshot& snapshot) const;
    void applyAttenuationPreset(LightSnapshot& snapshot, int preset) const;

    static QColor toQColor(const Ogre::ColourValue& colour);
    static Ogre::ColourValue toOgreColour(const QColor& color);

    static LightPropertiesController* s_singleton;

    bool m_colorsLinked = true;
    bool m_sliderEditActive = false;
    LightPropertyClass m_sliderEditClass = LightPropertyClass::Intensity;
    QList<LightSnapshot> m_sliderBefore;
};
