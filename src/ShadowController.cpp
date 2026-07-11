#include "ShadowController.h"

#include "AppSettingsKeys.h"
#include "LightManager.h"
#include "Manager.h"
#include "OgreWidget.h"
#include "SentryReporter.h"

#include <OgreCamera.h>
#include <OgreLight.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreSceneManager.h>
#include <OgreShadowCameraSetupPSSM.h>
#include <OgreTextureManager.h>
#include <OgreViewport.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <vector>
#include <algorithm>
#include <cmath>

namespace
{

bool hasActiveRenderContext()
{
    auto* root = Ogre::Root::getSingletonPtr();
    if (!root || !root->getRenderSystem())
        return false;

    try
    {
        if (root->getRenderTarget("TestHidden") || root->getRenderTarget("CLIHidden"))
            return true;
    }
    catch (...)
    {
    }

    // Root + render system is enough: editor viewports use arbitrary render-window names.
    return true;
}

QString resolveBundledMainMediaPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << appDir + QStringLiteral("/media/Main")
               << appDir + QStringLiteral("/../media/Main");
#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE
    candidates << appDir + QStringLiteral("/../../media/Main")
               << appDir + QStringLiteral("/../../../media/Main");
#endif
    for (const QString& path : candidates)
    {
        const QString canonical = QDir(path).canonicalPath();
        if (!canonical.isEmpty() && QFileInfo::exists(canonical + QStringLiteral("/Shadow.material")))
            return canonical;
    }
    return {};
}

void ensureOgreShadowResources()
{
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    if (!matMgr.getDefaultSettings())
        matMgr.initialise();

    const Ogre::String casterName = "Ogre/TextureShadowCaster";
    if (!matMgr.getByName(casterName, Ogre::RGN_INTERNAL))
    {
        Ogre::MaterialPtr mat = matMgr.create(casterName, Ogre::RGN_INTERNAL);
        mat->setReceiveShadows(false);
        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
        pass->setAmbient(Ogre::ColourValue::White);
        pass->setDiffuse(Ogre::ColourValue::Black);
        pass->setSpecular(Ogre::ColourValue(0, 0, 0, 1));
        pass->setEmissive(Ogre::ColourValue::Black);
        pass->setFog(true, Ogre::FOG_NONE);
        pass->setDepthBias(-1.0f, -1.0f);
    }

    auto& texMgr = Ogre::TextureManager::getSingleton();
    const Ogre::String fadeName = "spot_shadow_fade.dds";
    if (!texMgr.resourceExists(fadeName, Ogre::RGN_INTERNAL))
    {
        auto& rgm = Ogre::ResourceGroupManager::getSingleton();
        const QString mainPath = resolveBundledMainMediaPath();
        if (!mainPath.isEmpty())
        {
            rgm.addResourceLocation(mainPath.toStdString(), "FileSystem", Ogre::RGN_INTERNAL);
            if (!rgm.isResourceGroupInitialised(Ogre::RGN_INTERNAL))
                rgm.initialiseResourceGroup(Ogre::RGN_INTERNAL);
        }

        if (!texMgr.resourceExists(fadeName, Ogre::RGN_INTERNAL))
        {
            Ogre::TexturePtr tex = texMgr.createManual(
                fadeName,
                Ogre::RGN_INTERNAL,
                Ogre::TEX_TYPE_2D,
                4,
                4,
                0,
                Ogre::PF_R8G8B8A8);
            Ogre::HardwarePixelBufferSharedPtr buffer = tex->getBuffer();
            std::vector<Ogre::uint32> pixels(16, 0xFFFFFFFFu);
            buffer->blitFromMemory(
                Ogre::PixelBox(4, 4, 1, Ogre::PF_R8G8B8A8, pixels.data()));
        }
    }
}

Ogre::MaterialPtr shadowCasterMaterial()
{
    const Ogre::String matName = "QtMeshEditor/ShadowCaster";
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr mat = matMgr.getByName(
        matName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    if (!mat)
    {
        mat = matMgr.create(matName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_NONE);
        pass->setDepthBias(ShadowController::kDefaultDepthBias, ShadowController::kDefaultSlopeBias);
    }
    return mat;
}

class LightShadowBiasListener : public Ogre::ShadowTextureListener
{
public:
    void shadowTextureCasterPreViewProj(Ogre::Light* light,
                                        Ogre::Camera* camera,
                                        size_t iteration) override
    {
        (void)camera;
        (void)iteration;

        float depthBias = ShadowController::kDefaultDepthBias;
        float slopeBias = ShadowController::kDefaultSlopeBias;

        if (light && LightManager::isUserLight(light))
        {
            const auto& bindings = light->getUserObjectBindings();
            const auto depthAny = bindings.getUserAny(QStringLiteral("shadow_depth_bias").toStdString());
            const auto slopeAny = bindings.getUserAny(QStringLiteral("shadow_slope_bias").toStdString());
            if (depthAny.has_value())
                depthBias = Ogre::any_cast<float>(depthAny);
            if (slopeAny.has_value())
                slopeBias = Ogre::any_cast<float>(slopeAny);
        }

        Ogre::MaterialPtr mat = shadowCasterMaterial();
        if (mat && mat->getNumTechniques() > 0)
        {
            Ogre::Technique* tech = mat->getTechnique(0);
            if (tech && tech->getNumPasses() > 0)
                tech->getPass(0)->setDepthBias(depthBias, slopeBias);
        }
    }
};

LightShadowBiasListener g_shadowBiasListener;

Ogre::ShadowCameraSetupPtr makePssmSetup(int cascadeCount, float nearDist, float farDist, float lambda)
{
    Ogre::ShadowCameraSetupPtr setup = Ogre::PSSMShadowCameraSetup::create();
    auto* pssm = static_cast<Ogre::PSSMShadowCameraSetup*>(setup.get());
    const int splits = std::clamp(cascadeCount, 2, 4);
    pssm->calculateSplitPoints(static_cast<Ogre::uint>(splits),
                               nearDist,
                               farDist,
                               static_cast<Ogre::Real>(lambda));
    pssm->setSplitPadding(3.0f);
    return setup;
}

} // namespace

ShadowController* ShadowController::s_singleton = nullptr;

ShadowController* ShadowController::instance()
{
    if (!s_singleton)
        s_singleton = new ShadowController(); // NOSONAR — singleton
    return s_singleton;
}

ShadowController* ShadowController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

void ShadowController::kill()
{
    delete s_singleton; // NOSONAR — singleton
    s_singleton = nullptr;
}

ShadowController::ShadowController(QObject* parent)
    : QObject(parent)
{
    QSettings settings;
    m_qualityPreset = static_cast<QualityPreset>(
        settings.value(AppSettingsKeys::shadowQualityPreset(), static_cast<int>(QualityPreset::Medium))
            .toInt());
    m_cascadeCount = settings.value(AppSettingsKeys::shadowCascadeCount(), 3).toInt();
    m_splitLambda = settings.value(AppSettingsKeys::shadowSplitLambda(), 0.85).toDouble();
    m_spotShadowResolution =
        settings.value(AppSettingsKeys::shadowSpotResolution(), 1024).toInt();

    if (auto* lights = LightManager::getSingletonPtr())
    {
        connect(lights, &LightManager::lightChanged, this, &ShadowController::syncFromScene);
        connect(lights, &LightManager::lightCreated, this, [this](const LightHandle&) {
            QTimer::singleShot(0, this, &ShadowController::syncFromScene);
        });
        connect(lights, &LightManager::lightDeleted, this, [this]() {
            // Defer until the current scene-node teardown finishes — syncing while
            // Ogre is destroying lights/rig children corrupts the scene graph.
            QTimer::singleShot(0, this, &ShadowController::syncFromScene);
        });
    }
}

ShadowController::~ShadowController()
{
    if (auto* mgr = Manager::getSingletonPtr())
    {
        if (Ogre::SceneManager* sm = mgr->getSceneMgr())
            uninstallSceneShadows(sm);
    }
}

QStringList ShadowController::qualityPresetNames() const
{
    return {QStringLiteral("Off"),
            QStringLiteral("Low"),
            QStringLiteral("Medium"),
            QStringLiteral("High")};
}

QStringList ShadowController::spotShadowResolutionChoices() const
{
    return {QStringLiteral("512"), QStringLiteral("1024"), QStringLiteral("2048")};
}

QStringList ShadowController::cascadeCountChoices() const
{
    return {QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4")};
}

void ShadowController::setQualityPreset(int preset)
{
    const auto next = static_cast<QualityPreset>(std::clamp(preset, 0, 3));
    if (m_qualityPreset == next)
        return;

    m_qualityPreset = next;
    applyPresetDefaults(next);

    QSettings settings;
    settings.setValue(AppSettingsKeys::shadowQualityPreset(), static_cast<int>(next));

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Shadow quality: %1")
                                      .arg(qualityPresetNames().value(static_cast<int>(next))));
    emit settingsChanged();
    syncFromScene();
}

void ShadowController::setCascadeCount(int count)
{
    const int next = std::clamp(count, 2, 4);
    if (m_cascadeCount == next)
        return;

    m_cascadeCount = next;
    QSettings().setValue(AppSettingsKeys::shadowCascadeCount(), next);
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Shadow cascades: %1").arg(next));
    emit settingsChanged();
    syncFromScene();
}

void ShadowController::setSplitLambda(double lambda)
{
    const double next = std::clamp(lambda, 0.0, 1.0);
    if (std::abs(m_splitLambda - next) < 1e-6)
        return;

    m_splitLambda = next;
    QSettings().setValue(AppSettingsKeys::shadowSplitLambda(), next);
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Shadow PSSM split: %1").arg(next, 0, 'f', 2));
    emit settingsChanged();
    syncFromScene();
}

void ShadowController::setSpotShadowResolution(int pixels)
{
    int next = 1024;
    if (pixels <= 512)
        next = 512;
    else if (pixels >= 2048)
        next = 2048;

    if (m_spotShadowResolution == next)
        return;

    m_spotShadowResolution = next;
    QSettings().setValue(AppSettingsKeys::shadowSpotResolution(), next);
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Spot shadow map: %1").arg(next));
    emit settingsChanged();
    syncFromScene();
}

ShadowController::QualityProfile ShadowController::profileForPreset(QualityPreset preset) const
{
    QualityProfile profile;
    switch (preset)
    {
    case QualityPreset::Off:
        break;
    case QualityPreset::Low:
        profile.textureSize = 512;
        profile.cascades = 2;
        profile.splitLambda = 0.80f;
        profile.shadowFarDistance = 30.0f;
        break;
    case QualityPreset::Medium:
        profile.textureSize = 1024;
        profile.cascades = 3;
        profile.splitLambda = 0.85f;
        profile.shadowFarDistance = 50.0f;
        break;
    case QualityPreset::High:
        profile.textureSize = 2048;
        profile.cascades = 4;
        profile.splitLambda = 0.90f;
        profile.shadowFarDistance = 80.0f;
        break;
    }
    return profile;
}

void ShadowController::applyPresetDefaults(QualityPreset preset)
{
    const QualityProfile profile = profileForPreset(preset);
    if (preset == QualityPreset::Off)
        return;

    m_cascadeCount = profile.cascades;
    m_splitLambda = profile.splitLambda;
    if (preset == QualityPreset::Low)
        m_spotShadowResolution = 512;
    else if (preset == QualityPreset::Medium)
        m_spotShadowResolution = 1024;
    else
        m_spotShadowResolution = 2048;
}

void ShadowController::registerViewport(OgreWidget* widget)
{
    if (!widget)
        return;

    m_viewports.insert(widget);
    if (const Ogre::Viewport* vp = widget->getViewport())
    {
        const_cast<Ogre::Viewport*>(vp)->setShadowsEnabled(m_shadowsActive);
    }
}

void ShadowController::unregisterViewport(OgreWidget* widget)
{
    m_viewports.remove(widget);
}

void ShadowController::refreshViewports(bool enabled)
{
    m_shadowsActive = enabled;
    for (OgreWidget* widget : m_viewports)
    {
        if (!widget)
            continue;
        if (const Ogre::Viewport* vp = widget->getViewport())
            const_cast<Ogre::Viewport*>(vp)->setShadowsEnabled(enabled);
    }
}

bool ShadowController::anyUserLightCastsShadows() const
{
    auto* lights = LightManager::getSingletonPtr();
    if (!lights)
        return false;

    for (const LightHandle& handle : lights->lights())
    {
        if (handle.isValid() && handle.light->getCastShadows())
            return true;
    }
    return false;
}

int ShadowController::requiredShadowTextureCount(int directionalCasters) const
{
    int spotCasters = 0;
    int pointCasters = 0;

    if (auto* lights = LightManager::getSingletonPtr())
    {
        for (const LightHandle& handle : lights->lights())
        {
            if (!handle.isValid() || !handle.light->getCastShadows())
                continue;

            switch (handle.light->getType())
            {
            case Ogre::Light::LT_SPOTLIGHT:
                ++spotCasters;
                break;
            case Ogre::Light::LT_POINT:
                ++pointCasters;
                break;
            default:
                break;
            }
        }
    }

    return std::max(1, directionalCasters * m_cascadeCount + spotCasters + pointCasters);
}

void ShadowController::installSceneShadows(Ogre::SceneManager* sceneMgr)
{
    if (!sceneMgr)
        return;

    try
    {
    ensureOgreShadowResources();

    const QualityProfile profile = profileForPreset(m_qualityPreset);
    const int textureSize = std::max(profile.textureSize, m_spotShadowResolution);

    int directionalCasters = 0;
    if (auto* lights = LightManager::getSingletonPtr())
    {
        for (const LightHandle& handle : lights->lights())
        {
            if (handle.isValid() && handle.light->getCastShadows()
                && handle.light->getType() == Ogre::Light::LT_DIRECTIONAL)
            {
                ++directionalCasters;
            }
        }
    }

    const int textureCount = requiredShadowTextureCount(directionalCasters);

    sceneMgr->setShadowTechnique(Ogre::SHADOWTYPE_TEXTURE_MODULATIVE);
    sceneMgr->setShadowTextureSettings(static_cast<Ogre::uint16>(textureSize),
                                        static_cast<Ogre::uint16>(textureCount),
                                        Ogre::PF_DEPTH16);
    sceneMgr->setShadowFarDistance(profile.shadowFarDistance);
    sceneMgr->setShadowTextureSelfShadow(true);
    sceneMgr->setShadowTextureCountPerLightType(Ogre::Light::LT_DIRECTIONAL,
                                                static_cast<size_t>(m_cascadeCount));
    sceneMgr->setShadowTextureCountPerLightType(Ogre::Light::LT_SPOTLIGHT, 1);
    sceneMgr->setShadowTextureCountPerLightType(Ogre::Light::LT_POINT, 1);

    sceneMgr->setShadowTextureCasterMaterial(shadowCasterMaterial());

    if (directionalCasters > 0)
    {
        sceneMgr->setShadowCameraSetup(makePssmSetup(m_cascadeCount,
                                                     1.0f,
                                                     profile.shadowFarDistance,
                                                     static_cast<float>(m_splitLambda)));
    }
    else
    {
        sceneMgr->setShadowCameraSetup(Ogre::ShadowCameraSetupPtr());
    }

    sceneMgr->removeShadowTextureListener(&g_shadowBiasListener);
    sceneMgr->addShadowTextureListener(&g_shadowBiasListener);
    }
    catch (const Ogre::Exception&)
    {
        uninstallSceneShadows(sceneMgr);
        refreshViewports(false);
    }
}

void ShadowController::uninstallSceneShadows(Ogre::SceneManager* sceneMgr)
{
    if (!sceneMgr)
        return;

    sceneMgr->removeShadowTextureListener(&g_shadowBiasListener);
    if (sceneMgr->isShadowTechniqueInUse())
        sceneMgr->setShadowTechnique(Ogre::SHADOWTYPE_NONE);
}

void ShadowController::syncFromScene()
{
    if (!hasActiveRenderContext())
        return;

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr)
        return;

    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr)
        return;

    const bool wantShadows =
        m_qualityPreset != QualityPreset::Off && anyUserLightCastsShadows();

    if (!wantShadows)
    {
        uninstallSceneShadows(sceneMgr);
        refreshViewports(false);
        return;
    }

    installSceneShadows(sceneMgr);
    refreshViewports(true);
}
