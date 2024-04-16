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

    // Made public for testing
    static void swap(int& x, int& y);
    Ogre::Ray   rayFromScreenPoint(const QPoint& pos);

private:
    TransformOperator ();
    ~TransformOperator () = default;

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

    bool                                    mTrackingEnable = false;
    QPoint                                  mScreenStart = invalidPosition;
    SelectionBoxObject*                     m_pSelectionBox  = nullptr;
    Ogre::SceneNode*                        m_pSelectionBoxNode; //TODO try to move this in the SelectionBoxObject
    //TransformWidget*                        m_pTransformWidget;
    OgreWidget*                             m_pActiveWidget = nullptr;
    RotationGizmo*                          m_pRotationGizmo = nullptr;
    TranslationGizmo*                       m_pTranslationGizmo = nullptr;
    Ogre::SceneNode*                        m_pTransformNode = nullptr;
    //Ogre::SceneNode*                        m_pSelectedNode;
    Ogre::RaySceneQuery*                    m_pRayQuery  = nullptr;
    Ogre::PlaneBoundedVolumeListSceneQuery* m_pVolQuery  = nullptr;
    Ogre::Vector3                           mStartPoint = Ogre::Vector3::ZERO;
    Ogre::Vector3                           mTransformVector = Ogre::Vector3::ZERO;
    TransformState                          mTransformState = TS_NONE;
#ifdef Q_OS_MACOS
    int mWindowSizeModifier = 2;
#else
    int mWindowSizeModifier = 1;
#endif
};


#endif //TRANSFORM_OPERATOR

