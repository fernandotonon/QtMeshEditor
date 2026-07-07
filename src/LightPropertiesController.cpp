#include "LightPropertiesController.h"

#include "LightManager.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "ShadowController.h"
#include "UndoManager.h"
#include "SentryReporter.h"
#include "mainwindow.h"

#include <OgreColourValue.h>
#include <OgreLight.h>

#include <QColorDialog>
#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
constexpr double kIntensityMin = 0.0;
constexpr double kIntensityMax = 10.0;
constexpr double kRangeMin = 0.01;
constexpr double kRangeMax = 10000.0;

bool nearlyEqual(double a, double b, double epsilon = 1e-4)
{
    return std::abs(a - b) <= epsilon;
}

bool colourNearlyEqual(const Ogre::ColourValue& a, const Ogre::ColourValue& b)
{
    return nearlyEqual(a.r, b.r) && nearlyEqual(a.g, b.g) && nearlyEqual(a.b, b.b);
}
} // namespace

LightPropertiesController* LightPropertiesController::s_singleton = nullptr;

LightPropertiesController* LightPropertiesController::instance()
{
    if (!s_singleton)
        s_singleton = new LightPropertiesController(); // NOSONAR — singleton
    return s_singleton;
}

LightPropertiesController* LightPropertiesController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

void LightPropertiesController::kill()
{
    delete s_singleton; // NOSONAR — singleton
    s_singleton = nullptr;
}

LightPropertiesController::LightPropertiesController(QObject* parent)
    : QObject(parent)
{
    if (auto* sel = SelectionSet::getSingletonPtr())
        connect(sel, &SelectionSet::selectionChanged, this, &LightPropertiesController::refreshFromSelection);

    if (auto* lights = LightManager::getSingletonPtr())
    {
        connect(lights, &LightManager::lightChanged, this, &LightPropertiesController::refreshFromSelection);
        connect(lights, &LightManager::lightDeleted, this, &LightPropertiesController::refreshFromSelection);
    }
}

QColor LightPropertiesController::toQColor(const Ogre::ColourValue& colour)
{
    return QColor::fromRgbF(colour.r, colour.g, colour.b);
}

Ogre::ColourValue LightPropertiesController::toOgreColour(const QColor& color)
{
    return Ogre::ColourValue(static_cast<float>(color.redF()),
                             static_cast<float>(color.greenF()),
                             static_cast<float>(color.blueF()));
}

QList<LightSnapshot> LightPropertiesController::selectedSnapshots() const
{
    QList<LightSnapshot> snapshots;
    auto* sel = SelectionSet::getSingletonPtr();
    auto* lights = LightManager::getSingletonPtr();
    if (!sel || !lights)
        return snapshots;

    for (Ogre::SceneNode* node : sel->getNodesSelectionList())
    {
        if (!LightManager::sceneNodeIsUserLight(node))
            continue;

        const QString name = QString::fromStdString(node->getName());
        if (const LightHandle* handle = lights->findLight(name))
            snapshots.append(LightSnapshot::fromHandle(*handle));
    }
    return snapshots;
}

void LightPropertiesController::applyToSelection(
    const std::function<void(LightSnapshot&)>& mutator, bool emitChanged)
{
    auto* lights = LightManager::getSingleton();
    for (const LightSnapshot& before : selectedSnapshots())
    {
        LightSnapshot updated = before;
        mutator(updated);
        lights->applyProperties(before.name, updated);
    }
    if (emitChanged)
        emit propertiesChanged();
}

void LightPropertiesController::commitEdit(LightPropertyClass propertyClass,
                                           const QList<LightSnapshot>& before,
                                           const QList<LightSnapshot>& after)
{
    if (before.isEmpty() || before.size() != after.size())
        return;

    bool changed = false;
    for (int i = 0; i < before.size(); ++i)
    {
        if (!(before[i] == after[i]))
        {
            changed = true;
            break;
        }
    }
    if (!changed)
        return;

    UndoManager::getSingleton()->push(
        new EditLightPropertyCommand(propertyClass, before, after));
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  lightPropertyClassLabel(propertyClass));
}

void LightPropertiesController::pushImmediateEdit(
    LightPropertyClass propertyClass, const std::function<void(LightSnapshot&)>& mutator)
{
    const QList<LightSnapshot> before = selectedSnapshots();
    if (before.isEmpty())
        return;

    applyToSelection(mutator, false);

    const QList<LightSnapshot> after = selectedSnapshots();
    commitEdit(propertyClass, before, after);
    emit propertiesChanged();
}

void LightPropertiesController::clampSpotAngles(LightSnapshot& snapshot) const
{
    snapshot.spotlightInnerAngleDeg =
        std::clamp(snapshot.spotlightInnerAngleDeg, 0.0f, 180.0f);
    snapshot.spotlightOuterAngleDeg =
        std::clamp(snapshot.spotlightOuterAngleDeg, snapshot.spotlightInnerAngleDeg, 180.0f);
    snapshot.spotlightFalloff = std::max(snapshot.spotlightFalloff, 0.0f);
}

int LightPropertiesController::detectAttenuationPreset(const LightSnapshot& snapshot) const
{
    if (nearlyEqual(snapshot.attenuationConstant, 1.0)
        && nearlyEqual(snapshot.attenuationLinear, 0.0)
        && nearlyEqual(snapshot.attenuationQuadratic, 1.0))
        return 1; // Realistic

    if (nearlyEqual(snapshot.attenuationConstant, 1.0)
        && nearlyEqual(snapshot.attenuationLinear, 0.05)
        && nearlyEqual(snapshot.attenuationQuadratic, 0.0))
        return 2; // Old-school

    return 0; // Custom
}

void LightPropertiesController::applyAttenuationPreset(LightSnapshot& snapshot, int preset) const
{
    switch (preset)
    {
    case 1:
        snapshot.attenuationConstant = 1.0f;
        snapshot.attenuationLinear = 0.0f;
        snapshot.attenuationQuadratic = 1.0f;
        break;
    case 2:
        snapshot.attenuationConstant = 1.0f;
        snapshot.attenuationLinear = 0.05f;
        snapshot.attenuationQuadratic = 0.0f;
        break;
    default:
        break;
    }
}

bool LightPropertiesController::hasLightSelection() const
{
    return !selectedSnapshots().isEmpty();
}

int LightPropertiesController::selectionCount() const
{
    return selectedSnapshots().size();
}

QStringList LightPropertiesController::lightTypeChoices() const
{
    return {QStringLiteral("Point"), QStringLiteral("Directional"), QStringLiteral("Spot")};
}

QStringList LightPropertiesController::attenuationPresetChoices() const
{
    return {QStringLiteral("Custom"),
            QStringLiteral("Realistic"),
            QStringLiteral("Old-school")};
}

int LightPropertiesController::lightType() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return Ogre::Light::LT_POINT;

    const auto first = snapshots.first().type;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (snapshot.type != first)
            return Ogre::Light::LT_POINT;
    }
    return static_cast<int>(first);
}

bool LightPropertiesController::mixedLightType() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const auto first = snapshots.first().type;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (snapshot.type != first)
            return true;
    }
    return false;
}

void LightPropertiesController::setLightType(int type)
{
    const auto ogreType = static_cast<Ogre::Light::LightTypes>(type);
    pushImmediateEdit(LightPropertyClass::Type, [ogreType](LightSnapshot& snapshot) {
        snapshot.type = ogreType;
        if (ogreType == Ogre::Light::LT_POINT)
            snapshot.usesDirection = false;
        else
            snapshot.usesDirection = true;
    });
}

bool LightPropertiesController::enabled() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    return !snapshots.isEmpty() && snapshots.first().enabled;
}

bool LightPropertiesController::mixedEnabled() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const bool first = snapshots.first().enabled;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (snapshot.enabled != first)
            return true;
    }
    return false;
}

void LightPropertiesController::setEnabled(bool value)
{
    pushImmediateEdit(LightPropertyClass::Enabled,
                      [value](LightSnapshot& snapshot) { snapshot.enabled = value; });
}

QColor LightPropertiesController::diffuseColor() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return Qt::white;
    return toQColor(snapshots.first().diffuse);
}

bool LightPropertiesController::mixedDiffuseColor() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const Ogre::ColourValue first = snapshots.first().diffuse;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!colourNearlyEqual(snapshot.diffuse, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setDiffuseColor(const QColor& color)
{
    const Ogre::ColourValue diffuse = toOgreColour(color);
    pushImmediateEdit(LightPropertyClass::Colour, [this, diffuse](LightSnapshot& snapshot) {
        snapshot.diffuse = diffuse;
        if (m_colorsLinked)
            snapshot.specular = diffuse;
    });
}

QColor LightPropertiesController::specularColor() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return Qt::white;
    return toQColor(snapshots.first().specular);
}

bool LightPropertiesController::mixedSpecularColor() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const Ogre::ColourValue first = snapshots.first().specular;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!colourNearlyEqual(snapshot.specular, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setSpecularColor(const QColor& color)
{
    const Ogre::ColourValue specular = toOgreColour(color);
    pushImmediateEdit(LightPropertyClass::Colour,
                      [specular](LightSnapshot& snapshot) { snapshot.specular = specular; });
}

bool LightPropertiesController::colorsLinked() const
{
    return m_colorsLinked;
}

void LightPropertiesController::setColorsLinked(bool linked)
{
    if (m_colorsLinked == linked)
        return;
    m_colorsLinked = linked;
    emit propertiesChanged();
}

double LightPropertiesController::intensity() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return 1.0;
    return snapshots.first().powerScale;
}

bool LightPropertiesController::mixedIntensity() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().powerScale;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.powerScale, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setIntensity(double value)
{
    const float clamped =
        static_cast<float>(std::clamp(value, kIntensityMin, kIntensityMax));
    if (m_sliderEditActive)
    {
        applyToSelection(
            [clamped](LightSnapshot& snapshot) { snapshot.powerScale = clamped; }, true);
        return;
    }

    pushImmediateEdit(LightPropertyClass::Intensity,
                      [clamped](LightSnapshot& snapshot) { snapshot.powerScale = clamped; });
}

double LightPropertiesController::range() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return 10.0;
    return snapshots.first().attenuationRange;
}

bool LightPropertiesController::mixedRange() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().attenuationRange;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.attenuationRange, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setRange(double value)
{
    const float clamped = static_cast<float>(std::clamp(value, kRangeMin, kRangeMax));
    if (m_sliderEditActive)
    {
        applyToSelection(
            [clamped](LightSnapshot& snapshot) { snapshot.attenuationRange = clamped; }, true);
        return;
    }

    pushImmediateEdit(LightPropertyClass::Range,
                      [clamped](LightSnapshot& snapshot) { snapshot.attenuationRange = clamped; });
}

int LightPropertiesController::attenuationPreset() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return 0;
    return detectAttenuationPreset(snapshots.first());
}

bool LightPropertiesController::mixedAttenuationPreset() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const int first = detectAttenuationPreset(snapshots.first());
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (detectAttenuationPreset(snapshot) != first)
            return true;
    }
    return false;
}

void LightPropertiesController::setAttenuationPreset(int preset)
{
    pushImmediateEdit(LightPropertyClass::Attenuation,
                      [this, preset](LightSnapshot& snapshot) {
                          applyAttenuationPreset(snapshot, preset);
                      });
}

double LightPropertiesController::attenuationConstant() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return 1.0;
    return snapshots.first().attenuationConstant;
}

bool LightPropertiesController::mixedAttenuationConstant() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().attenuationConstant;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.attenuationConstant, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setAttenuationConstant(double value)
{
    const float clamped = static_cast<float>(std::max(value, 0.0));
    pushImmediateEdit(LightPropertyClass::Attenuation, [clamped](LightSnapshot& snapshot) {
        snapshot.attenuationConstant = clamped;
    });
}

double LightPropertiesController::attenuationLinear() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return 0.0;
    return snapshots.first().attenuationLinear;
}

bool LightPropertiesController::mixedAttenuationLinear() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().attenuationLinear;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.attenuationLinear, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setAttenuationLinear(double value)
{
    const float clamped = static_cast<float>(std::max(value, 0.0));
    pushImmediateEdit(LightPropertyClass::Attenuation,
                      [clamped](LightSnapshot& snapshot) { snapshot.attenuationLinear = clamped; });
}

double LightPropertiesController::attenuationQuadratic() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return 0.0;
    return snapshots.first().attenuationQuadratic;
}

bool LightPropertiesController::mixedAttenuationQuadratic() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().attenuationQuadratic;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.attenuationQuadratic, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setAttenuationQuadratic(double value)
{
    const float clamped = static_cast<float>(std::max(value, 0.0));
    pushImmediateEdit(LightPropertyClass::Attenuation, [clamped](LightSnapshot& snapshot) {
        snapshot.attenuationQuadratic = clamped;
    });
}

double LightPropertiesController::spotInnerAngle() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return 30.0;
    return snapshots.first().spotlightInnerAngleDeg;
}

bool LightPropertiesController::mixedSpotInnerAngle() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().spotlightInnerAngleDeg;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.spotlightInnerAngleDeg, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setSpotInnerAngle(double degrees)
{
    const float clamped = static_cast<float>(std::clamp(degrees, 0.0, 180.0));
    if (m_sliderEditActive)
    {
        applyToSelection(
            [this, clamped](LightSnapshot& snapshot) {
                snapshot.spotlightInnerAngleDeg = clamped;
                clampSpotAngles(snapshot);
            },
            true);
        return;
    }

    pushImmediateEdit(LightPropertyClass::SpotCone, [this, clamped](LightSnapshot& snapshot) {
        snapshot.spotlightInnerAngleDeg = clamped;
        clampSpotAngles(snapshot);
    });
}

double LightPropertiesController::spotOuterAngle() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return 40.0;
    return snapshots.first().spotlightOuterAngleDeg;
}

bool LightPropertiesController::mixedSpotOuterAngle() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().spotlightOuterAngleDeg;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.spotlightOuterAngleDeg, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setSpotOuterAngle(double degrees)
{
    const float clamped = static_cast<float>(std::clamp(degrees, 0.0, 180.0));
    if (m_sliderEditActive)
    {
        applyToSelection(
            [this, clamped](LightSnapshot& snapshot) {
                snapshot.spotlightOuterAngleDeg = clamped;
                clampSpotAngles(snapshot);
            },
            true);
        return;
    }

    pushImmediateEdit(LightPropertyClass::SpotCone, [this, clamped](LightSnapshot& snapshot) {
        snapshot.spotlightOuterAngleDeg = clamped;
        clampSpotAngles(snapshot);
    });
}

double LightPropertiesController::spotFalloff() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return 1.0;
    return snapshots.first().spotlightFalloff;
}

bool LightPropertiesController::mixedSpotFalloff() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().spotlightFalloff;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.spotlightFalloff, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setSpotFalloff(double value)
{
    const float clamped = static_cast<float>(std::max(value, 0.0));
    if (m_sliderEditActive)
    {
        applyToSelection(
            [clamped](LightSnapshot& snapshot) { snapshot.spotlightFalloff = clamped; }, true);
        return;
    }

    pushImmediateEdit(LightPropertyClass::SpotCone,
                      [clamped](LightSnapshot& snapshot) { snapshot.spotlightFalloff = clamped; });
}

bool LightPropertiesController::isPointOrSpot() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (snapshot.type == Ogre::Light::LT_POINT || snapshot.type == Ogre::Light::LT_SPOTLIGHT)
            return true;
    }
    return false;
}

bool LightPropertiesController::isSpot() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (snapshot.type == Ogre::Light::LT_SPOTLIGHT)
            return true;
    }
    return false;
}

bool LightPropertiesController::isDirectional() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (snapshot.type == Ogre::Light::LT_DIRECTIONAL)
            return true;
    }
    return false;
}

bool LightPropertiesController::castShadows() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return false;
    return snapshots.first().castShadows;
}

bool LightPropertiesController::mixedCastShadows() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const bool first = snapshots.first().castShadows;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (snapshot.castShadows != first)
            return true;
    }
    return false;
}

void LightPropertiesController::setCastShadows(bool value)
{
    pushImmediateEdit(LightPropertyClass::Shadow, [value](LightSnapshot& snapshot) {
        snapshot.castShadows = value;
    });
    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.shadow_toggle"),
                                  value ? QStringLiteral("on") : QStringLiteral("off"));
}

double LightPropertiesController::shadowDepthBias() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return ShadowController::kDefaultDepthBias;
    return snapshots.first().shadowDepthBias;
}

bool LightPropertiesController::mixedShadowDepthBias() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().shadowDepthBias;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.shadowDepthBias, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setShadowDepthBias(double value)
{
    const float clamped = static_cast<float>(std::max(value, 0.0));
    if (m_sliderEditActive)
    {
        applyToSelection(
            [clamped](LightSnapshot& snapshot) { snapshot.shadowDepthBias = clamped; }, true);
        return;
    }

    pushImmediateEdit(LightPropertyClass::Shadow,
                      [clamped](LightSnapshot& snapshot) { snapshot.shadowDepthBias = clamped; });
}

double LightPropertiesController::shadowSlopeBias() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.isEmpty())
        return ShadowController::kDefaultSlopeBias;
    return snapshots.first().shadowSlopeBias;
}

bool LightPropertiesController::mixedShadowSlopeBias() const
{
    const QList<LightSnapshot> snapshots = selectedSnapshots();
    if (snapshots.size() < 2)
        return false;

    const float first = snapshots.first().shadowSlopeBias;
    for (const LightSnapshot& snapshot : snapshots)
    {
        if (!nearlyEqual(snapshot.shadowSlopeBias, first))
            return true;
    }
    return false;
}

void LightPropertiesController::setShadowSlopeBias(double value)
{
    const float clamped = static_cast<float>(std::max(value, 0.0));
    if (m_sliderEditActive)
    {
        applyToSelection(
            [clamped](LightSnapshot& snapshot) { snapshot.shadowSlopeBias = clamped; }, true);
        return;
    }

    pushImmediateEdit(LightPropertyClass::Shadow,
                      [clamped](LightSnapshot& snapshot) { snapshot.shadowSlopeBias = clamped; });
}

void LightPropertiesController::beginSliderEdit(int propertyClass)
{
    if (m_sliderEditActive)
        endSliderEdit(static_cast<int>(m_sliderEditClass));

    m_sliderEditActive = true;
    m_sliderEditClass = static_cast<LightPropertyClass>(propertyClass);
    m_sliderBefore = selectedSnapshots();
}

void LightPropertiesController::endSliderEdit(int propertyClass)
{
    Q_UNUSED(propertyClass);
    if (!m_sliderEditActive)
        return;

    m_sliderEditActive = false;
    const QList<LightSnapshot> after = selectedSnapshots();
    commitEdit(m_sliderEditClass, m_sliderBefore, after);
    m_sliderBefore.clear();
}

void LightPropertiesController::pickDiffuseColor()
{
    QWidget* parent = Manager::getSingletonPtr() ? Manager::getSingleton()->getMainWindow()
                                                   : nullptr;
    const QColor picked = QColorDialog::getColor(
        diffuseColor(), parent, QObject::tr("Diffuse colour"), QColorDialog::DontUseNativeDialog);
    if (!picked.isValid())
        return;
    setDiffuseColor(picked);
}

void LightPropertiesController::pickSpecularColor()
{
    QWidget* parent = Manager::getSingletonPtr() ? Manager::getSingleton()->getMainWindow()
                                                   : nullptr;
    const QColor picked = QColorDialog::getColor(
        specularColor(), parent, QObject::tr("Specular colour"), QColorDialog::DontUseNativeDialog);
    if (!picked.isValid())
        return;
    setSpecularColor(picked);
}

void LightPropertiesController::refreshFromSelection()
{
    emit propertiesChanged();
}
