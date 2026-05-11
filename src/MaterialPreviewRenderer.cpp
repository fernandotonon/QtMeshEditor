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

#include "ProceduralBoxGenerator.h"
#include "ProceduralPlaneGenerator.h"
#include "ProceduralSphereGenerator.h"

#include <algorithm>
#include <cmath>

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
            // Remove the render textures first.
            if (m_rttTexture) {
                Ogre::TextureManager::getSingleton().remove(m_rttTexture);
                m_rttTexture.reset();
            }
            if (m_interactiveRtt) {
                Ogre::TextureManager::getSingleton().remove(m_interactiveRtt);
                m_interactiveRtt.reset();
                m_interactiveRenderTarget = nullptr;
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
    auto* root = Ogre::Root::getSingletonPtr();
    if (!root || !root->getRenderSystem()) {
        // Ogre torn down underneath us (e.g. Manager::kill() in tests).
        // Clear our cached pointers so we don't dereference them later.
        m_sceneMgr = nullptr;
        m_camera = nullptr;
        m_light = nullptr;
        m_lightNode = nullptr;
        m_sphere = nullptr;
        m_sphereNode = nullptr;
        m_rttTexture.reset();
        m_renderTarget = nullptr;
        m_interactiveRtt.reset();
        m_interactiveRenderTarget = nullptr;
        m_initialized = false;
        return false;
    }

    // The scene manager pointer can be left dangling when Ogre::Root
    // is destroyed and re-created (test fixtures Manager::kill()).
    // Treat "ours is not registered with Root" as "not initialised".
    if (m_initialized && m_sceneMgr && !root->hasSceneManager("MaterialPreviewSM")) {
        m_sceneMgr = nullptr;
        m_camera = nullptr;
        m_light = nullptr;
        m_lightNode = nullptr;
        m_sphere = nullptr;
        m_sphereNode = nullptr;
        m_rttTexture.reset();
        m_renderTarget = nullptr;
        m_interactiveRtt.reset();
        m_interactiveRenderTarget = nullptr;
        m_initialized = false;
    }

    if (m_initialized)
        return true;

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

        // Directional light from upper-right. The light is attached to a
        // dedicated scene node so renderInteractivePreview can rotate it
        // (yaw) to simulate orbiting the environment around the model.
        m_light = m_sceneMgr->createLight("PreviewLight");
        m_light->setType(Ogre::Light::LT_DIRECTIONAL);
        m_light->setDiffuseColour(0.8f, 0.8f, 0.8f);
        m_light->setSpecularColour(1.0f, 1.0f, 1.0f);
        m_lightNode = m_sceneMgr->getRootSceneNode()->createChildSceneNode();
        m_lightNode->attachObject(m_light);
        m_lightNode->setDirection(Ogre::Vector3(-1, -1, -1).normalisedCopy());

        // Create the default sphere mesh via the shared helper. The
        // entity defaults to ShapeSphere; renderInteractivePreview can
        // switch it on demand.
        const Ogre::String meshName = ensureShapeMesh(ShapeSphere);

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

Ogre::String MaterialPreviewRenderer::ensureShapeMesh(Shape shape)
{
    // Lazily create the procedural mesh for each preview shape. Names
    // are stable so subsequent calls reuse the same MeshManager entry.
    auto& meshMgr = Ogre::MeshManager::getSingleton();
    switch (shape) {
        case ShapeCube: {
            const Ogre::String name = "__MaterialPreviewCube__";
            if (!meshMgr.resourceExists(name)) {
                Procedural::BoxGenerator()
                    .setSizeX(1.6f).setSizeY(1.6f).setSizeZ(1.6f)
                    .setNumSegX(1).setNumSegY(1).setNumSegZ(1)
                    .setUTile(1.0f).setVTile(1.0f)
                    .realizeMesh(name);
            }
            return name;
        }
        case ShapePlane: {
            const Ogre::String name = "__MaterialPreviewPlane__";
            if (!meshMgr.resourceExists(name)) {
                // Slight tilt is applied per-render via the entity node
                // so a flat textured-plane reads as a material sample
                // rather than a solid square.
                Procedural::PlaneGenerator()
                    .setSizeX(2.0f).setSizeY(2.0f)
                    .setNumSegX(1).setNumSegY(1)
                    .setUTile(1.0f).setVTile(1.0f)
                    .realizeMesh(name);
            }
            return name;
        }
        case ShapeSphere:
        default: {
            const Ogre::String name = "__MaterialPreviewSphere__";
            if (!meshMgr.resourceExists(name)) {
                Procedural::SphereGenerator()
                    .setRadius(1.0f)
                    .setUTile(1.0f)
                    .setVTile(1.0f)
                    .setNumRings(16)
                    .setNumSegments(16)
                    .realizeMesh(name);
            }
            return name;
        }
    }
}

void MaterialPreviewRenderer::resetToCanonicalThumbnailState()
{
    // Swap the preview entity back to the sphere mesh if the
    // interactive path left it on a Cube/Plane. The entity has to be
    // destroyed and recreated against the same scene node — Ogre
    // doesn't expose a "change mesh" on an attached Entity.
    if (!m_sceneMgr || !m_sphereNode) return;
    const Ogre::String sphereMesh = ensureShapeMesh(ShapeSphere);
    if (!m_sphere || m_interactiveCurrentShape != ShapeSphere) {
        if (m_sphere) {
            m_sphereNode->detachObject(m_sphere);
            m_sceneMgr->destroyEntity(m_sphere);
            m_sphere = nullptr;
        }
        m_sphere = m_sceneMgr->createEntity("PreviewSphereEntity", sphereMesh);
        m_sphereNode->attachObject(m_sphere);
        m_interactiveCurrentShape = ShapeSphere;
    }
    // The plane preview tilts the entity node; reset to identity so
    // the sphere renders straight-on as the cached preview expects.
    m_sphereNode->setOrientation(Ogre::Quaternion::IDENTITY);
    if (m_lightNode) {
        m_lightNode->setDirection(Ogre::Vector3(-1, -1, -1).normalisedCopy());
    }
}

QImage MaterialPreviewRenderer::renderPreview(const QString& materialName)
{
    if (!ensureScene())
        return {};

    // Slice I: the interactive preview shares the same Ogre scene
    // (entity, node, light). Restore the canonical "Sphere + default
    // light" pose so the thumbnail cache always reflects that state.
    resetToCanonicalThumbnailState();

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

QString MaterialPreviewRenderer::renderInteractivePreview(const QString& materialName,
                                                            int size,
                                                            int shape,
                                                            double yawDegrees)
{
    if (!ensureScene())
        return {};

    auto* matMgr = Ogre::MaterialManager::getSingletonPtr();
    if (!matMgr) return {};
    const std::string stdName = materialName.toStdString();
    if (!matMgr->resourceExists(stdName)) return {};

    // Clamp size to a sane band: too small wastes pixels, too large
    // burns frame time for a docked preview.
    const int cappedSize = std::clamp(size, 32, 1024);

    // Allocate / resize the interactive render target when the requested
    // size changes. Reuses the existing texture when the size matches.
    try {
        if (!m_interactiveRtt || m_interactiveSize != cappedSize) {
            if (m_interactiveRtt) {
                Ogre::TextureManager::getSingleton().remove(m_interactiveRtt);
                m_interactiveRtt.reset();
                m_interactiveRenderTarget = nullptr;
            }
            m_interactiveRtt = Ogre::TextureManager::getSingleton().createManual(
                "MatPreviewInteractiveRTT",
                Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                Ogre::TEX_TYPE_2D,
                cappedSize, cappedSize, 0,
                Ogre::PF_BYTE_RGBA, Ogre::TU_RENDERTARGET);
            m_interactiveRenderTarget = m_interactiveRtt->getBuffer()->getRenderTarget();
            Ogre::Viewport* vp = m_interactiveRenderTarget->addViewport(m_camera);
            vp->setClearEveryFrame(true);
            vp->setBackgroundColour(Ogre::ColourValue(0.12f, 0.12f, 0.13f, 1.0f));
            vp->setOverlaysEnabled(false);
            m_interactiveSize = cappedSize;
        }

        // Swap the sphere entity's mesh when the user picks a different
        // shape. We avoid recreating the entity (which churns the scene
        // node hierarchy) — destroying and recreating against the same
        // node is the cleanest way Ogre supports changing the mesh.
        const Shape requestedShape =
            (shape == ShapeCube)  ? ShapeCube  :
            (shape == ShapePlane) ? ShapePlane :
                                    ShapeSphere;
        if (requestedShape != m_interactiveCurrentShape || !m_sphere) {
            const Ogre::String meshName = ensureShapeMesh(requestedShape);
            if (m_sphere) {
                m_sphereNode->detachObject(m_sphere);
                m_sceneMgr->destroyEntity(m_sphere);
                m_sphere = nullptr;
            }
            m_sphere = m_sceneMgr->createEntity("PreviewSphereEntity", meshName);
            m_sphereNode->attachObject(m_sphere);
            m_interactiveCurrentShape = requestedShape;
        }

        // Tilt the plane slightly so a flat-textured material reads as
        // a material sample rather than an opaque rectangle.
        if (requestedShape == ShapePlane) {
            m_sphereNode->setOrientation(Ogre::Quaternion(
                Ogre::Radian(Ogre::Math::PI * -0.25f), Ogre::Vector3::UNIT_X));
        } else {
            m_sphereNode->setOrientation(Ogre::Quaternion::IDENTITY);
        }

        m_sphere->setMaterialName(stdName);

        // Apply environment yaw — rotate the light around the world
        // up-axis so the model appears illuminated from a different
        // angle. Modulo to [0, 360) so the cache key is stable.
        const double wrappedYaw = std::fmod(std::fmod(yawDegrees, 360.0) + 360.0, 360.0);
        const Ogre::Radian yaw(Ogre::Degree(static_cast<float>(wrappedYaw)));
        Ogre::Vector3 baseDir = Ogre::Vector3(-1, -1, -1).normalisedCopy();
        Ogre::Quaternion rot(yaw, Ogre::Vector3::UNIT_Y);
        m_lightNode->setDirection((rot * baseDir).normalisedCopy());

        m_interactiveRenderTarget->update();

        QImage image(cappedSize, cappedSize, QImage::Format_RGBA8888);
        Ogre::PixelBox pb(cappedSize, cappedSize, 1, Ogre::PF_BYTE_RGBA, image.bits());
        m_interactiveRenderTarget->copyContentsToMemory(
            Ogre::Box(0, 0, cappedSize, cappedSize), pb,
            Ogre::RenderTarget::FB_AUTO);

        QByteArray ba;
        QBuffer buf(&ba);
        buf.open(QIODevice::WriteOnly);
        if (!image.save(&buf, "PNG")) return {};
        return QStringLiteral("data:image/png;base64,") + ba.toBase64();
    } catch (const Ogre::Exception&) {
        return {};
    } catch (...) {
        return {};
    }
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
