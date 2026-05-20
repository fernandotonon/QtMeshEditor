#include "ModelTurntableRenderer.h"

#include "Manager.h"

#include <QPainter>

#include <OgreHardwarePixelBuffer.h>
#include <OgreRoot.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <cmath>

namespace {

Ogre::SceneManager *sceneMgr()
{
    return Manager::getSingletonPtr() ? Manager::getSingleton()->getSceneMgr() : nullptr;
}

struct TurntableState {
    Ogre::Camera *camera = nullptr;
    Ogre::SceneNode *cameraNode = nullptr;
    Ogre::TexturePtr rttTexture;
    Ogre::RenderTarget *renderTarget = nullptr;
    int rttWidth = 0;
    int rttHeight = 0;
};

TurntableState &state()
{
    static TurntableState s;
    return s;
}

bool ensureRenderTarget(int width, int height, const Ogre::ColourValue &bg, QString *errorOut)
{
    auto *sm = sceneMgr();
    auto *root = Ogre::Root::getSingletonPtr();
    if (!sm || !root || !root->getRenderSystem()) {
        if (errorOut)
            *errorOut = QStringLiteral("Ogre is not initialized");
        return false;
    }

    TurntableState &st = state();
    if (st.renderTarget && st.rttWidth == width && st.rttHeight == height)
        return true;

    ModelTurntableRenderer::shutdown();

    try {
        st.rttTexture = Ogre::TextureManager::getSingleton().createManual(
            "ModelTurntableRTT", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, Ogre::TEX_TYPE_2D,
            static_cast<Ogre::uint32>(width), static_cast<Ogre::uint32>(height), 0, Ogre::PF_BYTE_RGBA,
            Ogre::TU_RENDERTARGET);
        st.renderTarget = st.rttTexture->getBuffer()->getRenderTarget();
        st.rttWidth = width;
        st.rttHeight = height;

        if (!st.camera) {
            st.camera = sm->createCamera("ModelTurntableCamera");
            st.camera->setNearClipDistance(0.01f);
            st.camera->setFarClipDistance(100000.0f);
            st.camera->setFOVy(Ogre::Degree(45.0f));
            st.cameraNode = sm->getRootSceneNode()->createChildSceneNode("ModelTurntableCameraNode");
            st.cameraNode->attachObject(st.camera);
        }

        if (st.renderTarget->getNumViewports() == 0) {
            Ogre::Viewport *vp = st.renderTarget->addViewport(st.camera);
            vp->setClearEveryFrame(true);
            vp->setBackgroundColour(bg);
            vp->setOverlaysEnabled(false);
            vp->setShadowsEnabled(true);
        } else {
            st.renderTarget->getViewport(0)->setBackgroundColour(bg);
        }

        const Ogre::Real aspect =
            height > 0 ? static_cast<Ogre::Real>(width) / static_cast<Ogre::Real>(height) : 1.0f;
        st.camera->setAspectRatio(aspect);
        return true;
    } catch (const Ogre::Exception &e) {
        ModelTurntableRenderer::shutdown();
        if (errorOut)
            *errorOut = QString::fromStdString(e.getFullDescription());
        return false;
    } catch (...) {
        ModelTurntableRenderer::shutdown();
        if (errorOut)
            *errorOut = QStringLiteral("Failed to create turntable render target");
        return false;
    }
}

Ogre::AxisAlignedBox combinedWorldBounds(const QList<Ogre::Entity *> &entities)
{
    Ogre::AxisAlignedBox box;
    box.setNull();
    for (Ogre::Entity *entity : entities) {
        if (!entity)
            continue;
        box.merge(entity->getWorldBoundingBox(true));
    }
    return box;
}

void placeCameraForYaw(Ogre::Entity * /*entity*/, const Ogre::AxisAlignedBox &bounds, float yawRadians,
                       float elevationRadians, float paddingFactor)
{
    TurntableState &st = state();
    if (!st.camera || !st.cameraNode || bounds.isNull() || bounds.isInfinite())
        return;

    const Ogre::Vector3 center = bounds.getCenter();
    Ogre::Real radius = (bounds.getMaximum() - bounds.getMinimum()).length() * 0.5f;
    if (radius < 0.1f)
        radius = 1.0f;

    const Ogre::Radian fovY = st.camera->getFOVy();
    const Ogre::Real aspect = st.camera->getAspectRatio();
    const Ogre::Radian fovX = Ogre::Radian(2.0f * std::atan(std::tan(fovY.valueRadians() * 0.5f) * aspect));
    const Ogre::Radian fov = std::min(fovX, fovY);
    Ogre::Real distance = radius / std::sin(fov.valueRadians() * 0.5f);
    distance *= paddingFactor;

    const float cosElev = std::cos(elevationRadians);
    const float sinElev = std::sin(elevationRadians);
    const Ogre::Vector3 offset(distance * cosElev * std::sin(yawRadians), distance * sinElev,
                               distance * cosElev * std::cos(yawRadians));

    st.cameraNode->setPosition(center + offset);
    st.cameraNode->lookAt(center, Ogre::Node::TS_WORLD);
}

QImage readRenderTarget(int width, int height)
{
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    TurntableState &st = state();
    if (!st.renderTarget)
        return image;

    Ogre::PixelBox pb(static_cast<Ogre::uint32>(width), static_cast<Ogre::uint32>(height), 1,
                      Ogre::PF_BYTE_RGBA, image.bits());
    st.renderTarget->copyContentsToMemory(Ogre::Box(0, 0, width, height), pb,
                                          Ogre::RenderTarget::FB_AUTO);
    return image;
}

} // namespace

void ModelTurntableRenderer::shutdown()
{
    TurntableState &st = state();
    auto *sm = sceneMgr();
    if (st.renderTarget) {
        st.renderTarget->removeAllViewports();
        st.renderTarget = nullptr;
    }
    if (st.rttTexture) {
        Ogre::TextureManager::getSingleton().remove(st.rttTexture);
        st.rttTexture.reset();
    }
    st.rttWidth = 0;
    st.rttHeight = 0;

    if (sm && st.camera) {
        if (st.cameraNode) {
            st.cameraNode->detachObject(st.camera);
            sm->destroySceneNode(st.cameraNode);
            st.cameraNode = nullptr;
        }
        sm->destroyCamera(st.camera);
        st.camera = nullptr;
    }
}

bool ModelTurntableRenderer::renderToImages(const QList<Ogre::Entity *> &entities,
                                            const TurntableOptions &options, QList<QImage> *outFrames,
                                            QString *errorOut)
{
    if (!outFrames) {
        if (errorOut)
            *errorOut = QStringLiteral("Output frame list is null");
        return false;
    }
    outFrames->clear();

    if (entities.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("No entities to render");
        return false;
    }

    const int width = std::max(16, options.width);
    const int height = std::max(16, options.height);
    const int frameCount = std::clamp(options.frameCount, 1, 360);

    if (!ensureRenderTarget(width, height, options.background, errorOut))
        return false;

    const Ogre::AxisAlignedBox bounds = combinedWorldBounds(entities);
    if (bounds.isNull() || bounds.isInfinite()) {
        if (errorOut)
            *errorOut = QStringLiteral("Could not compute model bounds");
        return false;
    }

    const float elevationRad =
        Ogre::Degree(std::clamp(options.elevationDegrees, -80.0f, 80.0f)).valueRadians();

    outFrames->reserve(frameCount);
    try {
        for (int i = 0; i < frameCount; ++i) {
            const float yaw = Ogre::Math::TWO_PI * static_cast<float>(i) / static_cast<float>(frameCount);
            placeCameraForYaw(entities.first(), bounds, yaw, elevationRad, 1.25f);
            state().renderTarget->update();
            outFrames->append(readRenderTarget(width, height));
        }
        return true;
    } catch (const Ogre::Exception &e) {
        outFrames->clear();
        if (errorOut)
            *errorOut = QString::fromStdString(e.getFullDescription());
        return false;
    } catch (...) {
        outFrames->clear();
        if (errorOut)
            *errorOut = QStringLiteral("Turntable render failed");
        return false;
    }
}

QImage ModelTurntableRenderer::composeSpriteSheet(const QList<QImage> &frames, int columns)
{
    if (frames.isEmpty())
        return {};

    const int frameW = frames.first().width();
    const int frameH = frames.first().height();
    const int count = frames.size();
    const int cols = columns > 0 ? columns : count;
    const int rows = (count + cols - 1) / cols;

    QImage sheet(cols * frameW, rows * frameH, QImage::Format_RGBA8888);
    sheet.fill(Qt::transparent);

    QPainter painter(&sheet);
    for (int i = 0; i < count; ++i) {
        const QImage &src = frames.at(i);
        if (src.width() != frameW || src.height() != frameH)
            continue;
        const int col = i % cols;
        const int row = i / cols;
        painter.drawImage(col * frameW, row * frameH, src);
    }
    return sheet;
}
