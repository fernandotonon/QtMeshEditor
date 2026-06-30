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
}

HdrViewportController::~HdrViewportController() = default;

void HdrViewportController::registerWidget(OgreWidget* widget)
{
    if (!widget || m_pipelines.find(widget) != m_pipelines.end())
        return;

    m_pipelines.emplace(widget, std::make_unique<HdrViewportPipeline>(widget));
    if (auto* pipe = pipelineFor(widget))
        pipe->setSkyBoxVisible(HDREnvironmentManager::getSingleton()->defaultSkyBoxVisible());
    onEnvironmentChanged();
}

void HdrViewportController::unregisterWidget(OgreWidget* widget)
{
    m_pipelines.erase(widget);
}

void HdrViewportController::setSkyBoxVisible(OgreWidget* widget, bool visible)
{
    if (auto* pipe = pipelineFor(widget))
        pipe->setSkyBoxVisible(visible);
}

bool HdrViewportController::skyBoxVisible(const OgreWidget* widget) const
{
    if (!widget)
        return true;
    auto it = m_pipelines.find(const_cast<OgreWidget*>(widget));
    if (it == m_pipelines.end())
        return HDREnvironmentManager::getSingleton()->defaultSkyBoxVisible();
    return it->second->skyBoxVisible();
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
}

void HdrViewportController::onTonemapChanged()
{
    refreshAll();
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
