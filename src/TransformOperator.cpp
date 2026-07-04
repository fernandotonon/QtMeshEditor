#include <QtDebug>
#include <QSettings>
#include <QApplication>
#include <cmath>
#include <limits>

#include "GlobalDefinitions.h"

#include "TransformOperator.h"
#include "LightManager.h"
#include "LightVisualizer.h"
#include "LightsController.h"
#include "RotationGizmo.h"
#include "TranslationGizmo.h"
#include "ScaleGizmo.h"
#include "SelectionBoxObject.h"
#include "SelectionSet.h"
#include "OgreWidget.h"
#include "mainwindow.h"
#include "Manager.h"
#include "SentryReporter.h"
#include "MeshTransform.h"
#include "SubMeshTransform.h"
#include "Euler.h"
#include "ViewportGrid.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"
#include "commands/BoneTransformCommand.h"
#include "BoneDragRelease.h"
#include "EditModeController.h"
#include "AutoRigController.h"
#include "TexturePaintController.h"
#include "AnimationControlController.h"
#include "PropertiesPanelController.h"
#include "SkeletonDebug.h"
#include <Ogre.h>

// TODO  create a virtual class GizmoObject & add Rotation & Translation Gizmo to have only one interface

////////////////////////////////////////
// Static variable initialisation
TransformOperator* TransformOperator:: m_pSingleton = nullptr;
const QPoint  TransformOperator::invalidPosition(-1,-1);

////////////////////////////////////////
/// Static Member to build & destroy

TransformOperator* TransformOperator::getSingleton()
{
  if (m_pSingleton == nullptr)
  {
      m_pSingleton =  new TransformOperator();
  }

  return m_pSingleton;
}

////////////////////////////////////////
// Constructor & Destructor

TransformOperator::TransformOperator() : QObject(nullptr)
{
    // Get Manager singleton - it must already exist (created by MainWindow)
    // Use getSingletonPtr() to avoid creating a new instance with null parent
    Manager* manager = Manager::getSingletonPtr();
    if (!manager)
    {
        // This should not happen in normal operation - Manager should be created by MainWindow first
        throw std::runtime_error("TransformOperator requires Manager to be initialized first");
    }
    Ogre::SceneManager* pSceneMgr = manager->getSceneMgr();
    m_pTransformNode = pSceneMgr->getRootSceneNode()->createChildSceneNode(TRANSFORM_OBJECT_NAME);

    m_pRotationGizmo = new RotationGizmo(m_pTransformNode, "RotationGizmo", 2.0f);
    m_pRotationGizmo->setQueryFlags(GIZMO_QUERY_FLAGS);
    m_pRotationGizmo->setVisible(false);

    m_pTranslationGizmo = new TranslationGizmo(m_pTransformNode);
    m_pTranslationGizmo->setQueryFlags(GIZMO_QUERY_FLAGS);
    m_pTranslationGizmo->setVisible(false);

    m_pScaleGizmo = new ScaleGizmo(m_pTransformNode);
    m_pScaleGizmo->setQueryFlags(GIZMO_QUERY_FLAGS);
    m_pScaleGizmo->setVisible(false);

    // TODO move this node in the SelectionBoxObject class
    m_pSelectionBoxNode = pSceneMgr->getRootSceneNode()->createChildSceneNode(SELECTIONBOX_OBJECT_NAME);
    m_pSelectionBox = new SelectionBoxObject(SELECTIONBOX_OBJECT_NAME);
    m_pSelectionBox->setVisibilityFlags(GUI_VISIBILITY_FLAGS);
    m_pSelectionBoxNode->attachObject(m_pSelectionBox);
    m_pSelectionBox->setVisible(false);

    m_pVolQuery = pSceneMgr->createPlaneBoundedVolumeQuery(Ogre::PlaneBoundedVolumeList());
    m_pRayQuery = pSceneMgr->createRayQuery(Ogre::Ray());

    connect(SelectionSet::getSingleton(),SIGNAL(selectionChanged()),this,SLOT(onSelectionChanged()));

    // Bone selection from the Animation Control panel must also re-anchor
    // the gizmo (otherwise the gizmo stays at the previous scene-node
    // position and only snaps to the bone on the first drag event).
    connect(AnimationControlController::instance(),
            &AnimationControlController::boneListChanged,
            this, &TransformOperator::onSelectionChanged);

    // Update gizmo when edit mode selection changes (vertices selected/deselected)
    connect(EditModeController::instance(), &EditModeController::editSelectionChanged,
            this, &TransformOperator::onSelectionChanged);
    connect(EditModeController::instance(), &EditModeController::editModeChanged,
            this, &TransformOperator::onSelectionChanged);
    connect(EditModeController::instance(), &EditModeController::vertexPaintChanged,
            this, &TransformOperator::onSelectionChanged);
    // Texture paint toggle also affects mouse-tracking (we need
    // hover events without a button press) and gizmo visibility.
    connect(TexturePaintController::instance(), &TexturePaintController::texturePaintChanged,
            this, &TransformOperator::onSelectionChanged);

    QtInputManager::getInstance().AddMouseListener(this);

    // Load snap settings from QSettings.
    // Snap/enabled is a session toggle — always start disabled, don't read
    // any previously-persisted value. Also clear any legacy persisted value
    // so prior releases that stored `true` stop surfacing on next launch.
    QSettings settings;
    if (settings.contains("Snap/enabled"))
        settings.remove("Snap/enabled");
    mSnapEnabled    = false;
    mSnapGridSize   = settings.value("Snap/gridSize", 1.0).toDouble();
    mSnapAngleStep  = settings.value("Snap/angleStep", 15.0).toDouble();
    mSnapScaleStep  = settings.value("Snap/scaleStep", 0.25).toDouble();

    // Load pivot mode from QSettings
    int pivotVal = settings.value("Pivot/mode", static_cast<int>(PIVOT_CENTER)).toInt();
    if (pivotVal >= PIVOT_CENTER && pivotVal <= PIVOT_ORIGIN)
        mPivotMode = static_cast<PivotMode>(pivotVal);
}

TransformOperator::~TransformOperator()
{
    QtInputManager::getInstance().RemoveMouseListener(this);

    delete m_pRotationGizmo;
    m_pRotationGizmo = nullptr;
    delete m_pTranslationGizmo;
    m_pTranslationGizmo = nullptr;
    delete m_pScaleGizmo;
    m_pScaleGizmo = nullptr;

    if (auto manager = Manager::getSingletonPtr())
    {
        if (auto sceneMgr = manager->getSceneMgr())
        {
            if (m_pSelectionBoxNode)
            {
                m_pSelectionBoxNode->detachAllObjects();
            }
            if (m_pSelectionBox)
            {
                sceneMgr->destroyManualObject(m_pSelectionBox);
                m_pSelectionBox = nullptr;
            }
            if (m_pSelectionBoxNode)
            {
                sceneMgr->destroySceneNode(m_pSelectionBoxNode);
                m_pSelectionBoxNode = nullptr;
            }
            if (m_pTransformNode)
            {
                sceneMgr->destroySceneNode(m_pTransformNode);
                m_pTransformNode = nullptr;
            }
            if (m_pVolQuery)
            {
                sceneMgr->destroyQuery(m_pVolQuery);
                m_pVolQuery = nullptr;
            }
            if (m_pRayQuery)
            {
                sceneMgr->destroyQuery(m_pRayQuery);
                m_pRayQuery = nullptr;
            }
        }
    }
}

void TransformOperator::kill()
{
    if (m_pSingleton)
    {
        delete m_pSingleton;
        m_pSingleton = nullptr;
    }
}

void TransformOperator::swap(int& x, int& y)
{
    int temp = x;
    x = y; y = temp;
}

bool TransformOperator::shouldRouteToBoneGizmo(TransformState state,
                                               const Ogre::Bone* selectedBone,
                                               bool boneCanTranslate)
{
    if (!selectedBone) return false;
    if (state == TS_ROTATE || state == TS_SCALE) return true;
    if (state == TS_TRANSLATE) return boneCanTranslate;
    return false;
}

////////////////////////////////////////
// Snap settings

void TransformOperator::setSnapEnabled(bool enabled)
{
    if (mSnapEnabled != enabled)
    {
        mSnapEnabled = enabled;
        // Don't persist — snap is a session toggle, not a saved preference.
        SentryReporter::addBreadcrumb("ui.action",
            mSnapEnabled ? "Snap enabled" : "Snap disabled");
        emit snapSettingsChanged();
    }
}

void TransformOperator::setSnapGridSize(double size)
{
    if (size > 0.0 && mSnapGridSize != size)
    {
        mSnapGridSize = size;
        QSettings settings;
        settings.setValue("Snap/gridSize", mSnapGridSize);
        emit snapSettingsChanged();
    }
}

void TransformOperator::setSnapAngleStep(double degrees)
{
    if (degrees > 0.0 && mSnapAngleStep != degrees)
    {
        mSnapAngleStep = degrees;
        QSettings settings;
        settings.setValue("Snap/angleStep", mSnapAngleStep);
        emit snapSettingsChanged();
    }
}

void TransformOperator::setSnapScaleStep(double step)
{
    if (step > 0.0 && mSnapScaleStep != step)
    {
        mSnapScaleStep = step;
        QSettings settings;
        settings.setValue("Snap/scaleStep", mSnapScaleStep);
        emit snapSettingsChanged();
    }
}

QList<double> TransformOperator::gridSizePresets()
{
    return { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0 };
}

QList<double> TransformOperator::angleStepPresets()
{
    return { 5.0, 15.0, 45.0, 90.0 };
}

QList<double> TransformOperator::scaleStepPresets()
{
    return { 0.1, 0.25, 0.5 };
}

double TransformOperator::snapValue(double value, double step)
{
    return std::round(value / step) * step;
}

Ogre::Vector3 TransformOperator::snapTranslation(const Ogre::Vector3& translation, double gridSize)
{
    return Ogre::Vector3(
        static_cast<Ogre::Real>(snapValue(translation.x, gridSize)),
        static_cast<Ogre::Real>(snapValue(translation.y, gridSize)),
        static_cast<Ogre::Real>(snapValue(translation.z, gridSize))
    );
}

Ogre::Real TransformOperator::snapAngle(Ogre::Real degrees, double angleStep)
{
    return static_cast<Ogre::Real>(snapValue(degrees, angleStep));
}

Ogre::Vector3 TransformOperator::snapScale(const Ogre::Vector3& scale, double scaleStep)
{
    return Ogre::Vector3(
        static_cast<Ogre::Real>(snapValue(scale.x, scaleStep)),
        static_cast<Ogre::Real>(snapValue(scale.y, scaleStep)),
        static_cast<Ogre::Real>(snapValue(scale.z, scaleStep))
    );
}
void TransformOperator::setPivotMode(PivotMode mode)
{
    if (mPivotMode != mode)
    {
        mPivotMode = mode;
        QSettings settings;
        settings.setValue("Pivot/mode", static_cast<int>(mPivotMode));
        QString modeName;
        switch (mode) {
        case PIVOT_CENTER: modeName = "Center"; break;
        case PIVOT_BOTTOM: modeName = "Bottom"; break;
        case PIVOT_ORIGIN: modeName = "Origin"; break;
        }
        SentryReporter::addBreadcrumb("ui.action",
            QString("Pivot mode changed to %1").arg(modeName));
        updateGizmoPosition();
        emit pivotModeChanged(mPivotMode);
    }
}

void TransformOperator::cyclePivotMode()
{
    switch (mPivotMode) {
    case PIVOT_CENTER: setPivotMode(PIVOT_BOTTOM); break;
    case PIVOT_BOTTOM: setPivotMode(PIVOT_ORIGIN); break;
    case PIVOT_ORIGIN: setPivotMode(PIVOT_CENTER); break;
    }
}

Ogre::Vector3 TransformOperator::getPivotPoint() const
{
    auto* sel = SelectionSet::getSingleton();
    if (sel->isEmpty())
        return Ogre::Vector3::ZERO;

    switch (mPivotMode)
    {
    case PIVOT_CENTER:
    {
        // True geometric center of bounding box
        if (sel->hasNodes())
        {
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            for (int i = 0; i < sel->getNodesCount(); ++i)
            {
                Ogre::SceneNode* node = sel->getSceneNode(i);
                // Use world bounding box center if entities are attached,
                // otherwise use node position
                if (node->numAttachedObjects() > 0)
                {
                    Ogre::AxisAlignedBox aabb;
                    for (auto& obj : node->getAttachedObjects())
                        aabb.merge(obj->getWorldBoundingBox(true));
                    if (aabb.isFinite())
                    {
                        center += aabb.getCenter();
                        continue;
                    }
                }
                center += node->getPosition();
            }
            return center / static_cast<Ogre::Real>(sel->getNodesCount());
        }
        else if (sel->hasEntities())
        {
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            for (int i = 0; i < sel->getEntitiesCount(); ++i)
            {
                Ogre::Entity* ent = sel->getEntity(i);
                const Ogre::AxisAlignedBox bb = ent->getWorldBoundingBox(true);
                if (bb.isFinite())
                    center += bb.getCenter();
                else
                    center += ent->getParentSceneNode()->getPosition();
            }
            return center / static_cast<Ogre::Real>(sel->getEntitiesCount());
        }
        else if (sel->hasSubEntities())
        {
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            for (int i = 0; i < sel->getSubEntitiesCount(); ++i)
            {
                Ogre::SubEntity* sub = sel->getSubEntity(i);
                const Ogre::AxisAlignedBox bb = sub->getParent()->getWorldBoundingBox(true);
                if (bb.isFinite())
                    center += bb.getCenter();
                else
                    center += sub->getParent()->getParentSceneNode()->getPosition();
            }
            return center / static_cast<Ogre::Real>(sel->getSubEntitiesCount());
        }
        break;
    }
    case PIVOT_BOTTOM:
    {
        // Bottom-center: center of bounding box but Y = minimum Y
        if (sel->hasNodes())
        {
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            Ogre::Real minY = std::numeric_limits<Ogre::Real>::max();
            int count = sel->getNodesCount();
            for (int i = 0; i < count; ++i)
            {
                Ogre::SceneNode* node = sel->getSceneNode(i);
                if (node->numAttachedObjects() > 0)
                {
                    Ogre::AxisAlignedBox aabb;
                    for (auto& obj : node->getAttachedObjects())
                        aabb.merge(obj->getWorldBoundingBox(true));
                    if (aabb.isFinite())
                    {
                        center += aabb.getCenter();
                        if (aabb.getMinimum().y < minY)
                            minY = aabb.getMinimum().y;
                        continue;
                    }
                }
                center += node->getPosition();
                if (node->getPosition().y < minY)
                    minY = node->getPosition().y;
            }
            center /= static_cast<Ogre::Real>(count);
            center.y = minY;
            return center;
        }
        else if (sel->hasEntities())
        {
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            Ogre::Real minY = std::numeric_limits<Ogre::Real>::max();
            int count = sel->getEntitiesCount();
            for (int i = 0; i < count; ++i)
            {
                Ogre::Entity* ent = sel->getEntity(i);
                const Ogre::AxisAlignedBox bb = ent->getWorldBoundingBox(true);
                if (bb.isFinite())
                {
                    center += bb.getCenter();
                    if (bb.getMinimum().y < minY)
                        minY = bb.getMinimum().y;
                }
                else
                {
                    center += ent->getParentSceneNode()->getPosition();
                    if (ent->getParentSceneNode()->getPosition().y < minY)
                        minY = ent->getParentSceneNode()->getPosition().y;
                }
            }
            center /= static_cast<Ogre::Real>(count);
            center.y = minY;
            return center;
        }
        else if (sel->hasSubEntities())
        {
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            Ogre::Real minY = std::numeric_limits<Ogre::Real>::max();
            int count = sel->getSubEntitiesCount();
            for (int i = 0; i < count; ++i)
            {
                Ogre::SubEntity* sub = sel->getSubEntity(i);
                const Ogre::AxisAlignedBox bb = sub->getParent()->getWorldBoundingBox(true);
                if (bb.isFinite())
                {
                    center += bb.getCenter();
                    if (bb.getMinimum().y < minY)
                        minY = bb.getMinimum().y;
                }
                else
                {
                    center += sub->getParent()->getParentSceneNode()->getPosition();
                    if (sub->getParent()->getParentSceneNode()->getPosition().y < minY)
                        minY = sub->getParent()->getParentSceneNode()->getPosition().y;
                }
            }
            center /= static_cast<Ogre::Real>(count);
            center.y = minY;
            return center;
        }
        break;
    }
    case PIVOT_ORIGIN:
    {
        // Node's own local origin (node position / parent scene node position)
        if (sel->hasNodes())
        {
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            for (int i = 0; i < sel->getNodesCount(); ++i)
                center += sel->getSceneNode(i)->getPosition();
            return center / static_cast<Ogre::Real>(sel->getNodesCount());
        }
        else if (sel->hasEntities())
        {
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            for (int i = 0; i < sel->getEntitiesCount(); ++i)
                center += sel->getEntity(i)->getParentSceneNode()->getPosition();
            return center / static_cast<Ogre::Real>(sel->getEntitiesCount());
        }
        else if (sel->hasSubEntities())
        {
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            for (int i = 0; i < sel->getSubEntitiesCount(); ++i)
                center += sel->getSubEntity(i)->getParent()->getParentSceneNode()->getPosition();
            return center / static_cast<Ogre::Real>(sel->getSubEntitiesCount());
        }
        break;
    }
    }

    return Ogre::Vector3::ZERO;
}

const Ogre::ColourValue& TransformOperator::getSelectionBoxColour() const
{   return m_pSelectionBox->getBoxColour();   }

void TransformOperator::setSelectionBoxColour(const Ogre::ColourValue& colour)
{   m_pSelectionBox->setBoxColour(colour);    }

void TransformOperator::onTransformStateChange(const TransformState newState)
{
    mTransformState = newState;
    updateGizmo();
}

void TransformOperator::setTransformSpace(TransformSpace space)
{
    if (mTransformSpace != space)
    {
        mTransformSpace = space;
        updateGizmo();
        emit transformSpaceChanged(mTransformSpace);
    }
}

void TransformOperator::toggleTransformSpace()
{
    setTransformSpace(mTransformSpace == SPACE_WORLD ? SPACE_LOCAL : SPACE_WORLD);
}

void TransformOperator::removeSelected()
{
  SentryReporter::addBreadcrumb("ui.action", "Remove selected objects");
  SelectionSet* pCurrentSelection = SelectionSet::getSingleton();
  if (pCurrentSelection->isEmpty())
    return;

  if (pCurrentSelection->hasNodes())
  {
    bool allLights = true;
    for (Ogre::SceneNode* node : pCurrentSelection->getNodesSelectionList())
    {
      if (!LightManager::sceneNodeIsUserLight(node))
      {
        allLights = false;
        break;
      }
    }
    if (allLights)
    {
      LightsController::instance()->deleteSelectedLights();
      return;
    }
  }

  if (!pCurrentSelection->isEmpty())
  {
    foreach (Ogre::SceneNode* node, SelectionSet::getSingleton()->getNodesSelectionList())
    {
      Manager::getSingleton()->destroySceneNode(node);
    }
    pCurrentSelection->clearList();

    // Clear undo stack — destroyed nodes invalidate any stored commands
    UndoManager::getSingleton()->clear();
  }
}


void TransformOperator::updateGizmo()
{
    updateGizmoPosition();
    bool editModeHasSelection = EditModeController::instance()->isEditModeActive()
        && !EditModeController::instance()->selectedVertices().empty();
    const bool boneSelected = AnimationControlController::instance()->selectedBonePtr() != nullptr
                              && AnimationControlController::instance()->selectedEntity() != nullptr;

    if(SelectionSet::getSingleton()->hasNodes()||SelectionSet::getSingleton()->hasEntities()
       ||SelectionSet::getSingleton()->hasSubEntities()||editModeHasSelection||boneSelected)
    {
        // Determine gizmo orientation based on transform space.
        // Bone selection: keep the orientation already set by
        // updateGizmoPosition() (the bone's world orientation), so the
        // gizmo arrows align with the bone's local axes — what the user
        // actually wants when posing a bone. Without this branch the
        // orientation gets clobbered with identity below and the gizmo
        // shows in global axes only.
        Ogre::Quaternion gizmoOrientation = m_pTransformNode->getOrientation();
        if (!boneSelected) {
            if (mTransformSpace == SPACE_LOCAL && SelectionSet::getSingleton()->hasNodes()
                && SelectionSet::getSingleton()->getNodesCount() == 1)
            {
                gizmoOrientation = SelectionSet::getSingleton()->getSceneNode(0)->getOrientation();
            }
            else
            {
                gizmoOrientation = Manager::getSingleton()->getSceneMgr()->getRootSceneNode()->getOrientation();
            }
        }

        switch  (mTransformState) {
        case TransformOperator::TS_SELECT:
                m_pRotationGizmo->setVisible(false);
                m_pTranslationGizmo->setVisible(false);
                m_pScaleGizmo->setVisible(false);
                // Enable mouse tracking when ANY paint mode is on so
                // the hover preview / brush ring updates without a
                // pressed button. Vertex paint requires Edit Mode;
                // texture paint works in any mode.
                mTrackingEnable =
                    (EditModeController::instance()->isEditModeActive()
                     && EditModeController::instance()->vertexPaintEnabled())
                    || TexturePaintController::instance()->texturePaintEnabled();
          break;
        case TransformOperator::TS_TRANSLATE:
                m_pTransformNode->setOrientation(gizmoOrientation);
                m_pRotationGizmo->setVisible(false);
                m_pTranslationGizmo->setVisible(true);
                m_pScaleGizmo->setVisible(false);
                mTrackingEnable = true;
          break;
        case TransformOperator::TS_ROTATE:
                m_pTransformNode->setOrientation(gizmoOrientation);
                m_pRotationGizmo->setVisible(true);
                m_pTranslationGizmo->setVisible(false);
                m_pScaleGizmo->setVisible(false);
                mTrackingEnable = true;
          break;
        case TransformOperator::TS_SCALE:
                m_pTransformNode->setOrientation(gizmoOrientation);
                m_pRotationGizmo->setVisible(false);
                m_pTranslationGizmo->setVisible(false);
                m_pScaleGizmo->setVisible(true);
                mTrackingEnable = true;
          break;
        default:
                m_pRotationGizmo->setVisible(false);
                m_pTranslationGizmo->setVisible(false);
                m_pScaleGizmo->setVisible(false);
          break;
        }
    }
    else
    {
        m_pRotationGizmo->setVisible(false);
        m_pTranslationGizmo->setVisible(false);
        m_pScaleGizmo->setVisible(false);
    }
    if(m_pActiveWidget) {
        m_pActiveWidget->setMouseTracking(mTrackingEnable);
        // Crosshair cursor while any paint mode is on so the user
        // gets clear feedback that clicks will paint, not select.
        // Only override the cursor in TS_SELECT — in Translate/Rotate/
        // Scale the gizmo is the active interaction and the user
        // should see the default cursor over its handles.
        if (mTransformState == TS_SELECT) {
            const bool paintOn =
                (EditModeController::instance()->isEditModeActive()
                 && EditModeController::instance()->vertexPaintEnabled())
                || TexturePaintController::instance()->texturePaintEnabled();
            m_pActiveWidget->setCursor(paintOn ? Qt::CrossCursor : Qt::ArrowCursor);
        } else {
            m_pActiveWidget->setCursor(Qt::ArrowCursor);
        }
    }
}

void TransformOperator::tickTransformGizmoScale(const Ogre::Camera* camera)
{
    if (!camera || !m_pTransformNode) return;
    // Only scale when a gizmo is actually visible (something selected and
    // an active transform tool). Otherwise leave the node scale alone.
    if (mTransformState == TS_SELECT || mTransformState == TS_NONE) return;

    // Re-anchor the gizmo onto the selected bone every frame so it
    // tracks animation playback and slider scrubbing. Skip during a
    // bone drag — the move handler is already keeping the gizmo in
    // sync and we don't want to fight the user's input.
    const bool boneSelected = AnimationControlController::instance()->selectedBonePtr() != nullptr
                              && AnimationControlController::instance()->selectedEntity() != nullptr;
    if (boneSelected && !mBoneDragActive)
        updateGizmoPosition();

    if (SelectionSet::getSingleton()->isEmpty()
        && !(EditModeController::instance()->isEditModeActive()
             && !EditModeController::instance()->selectedVertices().empty())
        && !boneSelected)
    {
        return;
    }
    // Don't re-scale mid-drag. The scale path captures the gizmo's node
    // scale at press-time (mEditModeScaleStartPixel / mTransformVector);
    // mutating the node's scale per-frame would produce erratic ratios on
    // skeletal meshes in edit mode.
    if (mEditModeTransformActive) return;

    float dist = (camera->getDerivedPosition() - m_pTransformNode->getPosition()).length();
    if (dist < 1e-4f) dist = 1e-4f;
    // Target roughly constant pixel size; the 0.12 coefficient matches what
    // BevelGizmo uses so all on-screen gizmos look consistent.
    // Note: RotationGizmo is authored at 2.0f scale (see constructor) so its
    // circles encompass the translate/scale arrow handles — this factor is
    // applied in RotationGizmo itself, on top of the uniform node scale
    // here. Keeping the 2x in RotationGizmo means tweaking `s` updates all
    // three gizmos proportionally.
    float s = dist * 0.12f;
    m_pTransformNode->setScale(s, s, s);
}

void TransformOperator::updateGizmoPosition()
{
    Ogre::Vector3 currentPosition = Ogre::Vector3::ZERO;
    Ogre::Vector3 currentOrientation = Ogre::Vector3::ZERO;
    Ogre::Vector3 currentScale = Ogre::Vector3::UNIT_SCALE;

    // Bone-gizmo: when a bone is the active selection (Animation Control
    // panel), the gizmo follows the bone in world space. This takes
    // precedence over scene-node anchoring so the user gets immediate
    // visual feedback after picking a bone in the viewport.
    auto* animCtrl = AnimationControlController::instance();
    Ogre::Bone* activeBone = animCtrl->selectedBonePtr();
    Ogre::Entity* boneEntity = animCtrl->selectedEntity();
    if (activeBone && boneEntity && boneEntity->getParentSceneNode())
    {
        Ogre::SceneNode* entNode = boneEntity->getParentSceneNode();
        Ogre::Vector3 worldBonePos =
            entNode->convertLocalToWorldPosition(activeBone->_getDerivedPosition());
        // Gizmo orientation honors the WORLD/LOCAL toggle (X key):
        // WORLD → world axes (drag X = world-X). LOCAL → bone's frame
        // (drag X = bone-local-X). Default to WORLD because users
        // mostly want intuitive horizontal/vertical drags.
        Ogre::Quaternion gizmoOrient = Ogre::Quaternion::IDENTITY;
        if (mTransformSpace == SPACE_LOCAL) {
            gizmoOrient = entNode->_getDerivedOrientation() * activeBone->_getDerivedOrientation();
        }
        m_pTransformNode->setPosition(worldBonePos);
        m_pTransformNode->setOrientation(gizmoOrient);
        currentPosition = worldBonePos;
        emit selectedPositionChanged(currentPosition);
        emit selectedOrientationChanged(currentOrientation);
        emit selectedScaleChanged(currentScale);
        return;
    }

    // Edit mode: position gizmo at selected vertices centroid (world space)
    auto* editCtrl = EditModeController::instance();
    if (editCtrl->isEditModeActive() && !editCtrl->selectedVertices().empty()
        && editCtrl->editEntity() && editCtrl->editEntity()->getParentSceneNode())
    {
        Ogre::Vector3 localCentroid = editCtrl->getSelectedVerticesCentroid();
        Ogre::SceneNode* node = editCtrl->editEntity()->getParentSceneNode();
        Ogre::Vector3 worldCentroid = node->convertLocalToWorldPosition(localCentroid);
        m_pTransformNode->setPosition(worldCentroid);
    }
    else if(SelectionSet::getSingleton()->hasNodes())
    {
        currentOrientation  = SelectionSet::getSingleton()->getSelectionOrientation();
        currentScale        = SelectionSet::getSingleton()->getSelectionScale();
        Ogre::Vector3 pivotPoint = getPivotPoint();
        currentPosition     = pivotPoint;
        m_pTransformNode->setPosition(pivotPoint);
    }
    else if(SelectionSet::getSingleton()->hasEntities())
    {
        currentOrientation  = SelectionSet::getSingleton()->getSelectionOrientation();
        currentScale        = SelectionSet::getSingleton()->getSelectionScale();
        Ogre::Vector3 pivotPoint = getPivotPoint();
        currentPosition     = pivotPoint - SelectionSet::getSingleton()->getSelectionNodesCenter();
        m_pTransformNode->setPosition(pivotPoint);
    }
    else if(SelectionSet::getSingleton()->hasSubEntities())
    {
        try {
            // Position gizmo at the centroid of the selected sub-mesh vertices
            Ogre::Vector3 center = Ogre::Vector3::ZERO;
            int count = 0;
            for (int i = 0; i < SelectionSet::getSingleton()->getSubEntitiesCount(); ++i)
            {
                Ogre::SubEntity* sub = SelectionSet::getSingleton()->getSubEntity(i);
                if (!sub || !sub->getParent()) continue;
                Ogre::Entity* ent = sub->getParent();
                for (unsigned int s = 0; s < ent->getNumSubEntities(); ++s)
                {
                    if (ent->getSubEntity(s) == sub)
                    {
                        center += SubMeshTransform::getSubMeshCenter(ent, s);
                        ++count;
                        break;
                    }
                }
            }
            if (count > 0)
            {
                center /= static_cast<Ogre::Real>(count);
                Ogre::SubEntity* firstSub = SelectionSet::getSingleton()->getSubEntity(0);
                if (firstSub && firstSub->getParent() && firstSub->getParent()->getParentSceneNode()) {
                    Ogre::Vector3 nodePos = firstSub->getParent()->getParentSceneNode()->getPosition();
                    currentPosition = center;
                    m_pTransformNode->setPosition(center + nodePos);
                }
            }
        } catch (...) {
            // Sub-entity pointers may be stale — skip gizmo positioning
        }
    }

    // In edit mode, position gizmo at the centroid of selected vertices
    if (EditModeController::instance()->isEditModeActive()
        && !EditModeController::instance()->selectedVertices().empty()
        && EditModeController::instance()->editEntity())
    {
        auto* editCtrl = EditModeController::instance();
        Ogre::Vector3 localCentroid = editCtrl->getSelectedVerticesCentroid();
        Ogre::SceneNode* entityNode = editCtrl->editEntity()->getParentSceneNode();
        if (entityNode) {
            Ogre::Vector3 worldCentroid = entityNode->convertLocalToWorldPosition(localCentroid);
            m_pTransformNode->setPosition(worldCentroid);
            currentPosition = worldCentroid;
        }
    }

    emit selectedPositionChanged(currentPosition);
    emit selectedOrientationChanged(currentOrientation);
    emit selectedScaleChanged(currentScale);
}
void TransformOperator::onSelectionChanged()
{
    auto* viewportGrid = Manager::getSingleton()->getViewportGrid();

    //Change the objects view between Node or Mesh aspects
    if(SelectionSet::getSingleton()->hasNodes())
    {
        foreach(Ogre::SceneNode* obj,SelectionSet::getSingleton()->getNodesSelectionList())
        {
            //Restore node characteristics
            if(obj->getInitialScale()!=Ogre::Vector3::UNIT_SCALE
                    || obj->getInitialOrientation()!=Ogre::Quaternion::IDENTITY)
            {
                obj->resetToInitialState();
            }
        }

        //TODO - Create a grid to the nodes
        if(viewportGrid)
            viewportGrid->setPosition(Ogre::Vector3(0,0,0));
    }
    else
    {
        foreach(Ogre::Entity* obj,SelectionSet::getSingleton()->getEntitiesSelectionList())
        {
            //Reset the node characteristics
            if(obj->getParentSceneNode()->getScale()!=Ogre::Vector3::UNIT_SCALE
                    || obj->getParentSceneNode()->getOrientation()!=Ogre::Quaternion::IDENTITY)
                obj->getParentSceneNode()->setInitialState();

            obj->getParentSceneNode()->setScale(Ogre::Vector3::UNIT_SCALE);
            obj->getParentSceneNode()->setOrientation(Ogre::Quaternion::IDENTITY);

            //TODO - Create a grid to the nodes
            if(viewportGrid)
                viewportGrid->setPosition(obj->getParentSceneNode()->getPosition());
        }

        foreach(Ogre::SubEntity* obj,SelectionSet::getSingleton()->getSubEntitiesSelectionList())
        {
            //Reset the node characteristics
            if(obj->getParent()->getParentSceneNode()->getScale()!=Ogre::Vector3::UNIT_SCALE
                    || obj->getParent()->getParentSceneNode()->getOrientation()!=Ogre::Quaternion::IDENTITY)
                obj->getParent()->getParentSceneNode()->setInitialState();

            obj->getParent()->getParentSceneNode()->setScale(Ogre::Vector3::UNIT_SCALE);
            obj->getParent()->getParentSceneNode()->setOrientation(Ogre::Quaternion::IDENTITY);

            //TODO - Create a grid to the nodes
            if(viewportGrid)
                viewportGrid->setPosition(obj->getParent()->getParentSceneNode()->getPosition());
        }

        if(!SelectionSet::getSingleton()->hasEntities()&&!SelectionSet::getSingleton()->hasSubEntities())
        {
            //TODO - Create a grid to the nodes
            if(viewportGrid)
                viewportGrid->setPosition(Ogre::Vector3::ZERO);
        }
    }

    updateGizmo();
}

void TransformOperator::setActiveWidget(OgreWidget* ogreWidget)
{
    if(m_pActiveWidget) {
        m_pActiveWidget->setMouseTracking(false);
        disconnect(m_pActiveWidget, &QObject::destroyed, this, nullptr);
    }

    m_pActiveWidget = ogreWidget;

    if(m_pActiveWidget) {
        m_pActiveWidget->setMouseTracking(mTrackingEnable);
        connect(m_pActiveWidget, &QObject::destroyed, this, [this]() {
            m_pActiveWidget = nullptr;
        });
    }

}

// LCOV_EXCL_START — mouse/ray/viewport interaction requires active render window + camera
Ogre::Ray TransformOperator::rayFromScreenPoint(const QPoint& pos)
{
    if(m_pActiveWidget && m_pActiveWidget->getViewport()
       && m_pActiveWidget->getViewport()->getCamera())
    {
        int width = m_pActiveWidget->getViewport()->getActualWidth() / mWindowSizeModifier;
        int height = m_pActiveWidget->getViewport()->getActualHeight() / mWindowSizeModifier;

        Ogre::Real x = (Ogre::Real)(pos.x()) / (Ogre::Real)width;
        Ogre::Real y = (Ogre::Real)(pos.y()) / (Ogre::Real)height;

        return m_pActiveWidget->getViewport()->getCamera()->getCameraToViewportRay(x, y);
    }

    return Ogre::Ray();
}

Ogre::MovableObject* TransformOperator::performRaySelection(const QPoint& pos, bool findGizmo)
{
    if(m_pActiveWidget)
    {
        m_pRayQuery->setRay(rayFromScreenPoint(pos));

        if(findGizmo)
            m_pRayQuery->setQueryMask(GIZMO_QUERY_FLAGS);
        else
            m_pRayQuery->setQueryMask(SCENE_QUERY_FLAGS);

        Ogre::RaySceneQueryResult &queryResult =  m_pRayQuery->execute();
        Ogre::RaySceneQueryResult::iterator queryResultIterator = queryResult.begin();
        //Ogre::MovableObject *closest_movable;

        if(queryResultIterator != queryResult.end())
            if(queryResultIterator->movable)
                    return (queryResultIterator->movable);
    }

    return nullptr;
}


// from 0 to 1
void TransformOperator::performBoxSelection(const QPoint& first, const QPoint& second, SelectionMode mode)
{
    // deselect old & select the new ones if required
    if (mode == NEW_SELECT)
        SelectionSet::getSingleton()->clear();

    int left = first.x(), right = second.x(),
    top = first.y(), bottom = second.y();

    if (left > right)
            swap(left, right);

    if (top > bottom)
            swap(top, bottom);

    if ((right - left) * (bottom - top) < 0.0001)
    {
        // go to ray scene query as the rectangle is too small
        Ogre::MovableObject* obj = performRaySelection(first);
        if(obj)
        {
            if (mode == DEL_SELECT)
                SelectionSet::getSingleton()->removeOne(obj->getParentSceneNode());
            else
                SelectionSet::getSingleton()->append(obj->getParentSceneNode());
        }
        return;
    }

    // Creating ray in each rectangle corner
    Ogre::Ray topLeft = rayFromScreenPoint(QPoint(left, top));
    Ogre::Ray topRight = rayFromScreenPoint(QPoint(right, top));
    Ogre::Ray bottomLeft = rayFromScreenPoint(QPoint(left, bottom));
    Ogre::Ray bottomRight = rayFromScreenPoint(QPoint(right, bottom));

    // Creating planes to perform the planeBoundedVolume
    Ogre::PlaneBoundedVolume vol;
    vol.planes.push_back(Ogre::Plane(topLeft.getPoint(3), topRight.getPoint(3), bottomRight.getPoint(3)));         // front plane
    vol.planes.push_back(Ogre::Plane(topLeft.getOrigin(), topLeft.getPoint(100), topRight.getPoint(100)));         // top plane
    vol.planes.push_back(Ogre::Plane(topLeft.getOrigin(), bottomLeft.getPoint(100), topLeft.getPoint(100)));       // left plane
    vol.planes.push_back(Ogre::Plane(bottomLeft.getOrigin(), bottomRight.getPoint(100), bottomLeft.getPoint(100)));   // bottom plane
    vol.planes.push_back(Ogre::Plane(topRight.getOrigin(), topRight.getPoint(100), bottomRight.getPoint(100)));     // right plane

    // executing the query
    Ogre::PlaneBoundedVolumeList volList;
    volList.push_back(vol);

    m_pVolQuery->setVolumes(volList);
    m_pVolQuery->setQueryMask(SCENE_QUERY_FLAGS);
    Ogre::SceneQueryResult result = m_pVolQuery->execute();

    // add or append new selected obj
    Ogre::SceneQueryResultMovableList::iterator iter;
    for (iter = result.movables.begin(); iter != result.movables.end(); ++iter)
        if (mode == DEL_SELECT)
            SelectionSet::getSingleton()->removeOne((*iter)->getParentSceneNode());
        else
            SelectionSet::getSingleton()->append((*iter)->getParentSceneNode());

}

void TransformOperator::mousePressEvent(QMouseEvent *e)
{
    if (e->button()==Qt::LeftButton)
    {
        // Auto-rig marker placement (Mixamo-style) is active: left-click drops
        // the next marker on the mesh surface. Highest priority — like the
        // knife session, nothing else (selection/transform) fires while placing
        // markers. Dismissed via the dialog's Cancel/Commit.
        if (AutoRigController::instance()->markerMode())
        {
            AutoRigController::instance()->handleMarkerClick(m_pActiveWidget, e->pos());
            return;
        }

        auto* editCtrl = EditModeController::instance();

        // Knife session is active: left-click adds a cut point at the
        // hovered position. Nothing else (selection, bevel, transform)
        // fires while the knife is open — the session is dismissed with
        // Enter (commit) or Esc (cancel), or automatically on edit-mode
        // exit.
        if (editCtrl->knifeSessionActive())
        {
            editCtrl->addKnifePoint(m_pActiveWidget, e->pos().x(), e->pos().y());
            return;
        }

        // Bevel session is active: priority path. If the click hits the
        // gizmo handle → start a width-drag; anywhere else → commit the
        // current width and fall through to normal selection.
        if (editCtrl->bevelSessionActive())
        {
            auto* hit = performRaySelection(e->pos(), /*findGizmo=*/true);
            if (hit && editCtrl->isBevelGizmoHandle(hit)) {
                mBevelDragStartRay = rayFromScreenPoint(e->pos());
                mBevelDragStartWidth = editCtrl->bevelGizmoWidth();
                mBevelDragActive = true;
                SentryReporter::addBreadcrumb("ui.transform", "Bevel: drag start");
                return;
            }
            // Click anywhere else — commit and consume the click. Matches
            // Blender/Maya: a click outside the bevel gizmo ends the tool
            // without immediately starting a new selection or transform.
            editCtrl->commitBevel();
            return;
        }

        // Texture paint takes priority over selection/box pick: it works
        // in Material Mode without entering Edit Mode, so we handle it
        // before the Edit-Mode-gated branch below.
        {
            auto* texPaint = TexturePaintController::instance();
            if (texPaint->texturePaintEnabled() && mTransformState == TS_SELECT) {
                // For the Wand tool only: probe the mesh first. If the
                // click missed (empty space behind the model), Photoshop
                // / GIMP convention is to clear the current selection.
                // beginStroke would otherwise still return true on a miss
                // for texture paint (it creates the session before
                // hit-testing), masking the click-outside case.
                if (texPaint->brushTool() == TexturePaintController::ToolSmartSelect
                    && !texPaint->wouldStrokeHit(m_pActiveWidget, e->pos())) {
                    // Wand miss. If there's a mask, treat it as
                    // "click empty space to clear" — Photoshop /
                    // GIMP convention. Otherwise let the click
                    // fall through to the normal selection / box-
                    // pick path so the user can still select a
                    // different mesh without first switching off
                    // the wand.
                    if (texPaint->hasSelectionMask()) {
                        texPaint->clearSelectionMask();
                        SentryReporter::addBreadcrumb("ui.action",
                            "Wand: cleared selection (click outside mesh)");
                        return;
                    }
                    // No mask — fall through to normal selection.
                }
                if (texPaint->beginStroke(m_pActiveWidget, e->pos())) {
                    mTexturePaintDragActive = true;
                    SentryReporter::addBreadcrumb("ui.action", "Texture paint: stroke begin");
                    return;
                }
                // Cursor missed the mesh — fall through to normal selection.
            }
        }

        // In edit mode, delegate selection to EditModeController
        if (EditModeController::instance()->isEditModeActive() && mTransformState == TS_SELECT)
        {
            auto* editCtrl = EditModeController::instance();
            if (editCtrl->vertexPaintEnabled()) {
                if (editCtrl->beginVertexPaintStroke(m_pActiveWidget, e->pos())) {
                    mVertexPaintDragActive = true;
                    SentryReporter::addBreadcrumb("ui.action", "Vertex paint: stroke begin");
                    return;
                }
                // Paint mode on: do not start box / component selection on miss.
                return;
            }
            mScreenStart = e->pos();
            m_pSelectionBox->clear();
            m_pSelectionBox->setVisible(true);
            return;
        }

        // In edit mode with a transform tool and selected vertices: start vertex transform
        if (EditModeController::instance()->isEditModeActive()
            && mTransformState != TS_SELECT && mTransformState != TS_NONE
            && !EditModeController::instance()->selectedVertices().empty())
        {
            auto* editCtrl = EditModeController::instance();

            if (mTransformState == TS_TRANSLATE)
                SentryReporter::addBreadcrumb("ui.transform", "Edit mode: translate vertices");
            else if (mTransformState == TS_ROTATE)
                SentryReporter::addBreadcrumb("ui.transform", "Edit mode: rotate vertices");
            else if (mTransformState == TS_SCALE)
                SentryReporter::addBreadcrumb("ui.transform", "Edit mode: scale vertices");

            // Snapshot vertex positions for undo
            mEditModeStartPositions = editCtrl->snapshotVertexPositions();
            mEditModeUndoSnapshot = mEditModeStartPositions; // immutable copy for undo
            mEditModeTransformActive = true;

            // Reset snap accumulators
            mSnapTranslationAccum = Ogre::Vector3::ZERO;
            mSnapRotationAccum = Ogre::Vector3::ZERO;
            mSnapScaleAccum = Ogre::Vector3::ZERO;
            mSnapScaleCumulative = Ogre::Vector3::UNIT_SCALE;

            Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
            std::pair<bool, Ogre::Real> result = mouseRay.intersects(
                Ogre::Plane(mouseRay.getDirection(), m_pTransformNode->getPosition()));
            if (result.first) {
                mStartPoint = mouseRay.getPoint(result.second);
                if (mTransformState == TS_SCALE) {
                    mEditModeScalePivot = editCtrl->getSelectedVerticesCentroid();
                    mEditModeScaleStartPixel = e->pos();
                }
            }
            return;
        }

        if(mTransformState == TS_SELECT)
        {
            // Light / bone viewport picks before box-select. Walk hits
            // front-to-back and accept the first tagged movable.
            if (m_pRayQuery)
            {
                m_pRayQuery->setRay(rayFromScreenPoint(e->pos()));
                m_pRayQuery->setQueryMask(LIGHT_QUERY_FLAGS | BONE_QUERY_FLAGS);
                m_pRayQuery->setSortByDistance(true);
                Ogre::RaySceneQueryResult& res = m_pRayQuery->execute();
                for (const auto& hit : res)
                {
                    if (!hit.movable)
                        continue;

                    const QString lightName = LightVisualizer::lightNameForMovable(hit.movable);
                    if (!lightName.isEmpty())
                    {
                        Ogre::SceneNode* node = Manager::getSingleton()->getSceneNode(lightName);
                        if (node)
                        {
                            SelectionSet::getSingleton()->selectOne(node);
                            SentryReporter::addBreadcrumb(
                                QStringLiteral("ui.action"),
                                QStringLiteral("Light picked: %1").arg(lightName));
                        }
                        return;
                    }

                    Ogre::String boneName = SkeletonDebug::boneNameForMovable(hit.movable);
                    if (!boneName.empty())
                    {
                        AnimationControlController::instance()->selectBone(
                            QString::fromStdString(boneName));
                        SentryReporter::addBreadcrumb("ui.action",
                            QString("Bone picked: %1").arg(QString::fromStdString(boneName)));
                        return;
                    }
                }
            }

            mScreenStart = e->pos();
            m_pSelectionBox->clear();
            m_pSelectionBox->setVisible(true);

        }
        else if (e->button() == Qt::LeftButton
                 && shouldRouteToBoneGizmo(
                        mTransformState,
                        AnimationControlController::instance()->selectedBonePtr(),
                        AnimationControlController::instance()->boneCanTranslate(
                            AnimationControlController::instance()->selectedBonePtr())))
        {
            // Bone-gizmo press (translate/rotate/scale): drive the
            // bone's local TRS instead of any scene-node selection.
            // Translation is restricted on rigged non-root bones —
            // moving them tears the bone from its parent — so for
            // those, this branch is skipped and we fall through to the
            // entity-translate branch below. Rotation and scale always
            // go through the bone path because that's the primary
            // posing workflow.
            Ogre::Bone* bone = AnimationControlController::instance()->selectedBonePtr();
            mBoneDragActive       = true;
            mBoneStartPos         = bone->getPosition();
            mBoneStartOrient      = bone->getOrientation();
            mBoneStartScale       = bone->getScale();
            mBoneStartDerivedPos  = bone->_getDerivedPosition();
            mBoneDragGizmoOrigin  = m_pTransformNode->getPosition();
            // Lock the axis at PRESS time. The relevant gizmo varies
            // by mode: translate uses arrow handles, rotate uses
            // circle handles, scale uses cube handles.
            Ogre::MovableObject* pressGizmoAxis = performRaySelection(e->pos(), true);
            if (pressGizmoAxis) {
                if (mTransformState == TS_TRANSLATE)
                    mTransformVector = m_pTranslationGizmo->highlightAxis(pressGizmoAxis);
                else if (mTransformState == TS_ROTATE)
                    mTransformVector = m_pRotationGizmo->highlightCircle(pressGizmoAxis);
                else
                    mTransformVector = m_pScaleGizmo->highlightAxis(pressGizmoAxis);
            } else {
                mTransformVector = Ogre::Vector3::ZERO;
            }
            // Without this the skeleton's animation system overwrites
            // the bone's local TRS every frame from interpolated keys,
            // so our drag never visibly sticks. Manual-control mode
            // tells the skeleton to leave this bone alone.
            bone->setManuallyControlled(true);
            // setManuallyControlled only excludes this bone from
            // Skeleton::reset() — animation TRACKS still apply to it
            // each frame (the Ogre docs warn about this). Mute the
            // animation's contribution to this bone via the blend mask
            // so per-frame _updateAnimation doesn't overwrite our edit.
            // Capture the previous mask value per state so the release
            // can restore exactly what was there (instead of blanket
            // resetting to 1.0, which would destroy any layered/masked
            // animation setup the user had pre-drag).
            mBoneDragSavedMaskWeights.clear();
            if (Ogre::Entity* dragEnt = AnimationControlController::instance()->selectedEntity()) {
                if (Ogre::AnimationStateSet* states = dragEnt->getAllAnimationStates()) {
                    const auto numBones = static_cast<size_t>(dragEnt->getSkeleton()->getNumBones());
                    for (const auto& pair : states->getAnimationStates()) {
                        Ogre::AnimationState* st = pair.second;
                        if (!st->getEnabled()) continue;
                        const float before = st->hasBlendMask()
                            ? st->getBlendMaskEntry(bone->getHandle()) : 1.0f;
                        mBoneDragSavedMaskWeights.append({st->getAnimationName(), before});
                        if (!st->hasBlendMask()) st->createBlendMask(numBones, 1.0f);
                        st->setBlendMaskEntry(bone->getHandle(), 0.0f);
                    }
                }
            }
            // Also pause animation playback during the drag — even with
            // manualControlled=true on the picked bone, animation still
            // moves its parent bones, which compounds onto the local
            // edit and looks like the model is rotating wildly. Pausing
            // is what every DCC tool (Blender/Maya) does on bone scrub.
            mBoneDragWasPlaying = PropertiesPanelController::instance()->isPlaying();
            if (mBoneDragWasPlaying)
                PropertiesPanelController::instance()->setPlaying(false);
            const char* breadcrumb =
                mTransformState == TS_TRANSLATE ? "Translate bone" :
                mTransformState == TS_ROTATE    ? "Rotate bone"    :
                                                  "Scale bone";
            SentryReporter::addBreadcrumb("ui.transform", breadcrumb);

            Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
            auto result = mouseRay.intersects(
                Ogre::Plane(mouseRay.getDirection(), m_pTransformNode->getPosition()));
            if (result.first)
                mStartPoint = mouseRay.getPoint(result.second);
            // Scale uses a pixel-delta model rather than world-space
            // rays; capture the press-time pixel so the move handler
            // can compute deltas.
            if (mTransformState == TS_SCALE)
                mScaleDragStartPixel = e->pos();
        }
        else if((!SelectionSet::getSingleton()->isEmpty()) && (e->button() == Qt::LeftButton))
        {
            // Log a single breadcrumb per transform gesture (not per mouse-move frame)
            if(mTransformState == TS_TRANSLATE)
                SentryReporter::addBreadcrumb("ui.transform", "Translate selected");
            else if(mTransformState == TS_ROTATE)
                SentryReporter::addBreadcrumb("ui.transform", "Rotate selected");
            else if(mTransformState == TS_SCALE)
                SentryReporter::addBreadcrumb("ui.transform", "Scale selected");

            // Capture undo state for scene nodes
            mUndoStartPositions.clear();
            mUndoStartOrientations.clear();
            mUndoStartScales.clear();
            if (SelectionSet::getSingleton()->hasNodes())
            {
                for (Ogre::SceneNode* node : SelectionSet::getSingleton()->getNodesSelectionList())
                {
                    mUndoStartPositions.append(node->getPosition());
                    mUndoStartOrientations.append(node->getOrientation());
                    mUndoStartScales.append(node->getScale());
                }
            }

            // Capture undo state for sub-entities (vertex snapshots)
            mUndoSubEntities.clear();
            mUndoSubMeshPositions.clear();
            if (SelectionSet::getSingleton()->hasSubEntities())
            {
                SentryReporter::addBreadcrumb("ui.transform",
                    QString("Sub-mesh transform start (%1 sub-entities)")
                        .arg(SelectionSet::getSingleton()->getSubEntitiesCount()));

                for (Ogre::SubEntity* sub : SelectionSet::getSingleton()->getSubEntitiesSelectionList())
                {
                    Ogre::Entity* ent = sub->getParent();
                    unsigned int subIdx = 0;
                    for (unsigned int s = 0; s < ent->getNumSubEntities(); ++s)
                    {
                        if (ent->getSubEntity(s) == sub) { subIdx = s; break; }
                    }
                    mUndoSubEntities.append(sub);
                    mUndoSubMeshPositions.append(SubMeshTransform::readPositions(ent, subIdx));
                }
            }

            // Reset snap accumulators at drag start
            mSnapTranslationAccum = Ogre::Vector3::ZERO;
            mSnapRotationAccum = Ogre::Vector3::ZERO;
            mSnapScaleAccum = Ogre::Vector3::ZERO;
            mSnapScaleCumulative = Ogre::Vector3::UNIT_SCALE;

            // Checking the ray intersection with a plane parallel to viewport & on the geometric center of selection
            Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
            std::pair<bool, Ogre::Real> result = mouseRay.intersects(Ogre::Plane(mouseRay.getDirection(), m_pTransformNode->getPosition()));

            if(result.first)
            {
                mStartPoint = mouseRay.getPoint(result.second);
                // For scale: record start pixel for pixel-delta drag. The
                // world-space ray-plane math used to live here but it compounded
                // drift on trackpads (high event rate → many rebases → runaway).
                if(mTransformState == TS_SCALE) {
                    mScaleDragStartPixel = e->pos();
                }
            }
        }
    }
}

void TransformOperator::mouseMoveEvent(QMouseEvent *e)
{
    // Vertex paint drag: update on every move while LMB is held.
    auto* editCtrl = EditModeController::instance();
    if (mVertexPaintDragActive && editCtrl->isEditModeActive()
        && (e->buttons() & Qt::LeftButton) && m_pActiveWidget)
    {
        editCtrl->updateVertexPaintStroke(m_pActiveWidget, e->pos());
        // Don't consume: allow camera hover, etc., to still run.
    } else if (editCtrl->isEditModeActive()
               && editCtrl->vertexPaintEnabled()
               && m_pActiveWidget)
    {
        editCtrl->updateVertexPaintPreview(m_pActiveWidget, e->pos());
    }

    // Texture paint: while LMB held, update stroke; otherwise update
    // the hover preview so the user sees the brush ring even before
    // clicking. Decoupled from Edit Mode — texture paint owns its own
    // EditableMesh now.
    auto* texPaint = TexturePaintController::instance();
    if (mTexturePaintDragActive && (e->buttons() & Qt::LeftButton) && m_pActiveWidget)
    {
        texPaint->updateStroke(m_pActiveWidget, e->pos());
    } else if (texPaint->texturePaintEnabled() && m_pActiveWidget) {
        texPaint->updateMeshHover(m_pActiveWidget, e->pos());
    }

    // Knife hover preview: cheap to update on every move while the session
    // is active, and draws the ghost segment from the last confirmed
    // point to the cursor. Does not consume the event — other handlers
    // (camera, gizmo drag) still run below.
    if (editCtrl->knifeSessionActive() && m_pActiveWidget)
    {
        editCtrl->updateKnifeHover(m_pActiveWidget, e->pos().x(), e->pos().y());
    }

    // Bevel gizmo drag: priority path.
    if (mBevelDragActive && EditModeController::instance()->bevelSessionActive())
    {
        Ogre::Ray dragRay = rayFromScreenPoint(e->pos());
        EditModeController::instance()->updateBevelFromDrag(
            mBevelDragStartRay, dragRay, mBevelDragStartWidth);
        return;
    }

    // Edit mode vertex transform: intercept drag and route to EditModeController
    if (mEditModeTransformActive && EditModeController::instance()->isEditModeActive()
        && !mStartPoint.isZeroLength())
    {
        auto* editCtrl = EditModeController::instance();
        Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
        std::pair<bool, Ogre::Real> result = mouseRay.intersects(
            Ogre::Plane(mouseRay.getDirection(), m_pTransformNode->getPosition()));

        if (result.first)
        {
            Ogre::Vector3 point = mouseRay.getPoint(result.second);

            if (mTransformState == TS_TRANSLATE)
            {
                Ogre::Vector3 worldDelta = point - mStartPoint;
                Ogre::Vector3 translation;

                if (mTransformSpace == SPACE_LOCAL && !mTransformVector.isZeroLength()) {
                    Ogre::Quaternion gizmoOrientation = m_pTransformNode->getOrientation();
                    Ogre::Vector3 localDelta = gizmoOrientation.Inverse() * worldDelta;
                    localDelta *= mTransformVector;
                    translation = gizmoOrientation * localDelta;
                } else {
                    translation = worldDelta * mTransformVector;
                }

                // Convert world-space translation to local mesh space
                Ogre::SceneNode* entityNode = editCtrl->editEntity()->getParentSceneNode();
                if (entityNode) {
                    Ogre::Quaternion worldOrient = entityNode->_getDerivedOrientation();
                    Ogre::Vector3 worldScale = entityNode->_getDerivedScale();
                    Ogre::Vector3 localTranslation = worldOrient.Inverse() * translation;
                    localTranslation /= worldScale;

                    // Restore to start positions then apply new delta
                    editCtrl->restoreVertexPositions(mEditModeStartPositions);
                    editCtrl->translateSelectedVertices(localTranslation);
                }

                mStartPoint = point;
                // Re-snapshot so incremental deltas accumulate correctly
                mEditModeStartPositions = editCtrl->snapshotVertexPositions();
                updateGizmoPosition();
            }
            else if (mTransformState == TS_ROTATE)
            {
                Ogre::Vector3 vectorStart = mStartPoint - m_pTransformNode->getPosition();
                Ogre::Vector3 vectorEnd = point - m_pTransformNode->getPosition();

                Ogre::Quaternion rotation;
                if (mTransformSpace == SPACE_LOCAL && !mTransformVector.isZeroLength()) {
                    Ogre::Quaternion gizmoOri = m_pTransformNode->getOrientation();
                    Ogre::Vector3 localStart = gizmoOri.Inverse() * vectorStart;
                    Ogre::Vector3 localEnd = gizmoOri.Inverse() * vectorEnd;
                    Ogre::Quaternion localRot = localStart.getRotationTo(localEnd);
                    localRot.x *= mTransformVector.x;
                    localRot.y *= mTransformVector.y;
                    localRot.z *= mTransformVector.z;
                    localRot.normalise();
                    rotation = gizmoOri * localRot * gizmoOri.Inverse();
                } else {
                    rotation = vectorStart.getRotationTo(vectorEnd);
                    rotation.x *= mTransformVector.x;
                    rotation.y *= mTransformVector.y;
                    rotation.z *= mTransformVector.z;
                    rotation.normalise();
                }

                // Convert world rotation to local mesh space
                Ogre::SceneNode* entityNode = editCtrl->editEntity()->getParentSceneNode();
                if (entityNode) {
                    Ogre::Quaternion worldOrient = entityNode->_getDerivedOrientation();
                    Ogre::Quaternion localRotation = worldOrient.Inverse() * rotation * worldOrient;

                    editCtrl->restoreVertexPositions(mEditModeStartPositions);
                    editCtrl->rotateSelectedVertices(localRotation);
                }

                mStartPoint = point;
                mEditModeStartPositions = editCtrl->snapshotVertexPositions();
                updateGizmoPosition();
            }
            else if (mTransformState == TS_SCALE)
            {
                QPoint delta = e->pos() - mEditModeScaleStartPixel;
                float pixels = static_cast<float>(delta.x() - delta.y());
                const float kPixelsPerDouble = 100.0f;
                float ratio = std::pow(2.0f, pixels / kPixelsPerDouble);
                if (ratio < 0.01f) ratio = 0.01f;
                if (ratio > 100.0f) ratio = 100.0f;

                Ogre::Vector3 scaleFactor = (mTransformVector == Ogre::Vector3::ZERO)
                    ? Ogre::Vector3(ratio, ratio, ratio)
                    : Ogre::Vector3::UNIT_SCALE + (mTransformVector * (ratio - 1.0f));

                // Apply snap if enabled, matching object-mode behavior.
                bool snapping = mSnapEnabled || (e->modifiers() & Qt::ControlModifier);
                if (snapping) {
                    Ogre::Vector3 snappedDelta = snapScale(
                        scaleFactor - Ogre::Vector3::UNIT_SCALE, mSnapScaleStep);
                    scaleFactor = Ogre::Vector3::UNIT_SCALE + snappedDelta;
                    // Snap can round a delta near -1 down to exactly -1 (e.g.
                    // ratio=0.01 → delta=-0.99 → snapped to -1 at step=1.0),
                    // producing a zero scale that collapses geometry. Reclamp
                    // to the same 0.01 floor the pre-snap ratio enforces.
                    scaleFactor.x = std::max<Ogre::Real>(scaleFactor.x, 0.01f);
                    scaleFactor.y = std::max<Ogre::Real>(scaleFactor.y, 0.01f);
                    scaleFactor.z = std::max<Ogre::Real>(scaleFactor.z, 0.01f);
                }

                editCtrl->scaleFromSnapshot(mEditModeUndoSnapshot,
                                            mEditModeScalePivot,
                                            scaleFactor);
                updateGizmoPosition();
            }
        }
        return;
    }

    if(mTransformState == TS_SELECT
       && !(EditModeController::instance()->isEditModeActive()
            && EditModeController::instance()->vertexPaintEnabled()))
    {
        if(m_pSelectionBox->isVisible() && m_pActiveWidget)
        {
            int width = m_pActiveWidget->getViewport()->getActualWidth() / mWindowSizeModifier;
            int height = m_pActiveWidget->getViewport()->getActualHeight() / mWindowSizeModifier;

            float xStart  = (float)(mScreenStart.x())/(float)width*2.0f-1.0f;
            float xStop   = (float)(e->pos().x())/(float)width*2.0f-1.0f;
            float yStart  = 1.0f-(float)(mScreenStart.y())/(float)height*2.0f;
            float yStop   = 1.0f-(float)(e->pos().y())/(float)height*2.0f;

            m_pSelectionBox->drawBox(xStart, yStart, xStop, yStop);
        }
    }
    else if (mTransformState == TS_TRANSLATE && mBoneDragActive
             && AnimationControlController::instance()->selectedBonePtr())
    {
        // Axis is locked at PRESS time (see press handler). If the
        // user pressed off all arrows, mTransformVector is ZERO and
        // the drag is a no-op until release.
        if (mTransformVector.isZeroLength()) return;

        // Project mouse onto a plane through the press-time gizmo
        // position (not the current one): otherwise the projection
        // plane shifts as the gizmo follows the bone, producing
        // nonlinear drift as the user drags further from the start.
        Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
        auto result = mouseRay.intersects(
            Ogre::Plane(mouseRay.getDirection(), mBoneDragGizmoOrigin));
        if (!result.first) return;

        Ogre::Vector3 point = mouseRay.getPoint(result.second);
        Ogre::Vector3 rawWorldDelta = point - mStartPoint;

        // The gizmo's frame is rotated to match the bone (so its arrows
        // visually align with the bone's axes). mTransformVector is in
        // gizmo-local space — convert the world delta into gizmo-local,
        // mask with the highlighted axis, then convert back to world.
        // This is what the scene-node code does for SPACE_LOCAL; bones
        // always need it because their gizmo is never identity-aligned.
        Ogre::Quaternion gizmoOri = m_pTransformNode->getOrientation();
        Ogre::Vector3 localDelta  = gizmoOri.Inverse() * rawWorldDelta;
        localDelta *= mTransformVector;
        Ogre::Vector3 worldDelta  = gizmoOri * localDelta;

        Ogre::Bone* bone = AnimationControlController::instance()->selectedBonePtr();
        Ogre::Entity* ent = AnimationControlController::instance()->selectedEntity();
        if (!ent || !ent->getParentSceneNode()) return;

        // Convert world delta → skeleton-local delta (the skeleton
        // lives in the entity's parent scene node frame).
        Ogre::Quaternion entWorldOri   = ent->getParentSceneNode()->_getDerivedOrientation();
        Ogre::Vector3    entWorldScale = ent->getParentSceneNode()->_getDerivedScale();
        Ogre::Vector3 skelLocalDelta = entWorldOri.Inverse() * worldDelta;
        skelLocalDelta /= entWorldScale;

        // Drive the bone via its skeleton-local (derived) position. Ogre
        // handles parent-frame conversion internally, so this works
        // correctly for any bone regardless of parent orientation —
        // crucial when the parent itself is mid-animation pose.
        Ogre::Vector3 targetSkelPos = mBoneStartDerivedPos + skelLocalDelta;
        // _setDerivedPosition is a no-op on parentless bones (Ogre's
        // implementation only writes when there's a parent). For true
        // skeleton roots, fall back to setPosition since for them
        // local space == skeleton-local space.
        if (bone->getParent())
            bone->_setDerivedPosition(targetSkelPos);
        else
            bone->setPosition(targetSkelPos);
        bone->needUpdate(true);
        // Don't call _updateAnimation here — it triggers Skeleton::reset
        // which can re-apply animation tracks to OTHER bones each move
        // event, producing visible compound rotation of the rest of the
        // skeleton. The next render frame's normal animation pass will
        // refresh skinning + bone-visual TagPoints automatically.
        updateGizmoPosition();
        return;
    }
    else if (mTransformState == TS_ROTATE && mBoneDragActive
             && AnimationControlController::instance()->selectedBonePtr())
    {
        // Bone-gizmo rotation: drive the bone's local orientation. The
        // RotationGizmo's circles are anchored to the bone-world frame
        // (via the gizmo node), so dragging a circle rotates the bone
        // around the corresponding axis. Math mirrors the scene-node
        // rotate handler — start/end vectors from gizmo center, get
        // the rotation between them, mask to the highlighted circle.
        if (mTransformVector.isZeroLength()) return;
        Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
        auto result = mouseRay.intersects(
            Ogre::Plane(mouseRay.getDirection(), mBoneDragGizmoOrigin));
        if (!result.first) return;

        Ogre::Vector3 point = mouseRay.getPoint(result.second);
        Ogre::Vector3 vectorStart = mStartPoint - mBoneDragGizmoOrigin;
        Ogre::Vector3 vectorEnd   = point - mBoneDragGizmoOrigin;
        Ogre::Quaternion gizmoOri = m_pTransformNode->getOrientation();
        Ogre::Vector3 localStart  = gizmoOri.Inverse() * vectorStart;
        Ogre::Vector3 localEnd    = gizmoOri.Inverse() * vectorEnd;
        Ogre::Quaternion localRot = localStart.getRotationTo(localEnd);
        localRot.x *= mTransformVector.x;
        localRot.y *= mTransformVector.y;
        localRot.z *= mTransformVector.z;
        localRot.normalise();
        // Convert to world rotation, then accumulate onto the press-time
        // bone orientation. Re-anchor mStartPoint each event so the
        // delta is incremental (matches the scene-node behavior).
        Ogre::Quaternion worldRot = gizmoOri * localRot * gizmoOri.Inverse();
        Ogre::Bone* bone = AnimationControlController::instance()->selectedBonePtr();
        Ogre::Entity* ent = AnimationControlController::instance()->selectedEntity();
        if (!ent || !ent->getParentSceneNode()) return;

        // Map world rotation into bone-local space by composing with
        // the entity world + parent-bone derived orientations.
        Ogre::Quaternion entWorldOri = ent->getParentSceneNode()->_getDerivedOrientation();
        Ogre::Quaternion parentDerOri = Ogre::Quaternion::IDENTITY;
        if (bone->getParent())
            parentDerOri = static_cast<Ogre::Bone*>(bone->getParent())->_getDerivedOrientation();
        Ogre::Quaternion worldToParent = (entWorldOri * parentDerOri).Inverse();
        Ogre::Quaternion localFrameRot = worldToParent * worldRot * (entWorldOri * parentDerOri);

        bone->rotate(localFrameRot, Ogre::Node::TS_LOCAL);
        bone->needUpdate(true);
        mStartPoint = point;
        updateGizmoPosition();
        return;
    }
    else if (mTransformState == TS_SCALE && mBoneDragActive
             && AnimationControlController::instance()->selectedBonePtr())
    {
        // Bone-gizmo scale: pixel-delta drag against the press-time
        // pixel position, scale uniformly (or per-axis when an axis
        // is locked) on the bone's local scale. Less common than
        // rotation but completes the W/E/R triad.
        if (mTransformVector.isZeroLength()) return;
        const QPoint pixelDelta = e->pos() - mScaleDragStartPixel;
        const float pixels = static_cast<float>(pixelDelta.x() - pixelDelta.y());
        constexpr float kPixelsPerDouble = 100.0f;
        float ratio = std::pow(2.0f, pixels / kPixelsPerDouble);
        if (ratio < 0.01f) ratio = 0.01f;
        if (ratio > 100.0f) ratio = 100.0f;

        Ogre::Vector3 scaleFactor = (mTransformVector == Ogre::Vector3::ZERO)
            ? Ogre::Vector3(ratio, ratio, ratio)
            : Ogre::Vector3::UNIT_SCALE + (mTransformVector * (ratio - 1.0f));

        Ogre::Bone* bone = AnimationControlController::instance()->selectedBonePtr();
        bone->setScale(mBoneStartScale * scaleFactor);
        bone->needUpdate(true);
        updateGizmoPosition();
        return;
    }
    else if(mTransformState == TS_TRANSLATE && (!SelectionSet::getSingleton()->isEmpty()))
    {
        //Translate the selected object
        if(mStartPoint.isZeroLength())
        {
            // We've not start Dragging yet -> survey gizmo hit
            Ogre::MovableObject* gizmoAxis = performRaySelection(e->pos(), true);
            if(gizmoAxis)
                mTransformVector = m_pTranslationGizmo->highlightAxis(gizmoAxis);
            else
            {
                m_pTranslationGizmo->createAxis();
                mTransformVector = Ogre::Vector3::ZERO;
            }
        }
        else
        {
            // We've start Dragging -> let's translate !
            Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
            std::pair<bool, Ogre::Real> result = mouseRay.intersects(Ogre::Plane(mouseRay.getDirection(), m_pTransformNode->getPosition()));

            if(result.first)
            {
                Ogre::Vector3 point = mouseRay.getPoint(result.second);
                Ogre::Vector3 worldDelta = point - mStartPoint;
                Ogre::Vector3 translation;

                if (mTransformSpace == SPACE_LOCAL && !mTransformVector.isZeroLength())
                {
                    // Transform world delta into gizmo's local space, mask axis, transform back
                    Ogre::Quaternion gizmoOrientation = m_pTransformNode->getOrientation();
                    Ogre::Vector3 localDelta = gizmoOrientation.Inverse() * worldDelta;
                    localDelta *= mTransformVector;
                    translation = gizmoOrientation * localDelta;
                }
                else
                {
                    translation = worldDelta * mTransformVector;
                }

                // Apply snap if Ctrl is held or snap is permanently enabled
                bool snapping = mSnapEnabled || (e->modifiers() & Qt::ControlModifier);
                if (snapping)
                {
                    mSnapTranslationAccum += translation;
                    Ogre::Vector3 snapped = snapTranslation(mSnapTranslationAccum, mSnapGridSize);
                    if (snapped.isZeroLength())
                    {
                        // Not enough accumulated to reach a snap step yet — consume the raw delta
                        mStartPoint = point;
                    }
                    else
                    {
                        mSnapTranslationAccum -= snapped;
                        translateSelected(snapped);
                        mStartPoint = point;
                        emit selectedPositionChanged(SelectionSet::getSingleton()->getSelectionCenter());
                        updateGizmoPosition();
                    }
                }
                else
                {
                    translateSelected(translation);
                    mStartPoint = point;
                    emit selectedPositionChanged(SelectionSet::getSingleton()->getSelectionCenter());
                    updateGizmoPosition();
                }
            }
        }
    }
    else if(mTransformState == TS_ROTATE && (!SelectionSet::getSingleton()->isEmpty()))
    {
        if(mStartPoint.isZeroLength())
        {
            // We've not start Dragging yet -> survey gizmo hit
            Ogre::MovableObject* gizmoAxis = performRaySelection(e->pos(), true);
            if(gizmoAxis)
                mTransformVector = m_pRotationGizmo->highlightCircle(gizmoAxis);
            else
            {
                m_pRotationGizmo->createCircles();
                mTransformVector = Ogre::Vector3::ZERO;
            }
        }
        else
        {
            // We've start Dragging -> let's rotate !
            Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
            std::pair<bool, Ogre::Real> result = mouseRay.intersects(Ogre::Plane(mouseRay.getDirection(), m_pTransformNode->getPosition()));

            if(result.first)
            {
                Ogre::Vector3 point = mouseRay.getPoint(result.second);
                Ogre::Vector3 vectorStart = mStartPoint - m_pTransformNode->getPosition();
                Ogre::Vector3 vectorEnd = point - m_pTransformNode->getPosition();

                Ogre::Quaternion rotation;
                if (mTransformSpace == SPACE_LOCAL && !mTransformVector.isZeroLength())
                {
                    // Transform vectors into gizmo local space, compute rotation there, mask, transform back
                    Ogre::Quaternion gizmoOri = m_pTransformNode->getOrientation();
                    Ogre::Vector3 localStart = gizmoOri.Inverse() * vectorStart;
                    Ogre::Vector3 localEnd = gizmoOri.Inverse() * vectorEnd;
                    Ogre::Quaternion localRot = localStart.getRotationTo(localEnd);
                    localRot.x *= mTransformVector.x;
                    localRot.y *= mTransformVector.y;
                    localRot.z *= mTransformVector.z;
                    localRot.normalise();
                    rotation = gizmoOri * localRot * gizmoOri.Inverse();
                }
                else
                {
                    rotation = vectorStart.getRotationTo(vectorEnd);
                    rotation.x *= mTransformVector.x;
                    rotation.y *= mTransformVector.y;
                    rotation.z *= mTransformVector.z;
                    rotation.normalise();
                }

                // Apply snap if Ctrl is held or snap is permanently enabled
                bool snapping = mSnapEnabled || (e->modifiers() & Qt::ControlModifier);
                if (snapping)
                {
                    // Convert incremental rotation to Euler for accumulation
                    Ogre::Euler euler;
                    euler.fromQuaternion(rotation);
                    Ogre::Vector3 deltaDegs(euler.pitch().valueDegrees(),
                                            euler.yaw().valueDegrees(),
                                            euler.roll().valueDegrees());
                    mSnapRotationAccum += deltaDegs;

                    // Snap each axis independently
                    Ogre::Vector3 snappedDegs(
                        snapAngle(mSnapRotationAccum.x, mSnapAngleStep),
                        snapAngle(mSnapRotationAccum.y, mSnapAngleStep),
                        snapAngle(mSnapRotationAccum.z, mSnapAngleStep)
                    );

                    if (snappedDegs.isZeroLength())
                    {
                        mStartPoint = point;
                    }
                    else
                    {
                        mSnapRotationAccum -= snappedDegs;
                        Ogre::Euler snappedEuler(Ogre::Degree(snappedDegs.y),
                                                 Ogre::Degree(snappedDegs.x),
                                                 Ogre::Degree(snappedDegs.z));
                        rotateSelected(snappedEuler.toQuaternion());
                        mStartPoint = point;
                        emit selectedOrientationChanged(SelectionSet::getSingleton()->getSelectionOrientation());
                    }
                }
                else
                {
                    rotateSelected(rotation);
                    mStartPoint = point;
                    emit selectedOrientationChanged(SelectionSet::getSingleton()->getSelectionOrientation());
                }
            }
        }
    }
    else if(mTransformState == TS_SCALE && (!SelectionSet::getSingleton()->isEmpty()))
    {
        if(mStartPoint.isZeroLength())
        {
            // Survey gizmo hit for axis highlighting
            Ogre::MovableObject* gizmoAxis = performRaySelection(e->pos(), true);
            if(gizmoAxis)
                mTransformVector = m_pScaleGizmo->highlightAxis(gizmoAxis);
            else
            {
                m_pScaleGizmo->createAxis();
                mTransformVector = Ogre::Vector3::ZERO;
            }
        }
        else
        {
            // Pixel-delta scale against the frozen press-time baseline.
            // See the press-time handler above for why this replaced the
            // world-space ray-plane distance approach.
            QPoint delta = e->pos() - mScaleDragStartPixel;
            float pixels = static_cast<float>(delta.x() - delta.y());
            const float kPixelsPerDouble = 100.0f;
            float ratio = std::pow(2.0f, pixels / kPixelsPerDouble);
            if (ratio < 0.01f) ratio = 0.01f;
            if (ratio > 100.0f) ratio = 100.0f;

            Ogre::Vector3 scaleFactor = (mTransformVector == Ogre::Vector3::ZERO)
                ? Ogre::Vector3(ratio, ratio, ratio)
                : Ogre::Vector3::UNIT_SCALE + (mTransformVector * (ratio - 1.0f));

            bool snapping = mSnapEnabled || (e->modifiers() & Qt::ControlModifier);
            if (snapping) {
                Ogre::Vector3 snappedDelta = snapScale(scaleFactor - Ogre::Vector3::UNIT_SCALE,
                                                       mSnapScaleStep);
                scaleFactor = Ogre::Vector3::UNIT_SCALE + snappedDelta;
                // Reclamp per-component after snap — snapping can round a
                // delta near -1 down to exactly -1, producing zero scale.
                scaleFactor.x = std::max<Ogre::Real>(scaleFactor.x, 0.01f);
                scaleFactor.y = std::max<Ogre::Real>(scaleFactor.y, 0.01f);
                scaleFactor.z = std::max<Ogre::Real>(scaleFactor.z, 0.01f);
            }

            // Apply cumulative scale to the press-time baseline for each
            // selected object.
            if (SelectionSet::getSingleton()->hasNodes()) {
                auto nodes = SelectionSet::getSingleton()->getNodesSelectionList();
                for (int i = 0; i < nodes.size() && i < mUndoStartScales.size(); ++i) {
                    nodes[i]->setScale(mUndoStartScales[i] * scaleFactor);
                }
                updateGizmoPosition();
            } else {
                // Entities / sub-entities: scaleSelected undoes the previous
                // factor and applies the new one, so we can pass the cumulative
                // factor directly.
                scaleSelected(scaleFactor);
            }
            emit selectedScaleChanged(SelectionSet::getSingleton()->getSelectionScale());
        }
    }
}

void TransformOperator::mouseReleaseEvent(QMouseEvent *e)
{
    if (mVertexPaintDragActive && e->button() == Qt::LeftButton) {
        EditModeController::instance()->endVertexPaintStroke(/*commitUndo=*/true);
        mVertexPaintDragActive = false;
        SentryReporter::addBreadcrumb("ui.action", "Vertex paint: stroke end");
        return;
    }

    if (mTexturePaintDragActive && e->button() == Qt::LeftButton) {
        TexturePaintController::instance()->endStroke();
        mTexturePaintDragActive = false;
        SentryReporter::addBreadcrumb("ui.action", "Texture paint: stroke end");
        return;
    }

    // End bevel drag but keep the session open (user can re-grab or click
    // elsewhere to commit).
    if (mBevelDragActive && e->button() == Qt::LeftButton) {
        mBevelDragActive = false;
        SentryReporter::addBreadcrumb("ui.transform", "Bevel: drag end");
        return;
    }

    // Bone-gizmo drag end: capture after-state, push BoneTransformCommand
    // if the bone actually moved, fire auto-key. Bone TRS edits live
    // outside the SelectionSet so the scene-node release path below
    // still fires for any concurrent node selection.
    if (mBoneDragActive && e->button() == Qt::LeftButton)
    {
        Ogre::Bone* bone = AnimationControlController::instance()->selectedBonePtr();
        Ogre::SkeletonInstance* skel =
            AnimationControlController::instance()->selectedEntity()
                ? AnimationControlController::instance()->selectedEntity()->getSkeleton()
                : nullptr;
        if (bone && skel)
        {
            Ogre::Vector3    afterPos    = bone->getPosition();
            Ogre::Quaternion afterOrient = bone->getOrientation();
            Ogre::Vector3    afterScale  = bone->getScale();
            bool changed = (afterPos    != mBoneStartPos)
                        || (afterOrient != mBoneStartOrient)
                        || (afterScale  != mBoneStartScale);
            const bool autoKeyOn = AnimationControlController::instance()->autoKey();
            // Active = an animation state is currently ENABLED (driving
            // bones), not just selected in the panel. With no enabled
            // animation, the bone is at its bind pose and the user is
            // doing T-pose authoring → setInitialState. With at least
            // one enabled animation, the curve writes the bone each
            // frame and a setInitialState would shift the whole curve
            // → revert instead.
            bool hasActiveAnim = false;
            if (Ogre::Entity* dragEnt = AnimationControlController::instance()->selectedEntity()) {
                if (Ogre::AnimationStateSet* states = dragEnt->getAllAnimationStates()) {
                    for (const auto& pair : states->getAnimationStates()) {
                        if (pair.second->getEnabled()) { hasActiveAnim = true; break; }
                    }
                }
            }

            // Restore the per-state BlendMask values we captured at
            // press, BEFORE calling apply(). The Revert path calls
            // _updateAnimation which runs Skeleton::reset (bone →
            // initial) then animation tracks (initial + curve sample).
            // With the BlendMask still muted, the curve contributes
            // zero and the bone settles at initial (T-pose) instead
            // of the press-time pose. Restoring before apply() lets
            // the curve drive the bone correctly. Restore exactly the
            // pre-drag weights — not a hardcoded 1.0 — to preserve
            // any layered/masked setup the user had.
            if (Ogre::Entity* dragEnt = AnimationControlController::instance()->selectedEntity()) {
                if (Ogre::AnimationStateSet* states = dragEnt->getAllAnimationStates()) {
                    for (const auto& [stateName, weight] : mBoneDragSavedMaskWeights) {
                        if (!states->hasAnimationState(stateName)) continue;
                        Ogre::AnimationState* st = states->getAnimationState(stateName);
                        if (st->hasBlendMask())
                            st->setBlendMaskEntry(bone->getHandle(), weight);
                    }
                }
            }
            mBoneDragSavedMaskWeights.clear();

            const auto outcome = BoneDragRelease::apply(
                bone, mBoneStartPos, mBoneStartOrient, mBoneStartScale,
                hasActiveAnim, autoKeyOn,
                AnimationControlController::instance()->selectedEntity());
            if (outcome == BoneDragRelease::Result::Commit) {
                // autoKeyOnTransform → addKeyframe → AddKeyframeCommand
                // captures the durable artifact (the keyframe). The
                // bone's live local TRS is fully determined by the
                // curve at slider time after Skeleton::reset, so we
                // don't need a separate BoneTransformCommand here —
                // undoing the keyframe is enough.
                AnimationControlController::instance()->autoKeyOnTransform();
            } else if (outcome == BoneDragRelease::Result::CommitBind) {
                // bindMode=true so undo also reverts the bone's initial
                // (bind) state — otherwise Skeleton::reset would snap
                // back to the new bind on the next animation update.
                const std::string entityName =
                    AnimationControlController::instance()->selectedEntityName().toStdString();
                UndoManager::getSingleton()->push(
                    new BoneTransformCommand(entityName, bone->getName(),
                        mBoneStartPos, mBoneStartOrient, mBoneStartScale,
                        afterPos,      afterOrient,      afterScale,
                        /*bindMode=*/true));
            } else if (outcome == BoneDragRelease::Result::Revert) {
                // Restore the gizmo to the press-time anchor so the
                // viewport visually returns to the start of the drag.
                m_pTransformNode->setPosition(mBoneDragGizmoOrigin);
            }
        }
        // Restore playback if we paused it on press.
        if (mBoneDragWasPlaying) {
            PropertiesPanelController::instance()->setPlaying(true);
            mBoneDragWasPlaying = false;
        }
        mBoneDragActive = false;
        mStartPoint = Ogre::Vector3::ZERO;
        mTransformVector = Ogre::Vector3::ZERO;
        return;
    }

    // Push undo command for edit mode vertex transforms
    if (mEditModeTransformActive && (e->button() == Qt::LeftButton))
    {
        auto* editCtrl = EditModeController::instance();
        if (editCtrl->isEditModeActive() && !mEditModeStartPositions.empty())
        {
            // Snapshot current (post-transform) positions
            auto newPositions = editCtrl->snapshotVertexPositions();

            // Check if anything actually changed (compare against immutable undo snapshot)
            bool changed = false;
            for (const auto& [gi, pos] : mEditModeUndoSnapshot) {
                auto it = newPositions.find(gi);
                if (it != newPositions.end() && it->second != pos) {
                    changed = true;
                    break;
                }
            }

            if (changed) {
                QString desc;
                if (mTransformState == TS_TRANSLATE) desc = "Edit Vertex Translate";
                else if (mTransformState == TS_ROTATE) desc = "Edit Vertex Rotate";
                else if (mTransformState == TS_SCALE) desc = "Edit Vertex Scale";
                else desc = "Edit Vertex Transform";

                UndoManager::getSingleton()->push(
                    new EditVertexTransformCommand(mEditModeUndoSnapshot, newPositions, desc));

                // Validate mesh after edit
                editCtrl->validateMesh();
            }
        }

        mEditModeTransformActive = false;
        mEditModeStartPositions.clear();
        mEditModeUndoSnapshot.clear();
        mStartPoint = Ogre::Vector3::ZERO;

        // Don't fall through to object-mode handlers
        if (m_pSelectionBox->isVisible()) {
            m_pSelectionBox->clear();
            m_pSelectionBox->setVisible(false);
        }
        return;
    }

    // Push undo commands for sub-entity vertex transforms
    if (SelectionSet::getSingleton()->hasSubEntities() && !mUndoSubEntities.isEmpty()
        && (e->button() == Qt::LeftButton))
    {
        for (int i = 0; i < mUndoSubEntities.size(); ++i)
        {
            Ogre::SubEntity* sub = mUndoSubEntities[i];
            Ogre::Entity* ent = sub->getParent();
            unsigned int subIdx = 0;
            for (unsigned int s = 0; s < ent->getNumSubEntities(); ++s)
            {
                if (ent->getSubEntity(s) == sub) { subIdx = s; break; }
            }
            auto currentPositions = SubMeshTransform::readPositions(ent, subIdx);
            bool changed = (currentPositions != mUndoSubMeshPositions[i]);
            if (changed)
            {
                QString desc = QString("SubMesh %1 Transform").arg(subIdx);
                UndoManager::getSingleton()->push(
                    new SubMeshTransformCommand(sub, mUndoSubMeshPositions[i], desc));
            }
        }
        mUndoSubEntities.clear();
        mUndoSubMeshPositions.clear();
        mStartPoint = Ogre::Vector3::ZERO;
    }

    if((SelectionSet::getSingleton()->hasNodes()||SelectionSet::getSingleton()->hasEntities()) && (e->button() == Qt::LeftButton))
    {
        // Push undo command if a transform was performed on scene nodes
        bool nodeTransformCommitted = false;
        if (SelectionSet::getSingleton()->hasNodes() && !mUndoStartPositions.isEmpty())
        {
            auto nodes = SelectionSet::getSingleton()->getNodesSelectionList();
            bool changed = false;

            if (mTransformState == TS_TRANSLATE)
            {
                // Compute total delta from start positions
                Ogre::Vector3 totalDelta = Ogre::Vector3::ZERO;
                for (int i = 0; i < nodes.size() && i < mUndoStartPositions.size(); ++i)
                {
                    Ogre::Vector3 delta = nodes[i]->getPosition() - mUndoStartPositions[i];
                    if (!delta.isZeroLength())
                    {
                        totalDelta = delta;
                        changed = true;
                    }
                }
                if (changed)
                {
                    // Revert to start, then push command (which will redo)
                    for (int i = 0; i < nodes.size() && i < mUndoStartPositions.size(); ++i)
                        nodes[i]->setPosition(mUndoStartPositions[i]);
                    UndoManager::getSingleton()->push(new TranslateCommand(nodes, totalDelta));
                }
            }
            else if (mTransformState == TS_ROTATE)
            {
                for (int i = 0; i < nodes.size() && i < mUndoStartOrientations.size(); ++i)
                {
                    if (nodes[i]->getOrientation() != mUndoStartOrientations[i])
                    {
                        changed = true;
                        break;
                    }
                }
                if (changed)
                {
                    Ogre::Quaternion totalRotation = Ogre::Quaternion::IDENTITY;
                    if (!mUndoStartOrientations.isEmpty() && !nodes.isEmpty())
                        totalRotation = nodes[0]->getOrientation() * mUndoStartOrientations[0].Inverse();

                    // Revert to start, then push command
                    for (int i = 0; i < nodes.size() && i < mUndoStartPositions.size(); ++i)
                    {
                        nodes[i]->setPosition(mUndoStartPositions[i]);
                        nodes[i]->setOrientation(mUndoStartOrientations[i]);
                    }
                    UndoManager::getSingleton()->push(
                        new RotateCommand(nodes, totalRotation, m_pTransformNode->getPosition()));
                }
            }
            else if (mTransformState == TS_SCALE)
            {
                Ogre::Vector3 totalScale = Ogre::Vector3::UNIT_SCALE;
                for (int i = 0; i < nodes.size() && i < mUndoStartScales.size(); ++i)
                {
                    if (nodes[i]->getScale() != mUndoStartScales[i])
                    {
                        totalScale = nodes[i]->getScale() / mUndoStartScales[i];
                        changed = true;
                    }
                }
                if (changed)
                {
                    for (int i = 0; i < nodes.size() && i < mUndoStartScales.size(); ++i)
                        nodes[i]->setScale(mUndoStartScales[i]);
                    UndoManager::getSingleton()->push(new ScaleCommand(nodes, totalScale));
                }
            }

            nodeTransformCommitted = changed;
        }

        mUndoStartPositions.clear();
        mUndoStartOrientations.clear();
        mUndoStartScales.clear();
        mStartPoint = Ogre::Vector3::ZERO;

        // Auto-key only when an actual transform was committed — plain clicks
        // and zero-delta releases must not pollute tracks with duplicate keys.
        if (nodeTransformCommitted)
            AnimationControlController::instance()->autoKeyOnTransform();
    }

    if(m_pSelectionBox->isVisible())
    {
        // In edit mode, delegate to EditModeController for component selection
        if (EditModeController::instance()->isEditModeActive())
        {
            bool shiftHeld = e->modifiers().testFlag(Qt::ShiftModifier);
            bool ctrlHeld = e->modifiers().testFlag(Qt::ControlModifier);

            // If the rectangle is very small, treat as a click
            QRect rect(mScreenStart, e->pos());
            rect = rect.normalized();

            if (rect.width() < 5 && rect.height() < 5) {
                // Point select
                EditModeController::instance()->handleMouseClick(
                    mScreenStart, m_pActiveWidget, shiftHeld, ctrlHeld);
            } else {
                // Box select
                EditModeController::instance()->handleBoxSelect(
                    mScreenStart, e->pos(), m_pActiveWidget, shiftHeld);
            }

            mScreenStart = QPoint(invalidPosition);
            m_pSelectionBox->clear();
            m_pSelectionBox->setVisible(false);
            return;
        }

        // Perform a box selection
        SelectionMode   selectionMode = NEW_SELECT;
        if(e->modifiers().testFlag(Qt::ControlModifier))
            selectionMode = ADD_SELECT;
        else if(e->modifiers().testFlag(Qt::ShiftModifier))
            selectionMode = DEL_SELECT;

        performBoxSelection(mScreenStart, e->pos(), selectionMode);

        mScreenStart = QPoint(invalidPosition);

        m_pSelectionBox->clear();
        m_pSelectionBox->setVisible(false);
    }
}
// LCOV_EXCL_STOP

void TransformOperator::setSelectedPosition(const Ogre::Vector3& newPosition)
{
    if(SelectionSet::getSingleton()->hasNodes())
    {
        Ogre::Vector3 translation = newPosition - SelectionSet::getSingleton()->getSelectionCenter();
        translateSelected(translation);
    }
    else if(SelectionSet::getSingleton()->hasEntities())
    {
        Ogre::Vector3 translation = newPosition - SelectionSet::getSingleton()->getSelectionCenter()+SelectionSet::getSingleton()->getSelectionNodesCenter();
        translateSelected(translation);
    }
}

void TransformOperator::translateSelected(const Ogre::Vector3& translation)
{
    if(SelectionSet::getSingleton()->hasNodes())
    {
        foreach(Ogre::SceneNode* node,SelectionSet::getSingleton()->getNodesSelectionList())
            node->translate(translation);

       updateGizmoPosition();
    }
    else if(SelectionSet::getSingleton()->hasEntities())
    {
        foreach(Ogre::Entity* obj,SelectionSet::getSingleton()->getEntitiesSelectionList())
        {
            MeshTransform::translateMesh(obj,translation);
            obj->getParentSceneNode()->needUpdate(true);
        }
    }
    else if(SelectionSet::getSingleton()->hasSubEntities())
    {
        for (Ogre::SubEntity* sub : SelectionSet::getSingleton()->getSubEntitiesSelectionList())
        {
            Ogre::Entity* ent = sub->getParent();
            for (unsigned int s = 0; s < ent->getNumSubEntities(); ++s)
            {
                if (ent->getSubEntity(s) == sub)
                {
                    SubMeshTransform::translateSubMesh(ent, s, translation);
                    break;
                }
            }
        }
        updateGizmoPosition();
    }
}

void TransformOperator::setSelectedScale(const Ogre::Vector3& newScale)
{
    if(SelectionSet::getSingleton()->hasNodes())
    {
        Ogre::Vector3 scaleFactor = newScale / SelectionSet::getSingleton()->getSelectionScale();
        scaleSelected(scaleFactor);
    }
    else if(SelectionSet::getSingleton()->hasEntities())
    {
        scaleSelected(newScale);
    }
}

void TransformOperator::scaleSelected(const Ogre::Vector3& scaleFactor)
{
    if(SelectionSet::getSingleton()->hasNodes())
    {
        foreach(Ogre::SceneNode* node,SelectionSet::getSingleton()->getNodesSelectionList())
            node->scale(scaleFactor);

       updateGizmoPosition();
    }
    else if(SelectionSet::getSingleton()->hasEntities())
    {
        foreach(Ogre::Entity* obj,SelectionSet::getSingleton()->getEntitiesSelectionList())
        {
            MeshTransform::scaleMesh(obj,Ogre::Vector3::UNIT_SCALE/SelectionSet::getSingleton()->getEntityScaleFactor(obj));
            MeshTransform::scaleMesh(obj,scaleFactor);
            obj->getParentSceneNode()->needUpdate(true);
            SelectionSet::getSingleton()->setEntityScaleFactor(obj,scaleFactor);
        }
    }
    else if(SelectionSet::getSingleton()->hasSubEntities())
    {
        // Restore baseline positions (captured at drag start into
        // mUndoSubMeshPositions) before applying the cumulative scale.
        // Without this step, SubMeshTransform::scaleSubMesh is a relative
        // mutation on the current positions, so mouse-move events compound
        // the scaling exponentially. The entity path above already inverts
        // the previous scale via getEntityScaleFactor — this is the
        // equivalent for the sub-mesh path.
        const auto& subs = SelectionSet::getSingleton()->getSubEntitiesSelectionList();
        int idx = 0;
        for (Ogre::SubEntity* sub : subs)
        {
            Ogre::Entity* ent = sub->getParent();
            for (unsigned int s = 0; s < ent->getNumSubEntities(); ++s)
            {
                if (ent->getSubEntity(s) == sub)
                {
                    if (idx < mUndoSubMeshPositions.size()) {
                        SubMeshTransform::writePositions(ent, s,
                            mUndoSubMeshPositions[idx]);
                    }
                    SubMeshTransform::scaleSubMesh(ent, s, scaleFactor);
                    break;
                }
            }
            ++idx;
        }
        updateGizmoPosition();
    }
}

// TODO add a control between 0 360° or -180 180°
// TODO improve calculation to eliminate little approx when returning to 0 after 3D rotation
void TransformOperator::setSelectedOrientation(const Ogre::Vector3& newOrientation)
{
    if(SelectionSet::getSingleton()->hasNodes())
    {
       Ogre::Vector3 rotation = newOrientation - SelectionSet::getSingleton()->getSelectionOrientation();

       Ogre::Euler eulerAngle(Ogre::Degree(rotation.y),
                              Ogre::Degree(rotation.x),
                              Ogre::Degree(rotation.z)
                              );
       rotateSelected(eulerAngle);
    }
    else if(SelectionSet::getSingleton()->hasEntities())
    {
        rotateSelected(newOrientation);
    }
}

void TransformOperator::rotateSelected(const Ogre::Quaternion& rotation)
{
    if(SelectionSet::getSingleton()->hasNodes())
    {
        Ogre::Vector3 translation;
        foreach(Ogre::SceneNode* node,SelectionSet::getSingleton()->getNodesSelectionList())
        {
            translation = node->_getDerivedPosition() - m_pTransformNode->_getDerivedPosition();
            node->setPosition(m_pTransformNode->getPosition());
            node->rotate(rotation,Ogre::Node::TS_WORLD);
            node->setPosition(node->getPosition() + rotation * translation);
        }

       updateGizmoPosition();
    }
    else if(SelectionSet::getSingleton()->hasEntities())
    {
        // Convert quaternion to Euler for rotation tracking (used by spinboxes)
        Ogre::Euler euler;
        euler.fromQuaternion(rotation);
        Ogre::Vector3 eulerDelta(euler.pitch().valueDegrees(),
                                 euler.yaw().valueDegrees(),
                                 euler.roll().valueDegrees());

        foreach(Ogre::Entity* obj,SelectionSet::getSingleton()->getEntitiesSelectionList())
        {
            MeshTransform::rotateMesh(obj, rotation);
            obj->getParentSceneNode()->needUpdate(true);
            SelectionSet::getSingleton()->setEntityRotation(
                obj, SelectionSet::getSingleton()->getEntityRotation(obj) + eulerDelta);
        }
    }
    else if(SelectionSet::getSingleton()->hasSubEntities())
    {
        for (Ogre::SubEntity* sub : SelectionSet::getSingleton()->getSubEntitiesSelectionList())
        {
            Ogre::Entity* ent = sub->getParent();
            for (unsigned int s = 0; s < ent->getNumSubEntities(); ++s)
            {
                if (ent->getSubEntity(s) == sub)
                {
                    SubMeshTransform::rotateSubMesh(ent, s, rotation);
                    break;
                }
            }
        }
        updateGizmoPosition();
    }
}

void TransformOperator::rotateSelected(const Ogre::Vector3 &rotation)
{
    if(SelectionSet::getSingleton()->hasEntities())
        {
            foreach(Ogre::Entity* obj,SelectionSet::getSingleton()->getEntitiesSelectionList())
            {
                MeshTransform::rotateMesh(obj,rotation - SelectionSet::getSingleton()->getEntityRotation(obj));
                obj->getParentSceneNode()->needUpdate(true);
                SelectionSet::getSingleton()->setEntityRotation(obj,rotation);
            }
        }
}
