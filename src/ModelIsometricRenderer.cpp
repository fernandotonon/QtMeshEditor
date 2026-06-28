#include "ModelIsometricRenderer.h"

#include "GlobalDefinitions.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "RTShaderHelper.h"
#include "OgreRenderTargetUtil.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <QPainter>
#include <QTextStream>

#include <OgreAnimationState.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreRoot.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxIsometricDirections = 64;
constexpr int kMaxIsometricFrames = 360;
constexpr int kMaxIsometricCellSize = 8192;
constexpr int kMaxIsometricCells = 4096;
constexpr int kMaxIsometricSheetDim = 16384;

Ogre::SceneManager *sceneMgr()
{
  return Manager::getSingletonPtr() ? Manager::getSingleton()->getSceneMgr() : nullptr;
}

struct IsometricState {
  Ogre::SceneNode *pivotNode = nullptr;
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

IsometricState &state()
{
  static IsometricState s;
  return s;
}

void prepareSceneForCapture(const QList<Ogre::Entity *> &entities)
{
  SelectionSet::getSingleton()->clear();

  for (const Ogre::Entity *entity : entities) {
    if (!entity)
      continue;
    if (Ogre::SceneNode *node = entity->getParentSceneNode())
      node->showBoundingBox(false);
  }
}

/// Hide editor chrome (grid, non-export entities) during RTT capture.
/// The grid uses query flags that bypass the isometric viewport mask.
class EditorCaptureGuard {
public:
  explicit EditorCaptureGuard(const QList<Ogre::Entity *> &visibleEntities)
  {
    std::unordered_set<Ogre::Entity *> keep;
    keep.reserve(static_cast<std::size_t>(visibleEntities.size()));
    for (Ogre::Entity *entity : visibleEntities) {
      if (entity)
        keep.insert(entity);
    }

    if (Manager::getSingletonPtr() && Manager::getSingleton()->hasSceneNode("GridLine_node")) {
      m_gridNode = Manager::getSingleton()->getSceneNode("GridLine_node");
      if (m_gridNode) {
        m_gridWasVisible = m_gridNode->numAttachedObjects() == 0
                               || m_gridNode->getAttachedObject(0)->getVisible();
        m_gridNode->setVisible(false);
      }
    }

    if (Manager::getSingletonPtr()) {
      for (Ogre::Entity *other : Manager::getSingleton()->getEntities()) {
        if (!other || other->getMovableType() != "Entity" || keep.count(other) != 0)
          continue;
        if (Ogre::SceneNode *node = other->getParentSceneNode()) {
          const bool wasVisible =
              node->numAttachedObjects() == 0 || node->getAttachedObject(0)->getVisible();
          if (wasVisible) {
            m_hiddenNodes.emplace_back(node, true);
            node->setVisible(false);
          }
        }
      }
    }
  }

  EditorCaptureGuard(const EditorCaptureGuard &) = delete;
  EditorCaptureGuard &operator=(const EditorCaptureGuard &) = delete;

  ~EditorCaptureGuard() noexcept
  {
    try {
      if (m_gridNode)
        m_gridNode->setVisible(m_gridWasVisible);
      for (auto &[node, wasVisible] : m_hiddenNodes)
        node->setVisible(wasVisible);
    } catch (...) {
      // Best-effort restore; swallow to keep destructor noexcept.
    }
  }

private:
  Ogre::SceneNode *m_gridNode = nullptr;
  bool m_gridWasVisible = false;
  std::vector<std::pair<Ogre::SceneNode *, bool>> m_hiddenNodes;
};

void applyIsometricLighting(Ogre::SceneManager *sm)
{
  IsometricState &st = state();
  st.savedAmbient = sm->getAmbientLight();
  st.hasSavedAmbient = true;
  sm->setAmbientLight(Ogre::ColourValue(1.0f, 1.0f, 1.0f));

  if (!st.light) {
    st.light = sm->createLight("ModelIsometricLight");
    st.light->setType(Ogre::Light::LT_DIRECTIONAL);
    st.lightNode = sm->getRootSceneNode()->createChildSceneNode("ModelIsometricLightNode");
    st.lightNode->attachObject(st.light);
  }
  st.light->setDiffuseColour(0.85f, 0.85f, 0.85f);
  st.light->setSpecularColour(0.35f, 0.35f, 0.35f);
  st.lightNode->setDirection(Ogre::Vector3(-0.35f, -0.85f, -0.4f).normalisedCopy());
}

void restoreIsometricLighting(Ogre::SceneManager *sm)
{
  IsometricState &st = state();
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

  IsometricState &st = state();
  if (st.renderTarget && st.rttWidth == width && st.rttHeight == height) {
    if (st.renderTarget->getNumViewports() > 0) {
      Ogre::Viewport *vp = st.renderTarget->getViewport(0);
      vp->setBackgroundColour(bg);
      vp->setMaterialScheme(Ogre::MSN_SHADERGEN);
      vp->setVisibilityMask(SCENE_VISIBILITY_FLAGS);
    }
    return true;
  }

  ModelIsometricRenderer::shutdown();

  try {
    st.rttTexture = Ogre::TextureManager::getSingleton().createManual(
        "ModelIsometricRTT", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, Ogre::TEX_TYPE_2D,
        static_cast<Ogre::uint32>(width), static_cast<Ogre::uint32>(height), 0, Ogre::PF_BYTE_RGBA,
        Ogre::TU_RENDERTARGET);
    st.renderTarget = st.rttTexture->getBuffer()->getRenderTarget();
    OgreRenderTargetUtil::configureOffscreenRenderTarget(st.renderTarget);
    st.rttWidth = width;
    st.rttHeight = height;

    if (!st.camera) {
      st.camera = sm->createCamera("ModelIsometricCamera");
      st.camera->setNearClipDistance(0.01f);
      st.camera->setFarClipDistance(100000.0f);
      st.camera->setFOVy(Ogre::Degree(45.0f));
      st.pivotNode = sm->getRootSceneNode()->createChildSceneNode("ModelIsometricPivot");
      st.cameraNode = st.pivotNode->createChildSceneNode("ModelIsometricCameraNode");
      st.cameraNode->attachObject(st.camera);
    }

    if (st.renderTarget->getNumViewports() == 0) {
      Ogre::Viewport *vp = st.renderTarget->addViewport(st.camera);
      vp->setClearEveryFrame(true);
      vp->setBackgroundColour(bg);
      vp->setOverlaysEnabled(false);
      vp->setShadowsEnabled(true);
      vp->setVisibilityMask(SCENE_VISIBILITY_FLAGS);
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
    ModelIsometricRenderer::shutdown();
    if (errorOut)
      *errorOut = QString::fromStdString(e.getFullDescription());
    return false;
  } catch (...) {
    ModelIsometricRenderer::shutdown();
    if (errorOut)
      *errorOut = QStringLiteral("Failed to create isometric render target");
    return false;
  }
}

void refreshEntityBounds(const QList<Ogre::Entity *> &entities)
{
  for (const Ogre::Entity *entity : entities) {
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
  for (const Ogre::Entity *entity : entities) {
    if (!entity)
      continue;
    box.merge(entity->getWorldBoundingBox(true));
  }
  return box;
}

void recenterEntitiesAtOrigin(const QList<Ogre::Entity *> &entities, Ogre::AxisAlignedBox &bounds,
                              Ogre::Vector3 *outOffset = nullptr)
{
  if (outOffset)
    *outOffset = Ogre::Vector3::ZERO;

  if (bounds.isNull() || bounds.isInfinite())
    return;

  const Ogre::Vector3 center = bounds.getCenter();
  if (center.squaredLength() < 1e-10f)
    return;

  if (outOffset)
    *outOffset = center;

  std::unordered_set<Ogre::SceneNode *> shifted;
  for (const Ogre::Entity *entity : entities) {
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

void restoreEntitiesFromRecenter(const QList<Ogre::Entity *> &entities, const Ogre::Vector3 &offset)
{
  if (offset.squaredLength() < 1e-10f)
    return;

  std::unordered_set<Ogre::SceneNode *> shifted;
  for (const Ogre::Entity *entity : entities) {
    if (!entity)
      continue;
    Ogre::SceneNode *node = entity->getParentSceneNode();
    if (!node || !shifted.insert(node).second)
      continue;
    node->translate(offset, Ogre::Node::TS_WORLD);
  }
  refreshEntityBounds(entities);
}

struct RecenterGuard {
  const QList<Ogre::Entity *> &entities;
  Ogre::Vector3 offset;
  bool active = false;

  RecenterGuard(const QList<Ogre::Entity *> &ents, Ogre::Vector3 off) : entities(ents), offset(off)
  {
    active = offset.squaredLength() >= 1e-10f;
  }
  RecenterGuard(const RecenterGuard &) = delete;
  RecenterGuard &operator=(const RecenterGuard &) = delete;
  ~RecenterGuard() noexcept
  {
    if (!active)
      return;
    try {
      restoreEntitiesFromRecenter(entities, offset);
    } catch (...) {
      // Best-effort restore; swallow to keep destructor noexcept.
    }
  }
};

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

Ogre::Vector3 turntablePivotPoint(const Ogre::AxisAlignedBox &bounds)
{
  Ogre::Vector3 point = bounds.getCenter();
  const Ogre::Real height = bounds.getMaximum().y - bounds.getMinimum().y;
  point.y += height * 0.12f;
  return point;
}

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

Ogre::Real fitOrbitDistance(const Ogre::AxisAlignedBox &bounds, const Ogre::Vector3 &pivotPoint,
                            const Ogre::Vector3 &viewDir, const Ogre::Camera *camera, float paddingFactor)
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
                       float elevationRadians, float paddingFactor, float fixedDistance)
{
  IsometricState &st = state();
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

  const Ogre::Real distance =
      fixedDistance > 0.0f ? fixedDistance
                           : fitOrbitDistance(bounds, pivotPoint, viewDir, st.camera, paddingFactor);
  const float horiz = distance * horizUnit;
  const float axial = distance * axialUnit;

  st.pivotNode->setPosition(pivotPoint);
  st.pivotNode->setOrientation(Ogre::Quaternion(Ogre::Radian(angleRadians), orbitAxisVector(axis)));
  st.cameraNode->setPosition(cameraRestOffset(axis, horiz, axial));
  st.cameraNode->lookAt(Ogre::Vector3::ZERO, Ogre::Node::TS_PARENT);
}

void prepareMaterialsForCapture(const QList<Ogre::Entity *> &entities)
{
  std::unordered_set<const Ogre::Material *> processed;
  for (const Ogre::Entity *entity : entities) {
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
  IsometricState &st = state();
  if (!st.renderTarget)
    return image;

  Ogre::PixelBox pb(static_cast<Ogre::uint32>(width), static_cast<Ogre::uint32>(height), 1,
                    Ogre::PF_BYTE_RGBA, image.bits());
  st.renderTarget->copyContentsToMemory(Ogre::Box(0, 0, width, height), pb,
                                        Ogre::RenderTarget::FB_AUTO);
  return image;
}

void applyAnimationFrame(Ogre::Entity *entity, Ogre::AnimationState *animState, float time)
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

bool captureIsometricGrid(const Ogre::AxisAlignedBox &bounds, const IsometricOptions &options, int width,
                          int height, int directions, int frames, float elevationRad, float startAzimuthRad,
                          float directionStep, bool wantsAnimation, Ogre::Entity *animatedEntity,
                          Ogre::AnimationState *animState, float animLength, QList<QList<QImage>> *outRowsByDirection)
{
  outRowsByDirection->reserve(directions);
  for (int dir = 0; dir < directions; ++dir) {
    const float azimuth = startAzimuthRad - static_cast<float>(dir) * directionStep;
    placeCameraOnAxis(bounds, azimuth, options.upAxis, elevationRad, options.cameraPadding,
                      options.cameraDistance);

    QList<QImage> row;
    row.reserve(frames);
    for (int frame = 0; frame < frames; ++frame) {
      if (wantsAnimation) {
        const float t = (frames == 1) ? 0.0f
                                      : animLength * static_cast<float>(frame) / static_cast<float>(frames - 1);
        applyAnimationFrame(animatedEntity, animState, t);
      }
      Ogre::RenderTarget *renderTarget = state().renderTarget;
      if (!renderTarget)
        return false;
      renderTarget->update();
      row.append(readRenderTarget(width, height));
    }
    outRowsByDirection->append(row);
  }
  return true;
}

} // namespace

QString ModelIsometricRenderer::directionOrderConvention()
{
  return QStringLiteral(
      "Row 0 = front (camera on +Z, model facing camera); each subsequent row rotates the "
      "camera clockwise when viewed from above (+Y). Columns are evenly spaced animation frames "
      "left-to-right.");
}

void ModelIsometricRenderer::shutdown()
{
  IsometricState &st = state();
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

bool ModelIsometricRenderer::renderToGrid(const QList<Ogre::Entity *> &entities, Ogre::Entity *animatedEntity,
                                          const QString &animationName, int frameCount,
                                          const IsometricOptions &options,
                                          QList<QList<QImage>> *outRowsByDirection, QString *errorOut)
{
  if (!outRowsByDirection) {
    if (errorOut)
      *errorOut = QStringLiteral("Output grid is null");
    return false;
  }
  outRowsByDirection->clear();

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

  const int width = std::clamp(options.width, 16, kMaxIsometricCellSize);
  const int height = std::clamp(options.height, 16, kMaxIsometricCellSize);
  const int directions = std::clamp(options.directionCount, 1, kMaxIsometricDirections);
  const int frames = std::clamp(frameCount, 1, kMaxIsometricFrames);

  const std::int64_t sheetW = static_cast<std::int64_t>(frames) * width;
  if (const std::int64_t sheetH = static_cast<std::int64_t>(directions) * height;
      static_cast<std::int64_t>(directions) * frames > kMaxIsometricCells
      || sheetW > kMaxIsometricSheetDim || sheetH > kMaxIsometricSheetDim) {
    if (errorOut) {
      *errorOut =
          QStringLiteral("Grid too large (%1 directions × %2 frames at %3×%4 px; max %5 cells, %6 px/side)")
              .arg(directions)
              .arg(frames)
              .arg(width)
              .arg(height)
              .arg(kMaxIsometricCells)
              .arg(kMaxIsometricSheetDim);
    }
    return false;
  }

  const bool wantsAnimation = animatedEntity && !animationName.isEmpty();
  Ogre::AnimationState *animState = nullptr;
  float animLength = 0.0f;
  if (wantsAnimation) {
    if (!animatedEntity->hasSkeleton()) {
      if (errorOut)
        *errorOut = QStringLiteral("Animated entity has no skeleton");
      return false;
    }
    const Ogre::AnimationStateSet *states = animatedEntity->getAllAnimationStates();
    if (!states || !states->hasAnimationState(animationName.toStdString())) {
      if (errorOut)
        *errorOut = QStringLiteral("Animation '%1' not found").arg(animationName);
      return false;
    }
    animState = animatedEntity->getAllAnimationStates()->getAnimationState(animationName.toStdString());
    animLength = animState->getLength();
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

  Ogre::Vector3 recenterOffset = Ogre::Vector3::ZERO;
  recenterEntitiesAtOrigin(entities, bounds, &recenterOffset);
  bounds = combinedWorldBounds(entities);
  RecenterGuard recenterGuard(entities, recenterOffset);

  const float elevationRad =
      Ogre::Degree(std::clamp(options.elevationDegrees, -80.0f, 80.0f)).valueRadians();
  const float startAzimuthRad = Ogre::Degree(options.startAzimuthDegrees).valueRadians();
  const float directionStep = directions > 0 ? Ogre::Math::TWO_PI / static_cast<float>(directions) : 0.0f;

  prepareSceneForCapture(entities);
  prepareMaterialsForCapture(entities);
  applyIsometricLighting(sm);
  EditorCaptureGuard editorGuard(entities);

  if (wantsAnimation) {
    const Ogre::AnimationStateSet *states = animatedEntity->getAllAnimationStates();
    for (const auto &entry : states->getAnimationStates()) {
      if (entry.second)
        entry.second->setEnabled(false);
    }
  }

  SentryReporter::addBreadcrumb(
      "file.export",
      QStringLiteral("isometric render start dirs=%1 frames=%2 animated=%3")
          .arg(directions)
          .arg(frames)
          .arg(wantsAnimation ? animationName : QStringLiteral("static")));

  outRowsByDirection->reserve(directions);
  try {
    if (!captureIsometricGrid(bounds, options, width, height, directions, frames, elevationRad, startAzimuthRad,
                             directionStep, wantsAnimation, animatedEntity, animState, animLength,
                             outRowsByDirection)) {
      outRowsByDirection->clear();
      restoreIsometricLighting(sm);
      if (errorOut)
        *errorOut = QStringLiteral("Isometric render target is not available");
      return false;
    }

    if (wantsAnimation && animState)
      animState->setEnabled(false);

    restoreIsometricLighting(sm);
    SentryReporter::addBreadcrumb("file.export",
                                  QStringLiteral("isometric render ok dirs=%1 frames=%2")
                                      .arg(directions)
                                      .arg(frames));
    return true;
  } catch (const Ogre::Exception &e) {
    outRowsByDirection->clear();
    restoreIsometricLighting(sm);
    if (errorOut)
      *errorOut = QString::fromStdString(e.getFullDescription());
    SentryReporter::addBreadcrumb("file.export", QStringLiteral("isometric render failed: Ogre exception"));
    return false;
  } catch (const std::exception &e) {
    outRowsByDirection->clear();
    restoreIsometricLighting(sm);
    if (errorOut)
      *errorOut = QString::fromUtf8(e.what());
    SentryReporter::addBreadcrumb("file.export", QStringLiteral("isometric render failed: std exception"));
    return false;
  } catch (...) {
    outRowsByDirection->clear();
    restoreIsometricLighting(sm);
    if (errorOut)
      *errorOut = QStringLiteral("Isometric render failed");
    SentryReporter::addBreadcrumb("file.export", QStringLiteral("isometric render failed"));
    return false;
  }
}

QImage ModelIsometricRenderer::composeDirectionGrid(const QList<QList<QImage>> &rowsByDirection)
{
  if (rowsByDirection.isEmpty())
    return {};

  const int directionCount = static_cast<int>(rowsByDirection.size());
  int frameCount = 0;
  int frameW = 0;
  int frameH = 0;
  for (const QList<QImage> &row : rowsByDirection) {
    if (row.isEmpty())
      return {};
    frameCount = std::max(frameCount, static_cast<int>(row.size()));
    if (frameW == 0) {
      frameW = row.first().width();
      frameH = row.first().height();
    }
  }
  if (frameCount <= 0 || frameW <= 0 || frameH <= 0)
    return {};

  QImage sheet(frameCount * frameW, directionCount * frameH, QImage::Format_RGBA8888);
  sheet.fill(Qt::transparent);

  QPainter painter(&sheet);
  for (int dir = 0; dir < directionCount; ++dir) {
    const QList<QImage> &row = rowsByDirection.at(dir);
    for (int frame = 0; frame < static_cast<int>(row.size()); ++frame) {
      const QImage &src = row.at(frame);
      if (src.width() != frameW || src.height() != frameH)
        continue;
      painter.drawImage(frame * frameW, dir * frameH, src);
    }
  }
  return sheet;
}

Ogre::Entity *ModelIsometricRenderer::findEntityWithAnimation(const QList<Ogre::Entity *> &entities,
                                                              const QString &animationName)
{
  const std::string anim = animationName.toStdString();
  for (Ogre::Entity *entity : entities) {
    if (!entity || !entity->hasSkeleton())
      continue;
    const Ogre::AnimationStateSet *states = entity->getAllAnimationStates();
    if (states && states->hasAnimationState(anim))
      return entity;
  }
  return nullptr;
}

QString ModelIsometricRenderer::formatAvailableAnimations(const QList<Ogre::Entity *> &entities)
{
  QString text;
  QTextStream stream(&text);
  for (const Ogre::Entity *entity : entities) {
    if (!entity || !entity->hasSkeleton())
      continue;
    const Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    if (!skel)
      continue;
    const QString entityLabel = QString::fromStdString(entity->getName());
    for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
      stream << "  [" << entityLabel << "] "
             << QString::fromStdString(skel->getAnimation(ai)->getName()) << "\n";
    }
  }
  return text;
}
