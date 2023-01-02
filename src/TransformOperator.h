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

#ifndef TRANSFORM_OPERATOR_H
#define TRANSFORM_OPERATOR_H

#include <QObject>
#include <QPoint>
#include <OgreRay.h>
#include "QtInputManager.h"

//class TransformWidget;
class OgreWidget;
class RotationGizmo;
class TranslationGizmo;
class SelectionBoxObject;
class SelectionSet;

namespace Ogre{
    class PlaneBoundedVolumeListSceneQuery;
    class RaySceneQuery;
    class SceneNode;
    class MovableObject;
}
class TransformOperator : public QObject, public QtMouseListener
{
    Q_OBJECT

public:

    static TransformOperator* getSingleton();
    static void kill();



    enum TransformState
    {
        TS_NONE,
        TS_SELECT,
        TS_TRANSLATE,
        TS_ROTATE,
    };
    enum SelectionMode
    {
        NEW_SELECT    = 0x00,
        ADD_SELECT    = 0x01,
        DEL_SELECT    = 0x02,
    };

    const Ogre::ColourValue& getSelectionBoxColour() const;

private:
    TransformOperator ();
    ~TransformOperator ();

    static void             swap(int& x, int& y);
    Ogre::Ray               rayFromScreenPoint(const QPoint& pos);
    Ogre::MovableObject*    performRaySelection(const QPoint& pos, bool findGizmo = false);
    void                    performBoxSelection(const QPoint& first, const QPoint& second, SelectionMode mode = NEW_SELECT);
    void                    updateGizmo();
    void                    updateGizmoPosition();

signals:
    void objectsDeleted();
    void selectedPositionChanged(const Ogre::Vector3& newPosition);
    void selectedScaleChanged(const Ogre::Vector3& newScale); //TODO emit this signal !!
    void selectedOrientationChanged(const Ogre::Vector3& newOrientation);

public slots:
    void onSelectionChanged();

    void onTransformStateChange(const TransformState newState);
    void setActiveWidget(OgreWidget* ogreWidget);
    //void setSelectedNode(Ogre::SceneNode* newNode); //TODO it should not exist....
    void setSelectedPosition(const Ogre::Vector3& newPosition);
    void translateSelected(const Ogre::Vector3& newPosition);
    void setSelectedScale(const Ogre::Vector3& newScale);
    void scaleSelected(const Ogre::Vector3& scaleFactor);
    void setSelectedOrientation(const Ogre::Vector3& newOrientation);
    void rotateSelected(const Ogre::Quaternion& rotation);
    void rotateSelected(const Ogre::Vector3& rotation);
    void removeSelected();

    void setSelectionBoxColour(const Ogre::ColourValue& colour);

protected:
    void mousePressEvent(QMouseEvent *e);
    void mouseMoveEvent(QMouseEvent *e);
    void mouseReleaseEvent(QMouseEvent *e);
    void wheelEvent(QWheelEvent *e){}

private:
    static TransformOperator*               m_pSingleton; // the only instance of this!
    static const QPoint                     invalidPosition;

    bool                                    mTrackingEnable;
    QPoint                                  mScreenStart;
    SelectionBoxObject*                     m_pSelectionBox;
    Ogre::SceneNode*                        m_pSelectionBoxNode; //TODO try to move this in the SelectionBoxObject
    //TransformWidget*                        m_pTransformWidget;
    OgreWidget*                             m_pActiveWidget;
    RotationGizmo*                          m_pRotationGizmo;
    TranslationGizmo*                       m_pTranslationGizmo;
    Ogre::SceneNode*                        m_pTransformNode;
    //Ogre::SceneNode*                        m_pSelectedNode;
    Ogre::RaySceneQuery*                    m_pRayQuery;
    Ogre::PlaneBoundedVolumeListSceneQuery* m_pVolQuery;
    Ogre::Vector3                           mStartPoint;
    Ogre::Vector3                           mTransformVector;
    TransformState                          mTransformState;

};


#endif //TRANSFORM_OPERATOR

