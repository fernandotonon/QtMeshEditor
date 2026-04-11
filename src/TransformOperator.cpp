#include <QtDebug>
#include <QSettings>
#include <QApplication>
#include <cmath>
#include <limits>

#include "GlobalDefinitions.h"

#include "TransformOperator.h"
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
#include "EditModeController.h"
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

    m_pRotationGizmo = new RotationGizmo(m_pTransformNode);
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

    QtInputManager::getInstance().AddMouseListener(this);

    // Load snap settings from QSettings
    QSettings settings;
    mSnapEnabled    = settings.value("Snap/enabled", false).toBool();
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

////////////////////////////////////////
// Snap settings

void TransformOperator::setSnapEnabled(bool enabled)
{
    if (mSnapEnabled != enabled)
    {
        mSnapEnabled = enabled;
        QSettings settings;
        settings.setValue("Snap/enabled", mSnapEnabled);
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
    if(!pCurrentSelection->isEmpty())
    {
        foreach(Ogre::SceneNode* node,SelectionSet::getSingleton()->getNodesSelectionList())
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
    if(SelectionSet::getSingleton()->hasNodes()||SelectionSet::getSingleton()->hasEntities()||SelectionSet::getSingleton()->hasSubEntities())
    {
        // Determine gizmo orientation based on transform space
        Ogre::Quaternion gizmoOrientation;
        if (mTransformSpace == SPACE_LOCAL && SelectionSet::getSingleton()->hasNodes()
            && SelectionSet::getSingleton()->getNodesCount() == 1)
        {
            gizmoOrientation = SelectionSet::getSingleton()->getSceneNode(0)->getOrientation();
        }
        else
        {
            gizmoOrientation = Manager::getSingleton()->getSceneMgr()->getRootSceneNode()->getOrientation();
        }

        switch  (mTransformState) {
        case TransformOperator::TS_SELECT:
                m_pRotationGizmo->setVisible(false);
                m_pTranslationGizmo->setVisible(false);
                m_pScaleGizmo->setVisible(false);
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
    if(m_pActiveWidget)
        m_pActiveWidget->setMouseTracking(mTrackingEnable);
}

void TransformOperator::updateGizmoPosition()
{
    Ogre::Vector3 currentPosition = Ogre::Vector3::ZERO;
    Ogre::Vector3 currentOrientation = Ogre::Vector3::ZERO;
    Ogre::Vector3 currentScale = Ogre::Vector3::UNIT_SCALE;

    if(SelectionSet::getSingleton()->hasNodes())
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
        // In edit mode, delegate selection to EditModeController
        if (EditModeController::instance()->isEditModeActive() && mTransformState == TS_SELECT)
        {
            mScreenStart = e->pos();
            m_pSelectionBox->clear();
            m_pSelectionBox->setVisible(true);
            return;
        }

        if(mTransformState == TS_SELECT)
        {
            mScreenStart = e->pos();
            m_pSelectionBox->clear();
            m_pSelectionBox->setVisible(true);

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
                // For scale: record initial distance from gizmo center to start point
                if(mTransformState == TS_SCALE)
                    mScaleStartDistance = (mStartPoint - m_pTransformNode->getPosition()).length();
            }
        }
    }
}

void TransformOperator::mouseMoveEvent(QMouseEvent *e)
{
    if(mTransformState == TS_SELECT)
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
            // Dragging -> compute scale factor from distance ratio
            Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
            std::pair<bool, Ogre::Real> result = mouseRay.intersects(Ogre::Plane(mouseRay.getDirection(), m_pTransformNode->getPosition()));

            if(result.first)
            {
                Ogre::Vector3 point = mouseRay.getPoint(result.second);
                Ogre::Real currentDistance = (point - m_pTransformNode->getPosition()).length();

                if(mScaleStartDistance > 0.001f)
                {
                    Ogre::Real ratio = currentDistance / mScaleStartDistance;
                    Ogre::Vector3 scaleFactor = Ogre::Vector3::UNIT_SCALE;

                    if(mTransformVector == Ogre::Vector3::ZERO)
                    {
                        // Uniform scale when no axis selected
                        scaleFactor = Ogre::Vector3(ratio, ratio, ratio);
                    }
                    else
                    {
                        // Per-axis scale
                        scaleFactor = Ogre::Vector3::UNIT_SCALE + (mTransformVector * (ratio - 1.0f));
                    }

                    // Apply snap if Ctrl is held or snap is permanently enabled
                    bool snapping = mSnapEnabled || (e->modifiers() & Qt::ControlModifier);
                    if (snapping)
                    {
                        // Accumulate the delta from identity (1.0)
                        mSnapScaleAccum += (scaleFactor - Ogre::Vector3::UNIT_SCALE);
                        Ogre::Vector3 snappedDelta = snapScale(mSnapScaleAccum, mSnapScaleStep);
                        if (snappedDelta.isZeroLength())
                        {
                            mScaleStartDistance = currentDistance;
                        }
                        else
                        {
                            mSnapScaleAccum -= snappedDelta;
                            // For nodes: use incremental factor (node->scale is multiplicative)
                            // For entities/sub-entities: use cumulative factor (scaleSelected
                            // undoes previous and applies absolute)
                            if (SelectionSet::getSingleton()->hasNodes())
                            {
                                Ogre::Vector3 snappedFactor = Ogre::Vector3::UNIT_SCALE + snappedDelta;
                                scaleSelected(snappedFactor);
                            }
                            else
                            {
                                mSnapScaleCumulative += snappedDelta;
                                scaleSelected(mSnapScaleCumulative);
                            }
                            mScaleStartDistance = currentDistance;
                            emit selectedScaleChanged(SelectionSet::getSingleton()->getSelectionScale());
                            updateGizmoPosition();
                        }
                    }
                    else
                    {
                        scaleSelected(scaleFactor);
                        mScaleStartDistance = currentDistance;
                        emit selectedScaleChanged(SelectionSet::getSingleton()->getSelectionScale());
                        updateGizmoPosition();
                    }
                }
            }
        }
    }
}

void TransformOperator::mouseReleaseEvent(QMouseEvent *e)
{
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
        mScaleStartDistance = 0.0f;
    }

    if((SelectionSet::getSingleton()->hasNodes()||SelectionSet::getSingleton()->hasEntities()) && (e->button() == Qt::LeftButton))
    {
        // Push undo command if a transform was performed on scene nodes
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
        }

        mUndoStartPositions.clear();
        mUndoStartOrientations.clear();
        mUndoStartScales.clear();
        mStartPoint = Ogre::Vector3::ZERO;
        mScaleStartDistance = 0.0f;
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
        for (Ogre::SubEntity* sub : SelectionSet::getSingleton()->getSubEntitiesSelectionList())
        {
            Ogre::Entity* ent = sub->getParent();
            for (unsigned int s = 0; s < ent->getNumSubEntities(); ++s)
            {
                if (ent->getSubEntity(s) == sub)
                {
                    SubMeshTransform::scaleSubMesh(ent, s, scaleFactor);
                    break;
                }
            }
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
