#include "HDR/HdrViewportPipeline.h"

#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrTonemap.h"
#include "OgreWidget.h"
#include "SentryReporter.h"

#include <OgreCompositorManager.h>
#include <OgreException.h>
#include <OgreMaterialManager.h>
#include <OgreRoot.h>
#include <OgreViewport.h>

#include <QHash>

#include <vector>

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

bool compositorResourceExists(const QString& name)
{
    if (!Ogre::CompositorManager::getSingletonPtr())
        return false;
    return Ogre::CompositorManager::getSingleton().resourceExists(name.toStdString());
}

QStringList hdrRenderTargetPixelFormats()
{
    return {QStringLiteral("PF_FLOAT16_RGBA"),
            QStringLiteral("PF_FLOAT32_RGBA"),
            QStringLiteral("PF_R11G11B10_FLOAT"),
            QStringLiteral("PF_R8G8B8A8")};
}

QString buildCompositorScript(const QString& compositorName,
                              const QString& tonemapMaterialName,
                              const QString& pixelFormat)
{
    // Must match OgreWidget::mViewport->setMaterialScheme(MSN_SHADERGEN) or RTSS
    // materials (and the skybox) will not draw inside render_scene.
    const QString scheme = QString::fromStdString(Ogre::MSN_SHADERGEN);
    return QStringLiteral(
               "compositor %1\n"
               "{\n"
               "    technique\n"
               "    {\n"
               "        texture rtHdr target_width target_height %3 depth_pool 0\n"
               "        target rtHdr\n"
               "        {\n"
               "            input none\n"
               "            material_scheme %4\n"
               "            pass clear\n"
               "            {\n"
               "                buffers colour depth\n"
               "            }\n"
               "            pass render_scene\n"
               "            {\n"
               "            }\n"
               "        }\n"
               "        target_output\n"
               "        {\n"
               "            input none\n"
               "            pass render_quad\n"
               "            {\n"
               "                input 0 rtHdr\n"
               "                material %2\n"
               "            }\n"
               "        }\n"
               "    }\n"
               "}")
        .arg(compositorName, tonemapMaterialName, pixelFormat, scheme);
}

bool parseCompositorScript(const QString& script)
{
    if (!Ogre::CompositorManager::getSingletonPtr())
        return false;

    try {
        const Ogre::String stdScript = script.toStdString();
        std::vector<char> owned(stdScript.begin(), stdScript.end());
        Ogre::DataStreamPtr dataStream(new Ogre::MemoryDataStream(
            owned.data(), owned.size(), false, true));
        Ogre::CompositorManager::getSingleton().parseScript(
            dataStream, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        return true;
    } catch (const Ogre::Exception&) {
        return false;
    }
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

    if (!base->isLoaded())
        base->load();

    Ogre::MaterialPtr cloned = base->clone(matName);
    if (!cloned->isLoaded())
        cloned->load();
}

void HdrViewportPipeline::ensureCompositorScript()
{
    if (registeredCompositors().value(m_compositorName))
        return;

    if (compositorResourceExists(m_compositorName)) {
        registeredCompositors().insert(m_compositorName, true);
        return;
    }

    ensureTonemapMaterial();
    if (!Ogre::MaterialManager::getSingletonPtr()
        || !Ogre::MaterialManager::getSingleton().resourceExists(
            m_tonemapMaterialName.toStdString())) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("render.hdr.pipeline"),
            QStringLiteral("tonemap material missing for compositor %1")
                .arg(m_compositorName));
        return;
    }

    for (const QString& pixelFormat : hdrRenderTargetPixelFormats()) {
        if (compositorResourceExists(m_compositorName)) {
            registeredCompositors().insert(m_compositorName, true);
            return;
        }

        parseCompositorScript(buildCompositorScript(
            m_compositorName, m_tonemapMaterialName, pixelFormat));

        if (compositorResourceExists(m_compositorName)) {
            registeredCompositors().insert(m_compositorName, true);
            SentryReporter::addBreadcrumb(
                QStringLiteral("render.hdr.pipeline"),
                QStringLiteral("registered compositor %1 format=%2")
                    .arg(m_compositorName, pixelFormat));
            return;
        }
    }

    SentryReporter::addBreadcrumb(
        QStringLiteral("render.hdr.pipeline"),
        QStringLiteral("failed to register compositor %1").arg(m_compositorName));
}

void HdrViewportPipeline::refresh()
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (hdrMgr && hdrMgr->hasEnvironment())
        syncViewportEnvironment();
    else
        disablePipeline();
}

void HdrViewportPipeline::updateTonemapUniforms()
{
    if (!m_enabled)
        return;

    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    const float whitePoint = hdrMgr ? hdrMgr->whitePoint() : 1.f;
    const HdrTonemap::Operator op =
        hdrMgr ? hdrMgr->tonemapOperator() : HdrTonemap::Operator::ACES;
    const float exposureEv = hdrMgr ? hdrMgr->exposureEv() : 0.f;
    updateTonemapMaterialConstants(m_tonemapMaterialName.toStdString(),
                                   op,
                                   exposureEv,
                                   whitePoint);
}

void HdrViewportPipeline::setSkyBoxVisible(bool visible)
{
    m_skyBoxVisible = visible;
    Ogre::Viewport* vp = m_viewport;
    if (!vp && m_widget)
        vp = const_cast<Ogre::Viewport*>(m_widget->getViewport());
    if (vp)
        vp->setSkiesEnabled(visible);
}

void HdrViewportPipeline::syncViewportEnvironment()
{
    if (!m_widget)
        return;

    const Ogre::Viewport* vpConst = m_widget->getViewport();
    if (!vpConst)
        return;

    Ogre::Viewport* vp = const_cast<Ogre::Viewport*>(vpConst);
    if (m_viewport != vp)
        removeCompositor();

    m_viewport = vp;
    m_viewport->setSkiesEnabled(m_skyBoxVisible);

    if (!kUseTonemapCompositor) {
        removeCompositor();
        m_enabled = false;
        return;
    }

    m_enabled = attachTonemapCompositor(vp);
}

bool HdrViewportPipeline::attachTonemapCompositor(Ogre::Viewport* vp)
{
    if (!vp || !Ogre::CompositorManager::getSingletonPtr())
        return false;

    ensureTonemapMaterial();
    ensureCompositorScript();

    if (!compositorResourceExists(m_compositorName))
        return false;

    if (!m_compositor) {
        try {
            m_compositor = Ogre::CompositorManager::getSingleton().addCompositor(
                vp, m_compositorName.toStdString());
            if (m_compositor) {
                Ogre::CompositorManager::getSingleton().setCompositorEnabled(
                    vp, m_compositorName.toStdString(), true);
            }
        } catch (const Ogre::Exception& e) {
            SentryReporter::addBreadcrumb(
                QStringLiteral("render.hdr.pipeline"),
                QStringLiteral("addCompositor failed for %1: %2")
                    .arg(m_compositorName, QString::fromUtf8(e.what())));
            m_compositor = nullptr;
        }
    }

    updateTonemapUniforms();

    if (!m_compositor)
        return false;

    Ogre::MaterialPtr mat =
        Ogre::MaterialManager::getSingleton().getByName(m_tonemapMaterialName.toStdString());
    Ogre::Pass* pass =
        mat && mat->getNumTechniques() > 0 ? mat->getTechnique(0)->getPass(0) : nullptr;
    if (!pass || !pass->getFragmentProgramParameters() || pass->getNumTextureUnitStates() == 0) {
        removeCompositor();
        return false;
    }

    return true;
}

void HdrViewportPipeline::removeCompositor()
{
    if (m_viewport && m_compositor && Ogre::CompositorManager::getSingletonPtr()) {
        const Ogre::String compName = m_compositorName.toStdString();
        Ogre::CompositorManager::getSingleton().setCompositorEnabled(m_viewport, compName, false);
        Ogre::CompositorManager::getSingleton().removeCompositor(m_viewport, compName);
    }
    m_compositor = nullptr;
    m_enabled = false;
}

void HdrViewportPipeline::disablePipeline()
{
    removeCompositor();
    if (!m_viewport && m_widget)
        m_viewport = const_cast<Ogre::Viewport*>(m_widget->getViewport());
    if (m_viewport)
        m_viewport->setSkiesEnabled(false);
    m_viewport = nullptr;
}
