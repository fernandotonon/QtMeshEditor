/*/////////////////////////////////////////////////////////////////////////////////
/// A QtMeshEditor file
///
/// Copyright (c) HogPog Team (www.hogpog.com.br)
///
/// The MIT License
///
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to deal
/// in the Software without restriction, including without limitation the rights
/// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
/// copies of the Software, and to permit persons to whom the Software is
/// furnished to do so, subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
/// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
/// THE SOFTWARE.
////////////////////////////////////////////////////////////////////////////////*/

#include <QtDebug>

#include "GlobalDefinitions.h"

#include "TransformOperator.h"
#include "RotationGizmo.h"
#include "TranslationGizmo.h"
#include "SelectionBoxObject.h"
#include "SelectionSet.h"
#include "OgreWidget.h"
#include "mainwindow.h"
#include "TransformWidget.h"
#include "Manager.h"
#include "MeshTransform.h"
#include "Euler.h"
#include "ViewportGrid.h"
#include <Ogre.h>

// TODO  create a virtual class GizmoObject & add Rotation & Translation Gizmo to have only one interface

////////////////////////////////////////
// Static variable initialisation
TransformOperator* TransformOperator:: m_pSingleton = 0;
const QPoint  TransformOperator::invalidPosition(-1,-1);

////////////////////////////////////////
/// Static Member to build & destroy

TransformOperator* TransformOperator::getSingleton()
{
  if (m_pSingleton == 0)
  {
      m_pSingleton =  new TransformOperator();
  }

  return m_pSingleton;
}

void TransformOperator::kill()
{
    if (m_pSingleton != 0)
    {
        delete m_pSingleton;
        m_pSingleton = 0;
    }
}

////////////////////////////////////////
// Constructor & Destructor

TransformOperator::TransformOperator()
    :QObject(0), m_pActiveWidget(0),
    m_pRotationGizmo(0), m_pTransformNode(0),/* m_pSelectedNode(0),*/
    m_pRayQuery(0), m_pVolQuery(0), mTransformState(TS_NONE), mStartPoint(Ogre::Vector3::ZERO),
    mTransformVector(Ogre::Vector3::ZERO),m_pSelectionBox(0),mScreenStart(invalidPosition),
    mTrackingEnable(false)
{

    Ogre::SceneManager* pSceneMgr = Manager::getSingleton()->getSceneMgr();
    m_pTransformNode = pSceneMgr->getRootSceneNode()->createChildSceneNode(TRANSFORM_OBJECT_NAME);

    m_pRotationGizmo = new RotationGizmo(m_pTransformNode);
    m_pRotationGizmo->setQueryFlags(GIZMO_QUERY_FLAGS);
    m_pRotationGizmo->setVisible(false);

    m_pTranslationGizmo = new TranslationGizmo(m_pTransformNode);
    m_pTranslationGizmo->setQueryFlags(GIZMO_QUERY_FLAGS);
    m_pTranslationGizmo->setVisible(false);

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
}

TransformOperator::~TransformOperator()
{
    QtInputManager::getInstance().RemoveMouseListener(this);

    //m_pSelectedNode = 0;

    Ogre::SceneManager* pSceneMgr = Manager::getSingleton()->getSceneMgr();

    if(m_pRayQuery)
    {
        pSceneMgr->destroyQuery(m_pRayQuery);
        m_pRayQuery = 0;
    }
    if(m_pVolQuery)
    {
        pSceneMgr->destroyQuery(m_pVolQuery);
        m_pVolQuery =0;
    }

    if(m_pRotationGizmo)
    {
        delete m_pRotationGizmo;
        m_pRotationGizmo = 0;
    }

    if(m_pTranslationGizmo)
    {
        delete m_pTranslationGizmo;
        m_pTranslationGizmo = 0;
    }
    if(m_pSelectionBox)
    {
        delete m_pSelectionBox;
        m_pSelectionBox = 0;
    }

    pSceneMgr->destroySceneNode(m_pTransformNode); //TODO destroy SelectionBoxNode
}

void TransformOperator::swap(int& x, int& y)
{
    float temp = x;
    x = y; y = temp;
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

void TransformOperator::removeSelected()
{
    SelectionSet* pCurrentSelection = SelectionSet::getSingleton();
    if(!pCurrentSelection->isEmpty())
    {
        foreach(Ogre::SceneNode* node,SelectionSet::getSingleton()->getNodesSelectionList())
        {
            Manager::getSingleton()->destroySceneNode(node);
        }
        pCurrentSelection->clearList();
    }
}


// TODO add scale gizmo
void TransformOperator::updateGizmo()
{
    updateGizmoPosition();
    if(SelectionSet::getSingleton()->hasNodes()||SelectionSet::getSingleton()->hasEntities())
    {
        switch  (mTransformState) {
        case TransformOperator::TS_SELECT:
                m_pRotationGizmo->setVisible(false);
                m_pTranslationGizmo->setVisible(false);
          break;
        case TransformOperator::TS_TRANSLATE:
                m_pTransformNode->setOrientation(Manager::getSingleton()->getSceneMgr()->getRootSceneNode()->getOrientation());
                m_pRotationGizmo->setVisible(false);
                m_pTranslationGizmo->setVisible(true);
                mTrackingEnable = true;
          break;
        case TransformOperator::TS_ROTATE: // TODO change orientation when LOCAL mode is selected
                m_pTransformNode->setOrientation(Manager::getSingleton()->getSceneMgr()->getRootSceneNode()->getOrientation());
                m_pRotationGizmo->setVisible(true);
                m_pTranslationGizmo->setVisible(false);
                mTrackingEnable = true;
          break;
        default:
                m_pRotationGizmo->setVisible(false);
                m_pTranslationGizmo->setVisible(false);
          break;
        }
    }
    else
    {
        m_pRotationGizmo->setVisible(false);
        m_pTranslationGizmo->setVisible(false);
    }
    if(m_pActiveWidget)
        m_pActiveWidget->setMouseTracking(mTrackingEnable);
}

void TransformOperator::updateGizmoPosition()
{
    //TODO add a parent or local transform (get the orientation of object node)
/*
    if(m_pSelectedNode)
    {
        m_pTransformNode->setPosition(m_pSelectedNode->getPosition());

        if(mTransformState == TS_ROTATE)
            m_pTransformNode->setOrientation(m_pSelectedNode->getOrientation());
    }
*/
    Ogre::Vector3 currentPosition = Ogre::Vector3::ZERO;
    Ogre::Vector3 currentOrientation = Ogre::Vector3::ZERO;
    Ogre::Vector3 currentScale = Ogre::Vector3::UNIT_SCALE;

    if(SelectionSet::getSingleton()->hasNodes())
    {
        currentPosition     = SelectionSet::getSingleton()->getSelectionCenter();
        currentOrientation  = SelectionSet::getSingleton()->getSelectionOrientation();
        currentScale        = SelectionSet::getSingleton()->getSelectionScale();
        m_pTransformNode->setPosition(currentPosition);
    }
    else if(SelectionSet::getSingleton()->hasEntities())
    {
        currentPosition     = SelectionSet::getSingleton()->getSelectionCenter()-SelectionSet::getSingleton()->getSelectionNodesCenter();
        currentOrientation  = SelectionSet::getSingleton()->getSelectionOrientation();
        currentScale        = SelectionSet::getSingleton()->getSelectionScale();
        m_pTransformNode->setPosition(currentPosition + SelectionSet::getSingleton()->getSelectionNodesCenter());
    }
    emit selectedPositionChanged(currentPosition);
    emit selectedOrientationChanged(currentOrientation);
    emit selectedScaleChanged(currentScale);
}
void TransformOperator::onSelectionChanged()
{
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
        if(Manager::getSingleton()->getViewportGrid())
            Manager::getSingleton()->getViewportGrid()->setPosition(Ogre::Vector3(0,0,0));
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
            Manager::getSingleton()->getViewportGrid()->setPosition(obj->getParentSceneNode()->getPosition());
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
            Manager::getSingleton()->getViewportGrid()->setPosition(obj->getParent()->getParentSceneNode()->getPosition());
        }

        if(!SelectionSet::getSingleton()->hasEntities()&&!SelectionSet::getSingleton()->hasSubEntities())
        {
            //TODO - Create a grid to the nodes
            Manager::getSingleton()->getViewportGrid()->setPosition(Ogre::Vector3::ZERO);
        }
    }

    updateGizmo();
}

void TransformOperator::setActiveWidget(OgreWidget* ogreWidget)
{
    if(m_pActiveWidget)
        m_pActiveWidget->setMouseTracking(false);

    m_pActiveWidget = ogreWidget;

    if(m_pActiveWidget)
        m_pActiveWidget->setMouseTracking(mTrackingEnable);

}

Ogre::Ray TransformOperator::rayFromScreenPoint(const QPoint& pos)
{
    if(m_pActiveWidget)
    {
        int width = m_pActiveWidget->getViewport()->getActualWidth();
        int height = m_pActiveWidget->getViewport()->getActualHeight();

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

    return 0;
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

        if(mTransformState == TS_SELECT)
        {
            mScreenStart = e->pos();
            m_pSelectionBox->clear();
            m_pSelectionBox->setVisible(true);

        }
        else if((!SelectionSet::getSingleton()->isEmpty()) && (e->button() == Qt::LeftButton))
        {
            // Checking the ray intersection with a plane parallele to viewport & on the geometric center of selection
            Ogre::Ray mouseRay = rayFromScreenPoint(e->pos());
            std::pair<bool, Ogre::Real> result = mouseRay.intersects(Ogre::Plane(mouseRay.getDirection(), m_pTransformNode->getPosition()));

            if(result.first)
                mStartPoint = mouseRay.getPoint(result.second);
        }
    }
}

void TransformOperator::mouseMoveEvent(QMouseEvent *e)
{
    if(mTransformState == TS_SELECT)
    {
        if(m_pSelectionBox->isVisible() && m_pActiveWidget)
        {
            int width = m_pActiveWidget->getViewport()->getActualWidth();
            int height = m_pActiveWidget->getViewport()->getActualHeight();

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
                // Checking the ray intersection with a plane parallel to viewport & on the geometric center of selection
                Ogre::Vector3 point = mouseRay.getPoint(result.second);
                Ogre::Vector3 translation = (mouseRay.getPoint(result.second) - mStartPoint) * mTransformVector;

                translateSelected(translation);
                mStartPoint = point;

                emit selectedPositionChanged(SelectionSet::getSingleton()->getSelectionCenter()); //TODO connect this signal

                updateGizmoPosition();
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

                Ogre::Quaternion rotation = vectorStart.getRotationTo(vectorEnd);
                rotation.x = rotation.x * mTransformVector.x;
                rotation.y = rotation.y * mTransformVector.y;
                rotation.z = rotation.z * mTransformVector.z;
                rotation.normalise();

                rotateSelected(rotation); //Rotate the Node
                rotateSelected(Ogre::Vector3(rotation.y,rotation.z,rotation.x)*100+SelectionSet::getSingleton()->getSelectionOrientation()); //Rotate the Mesh
                mStartPoint = point;

                emit selectedOrientationChanged(SelectionSet::getSingleton()->getSelectionOrientation()); //TODO connect this signal
            }
        }
    }
}

void TransformOperator::mouseReleaseEvent(QMouseEvent *e)
{
    if((SelectionSet::getSingleton()->hasNodes()||SelectionSet::getSingleton()->hasEntities()) && (e->button() == Qt::LeftButton))
        mStartPoint = Ogre::Vector3::ZERO;

    if(m_pSelectionBox->isVisible())
    {
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
}

// TODO add a GUI scale by Vector length between mStartPoint & current pos on ground plane
// to do this use scaleSelected instead of setSelectedScale
void TransformOperator::setSelectedScale(const Ogre::Vector3& newScale)
{
    if(SelectionSet::getSingleton()->hasNodes())
    {
        Ogre::Vector3 scaleFactor = newScale / SelectionSet::getSingleton()->getSelectionScale();
        scaleSelected(scaleFactor);
        //TODO scaling in selection CS
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
    /* Code to rotate each object on it's own local CS
    if(!SelectionSet::getSingleton()->isEmpty())
    {

        foreach(Ogre::SceneNode* node,SelectionSet::getSingleton()->getSelectionList())
        {
            node->rotate(rotation,Ogre::Node::TS_WORLD);
        }

       //updateGizmoPosition();  //required in case of Local mode
    }
    */
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
