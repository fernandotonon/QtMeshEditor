#ifndef TRANSFORM_OPERATOR_H
#define TRANSFORM_OPERATOR_H

#include <QObject>
#include <QPoint>
#include <QList>
#include <vector>
#include <map>
#include <OgreRay.h>
#include <OgreVector.h>
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
    class SubEntity;
    class Bone;
}
class TransformOperator : public QObject, public QtMouseListener
{
    Q_OBJECT

public:

    static TransformOperator* getSingleton();
    /// Non-creating accessor: null until getSingleton() first runs. Use from
    /// paths that must not construct the gizmo machinery (headless CLI).
    static TransformOperator* getSingletonPtr() { return m_pSingleton; }
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
    enum PivotMode
    {
        PIVOT_CENTER,
        PIVOT_BOTTOM,
        PIVOT_ORIGIN,
    };
    Q_ENUM(PivotMode)

    const Ogre::ColourValue& getSelectionBoxColour() const;
    TransformSpace getTransformSpace() const { return mTransformSpace; }

    PivotMode pivotMode() const { return mPivotMode; }
    void setPivotMode(PivotMode mode);
    void cyclePivotMode();
    Ogre::Vector3 getPivotPoint() const;

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

    /// Decides whether a left-click during translate/rotate/scale should
    /// route to the bone-gizmo branch. Rotate and scale always go through
    /// the bone gizmo when a bone is selected — that's the primary posing
    /// workflow. Translate only goes there when the selected bone supports
    /// translation (root or unrigged); otherwise the click falls through to
    /// the entity-translate branch so the user can move the whole model.
    /// Returns false when no bone is selected.
    static bool shouldRouteToBoneGizmo(TransformState state,
                                       const Ogre::Bone* selectedBone,
                                       bool boneCanTranslate);

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
    void pivotModeChanged(PivotMode newMode);
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

public:
    /// Resize the active transform gizmo (translate/rotate/scale) so it
    /// keeps a roughly constant pixel size regardless of camera distance.
    /// Called per-frame from OgreWidget::frameStarted.
    void tickTransformGizmoScale(const Ogre::Camera* camera);
private:
    //Ogre::SceneNode*                        m_pSelectedNode;
    Ogre::RaySceneQuery*                    m_pRayQuery  = nullptr;
    Ogre::PlaneBoundedVolumeListSceneQuery* m_pVolQuery  = nullptr;
    Ogre::Vector3                           mStartPoint = Ogre::Vector3::ZERO;
    Ogre::Vector3                           mTransformVector = Ogre::Vector3::ZERO;
    TransformState                          mTransformState = TS_NONE;
    TransformSpace                          mTransformSpace = SPACE_WORLD;
    PivotMode                               mPivotMode = PIVOT_CENTER;

    // Snap settings (persisted in QSettings)
    bool                                    mSnapEnabled = false;
    double                                  mSnapGridSize = 1.0;
    double                                  mSnapAngleStep = 15.0;
    double                                  mSnapScaleStep = 0.25;

    // Snap accumulators (track sub-snap-threshold motion during drag)
    Ogre::Vector3                           mSnapTranslationAccum = Ogre::Vector3::ZERO;
    Ogre::Vector3                           mSnapRotationAccum = Ogre::Vector3::ZERO;
    Ogre::Vector3                           mSnapScaleAccum = Ogre::Vector3::ZERO;
    // Cumulative snapped scale factor applied during this drag (for entity absolute-scale path)
    Ogre::Vector3                           mSnapScaleCumulative = Ogre::Vector3::UNIT_SCALE;

    // Undo state: captured at mouse press, used to create command at mouse release
    QList<Ogre::Vector3>                    mUndoStartPositions;
    QList<Ogre::Quaternion>                 mUndoStartOrientations;
    QList<Ogre::Vector3>                    mUndoStartScales;
    QPoint                                  mScaleDragStartPixel;  ///< Press-time mouse pos (pixel-delta scale)

    // Sub-entity undo state: vertex positions captured at drag start
    QList<Ogre::SubEntity*>                 mUndoSubEntities;
    QList<std::vector<Ogre::Vector3>>       mUndoSubMeshPositions;

    // Edit mode undo state: vertex positions captured at drag start
    std::map<int, Ogre::Vector3>            mEditModeStartPositions;   ///< Rolling snapshot (updated each frame)
    std::map<int, Ogre::Vector3>            mEditModeUndoSnapshot;     ///< Immutable snapshot from press (for undo)
    Ogre::Vector3                           mEditModeScalePivot = Ogre::Vector3::ZERO; ///< Fixed pivot for scale drag
    QPoint                                  mEditModeScaleStartPixel;  ///< Screen pos at drag start (for pixel-delta scale)
    bool                                    mEditModeTransformActive = false;

    // Bevel-gizmo drag state. Active only while a bevel session is running
    // AND the user has mouse-pressed the gizmo handle.
    bool                                    mBevelDragActive = false;
    Ogre::Ray                               mBevelDragStartRay{Ogre::Vector3::ZERO, Ogre::Vector3::UNIT_Z};
    float                                   mBevelDragStartWidth = 0.0f;

    // Vertex paint drag state (edit mode only).
    bool                                    mVertexPaintDragActive = false;
    bool                                    mTexturePaintDragActive = false;

    // Bone-gizmo drag state. Active when a bone is the current selection
    // (per AnimationControlController::selectedBonePtr) and the user
    // mouse-presses inside the viewport with TS_TRANSLATE / TS_ROTATE /
    // TS_SCALE. The "before" TRS is captured at press time; the BoneTransformCommand
    // is pushed at release with the after-state.
    bool                                    mBoneDragActive = false;
    Ogre::Vector3                           mBoneStartPos = Ogre::Vector3::ZERO;
    Ogre::Quaternion                        mBoneStartOrient = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3                           mBoneStartScale = Ogre::Vector3::UNIT_SCALE;
    // Skeleton-local derived position captured at press. Used to drive
    // the bone via _setDerivedPosition during the drag (correct for
    // any bone in the hierarchy, regardless of parent orientation).
    Ogre::Vector3                           mBoneStartDerivedPos = Ogre::Vector3::ZERO;
    // World-space gizmo origin captured at press. Used to define a
    // stable mouse-projection plane during the drag; otherwise the
    // gizmo (which follows the bone) shifts the plane each event,
    // producing nonlinear/cumulative drift as the user drags further.
    Ogre::Vector3                           mBoneDragGizmoOrigin = Ogre::Vector3::ZERO;
    // Playback state captured at press so we can pause animation
    // during a bone drag (otherwise the per-frame _updateAnimation
    // reset wipes our local-pose edit) and restore it on release.
    bool                                    mBoneDragWasPlaying = false;
    // BlendMask weights captured at press so we can restore them
    // exactly on release (rather than blanket-resetting to 1.0,
    // which would destroy any layered/masked animation setup).
    // Each entry: (animation-state name, weight before drag).
    QList<std::pair<std::string, float>>    mBoneDragSavedMaskWeights;
#ifdef Q_OS_MACOS
    int mWindowSizeModifier = 2;
#else
    int mWindowSizeModifier = 1;
#endif
};


#endif //TRANSFORM_OPERATOR
