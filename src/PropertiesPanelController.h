#ifndef PROPERTIES_PANEL_CONTROLLER_H
#define PROPERTIES_PANEL_CONTROLLER_H

#include <QObject>
#include <QStringList>
#include <QColor>
#include <QVariantList>
#include <QVariantMap>
#include <QQmlEngine>
#include "SceneTreeModel.h"

class PropertiesPanelController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Theme colors (synced from QPalette)
    Q_PROPERTY(QColor panelColor READ panelColor NOTIFY themeChanged)
    Q_PROPERTY(QColor headerColor READ headerColor NOTIFY themeChanged)
    Q_PROPERTY(QColor textColor READ textColor NOTIFY themeChanged)
    Q_PROPERTY(QColor borderColor READ borderColor NOTIFY themeChanged)
    Q_PROPERTY(QColor inputColor READ inputColor NOTIFY themeChanged)
    Q_PROPERTY(QColor controlBgColor READ controlBgColor NOTIFY themeChanged)
    Q_PROPERTY(QColor highlightColor READ highlightColor NOTIFY themeChanged)
    Q_PROPERTY(bool autoUpdaterEnabled READ autoUpdaterEnabled CONSTANT)

    // Transform properties
    Q_PROPERTY(double posX READ posX WRITE setPosX NOTIFY transformChanged)
    Q_PROPERTY(double posY READ posY WRITE setPosY NOTIFY transformChanged)
    Q_PROPERTY(double posZ READ posZ WRITE setPosZ NOTIFY transformChanged)
    Q_PROPERTY(double rotX READ rotX WRITE setRotX NOTIFY transformChanged)
    Q_PROPERTY(double rotY READ rotY WRITE setRotY NOTIFY transformChanged)
    Q_PROPERTY(double rotZ READ rotZ WRITE setRotZ NOTIFY transformChanged)
    Q_PROPERTY(double scaleX READ scaleX WRITE setScaleX NOTIFY transformChanged)
    Q_PROPERTY(double scaleY READ scaleY WRITE setScaleY NOTIFY transformChanged)
    Q_PROPERTY(double scaleZ READ scaleZ WRITE setScaleZ NOTIFY transformChanged)

    // Selection state
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool mergeAnimationsEnabled READ mergeAnimationsEnabled NOTIFY selectionChanged)
    Q_PROPERTY(bool hasEntitySelection READ hasEntitySelection NOTIFY selectionChanged)
    Q_PROPERTY(bool hasMeshInSelection READ hasMeshInSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool receiveShadows READ receiveShadows WRITE setReceiveShadows NOTIFY selectionChanged)
    Q_PROPERTY(bool mixedReceiveShadows READ mixedReceiveShadows NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionName READ selectionName NOTIFY selectionChanged)
    Q_PROPERTY(QString transformTargetKind READ transformTargetKind NOTIFY transformTargetMetadataChanged)
    Q_PROPERTY(QString transformTargetLabel READ transformTargetLabel NOTIFY transformTargetMetadataChanged)
    Q_PROPERTY(QString transformTargetDetail READ transformTargetDetail NOTIFY transformTargetMetadataChanged)
    Q_PROPERTY(bool transformAffectsMesh READ transformAffectsMesh NOTIFY transformTargetMetadataChanged)
    Q_PROPERTY(QStringList sceneNodeNames READ sceneNodeNames NOTIFY sceneChanged)
    Q_PROPERTY(SceneTreeModel* sceneTreeModel READ sceneTreeModel CONSTANT)

    // Animation
    Q_PROPERTY(bool hasAnimations READ hasAnimations NOTIFY selectionChanged)
    // Reactive so the Skeleton section binding re-evaluates on selection /
    // rig changes (was a Q_INVOKABLE, so QML read it once and went stale —
    // leaving an empty Skeleton group on a not-yet-rigged mesh).
    Q_PROPERTY(bool hasSkeletonSelection READ hasSkeletonSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool playing READ isPlaying WRITE setPlaying NOTIFY playingChanged)

    // Pivot mode
    Q_PROPERTY(int pivotMode READ pivotMode WRITE setPivotMode NOTIFY pivotModeChanged)

    // Drag-and-drop state for scene tree reparenting
    Q_PROPERTY(QString draggedNodeName MEMBER m_draggedNodeName NOTIFY draggedNodeNameChanged)

    // Undo history
    Q_PROPERTY(QVariantList undoHistory READ undoHistory NOTIFY undoHistoryChanged)
    Q_PROPERTY(int undoIndex READ undoIndex NOTIFY undoHistoryChanged)

    // Snap properties
    Q_PROPERTY(bool snapEnabled READ snapEnabled WRITE setSnapEnabled NOTIFY snapEnabledChanged)
    Q_PROPERTY(double snapGridSize READ snapGridSize WRITE setSnapGridSize NOTIFY snapGridSizeChanged)
    Q_PROPERTY(double snapAngleStep READ snapAngleStep WRITE setSnapAngleStep NOTIFY snapAngleStepChanged)
    Q_PROPERTY(double snapScaleStep READ snapScaleStep WRITE setSnapScaleStep NOTIFY snapScaleStepChanged)

    // Primitive properties
    Q_PROPERTY(bool hasPrimitive READ hasPrimitive NOTIFY selectionChanged)
    Q_PROPERTY(QString primitiveType READ primitiveType NOTIFY selectionChanged)
    Q_PROPERTY(double primSizeX READ primSizeX WRITE setPrimSizeX NOTIFY primitiveChanged)
    Q_PROPERTY(double primSizeY READ primSizeY WRITE setPrimSizeY NOTIFY primitiveChanged)
    Q_PROPERTY(double primSizeZ READ primSizeZ WRITE setPrimSizeZ NOTIFY primitiveChanged)
    Q_PROPERTY(double primRadius READ primRadius WRITE setPrimRadius NOTIFY primitiveChanged)
    Q_PROPERTY(double primRadius2 READ primRadius2 WRITE setPrimRadius2 NOTIFY primitiveChanged)
    Q_PROPERTY(double primHeight READ primHeight WRITE setPrimHeight NOTIFY primitiveChanged)
    Q_PROPERTY(int primSegX READ primSegX WRITE setPrimSegX NOTIFY primitiveChanged)
    Q_PROPERTY(int primSegY READ primSegY WRITE setPrimSegY NOTIFY primitiveChanged)
    Q_PROPERTY(int primSegZ READ primSegZ WRITE setPrimSegZ NOTIFY primitiveChanged)
    Q_PROPERTY(double primUTile READ primUTile WRITE setPrimUTile NOTIFY primitiveChanged)
    Q_PROPERTY(double primVTile READ primVTile WRITE setPrimVTile NOTIFY primitiveChanged)

    // Primitive field visibility & labels (driven by primitive type)
    Q_PROPERTY(QVariantMap primFieldConfig READ primFieldConfig NOTIFY selectionChanged)

public:
    static PropertiesPanelController* instance();
    static PropertiesPanelController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    // Theme colors
    QColor panelColor() const;
    QColor headerColor() const;
    QColor textColor() const;
    QColor borderColor() const;
    QColor inputColor() const;
    QColor controlBgColor() const;
    QColor highlightColor() const;
    bool autoUpdaterEnabled() const;

    // Transform accessors
    double posX() const;
    double posY() const;
    double posZ() const;
    double rotX() const;
    double rotY() const;
    double rotZ() const;
    double scaleX() const;
    double scaleY() const;
    double scaleZ() const;

    // Transform mutators
    void setPosX(double v);
    void setPosY(double v);
    void setPosZ(double v);
    void setRotX(double v);
    void setRotY(double v);
    void setRotZ(double v);
    void setScaleX(double v);
    void setScaleY(double v);
    void setScaleZ(double v);

    // Pivot mode accessors/mutators
    int pivotMode() const;
    void setPivotMode(int mode);
    Q_INVOKABLE void cyclePivotMode();

    // Snap accessors/mutators
    bool snapEnabled() const;
    double snapGridSize() const;
    double snapAngleStep() const;
    double snapScaleStep() const;
    void setSnapEnabled(bool enabled);
    void setSnapGridSize(double size);
    void setSnapAngleStep(double degrees);
    void setSnapScaleStep(double step);

    Q_INVOKABLE QVariantList gridSizePresets() const;
    Q_INVOKABLE QVariantList angleStepPresets() const;
    Q_INVOKABLE QVariantList scaleStepPresets() const;

    // Selection state
    bool hasSelection() const;
    bool mergeAnimationsEnabled() const;
    bool hasEntitySelection() const;
    bool hasMeshInSelection() const;
    bool receiveShadows() const;
    void setReceiveShadows(bool enabled);
    bool mixedReceiveShadows() const;
    QString selectionName() const;
    QString transformTargetKind() const;
    QString transformTargetLabel() const;
    QString transformTargetDetail() const;
    bool transformAffectsMesh() const;
    QStringList sceneNodeNames() const;

    SceneTreeModel* sceneTreeModel() const;

    bool hasAnimations() const;

    // Primitive
    bool hasPrimitive() const;
    QString primitiveType() const;
    double primSizeX() const;
    double primSizeY() const;
    double primSizeZ() const;
    double primRadius() const;
    double primRadius2() const;
    double primHeight() const;
    QVariantMap primFieldConfig() const;
    int primSegX() const;
    int primSegY() const;
    int primSegZ() const;
    double primUTile() const;
    double primVTile() const;

    void setPrimSizeX(double v);
    void setPrimSizeY(double v);
    void setPrimSizeZ(double v);
    void setPrimRadius(double v);
    void setPrimRadius2(double v);
    void setPrimHeight(double v);
    void setPrimSegX(int v);
    void setPrimSegY(int v);
    void setPrimSegZ(int v);
    void setPrimUTile(double v);
    void setPrimVTile(double v);

    // Undo history
    QVariantList undoHistory() const;
    int undoIndex() const;
    Q_INVOKABLE void undoToIndex(int index);
    Q_INVOKABLE void clearUndoHistory();

    Q_INVOKABLE QVariantList shortcutData() const;

    // Generic QSettings accessors for Preferences dialog
    Q_INVOKABLE QVariant getSetting(const QString& key, const QVariant& defaultValue) const;
    Q_INVOKABLE void setSetting(const QString& key, const QVariant& value);

    Q_INVOKABLE void triggerMergeAnimations();
    Q_INVOKABLE void triggerMaterialEditor();
    /// Slice I: apply the named Ogre material to every selected
    /// sub-entity (or every sub-entity of selected entities). Returns
    /// the number of sub-entities affected; 0 means nothing was
    /// selected.
    Q_INVOKABLE int applyMaterialToSelection(const QString& materialName);

    /// Delete one scene node by name (scene-tree trash control).
    Q_INVOKABLE void deleteSceneTreeNode(const QString& nodeName);
    Q_INVOKABLE void renameSceneTreeLight(const QString& oldName, const QString& newName);
    /// Remove all user objects from the scene (after confirmation).
    Q_INVOKABLE void clearSceneTreeAllNodes();

    Q_INVOKABLE void selectNodeByName(const QString& name);
    Q_INVOKABLE bool canReparentNode(const QString& nodeName, const QString& newParentName);
    Q_INVOKABLE bool reparentNode(const QString& nodeName, const QString& newParentName);
    void setAnimationWidget(class AnimationWidget* widget) { mAnimationWidget = widget; }

    // Skeleton (bone/skeleton viz — independent of animation clips).
    // Returns one entry per selected entity that HAS a skeleton, regardless of
    // whether it has any animation states. Each entry: { entity, showSkeleton,
    // showWeights }. This is the data behind the "Skeleton" inspector section,
    // which must surface for skinned-but-non-animated meshes (e.g. a freshly
    // auto-rigged static mesh) — unlike animationData() which skips entities
    // with no animation clips.
    Q_INVOKABLE QVariantList skeletonData() const;
    /// True when the first resolved selection has a skeleton. Drives the
    /// "Skeleton" section's visibility. Exposed as a NOTIFY'd Q_PROPERTY above
    /// so the QML binding stays reactive across selection / rig changes.
    bool hasSkeletonSelection() const;

    // Animation
    Q_INVOKABLE QVariantList animationData() const;  // grouped per entity
    Q_INVOKABLE void toggleAnimationEnabled(const QString& entityName, const QString& animName, bool enabled);
    Q_INVOKABLE void toggleAnimationLoop(const QString& entityName, const QString& animName, bool loop);
    Q_INVOKABLE void setPlaying(bool playing);
    Q_INVOKABLE bool isPlaying() const { return mPlaying; }
    Q_INVOKABLE void toggleSkeletonDebug(const QString& entityName, bool show);
    Q_INVOKABLE void toggleBoneWeights(const QString& entityName, bool show);
    /// True when the skeleton debug overlay is active for `entityName`.
    Q_INVOKABLE bool isSkeletonDebugActive(const QString& entityName) const;
    /// True when any entity in the scene has skeleton debug enabled.
    bool hasAnySkeletonDebugActive() const;
    /// Apply skeleton overlay visibility without pushing undo (used by undo commands).
    void applySkeletonDebug(const QString& entityName, bool show);
    /// Rebuild skeleton/weight viewport overlays after bone CRUD.
    Q_INVOKABLE void refreshSkeletonOverlays(const QString& entityName);
    /// Rest-pose ghost overlay (#557) across active skeleton debug instances.
    void setRestPoseGhostVisible(bool show);
    Q_INVOKABLE bool renameAnimation(const QString& entityName, const QString& oldName, const QString& newName);
    /// Remove an animation from the entity's skeleton (irreversible). Disables
    /// debug overlays + playback first (rename-style safety) so stale pointers
    /// can't crash the poll timer, then re-selects to refresh widgets and emits
    /// animationStateChanged. Returns false if the entity/animation isn't found.
    Q_INVOKABLE bool deleteAnimation(const QString& entityName, const QString& animName);

    /// Analyze an animation's redundant keyframes without modifying it.
    /// `preset` is one of: "conservative", "balanced", "aggressive".
    /// Returns a map with: total, redundant, percent.
    Q_INVOKABLE QVariantMap analyzeAnimationKeyframes(const QString& entityName,
                                                      const QString& animName,
                                                      const QString& preset = QStringLiteral("conservative"));

    /// Remove redundant keyframes from an animation in-place. Returns the
    /// number of keyframes removed (0 on no-op or failure).
    /// `preset` is one of: "conservative", "balanced", "aggressive".
    Q_INVOKABLE int simplifyAnimation(const QString& entityName,
                                      const QString& animName,
                                      const QString& preset = QStringLiteral("conservative"));

    /// Bake every bone track in `animName` at the given density level
    /// (mirrors AnimationControlController::resampleAllSegmentsForBone:
    /// 0=Sparse / 1=Medium / 2=Dense / 3=30 FPS / 4=60 FPS). Bundles
    /// every per-channel resample under one undo macro so Ctrl+Z
    /// reverts the whole animation in one step. Returns the number of
    /// (bone, channel) tracks resampled.
    Q_INVOKABLE int bakeAnimation(const QString& entityName,
                                  const QString& animName,
                                  int density);

    /// Decimate every bone track in `animName` to `targetFps` keyframes
    /// per second. Wraps DecimateTrackCommand per bone in one undo
    /// macro. Returns the total number of keyframes removed.
    Q_INVOKABLE int reduceAnimationToFps(const QString& entityName,
                                          const QString& animName,
                                          int targetFps);

    /// Export the current animated pose of the first selected animated entity as a static mesh.
    /// Opens a file save dialog if no path is provided.
    Q_INVOKABLE bool exportCurrentPose(const QString& path = QString());

public slots:
    void onSelectionChanged();
    void onTransformChanged();
    void onSceneChanged();
    void refreshTheme();

    /// Re-emit selectionChanged so selection-derived bindings (e.g. the
    /// Skeleton section's hasSkeletonSelection) re-evaluate when the SELECTION
    /// itself didn't change but a property of it did — notably after a rig/unrig
    /// flips the entity's skeleton state. Called by AutoRigController.
    void notifySelectionMetadataChanged() { emit selectionChanged(); }

signals:
    void transformChanged();
    void selectionChanged();
    /// Emitted when the transform-target metadata (`transformTargetKind`,
    /// `transformTargetLabel`, `transformTargetDetail`, `transformAffectsMesh`)
    /// may have changed. Fires on selection deltas and on Edit Mode toggles.
    void transformTargetMetadataChanged();
    void sceneChanged();
    void themeChanged();
    void primitiveChanged();
    void playingChanged();
    void animationStateChanged();
    void pivotModeChanged();
    void undoHistoryChanged();
    void snapSettingsChanged();
    void snapEnabledChanged();
    void snapGridSizeChanged();
    void snapAngleStepChanged();
    void snapScaleStepChanged();
    void draggedNodeNameChanged();

private:
    PropertiesPanelController();
    ~PropertiesPanelController() override = default;

    static PropertiesPanelController* m_pSingleton;

    double mPosX = 0, mPosY = 0, mPosZ = 0;
    double mRotX = 0, mRotY = 0, mRotZ = 0;
    double mScaleX = 1, mScaleY = 1, mScaleZ = 1;

    SceneTreeModel* mSceneTreeModel = nullptr;
    bool mPlaying = false;
    class AnimationWidget* mAnimationWidget = nullptr;
    QString m_draggedNodeName;
};

#endif // PROPERTIES_PANEL_CONTROLLER_H
