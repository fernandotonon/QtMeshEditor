#include "HDR/HdrViewportPipeline.h"

#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrTonemap.h"
#include "OgreWidget.h"

#include <OgreCompositorManager.h>
#include <OgreMaterialManager.h>
#include <OgreRoot.h>
#include <OgreViewport.h>

namespace {

constexpr const char* kTonemapMaterialName = "QtMesh/HdrTonemapPass";

void updateTonemapMaterialConstants()
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (!hdrMgr || !Ogre::MaterialManager::getSingletonPtr())
        return;

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().getByName(kTonemapMaterialName);
    if (!mat || mat->getNumTechniques() == 0)
        return;

    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    if (!pass->getFragmentProgramParameters())
        return;

    const float exposureMul = HdrTonemap::exposureMultiplier(hdrMgr->exposureEv());
    const int tonemapOp = static_cast<int>(hdrMgr->tonemapOperator());
    pass->getFragmentProgramParameters()->setNamedConstant("exposureMul", exposureMul);
    pass->getFragmentProgramParameters()->setNamedConstant("tonemapOp", tonemapOp);
    pass->getFragmentProgramParameters()->setNamedConstant("whitePoint", hdrMgr->whitePoint());
}

} // namespace

HdrViewportPipeline::HdrViewportPipeline(OgreWidget* widget)
    : m_widget(widget)
{
}

HdrViewportPipeline::~HdrViewportPipeline()
{
    disablePipeline();
}

void HdrViewportPipeline::refresh()
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (hdrMgr && hdrMgr->hasEnvironment())
        enablePipeline();
    else
        disablePipeline();
}

void HdrViewportPipeline::updateTonemapUniforms()
{
    if (!m_enabled)
        return;
    updateTonemapMaterialConstants();
}

void HdrViewportPipeline::setSkyBoxVisible(bool visible)
{
    m_skyBoxVisible = visible;
    if (m_viewport)
        m_viewport->setSkiesEnabled(visible);
}

void HdrViewportPipeline::enablePipeline()
{
    if (!m_widget)
        return;

    const Ogre::Viewport* vpConst = m_widget->getViewport();
    if (!vpConst)
        return;

    m_viewport = const_cast<Ogre::Viewport*>(vpConst);
    m_skyBoxVisible = HDREnvironmentManager::getSingleton()->defaultSkyBoxVisible();
    m_viewport->setSkiesEnabled(m_skyBoxVisible);

    if (!Ogre::CompositorManager::getSingletonPtr())
        return;

    if (!m_compositor) {
        m_compositor = Ogre::CompositorManager::getSingleton().addCompositor(
            m_viewport, kCompositorName);
        if (m_compositor)
            Ogre::CompositorManager::getSingleton().setCompositorEnabled(
                m_viewport, kCompositorName, true);
    }

    updateTonemapMaterialConstants();
    m_enabled = m_compositor != nullptr;
}

void HdrViewportPipeline::disablePipeline()
{
    if (m_viewport && m_compositor && Ogre::CompositorManager::getSingletonPtr()) {
        Ogre::CompositorManager::getSingleton().setCompositorEnabled(
            m_viewport, kCompositorName, false);
        Ogre::CompositorManager::getSingleton().removeCompositor(m_viewport, kCompositorName);
    }
    m_compositor = nullptr;
    m_viewport = nullptr;
    m_enabled = false;
}
