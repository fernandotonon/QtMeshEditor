#include "ModelTurntableRenderer.h"

#include "GlobalDefinitions.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "RTShaderHelper.h"
#include "OgreRenderTargetUtil.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <QPainter>

#include <OgreHardwarePixelBuffer.h>
#include <OgreRoot.h>
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
  Ogre::SceneNode *pivotNode = nullptr;
  Ogre::Camera *camera = nullptr;
  Ogre::SceneNode *cameraNode = nullptr;
  Ogre::Light *light = nullptr;
  Ogre::SceneNode *lightNode = nullptr;
  Ogre::Light *fillLight = nullptr;
  Ogre::SceneNode *fillLightNode = nullptr;
  Ogre::Light *rimLight = nullptr;
  Ogre::SceneNode *rimLightNode = nullptr;
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

void applyTurntableLighting(Ogre::SceneManager *sm, bool studio)
{
  TurntableState &st = state();
  st.savedAmbient = sm->getAmbientLight();
  st.hasSavedAmbient = true;
  // #933: DCC-style shaded default. Full-white ambient rendered untextured
  // models as flat silhouettes (zero shading); a low ambient + key + fill
  // gives shaped gray renders — like every DCC's default viewport.
  sm->setAmbientLight(studio ? Ogre::ColourValue(0.25f, 0.25f, 0.27f)
                             : Ogre::ColourValue(0.35f, 0.35f, 0.36f));

  if (!st.light) {
    st.light = sm->createLight("ModelTurntableLight");
    st.light->setType(Ogre::Light::LT_DIRECTIONAL);
    st.lightNode = sm->getRootSceneNode()->createChildSceneNode("ModelTurntableLightNode");
    st.lightNode->attachObject(st.light);
  }
  // Key: from the upper camera-left, slightly warm in studio mode.
  st.light->setDiffuseColour(studio ? Ogre::ColourValue(1.0f, 0.98f, 0.94f)
                                    : Ogre::ColourValue(0.9f, 0.9f, 0.9f));
  st.light->setSpecularColour(0.35f, 0.35f, 0.35f);
  st.lightNode->setDirection(Ogre::Vector3(-0.35f, -0.85f, -0.4f).normalisedCopy());

  if (!st.fillLight) {
    st.fillLight = sm->createLight("ModelTurntableFillLight");
    st.fillLight->setType(Ogre::Light::LT_DIRECTIONAL);
    st.fillLightNode = sm->getRootSceneNode()->createChildSceneNode("ModelTurntableFillLightNode");
    st.fillLightNode->attachObject(st.fillLight);
  }
  // Fill: from the opposite lower side, cool in studio mode, no speculars.
  st.fillLight->setDiffuseColour(studio ? Ogre::ColourValue(0.30f, 0.33f, 0.38f)
                                        : Ogre::ColourValue(0.35f, 0.35f, 0.37f));
  st.fillLight->setSpecularColour(0.0f, 0.0f, 0.0f);
  st.fillLightNode->setDirection(Ogre::Vector3(0.6f, -0.15f, 0.65f).normalisedCopy());

  if (!st.rimLight) {
    st.rimLight = sm->createLight("ModelTurntableRimLight");
    st.rimLight->setType(Ogre::Light::LT_DIRECTIONAL);
    st.rimLightNode = sm->getRootSceneNode()->createChildSceneNode("ModelTurntableRimLightNode");
    st.rimLightNode->attachObject(st.rimLight);
  }
  // Rim: from behind/above, separates the silhouette — studio preset only.
  st.rimLight->setDiffuseColour(studio ? Ogre::ColourValue(0.5f, 0.5f, 0.55f)
                                       : Ogre::ColourValue(0.0f, 0.0f, 0.0f));
  st.rimLight->setSpecularColour(studio ? Ogre::ColourValue(0.4f, 0.4f, 0.45f)
                                        : Ogre::ColourValue(0.0f, 0.0f, 0.0f));
  st.rimLightNode->setDirection(Ogre::Vector3(0.15f, -0.35f, 0.92f).normalisedCopy());
  st.rimLight->setVisible(studio);
  // The RTSS shaders bake the light configuration — regenerate so the fill/
  // rim actually contribute (the take_screenshot lesson, #933).
  RTShaderHelper::invalidateShadergenScheme();
}

void restoreTurntableLighting(Ogre::SceneManager *sm)
{
  TurntableState &st = state();
  if (st.hasSavedAmbient) {
    sm->setAmbientLight(st.savedAmbient);
    st.hasSavedAmbient = false;
  }
  RTShaderHelper::invalidateShadergenScheme();
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
      vp->setBackgroundColour(bg);
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
    OgreRenderTargetUtil::configureOffscreenRenderTarget(st.renderTarget);
    st.rttWidth = width;
    st.rttHeight = height;

    if (!st.camera) {
      st.camera = sm->createCamera("ModelTurntableCamera");
      st.camera->setNearClipDistance(0.01f);
      st.camera->setFarClipDistance(100000.0f);
      st.camera->setFOVy(Ogre::Degree(45.0f));
      st.pivotNode = sm->getRootSceneNode()->createChildSceneNode("ModelTurntablePivot");
      st.cameraNode = st.pivotNode->createChildSceneNode("ModelTurntableCameraNode");
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

  std::unordered_set<Ogre::SceneNode *> shifted;
  for (Ogre::Entity *entity : entities) {
    if (!entity)
      continue;
    Ogre::SceneNode *node = entity->getParentSceneNode();
    if (!node || !shifted.insert(node).second)
      continue;
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

/// Look-at / orbit point. After recentering this is near the origin; a small upward
/// bias keeps typical upright characters visually centered in the frame.
Ogre::Vector3 turntablePivotPoint(const Ogre::AxisAlignedBox &bounds)
{
  Ogre::Vector3 point = bounds.getCenter();
  const Ogre::Real height = bounds.getMaximum().y - bounds.getMinimum().y;
  point.y += height * 0.12f;
  return point;
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

/// Build an orthonormal camera basis (GLM-style: right = forward x up).
void cameraAxesFromViewDir(const Ogre::Vector3 &viewDir, const Ogre::Vector3 &worldUp, Ogre::Vector3 &outSide,
                           Ogre::Vector3 &outUp)
{
  Ogre::Vector3 forward = viewDir;
  if (forward.squaredLength() < 1e-8f)
    forward = Ogre::Vector3(0.0f, 0.0f, 1.0f);
  forward.normalise();

  Ogre::Vector3 side = forward.crossProduct(worldUp);
  if (side.squaredLength() < 1e-8f)
    side = forward.crossProduct(Ogre::Vector3::UNIT_X);
  side.normalise();
  outSide = side;
  outUp = side.crossProduct(forward);
  outUp.normalise();
}

/// Minimum orbit radius so every AABB corner fits in the camera frustum from `viewDir`.
Ogre::Real fitOrbitDistance(const Ogre::AxisAlignedBox &bounds, const Ogre::Vector3 &pivotPoint,
                            const Ogre::Vector3 &viewDir, Ogre::Camera *camera, float paddingFactor)
{
  const Ogre::Vector3 center = pivotPoint;
  Ogre::Vector3 dir = viewDir;
  if (dir.squaredLength() < 1e-8f)
    dir = Ogre::Vector3(0.0f, 0.0f, 1.0f);
  dir.normalise();

  Ogre::Vector3 side;
  Ogre::Vector3 up;
  cameraAxesFromViewDir(dir, Ogre::Vector3::UNIT_Y, side, up);

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
  if (!st.camera || !st.cameraNode || !st.pivotNode || bounds.isNull() || bounds.isInfinite())
    return;

  const Ogre::Vector3 pivotPoint = turntablePivotPoint(bounds);

  const float horizUnit = std::cos(elevationRadians);
  const float axialUnit = std::sin(elevationRadians);
  Ogre::Vector3 restOffset = cameraRestOffset(axis, horizUnit, axialUnit);
  if (restOffset.squaredLength() < 1e-8f)
    restOffset = cameraRestOffset(axis, 1.0f, 0.0f);

  Ogre::Vector3 localViewDir = -restOffset;
  localViewDir.normalise();
  const Ogre::Quaternion orbitRot(Ogre::Radian(angleRadians), orbitAxisVector(axis));
  Ogre::Vector3 viewDir = orbitRot * localViewDir;

  const Ogre::Real distance = fitOrbitDistance(bounds, pivotPoint, viewDir, st.camera, paddingFactor);
  const float horiz = distance * horizUnit;
  const float axial = distance * axialUnit;

  // Pivot at the framing point; rotate on one axis; child camera always looks at pivot origin.
  st.pivotNode->setPosition(pivotPoint);
  st.pivotNode->setOrientation(Ogre::Quaternion(Ogre::Radian(angleRadians), orbitAxisVector(axis)));
  st.cameraNode->setPosition(cameraRestOffset(axis, horiz, axial));
  st.cameraNode->lookAt(Ogre::Vector3::ZERO, Ogre::Node::TS_PARENT);
}

void prepareMaterialsForTurntable(const QList<Ogre::Entity *> &entities)
{
  std::unordered_set<const Ogre::Material *> processed;
  for (Ogre::Entity *entity : entities) {
    if (!entity)
      continue;
    MeshImporterExporter::applyNormalMapsToEntity(entity);
    for (unsigned int sub = 0; sub < entity->getNumSubEntities(); ++sub) {
      Ogre::MaterialPtr mat = entity->getSubEntity(sub)->getMaterial();
      if (!mat)
        continue;
      if (!processed.insert(mat.get()).second)
        continue;
      RTShaderHelper::finalizeShaderGenMaterial(mat);
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
      }
      sm->destroyLight(st.light);
    }
    if (st.camera) {
      if (st.cameraNode)
        st.cameraNode->detachObject(st.camera);
      sm->destroyCamera(st.camera);
    }
    if (st.cameraNode)
      sm->destroySceneNode(st.cameraNode);
    if (st.pivotNode)
      sm->destroySceneNode(st.pivotNode);
  }
  st.lightNode = nullptr;
  st.light = nullptr;
  st.camera = nullptr;
  st.cameraNode = nullptr;
  st.pivotNode = nullptr;
}

namespace {

// #936: pose `entity` at `time` of `animState` — same recipe as the isometric
// renderer (enable + set time + notify + fire queued so software/hardware
// animation state updates, then force the entity update).
void applyTurntableAnimationFrame(Ogre::Entity *entity, Ogre::AnimationState *animState, float time)
{
  if (!entity || !animState)
    return;
  animState->setEnabled(true);
  animState->setTimePosition(time);
  if (Ogre::AnimationStateSet *states = entity->getAllAnimationStates())
    states->_notifyDirty();
  Ogre::FrameEvent ev{};
  ev.timeSinceLastFrame = 0.0f;
  ev.timeSinceLastEvent = 0.0f;
  if (Ogre::Root *root = Ogre::Root::getSingletonPtr())
    root->_fireFrameRenderingQueued(ev);
  entity->_updateAnimation();
}

} // namespace

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

  // #936: resolve the animated entity + state up front so a bad name fails
  // with a clear error instead of silently rendering the bind pose.
  const bool wantsAnimation = !options.animationName.isEmpty();
  Ogre::Entity *animatedEntity = nullptr;
  Ogre::AnimationState *animState = nullptr;
  float animLength = 0.0f;
  if (wantsAnimation || options.atSeconds >= 0.0f) {
    const std::string wanted = options.animationName.toStdString();
    for (Ogre::Entity *entity : entities) {
      if (!entity || !entity->hasSkeleton())
        continue;
      Ogre::AnimationStateSet *states = entity->getAllAnimationStates();
      if (!states)
        continue;
      if (wantsAnimation) {
        if (states->hasAnimationState(wanted)) {
          animatedEntity = entity;
          animState = states->getAnimationState(wanted);
          break;
        }
      } else if (!states->getAnimationStates().empty()) {
        // --at with no name: pose the first animation found.
        animatedEntity = entity;
        animState = states->getAnimationStates().begin()->second;
        break;
      }
    }
    if (!animState) {
      if (errorOut)
        *errorOut = wantsAnimation
            ? QStringLiteral("Animation '%1' not found").arg(options.animationName)
            : QStringLiteral("--at needs a skeletal animation, none found");
      return false;
    }
    animLength = animState->getLength();
    // Only the sampled clip drives the pose — disable everything else.
    if (Ogre::AnimationStateSet *states = animatedEntity->getAllAnimationStates())
      for (const auto &entry : states->getAnimationStates())
        if (entry.second)
          entry.second->setEnabled(false);
  }

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
  // Tangents + RTSS normal wiring, then strip any duplicate normal-map TUS that
  // would still modulate in the FFP chain (common on FBX like Jump.fbx).
  prepareMaterialsForTurntable(entities);
  applyTurntableLighting(sm, options.studio);

  SentryReporter::addBreadcrumb("cli.turntable",
                                QStringLiteral("render start frames=%1").arg(frameCount));

  // #936: --at poses once, before framing bounds matter for the orbit.
  if (animState && !wantsAnimation && options.atSeconds >= 0.0f)
    applyTurntableAnimationFrame(animatedEntity, animState,
                                 std::min(options.atSeconds, animLength));

  outFrames->reserve(frameCount);
  try {
    for (int i = 0; i < frameCount; ++i) {
      if (wantsAnimation) {
        // Frame i at `length * i/(frames-1)` — the issue's sampling contract.
        const float t = frameCount > 1
            ? animLength * static_cast<float>(i) / static_cast<float>(frameCount - 1)
            : 0.0f;
        applyTurntableAnimationFrame(animatedEntity, animState, t);
      }
      // Animation sampling keeps the camera FIXED at the front unless the
      // caller combines both via --orbit.
      const float angle = (wantsAnimation && !options.orbitWithAnimation)
          ? 0.0f
          : Ogre::Math::TWO_PI * static_cast<float>(i) / static_cast<float>(frameCount);
      placeCameraOnAxis(bounds, angle, options.axis, elevationRad, 1.25f);
      state().renderTarget->update();
      outFrames->append(readRenderTarget(width, height));
    }
    if (animState)
      animState->setEnabled(false);
    restoreTurntableLighting(sm);
    SentryReporter::addBreadcrumb("cli.turntable",
                                  QStringLiteral("render ok frames=%1").arg(outFrames->size()));
    return true;
  } catch (const Ogre::Exception &e) {
    outFrames->clear();
    if (animState)
      animState->setEnabled(false);   // don't leak the sampled clip's state
    restoreTurntableLighting(sm);
    if (errorOut)
      *errorOut = QString::fromStdString(e.getFullDescription());
    SentryReporter::addBreadcrumb("cli.turntable", QStringLiteral("render failed: Ogre exception"));
    return false;
  } catch (...) {
    outFrames->clear();
    if (animState)
      animState->setEnabled(false);
    restoreTurntableLighting(sm);
    if (errorOut)
      *errorOut = QStringLiteral("Turntable render failed");
    SentryReporter::addBreadcrumb("cli.turntable", QStringLiteral("render failed"));
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
