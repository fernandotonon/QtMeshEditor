#include "HDR/HdrViewportController.h"

#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrViewportPipeline.h"
#include "Manager.h"
#include "OgreWidget.h"

#include <OgreSceneManager.h>

HdrViewportController* HdrViewportController::s_singleton = nullptr;

HdrViewportController* HdrViewportController::getSingleton()
{
    if (!s_singleton)
        s_singleton = new HdrViewportController(); // NOSONAR — singleton
    return s_singleton;
}

HdrViewportController* HdrViewportController::getSingletonPtr()
{
    return s_singleton;
}

void HdrViewportController::kill()
{
    delete s_singleton; // NOSONAR — singleton
    s_singleton = nullptr;
}

HdrViewportController::HdrViewportController(QObject* parent)
    : QObject(parent)
{
    auto* hdrMgr = HDREnvironmentManager::getSingleton();
    connect(hdrMgr, &HDREnvironmentManager::environmentChanged,
            this, &HdrViewportController::onEnvironmentChanged);
    connect(hdrMgr, &HDREnvironmentManager::tonemapChanged,
            this, &HdrViewportController::onTonemapChanged);
    connect(hdrMgr, &HDREnvironmentManager::skyboxDefaultChanged,
            this, &HdrViewportController::onSkyboxDefaultChanged);
}

HdrViewportController::~HdrViewportController() = default;

void HdrViewportController::registerWidget(OgreWidget* widget)
{
    if (!widget)
        return;

    auto it = m_pipelines.find(widget);
    if (it != m_pipelines.end()) {
        it->second->refresh();
        syncAllSkyBoxesFromDefault();
        return;
    }

    m_pipelines.emplace(widget, std::make_unique<HdrViewportPipeline>(widget));
    onEnvironmentChanged();
}

void HdrViewportController::unregisterWidget(OgreWidget* widget)
{
    m_pipelines.erase(widget);
}

void HdrViewportController::setActiveWidget(OgreWidget* widget)
{
    m_activeWidget = widget;
}

void HdrViewportController::tickViewport(OgreWidget* widget)
{
    if (auto* pipe = pipelineFor(widget))
        pipe->updateTonemapUniforms();
}

void HdrViewportController::syncAllSkyBoxesFromDefault()
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    const bool visible = hdrMgr ? hdrMgr->defaultSkyBoxVisible() : true;

    if (auto* mgr = Manager::getSingletonPtr(); hdrMgr && mgr && mgr->getSceneMgr()
        && hdrMgr->hasEnvironment()) {
        mgr->getSceneMgr()->setSkyRenderingEnabled(visible);
    }

    for (auto& [widget, pipeline] : m_pipelines) {
        Q_UNUSED(widget);
        pipeline->setSkyBoxVisible(visible);
    }
}

void HdrViewportController::tickActiveViewports()
{
    for (auto& [widget, pipeline] : m_pipelines) {
        Q_UNUSED(widget);
        pipeline->updateTonemapUniforms();
    }
}

void HdrViewportController::onEnvironmentChanged()
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (auto* mgr = Manager::getSingletonPtr()) {
        if (hdrMgr && hdrMgr->hasEnvironment())
            hdrMgr->applySkyBox(mgr->getSceneMgr());
        else if (hdrMgr)
            hdrMgr->removeSkyBox(mgr->getSceneMgr());
    }
    refreshAll();
    syncAllSkyBoxesFromDefault();
}

void HdrViewportController::onTonemapChanged()
{
    for (auto& [widget, pipeline] : m_pipelines) {
        Q_UNUSED(widget);
        pipeline->updateTonemapUniforms();
    }
}

void HdrViewportController::onSkyboxDefaultChanged()
{
    syncAllSkyBoxesFromDefault();
}

HdrViewportPipeline* HdrViewportController::pipelineFor(OgreWidget* widget) const
{
    auto it = m_pipelines.find(widget);
    return it == m_pipelines.end() ? nullptr : it->second.get();
}

void HdrViewportController::refreshAll()
{
    for (auto& [widget, pipeline] : m_pipelines) {
        Q_UNUSED(widget);
        pipeline->refresh();
    }
}
