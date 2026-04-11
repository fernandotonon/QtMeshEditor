#include "MaterialPreviewRenderer.h"

#include <QBuffer>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <OgreHardwarePixelBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreTextureManager.h>
#include <OgreRoot.h>

#include "ProceduralSphereGenerator.h"

MaterialPreviewRenderer* MaterialPreviewRenderer::m_pSingleton = nullptr;

MaterialPreviewRenderer* MaterialPreviewRenderer::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new MaterialPreviewRenderer();
    return m_pSingleton;
}

MaterialPreviewRenderer* MaterialPreviewRenderer::qmlInstance(QQmlEngine* engine, QJSEngine* /*scriptEngine*/)
{
    auto* inst = instance();
    if (engine)
        engine->setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void MaterialPreviewRenderer::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

MaterialPreviewRenderer::MaterialPreviewRenderer()
    : QObject(nullptr)
{
}

MaterialPreviewRenderer::~MaterialPreviewRenderer()
{
    if (m_initialized) {
        auto* root = Ogre::Root::getSingletonPtr();
        if (root) {
            // Remove the render texture first
            if (m_rttTexture) {
                Ogre::TextureManager::getSingleton().remove(m_rttTexture);
                m_rttTexture.reset();
            }

            // Destroy the preview scene manager (cleans up all its nodes/entities/lights)
            if (m_sceneMgr) {
                root->destroySceneManager(m_sceneMgr);
                m_sceneMgr = nullptr;
            }
        }
    }
}

bool MaterialPreviewRenderer::ensureScene()
{
    if (m_initialized)
        return true;

    auto* root = Ogre::Root::getSingletonPtr();
    if (!root || !root->getRenderSystem())
        return false;

    try {
        // Create a dedicated scene manager for previews
        m_sceneMgr = root->createSceneManager("DefaultSceneManager", "MaterialPreviewSM");

        // Ambient light for base illumination
        m_sceneMgr->setAmbientLight(Ogre::ColourValue(0.55f, 0.55f, 0.55f));

        // Camera looking at the origin (Ogre 14: position via scene node)
        m_camera = m_sceneMgr->createCamera("PreviewCam");
        m_camera->setNearClipDistance(0.1f);
        m_camera->setFarClipDistance(10.0f);
        m_camera->setAspectRatio(1.0f);

        auto* camNode = m_sceneMgr->getRootSceneNode()->createChildSceneNode();
        camNode->setPosition(Ogre::Vector3(0, 0, 2.5f));
        camNode->lookAt(Ogre::Vector3::ZERO, Ogre::Node::TS_WORLD);
        camNode->attachObject(m_camera);

        // Directional light from upper-right
        m_light = m_sceneMgr->createLight("PreviewLight");
        m_light->setType(Ogre::Light::LT_DIRECTIONAL);
        m_light->setDiffuseColour(0.8f, 0.8f, 0.8f);
        m_light->setSpecularColour(1.0f, 1.0f, 1.0f);
        auto* lightNode = m_sceneMgr->getRootSceneNode()->createChildSceneNode();
        lightNode->attachObject(m_light);
        lightNode->setDirection(Ogre::Vector3(-1, -1, -1).normalisedCopy());

        // Create sphere mesh using ogre-procedural
        const std::string meshName = "__MaterialPreviewSphere__";
        if (!Ogre::MeshManager::getSingleton().resourceExists(meshName)) {
            Procedural::SphereGenerator()
                .setRadius(1.0f)
                .setUTile(1.0f)
                .setVTile(1.0f)
                .setNumRings(16)
                .setNumSegments(16)
                .realizeMesh(meshName);
        }

        m_sphere = m_sceneMgr->createEntity("PreviewSphereEntity", meshName);
        m_sphereNode = m_sceneMgr->getRootSceneNode()->createChildSceneNode();
        m_sphereNode->attachObject(m_sphere);

        // Create render-to-texture
        m_rttTexture = Ogre::TextureManager::getSingleton().createManual(
            "MatPreviewRTT",
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
            Ogre::TEX_TYPE_2D,
            PREVIEW_SIZE, PREVIEW_SIZE,
            0,
            Ogre::PF_BYTE_RGBA,
            Ogre::TU_RENDERTARGET);

        m_renderTarget = m_rttTexture->getBuffer()->getRenderTarget();
        Ogre::Viewport* vp = m_renderTarget->addViewport(m_camera);
        vp->setClearEveryFrame(true);
        vp->setBackgroundColour(Ogre::ColourValue(0.15f, 0.15f, 0.15f, 1.0f));
        vp->setOverlaysEnabled(false);

        m_initialized = true;
        return true;
    } catch (const Ogre::Exception&) {
        // Clean up partial state on failure
        if (m_sceneMgr) {
            Ogre::Root::getSingletonPtr()->destroySceneManager(m_sceneMgr);
            m_sceneMgr = nullptr;
        }
        m_camera = nullptr;
        m_light = nullptr;
        m_sphere = nullptr;
        m_sphereNode = nullptr;
        m_renderTarget = nullptr;
        return false;
    } catch (...) {
        return false;
    }
}

QImage MaterialPreviewRenderer::renderPreview(const QString& materialName)
{
    if (!ensureScene())
        return {};

    // Check that the material exists
    auto* matMgr = Ogre::MaterialManager::getSingletonPtr();
    if (!matMgr)
        return {};

    std::string stdName = materialName.toStdString();
    if (!matMgr->resourceExists(stdName))
        return {};

    try {
        // Apply the material to the sphere
        m_sphere->setMaterialName(stdName);

        // Render
        m_renderTarget->update();

        // Read pixels from the render target
        QImage image(PREVIEW_SIZE, PREVIEW_SIZE, QImage::Format_RGBA8888);

        Ogre::PixelBox pb(PREVIEW_SIZE, PREVIEW_SIZE, 1, Ogre::PF_BYTE_RGBA, image.bits());
        m_renderTarget->copyContentsToMemory(
            Ogre::Box(0, 0, PREVIEW_SIZE, PREVIEW_SIZE), pb,
            Ogre::RenderTarget::FB_AUTO);

        return image;
    } catch (const Ogre::Exception&) {
        return {};
    } catch (...) {
        return {};
    }
}

QString MaterialPreviewRenderer::renderPreviewAsDataUri(const QString& materialName)
{
    // Check cache first
    auto it = m_cache.find(materialName);
    if (it != m_cache.end())
        return it.value();

    QImage image = renderPreview(materialName);
    if (image.isNull())
        return {};

    // Convert to PNG base64
    QByteArray ba;
    QBuffer buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    buffer.close();

    QString dataUri = QStringLiteral("data:image/png;base64,") + ba.toBase64();
    m_cache.insert(materialName, dataUri);
    return dataUri;
}

void MaterialPreviewRenderer::clearCache()
{
    m_cache.clear();
}

QString MaterialPreviewRenderer::firstMaterialNameInFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    // Ogre .material script: lines like "material SomeName" or "material SomeName : ParentName"
    static const QRegularExpression rx(
        QStringLiteral(R"(^\s*material\s+(\S+))"),
        QRegularExpression::MultilineOption);

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        auto match = rx.match(line);
        if (match.hasMatch()) {
            return match.captured(1);
        }
    }

    return {};
}
