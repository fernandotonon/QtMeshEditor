#include "ModelTurntableRenderer.h"

#include "GlobalDefinitions.h"
#include "Manager.h"
#include "RTShaderHelper.h"
#include "SelectionSet.h"

#include <QPainter>

#include <OgreHardwarePixelBuffer.h>
#include <OgreRoot.h>
#include <OgreRTShaderSystem.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {

Ogre::SceneManager *sceneMgr()
{
  return Manager::getSingletonPtr() ? Manager::getSingleton()->getSceneMgr() : nullptr;
}

struct TurntableState {
  Ogre::Camera *camera = nullptr;
  Ogre::SceneNode *cameraNode = nullptr;
  Ogre::Light *light = nullptr;
  Ogre::SceneNode *lightNode = nullptr;
  Ogre::TexturePtr rttTexture;
  Ogre::RenderTarget *renderTarget = nullptr;
  int rttWidth = 0;
  int rttHeight = 0;
  Ogre::ColourValue savedAmbient;
  bool hasSavedAmbient = false;
};

TurntableState &state()
{
  static TurntableState s;
  return s;
}

void prepareSceneForCapture(const QList<Ogre::Entity *> &entities)
{
  SelectionSet::getSingleton()->clear();

  for (Ogre::Entity *entity : entities) {
    if (!entity)
      continue;
    if (Ogre::SceneNode *node = entity->getParentSceneNode())
      node->showBoundingBox(false);
  }
}

void applyTurntableLighting(Ogre::SceneManager *sm)
{
  TurntableState &st = state();
  st.savedAmbient = sm->getAmbientLight();
  st.hasSavedAmbient = true;
  sm->setAmbientLight(Ogre::ColourValue(1.0f, 1.0f, 1.0f));

  if (!st.light) {
    st.light = sm->createLight("ModelTurntableLight");
    st.light->setType(Ogre::Light::LT_DIRECTIONAL);
    st.lightNode = sm->getRootSceneNode()->createChildSceneNode("ModelTurntableLightNode");
    st.lightNode->attachObject(st.light);
  }
  st.light->setDiffuseColour(0.85f, 0.85f, 0.85f);
  st.light->setSpecularColour(0.35f, 0.35f, 0.35f);
  st.lightNode->setDirection(Ogre::Vector3(-0.35f, -0.85f, -0.4f).normalisedCopy());
}

void restoreTurntableLighting(Ogre::SceneManager *sm)
{
  TurntableState &st = state();
  if (st.hasSavedAmbient) {
    sm->setAmbientLight(st.savedAmbient);
    st.hasSavedAmbient = false;
  }
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
  if (st.renderTarget && st.rttWidth == width && st.rttHeight == height) {
    if (st.renderTarget->getNumViewports() > 0) {
      Ogre::Viewport *vp = st.renderTarget->getViewport(0);
      vp->setMaterialScheme(Ogre::MSN_SHADERGEN);
      vp->setVisibilityMask(SCENE_VISIBILITY_FLAGS);
    }
    return true;
  }

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
      vp->setVisibilityMask(SCENE_VISIBILITY_FLAGS);
      // Match the main editor viewport so RTSS normal maps / PBR are used
      // instead of FFP multi-texture blending (normal map as a flat layer).
      vp->setMaterialScheme(Ogre::MSN_SHADERGEN);
    } else {
      Ogre::Viewport *vp = st.renderTarget->getViewport(0);
      vp->setBackgroundColour(bg);
      vp->setVisibilityMask(SCENE_VISIBILITY_FLAGS);
      vp->setMaterialScheme(Ogre::MSN_SHADERGEN);
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

void refreshEntityBounds(const QList<Ogre::Entity *> &entities)
{
  for (Ogre::Entity *entity : entities) {
    if (!entity)
      continue;
    if (Ogre::SceneNode *node = entity->getParentSceneNode())
      node->_update(true, true);
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

/// Move loaded entities so the combined bounds center sits at the world origin.
void recenterEntitiesAtOrigin(const QList<Ogre::Entity *> &entities, Ogre::AxisAlignedBox &bounds)
{
  if (bounds.isNull() || bounds.isInfinite())
    return;

  const Ogre::Vector3 center = bounds.getCenter();
  if (center.squaredLength() < 1e-10f)
    return;

  for (Ogre::Entity *entity : entities) {
    if (!entity)
      continue;
    if (Ogre::SceneNode *node = entity->getParentSceneNode())
      node->translate(-center, Ogre::Node::TS_WORLD);
  }

  bounds.setExtents(bounds.getMinimum() - center, bounds.getMaximum() - center);
  refreshEntityBounds(entities);
}

Ogre::Vector3 orbitAxisVector(TurntableAxis axis)
{
  switch (axis) {
  case TurntableAxis::X:
    return Ogre::Vector3::UNIT_X;
  case TurntableAxis::Z:
    return Ogre::Vector3::UNIT_Z;
  case TurntableAxis::Y:
  default:
    return Ogre::Vector3::UNIT_Y;
  }
}

/// Camera rest offset before orbit rotation (orbit applied around `axis` through center).
Ogre::Vector3 cameraRestOffset(TurntableAxis axis, float horizDistance, float axialDistance)
{
  switch (axis) {
  case TurntableAxis::X:
    return Ogre::Vector3(axialDistance, 0.0f, horizDistance);
  case TurntableAxis::Z:
    return Ogre::Vector3(horizDistance, 0.0f, axialDistance);
  case TurntableAxis::Y:
  default:
    return Ogre::Vector3(0.0f, axialDistance, horizDistance);
  }
}

/// Ogre cameras look down local -Z; keep world +Y as up so the horizon stays level.
void orientCameraToward(Ogre::SceneNode *cameraNode, const Ogre::Vector3 &eye, const Ogre::Vector3 &target,
                        const Ogre::Vector3 &worldUp)
{
  Ogre::Vector3 forward = target - eye;
  if (forward.squaredLength() < 1e-8f)
    return;
  forward.normalise();

  Ogre::Vector3 side = worldUp.crossProduct(forward);
  if (side.squaredLength() < 1e-8f) {
    // Looking straight up/down — pick a fallback up.
    side = Ogre::Vector3::UNIT_X.crossProduct(forward);
  }
  side.normalise();
  const Ogre::Vector3 up = forward.crossProduct(side);

  Ogre::Matrix3 rot;
  rot.FromAxes(side, up, -forward);
  cameraNode->setOrientation(Ogre::Quaternion(rot));
}

/// Minimum orbit radius so every AABB corner fits in the camera frustum from `viewDir`.
Ogre::Real fitOrbitDistance(const Ogre::AxisAlignedBox &bounds, const Ogre::Vector3 &viewDir,
                            Ogre::Camera *camera, float paddingFactor)
{
  const Ogre::Vector3 center = bounds.getCenter();
  Ogre::Vector3 dir = viewDir;
  if (dir.squaredLength() < 1e-8f)
    dir = Ogre::Vector3(0.0f, 0.0f, 1.0f);
  dir.normalise();

  Ogre::Vector3 up = Ogre::Vector3::UNIT_Y;
  Ogre::Vector3 side = up.crossProduct(dir);
  if (side.squaredLength() < 1e-8f) {
    up = Ogre::Vector3::UNIT_X;
    side = up.crossProduct(dir);
  }
  side.normalise();
  up = dir.crossProduct(side);
  up.normalise();

  const Ogre::Radian fovY = camera->getFOVy();
  const Ogre::Real aspect = camera->getAspectRatio();
  const float tanHalfY = std::tan(fovY.valueRadians() * 0.5f);
  const float tanHalfX = tanHalfY * aspect;

  const Ogre::Vector3 &bmin = bounds.getMinimum();
  const Ogre::Vector3 &bmax = bounds.getMaximum();
  Ogre::Real required = 0.1f;
  for (int xi = 0; xi < 2; ++xi) {
    for (int yi = 0; yi < 2; ++yi) {
      for (int zi = 0; zi < 2; ++zi) {
        const Ogre::Vector3 corner(xi ? bmax.x : bmin.x, yi ? bmax.y : bmin.y, zi ? bmax.z : bmin.z);
        const Ogre::Vector3 rel = corner - center;
        const float depthAlongView = rel.dotProduct(dir);
        const float x = std::abs(rel.dotProduct(side));
        const float y = std::abs(rel.dotProduct(up));
        const float need = depthAlongView + std::max(x / tanHalfX, y / tanHalfY);
        required = std::max(required, need);
      }
    }
  }
  return required * paddingFactor;
}

void placeCameraOnAxis(const Ogre::AxisAlignedBox &bounds, float angleRadians, TurntableAxis axis,
                       float elevationRadians, float paddingFactor)
{
  TurntableState &st = state();
  if (!st.camera || !st.cameraNode || bounds.isNull() || bounds.isInfinite())
    return;

  const Ogre::Vector3 center = bounds.getCenter();

  const float horizUnit = std::cos(elevationRadians);
  const float axialUnit = std::sin(elevationRadians);
  Ogre::Vector3 restDir = cameraRestOffset(axis, horizUnit, axialUnit);
  if (restDir.squaredLength() < 1e-8f)
    restDir = cameraRestOffset(axis, 1.0f, 0.0f);
  restDir.normalise();

  const Ogre::Real distance = fitOrbitDistance(bounds, restDir, st.camera, paddingFactor);
  const Ogre::Vector3 localOffset = cameraRestOffset(axis, distance * horizUnit, distance * axialUnit);
  const Ogre::Quaternion orbit(Ogre::Radian(angleRadians), orbitAxisVector(axis));
  const Ogre::Vector3 eye = center + orbit * localOffset;

  st.cameraNode->setPosition(eye);
  orientCameraToward(st.cameraNode, eye, center, Ogre::Vector3::UNIT_Y);
}

std::string normalMapTextureName(const Ogre::MaterialPtr &mat)
{
  if (!mat || mat->getNumTechniques() == 0)
    return {};
  Ogre::Pass *pass = mat->getTechnique(0)->getPass(0);
  if (!pass)
    return {};
  for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
    Ogre::TextureUnitState *tus = pass->getTextureUnitState(i);
    const Ogre::String &slot = tus->getName();
    if ((slot == "normal_map" || slot == "NormalMap") && !tus->getTextureName().empty())
      return tus->getTextureName();
  }
  return {};
}

void prepareRtssMaterials(const QList<Ogre::Entity *> &entities)
{
  auto *shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
  if (!shaderGen)
    return;

  std::unordered_set<std::string> processed;
  for (Ogre::Entity *entity : entities) {
    if (!entity)
      continue;
    for (unsigned int sub = 0; sub < entity->getNumSubEntities(); ++sub) {
      Ogre::MaterialPtr mat = entity->getSubEntity(sub)->getMaterial();
      if (!mat)
        continue;
      const std::string key = mat->getName();
      if (!processed.insert(key).second)
        continue;

      RTShaderHelper::wirePbrSlotsForFFP(mat.get());
      if (RTShaderHelper::applyPbrIfTagged(mat)) {
        mat->compile();
        continue;
      }

      const std::string normalTex = normalMapTextureName(mat);
      if (!normalTex.empty()) {
        RTShaderHelper::applyNormalMap(mat, normalTex);
      } else {
        shaderGen->createShaderBasedTechnique(
            *mat, Ogre::MaterialManager::DEFAULT_SCHEME_NAME,
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
        shaderGen->validateMaterial(Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat);
      }
      mat->compile();
    }
  }
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

bool ModelTurntableRenderer::parseAxis(const QString &text, TurntableAxis *outAxis)
{
  if (!outAxis)
    return false;
  const QString key = text.trimmed().toLower();
  if (key == QLatin1String("y")) {
    *outAxis = TurntableAxis::Y;
    return true;
  }
  if (key == QLatin1String("x")) {
    *outAxis = TurntableAxis::X;
    return true;
  }
  if (key == QLatin1String("z")) {
    *outAxis = TurntableAxis::Z;
    return true;
  }
  return false;
}

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

  if (sm) {
    if (st.hasSavedAmbient) {
      sm->setAmbientLight(st.savedAmbient);
      st.hasSavedAmbient = false;
    }
    if (st.light) {
      if (st.lightNode) {
        st.lightNode->detachObject(st.light);
        sm->destroySceneNode(st.lightNode);
        st.lightNode = nullptr;
      }
      sm->destroyLight(st.light);
      st.light = nullptr;
    }
    if (st.camera) {
      if (st.cameraNode) {
        st.cameraNode->detachObject(st.camera);
        sm->destroySceneNode(st.cameraNode);
        st.cameraNode = nullptr;
      }
      sm->destroyCamera(st.camera);
      st.camera = nullptr;
    }
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

  auto *sm = sceneMgr();
  if (!sm) {
    if (errorOut)
      *errorOut = QStringLiteral("Ogre scene manager is not available");
    return false;
  }

  const int width = std::max(16, options.width);
  const int height = std::max(16, options.height);
  const int frameCount = std::clamp(options.frameCount, 1, 360);

  if (!ensureRenderTarget(width, height, options.background, errorOut))
    return false;

  refreshEntityBounds(entities);
  Ogre::AxisAlignedBox bounds = combinedWorldBounds(entities);
  if (bounds.isNull() || bounds.isInfinite()) {
    if (errorOut)
      *errorOut = QStringLiteral("Could not compute model bounds");
    return false;
  }

  recenterEntitiesAtOrigin(entities, bounds);
  bounds = combinedWorldBounds(entities);

  const float elevationRad =
      Ogre::Degree(std::clamp(options.elevationDegrees, -80.0f, 80.0f)).valueRadians();

  prepareSceneForCapture(entities);
  prepareRtssMaterials(entities);
  applyTurntableLighting(sm);

  outFrames->reserve(frameCount);
  try {
    for (int i = 0; i < frameCount; ++i) {
      const float angle = Ogre::Math::TWO_PI * static_cast<float>(i) / static_cast<float>(frameCount);
      placeCameraOnAxis(bounds, angle, options.axis, elevationRad, 1.25f);
      state().renderTarget->update();
      outFrames->append(readRenderTarget(width, height));
    }
    restoreTurntableLighting(sm);
    return true;
  } catch (const Ogre::Exception &e) {
    outFrames->clear();
    restoreTurntableLighting(sm);
    if (errorOut)
      *errorOut = QString::fromStdString(e.getFullDescription());
    return false;
  } catch (...) {
    outFrames->clear();
    restoreTurntableLighting(sm);
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
