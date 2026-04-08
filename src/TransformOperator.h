#ifndef TRANSFORM_OPERATOR_H
#define TRANSFORM_OPERATOR_H

#include <QObject>
#include <QPoint>
#include <QList>
#include <OgreRay.h>
#include "QtInputManager.h"

//class TransformWidget;
class OgreWidget;
class RotationGizmo;
class TranslationGizmo;
class ScaleGizmo;
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
        TS_SCALE,
    };
    enum SelectionMode
    {
        NEW_SELECT    = 0x00,
        ADD_SELECT    = 0x01,
        DEL_SELECT    = 0x02,
    };
    enum TransformSpace
    {
        SPACE_WORLD,
        SPACE_LOCAL,
    };

    const Ogre::ColourValue& getSelectionBoxColour() const;
    TransformSpace getTransformSpace() const { return mTransformSpace; }

    // --- Snap settings ---
    bool isSnapEnabled() const { return mSnapEnabled; }
    void setSnapEnabled(bool enabled);

    double snapGridSize() const { return mSnapGridSize; }
    void setSnapGridSize(double size);

    double snapAngleStep() const { return mSnapAngleStep; }
    void setSnapAngleStep(double degrees);

    double snapScaleStep() const { return mSnapScaleStep; }
    void setSnapScaleStep(double step);

    // Available preset values
    static QList<double> gridSizePresets();
    static QList<double> angleStepPresets();
    static QList<double> scaleStepPresets();

    // Snap helpers (public for testing)
    static double snapValue(double value, double step);
    static Ogre::Vector3 snapTranslation(const Ogre::Vector3& translation, double gridSize);
    static Ogre::Real snapAngle(Ogre::Real degrees, double angleStep);
    static Ogre::Vector3 snapScale(const Ogre::Vector3& scale, double scaleStep);

    // Made public for testing
    static void swap(int& x, int& y);
    Ogre::Ray   rayFromScreenPoint(const QPoint& pos);

private:
    TransformOperator ();
    ~TransformOperator () override;

    Ogre::MovableObject*    performRaySelection(const QPoint& pos, bool findGizmo = false);
    void                    performBoxSelection(const QPoint& first, const QPoint& second, SelectionMode mode = NEW_SELECT);
    void                    updateGizmo();
    void                    updateGizmoPosition();

signals:
    void objectsDeleted();
    void selectedPositionChanged(const Ogre::Vector3& newPosition);
    void selectedScaleChanged(const Ogre::Vector3& newScale);
    void selectedOrientationChanged(const Ogre::Vector3& newOrientation);
    void transformSpaceChanged(TransformSpace newSpace);
    void snapSettingsChanged();

public slots:
    void onSelectionChanged();

    void onTransformStateChange(const TransformState newState);
    void setTransformSpace(TransformSpace space);
    void toggleTransformSpace();
    void setActiveWidget(OgreWidget* ogreWidget);
    OgreWidget* getActiveWidget() const { return m_pActiveWidget; }
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
    ScaleGizmo*                             m_pScaleGizmo = nullptr;
    Ogre::SceneNode*                        m_pTransformNode = nullptr;
    //Ogre::SceneNode*                        m_pSelectedNode;
    Ogre::RaySceneQuery*                    m_pRayQuery  = nullptr;
    Ogre::PlaneBoundedVolumeListSceneQuery* m_pVolQuery  = nullptr;
    Ogre::Vector3                           mStartPoint = Ogre::Vector3::ZERO;
    Ogre::Vector3                           mTransformVector = Ogre::Vector3::ZERO;
    TransformState                          mTransformState = TS_NONE;
    TransformSpace                          mTransformSpace = SPACE_WORLD;
    Ogre::Real                              mScaleStartDistance = 0.0f;

    // Snap settings (persisted in QSettings)
    bool                                    mSnapEnabled = false;
    double                                  mSnapGridSize = 1.0;
    double                                  mSnapAngleStep = 15.0;
    double                                  mSnapScaleStep = 0.25;

    // Snap accumulators (track sub-snap-threshold motion during drag)
    Ogre::Vector3                           mSnapTranslationAccum = Ogre::Vector3::ZERO;
    Ogre::Vector3                           mSnapRotationAccum = Ogre::Vector3::ZERO;
    Ogre::Vector3                           mSnapScaleAccum = Ogre::Vector3::ZERO;

    // Undo state: captured at mouse press, used to create command at mouse release
    QList<Ogre::Vector3>                    mUndoStartPositions;
    QList<Ogre::Quaternion>                 mUndoStartOrientations;
    QList<Ogre::Vector3>                    mUndoStartScales;
#ifdef Q_OS_MACOS
    int mWindowSizeModifier = 2;
#else
    int mWindowSizeModifier = 1;
#endif
};


#endif //TRANSFORM_OPERATOR
