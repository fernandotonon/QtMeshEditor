#include "MeshDepthRenderer.h"

#include "Manager.h"

#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>
#include <OgreViewport.h>

#include <cmath>

namespace {

Ogre::SceneManager* sceneMgr()
{
    return Manager::getSingletonPtr() ? Manager::getSingleton()->getSceneMgr() : nullptr;
}

struct DepthState {
    Ogre::Camera*       camera = nullptr;
    Ogre::SceneNode*    cameraNode = nullptr;
    Ogre::TexturePtr    rttTexture;
    Ogre::RenderTarget* renderTarget = nullptr;
    int                 size = 0;
};

DepthState& state()
{
    static DepthState s;
    return s;
}

// A flat self-illuminated white material. Combined with linear fog
// (fog colour = black), white geometry fades to black with distance
// — a depth gradient with no custom shaders. Created once, reused.
const char* kDepthMaterialName = "QtMesh/DepthControlNet";

Ogre::MaterialPtr ensureDepthMaterial()
{
    auto& mm = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr mat = mm.getByName(kDepthMaterialName);
    if (mat) return mat;

    mat = mm.create(kDepthMaterialName,
                    Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(true);
    // Pure white self-illumination so the surface colour is depth-
    // independent before fog is applied.
    pass->setSelfIllumination(1.0f, 1.0f, 1.0f);
    pass->setDiffuse(0, 0, 0, 1);
    pass->setSpecular(0, 0, 0, 1);
    pass->setAmbient(0, 0, 0);
    // Fog must affect this pass.
    pass->setFog(false);  // false => inherit scene fog
    return mat;
}

bool ensureRenderTarget(int size, QString* errorOut)
{
    auto* sm = sceneMgr();
    auto* root = Ogre::Root::getSingletonPtr();
    if (!sm || !root || !root->getRenderSystem()) {
        if (errorOut) *errorOut = QStringLiteral("Ogre is not initialized");
        return false;
    }

    DepthState& st = state();
    if (st.renderTarget && st.size == size)
        return true;

    MeshDepthRenderer::shutdown();

    st.rttTexture = Ogre::TextureManager::getSingleton().createManual(
        "QtMeshDepthRTT", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_2D,
        static_cast<Ogre::uint32>(size), static_cast<Ogre::uint32>(size), 0,
        Ogre::PF_BYTE_RGBA, Ogre::TU_RENDERTARGET);
    st.renderTarget = st.rttTexture->getBuffer()->getRenderTarget();
    st.size = size;

    if (!st.camera) {
        st.camera = sm->createCamera("QtMeshDepthCamera");
        st.camera->setNearClipDistance(0.01f);
        st.camera->setFarClipDistance(100000.0f);
        st.camera->setFOVy(Ogre::Degree(45.0f));
        st.cameraNode = sm->getRootSceneNode()->createChildSceneNode("QtMeshDepthCameraNode");
        st.cameraNode->attachObject(st.camera);
    }

    if (st.renderTarget->getNumViewports() == 0) {
        Ogre::Viewport* vp = st.renderTarget->addViewport(st.camera);
        vp->setClearEveryFrame(true);
        vp->setBackgroundColour(Ogre::ColourValue::Black);
        vp->setOverlaysEnabled(false);
        vp->setShadowsEnabled(false);
        vp->setSkiesEnabled(false);
    }
    return true;
}

QImage readRenderTarget(int size)
{
    DepthState& st = state();
    QImage img(size, size, QImage::Format_RGBA8888);
    Ogre::PixelBox pb(static_cast<Ogre::uint32>(size),
                      static_cast<Ogre::uint32>(size), 1,
                      Ogre::PF_BYTE_RGBA, img.bits());
    st.renderTarget->copyContentsToMemory(pb, Ogre::RenderTarget::FB_AUTO);
    return img;
}

} // namespace

QImage MeshDepthRenderer::renderDepthMap(Ogre::Entity* entity, int size,
                                         QString* errorOut)
{
    if (!entity) {
        if (errorOut) *errorOut = QStringLiteral("null entity");
        return QImage();
    }
    size = std::clamp(size, 64, 2048);
    if (!ensureRenderTarget(size, errorOut))
        return QImage();

    auto* sm = sceneMgr();
    DepthState& st = state();

    // Frame the camera on the entity's bounding sphere so the whole
    // mesh fills the view (matches how the generated texture is
    // planar-projected back).
    const Ogre::AxisAlignedBox aabb = entity->getWorldBoundingBox(true);
    const Ogre::Vector3 center = aabb.getCenter();
    const Ogre::Real radius = aabb.getHalfSize().length();
    if (radius <= 0.0f) {
        if (errorOut) *errorOut = QStringLiteral("entity has zero-size bounding box");
        return QImage();
    }
    // Distance so the sphere fits in the 45° vertical FOV.
    const Ogre::Real fovY = st.camera->getFOVy().valueRadians();
    const Ogre::Real dist = radius / std::sin(fovY * 0.5f) * 1.15f;  // 15% margin
    const Ogre::Vector3 camPos = center + Ogre::Vector3(0, 0, dist);
    st.cameraNode->setPosition(camPos);
    // Ogre 14: orient the node, not the camera. Look from camPos
    // toward the entity center.
    st.cameraNode->lookAt(center, Ogre::Node::TS_WORLD,
                          Ogre::Vector3::NEGATIVE_UNIT_Z);

    // Linear fog spanning the near/far faces of the bounding sphere
    // gives a depth gradient: surfaces at (dist - radius) → white,
    // (dist + radius) → black.
    const Ogre::ColourValue savedFogColour = sm->getFogColour();
    const Ogre::FogMode savedFogMode = sm->getFogMode();
    const Ogre::Real savedFogStart = sm->getFogStart();
    const Ogre::Real savedFogEnd = sm->getFogEnd();
    const Ogre::ColourValue savedAmbient = sm->getAmbientLight();

    sm->setFog(Ogre::FOG_LINEAR, Ogre::ColourValue::Black,
               0.0f, dist - radius, dist + radius);
    sm->setAmbientLight(Ogre::ColourValue::White);

    // Swap the entity's materials to the flat white depth material,
    // remembering the originals so we can restore them.
    Ogre::MaterialPtr depthMat = ensureDepthMaterial();
    std::vector<Ogre::String> savedMaterials;
    savedMaterials.reserve(entity->getNumSubEntities());
    for (unsigned int i = 0; i < entity->getNumSubEntities(); ++i) {
        Ogre::SubEntity* se = entity->getSubEntity(i);
        savedMaterials.push_back(se->getMaterialName());
        se->setMaterial(depthMat);
    }

    // Render a single frame.
    st.renderTarget->update();
    QImage rgba = readRenderTarget(size);

    // Restore entity materials + scene fog/ambient.
    for (unsigned int i = 0; i < entity->getNumSubEntities(); ++i)
        entity->getSubEntity(i)->setMaterialName(savedMaterials[i]);
    sm->setFog(savedFogMode, savedFogColour, 0.0f, savedFogStart, savedFogEnd);
    sm->setAmbientLight(savedAmbient);

    // Collapse to grayscale (the channels are equal already, but be
    // explicit so downstream code can rely on it).
    return rgba.convertToFormat(QImage::Format_Grayscale8)
        .convertToFormat(QImage::Format_RGB888);
}

void MeshDepthRenderer::shutdown()
{
    DepthState& st = state();
    auto* sm = sceneMgr();
    if (st.renderTarget) {
        st.renderTarget->removeAllViewports();
        st.renderTarget = nullptr;
    }
    if (st.rttTexture) {
        Ogre::TextureManager::getSingleton().remove(st.rttTexture);
        st.rttTexture.reset();
    }
    if (sm && st.cameraNode) {
        st.cameraNode->detachAllObjects();
        sm->getRootSceneNode()->removeAndDestroyChild(st.cameraNode->getName());
        st.cameraNode = nullptr;
    }
    if (sm && st.camera) {
        sm->destroyCamera(st.camera);
        st.camera = nullptr;
    }
    st.size = 0;
}
