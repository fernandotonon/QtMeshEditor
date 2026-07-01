#include "HDR/HdrViewportPipeline.h"

#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrTonemap.h"
#include "OgreWidget.h"

#include <OgreCompositorManager.h>
#include <OgreMaterialManager.h>
#include <OgreRoot.h>
#include <OgreViewport.h>

#include <QHash>

namespace {

int nextPipelineId()
{
    static int s_id = 0;
    return ++s_id;
}

QHash<QString, bool>& registeredCompositors()
{
    static QHash<QString, bool> s_registered;
    return s_registered;
}

void updateTonemapMaterialConstants(const Ogre::String& materialName,
                                    HdrTonemap::Operator op,
                                    float exposureEv,
                                    float whitePoint)
{
    if (!Ogre::MaterialManager::getSingletonPtr())
        return;

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().getByName(materialName);
    if (!mat || mat->getNumTechniques() == 0)
        return;

    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    if (!pass->getFragmentProgramParameters())
        return;

    const float exposureMul = HdrTonemap::exposureMultiplier(exposureEv);
    const int tonemapOp = static_cast<int>(op);
    pass->getFragmentProgramParameters()->setNamedConstant("exposureMul", exposureMul);
    pass->getFragmentProgramParameters()->setNamedConstant("tonemapOp", tonemapOp);
    pass->getFragmentProgramParameters()->setNamedConstant("whitePoint", whitePoint);
}

} // namespace

HdrViewportPipeline::HdrViewportPipeline(OgreWidget* widget)
    : m_widget(widget)
{
    const int id = nextPipelineId();
    m_compositorName = QStringLiteral("%1_%2").arg(kCompositorBaseName).arg(id);
    m_tonemapMaterialName =
        QStringLiteral("%1_%2").arg(QLatin1String(kTonemapMaterialBaseName)).arg(id);

    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        m_skyBoxVisible = hdrMgr->defaultSkyBoxVisible();
}

HdrViewportPipeline::~HdrViewportPipeline()
{
    disablePipeline();
}

void HdrViewportPipeline::ensureTonemapMaterial()
{
    if (!Ogre::MaterialManager::getSingletonPtr())
        return;

    const Ogre::String matName = m_tonemapMaterialName.toStdString();
    if (Ogre::MaterialManager::getSingleton().resourceExists(matName))
        return;

    Ogre::MaterialPtr base =
        Ogre::MaterialManager::getSingleton().getByName(kTonemapMaterialBaseName);
    if (!base)
        return;

    base->clone(matName);
}

void HdrViewportPipeline::ensureCompositorScript()
{
    if (registeredCompositors().contains(m_compositorName))
        return;

    const QString script = QStringLiteral(
                               "compositor %1\n"
                               "{\n"
                               "    technique\n"
                               "    {\n"
                               "        texture rtHdr target_width target_height PF_FLOAT16_RGBA\n"
                               "        target rtHdr\n"
                               "        {\n"
                               "            pass render_scene\n"
                               "            {\n"
                               "            }\n"
                               "        }\n"
                               "        target_output\n"
                               "        {\n"
                               "            pass render_quad\n"
                               "            {\n"
                               "                input 0 rtHdr\n"
                               "                material %2\n"
                               "            }\n"
                               "        }\n"
                               "    }\n"
                               "}")
                               .arg(m_compositorName, m_tonemapMaterialName);

    const Ogre::String stdScript = script.toStdString();
    Ogre::MemoryDataStream* stream = new Ogre::MemoryDataStream(
        stdScript.data(), stdScript.size(), true);
    Ogre::DataStreamPtr dataStream(stream);
    Ogre::CompositorManager::getSingleton().parseScript(
        dataStream, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    registeredCompositors().insert(m_compositorName, true);
}

void HdrViewportPipeline::refresh()
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (hdrMgr && hdrMgr->hasEnvironment())
        enablePipeline();
    else
        disablePipeline();
}

HdrTonemap::Operator HdrViewportPipeline::effectiveTonemapOperator() const
{
    if (m_tonemapOverride)
        return m_localTonemapOperator;
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return hdrMgr->tonemapOperator();
    return HdrTonemap::Operator::ACES;
}

float HdrViewportPipeline::effectiveExposureEv() const
{
    if (m_tonemapOverride)
        return m_localExposureEv;
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return hdrMgr->exposureEv();
    return 0.f;
}

void HdrViewportPipeline::updateTonemapUniforms()
{
    if (!m_enabled)
        return;

    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    const float whitePoint = hdrMgr ? hdrMgr->whitePoint() : 1.f;
    updateTonemapMaterialConstants(m_tonemapMaterialName.toStdString(),
                                   effectiveTonemapOperator(),
                                   effectiveExposureEv(),
                                   whitePoint);
}

void HdrViewportPipeline::setSkyBoxVisible(bool visible)
{
    m_skyBoxVisible = visible;
    if (m_viewport)
        m_viewport->setSkiesEnabled(visible);
}

void HdrViewportPipeline::setTonemapOverride(bool enabled)
{
    m_tonemapOverride = enabled;
    updateTonemapUniforms();
}

void HdrViewportPipeline::setTonemapOperator(HdrTonemap::Operator op)
{
    m_localTonemapOperator = op;
    if (m_tonemapOverride)
        updateTonemapUniforms();
}

void HdrViewportPipeline::setExposureEv(float exposureEv)
{
    m_localExposureEv = exposureEv;
    if (m_tonemapOverride)
        updateTonemapUniforms();
}

HdrTonemap::Operator HdrViewportPipeline::tonemapOperator() const
{
    return m_localTonemapOperator;
}

float HdrViewportPipeline::exposureEv() const
{
    return m_localExposureEv;
}

void HdrViewportPipeline::enablePipeline()
{
    if (!m_widget)
        return;

    const Ogre::Viewport* vpConst = m_widget->getViewport();
    if (!vpConst)
        return;

    Ogre::Viewport* vp = const_cast<Ogre::Viewport*>(vpConst);
    if (m_viewport != vp && m_compositor)
        disablePipeline();

    m_viewport = vp;
    m_viewport->setSkiesEnabled(m_skyBoxVisible);

    if (!Ogre::CompositorManager::getSingletonPtr())
        return;

    ensureTonemapMaterial();
    ensureCompositorScript();

    if (!m_compositor) {
        m_compositor = Ogre::CompositorManager::getSingleton().addCompositor(
            m_viewport, m_compositorName.toStdString());
        if (m_compositor) {
            Ogre::CompositorManager::getSingleton().setCompositorEnabled(
                m_viewport, m_compositorName.toStdString(), true);
        }
    }

    updateTonemapUniforms();
    m_enabled = m_compositor != nullptr;
}

void HdrViewportPipeline::disablePipeline()
{
    if (m_viewport && m_compositor && Ogre::CompositorManager::getSingletonPtr()) {
        const Ogre::String compName = m_compositorName.toStdString();
        Ogre::CompositorManager::getSingleton().setCompositorEnabled(m_viewport, compName, false);
        Ogre::CompositorManager::getSingleton().removeCompositor(m_viewport, compName);
    }
    m_compositor = nullptr;
    m_viewport = nullptr;
    m_enabled = false;
}
