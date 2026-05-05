#ifndef ANIMATIONCONTROLCONTROLLER_H
#define ANIMATIONCONTROLCONTROLLER_H

#include <QObject>
#include <QVariantList>
#include <QStringList>
#include <QColor>
#include <QTimer>
#include <QQmlEngine>
#include <string>

namespace Ogre {
    class Bone;
    class Entity;
    class NodeAnimationTrack;
    class SkeletonInstance;
    class TransformKeyFrame;
}

class AnimationControlController : public QObject
{
    Q_OBJECT

    // Theme colors (same QPalette derivation as PropertiesPanelController)
    Q_PROPERTY(QColor panelColor     READ panelColor     NOTIFY themeChanged)
    Q_PROPERTY(QColor headerColor    READ headerColor    NOTIFY themeChanged)
    Q_PROPERTY(QColor textColor      READ textColor      NOTIFY themeChanged)
    Q_PROPERTY(QColor borderColor    READ borderColor    NOTIFY themeChanged)
    Q_PROPERTY(QColor inputColor     READ inputColor     NOTIFY themeChanged)
    Q_PROPERTY(QColor highlightColor READ highlightColor NOTIFY themeChanged)
    Q_PROPERTY(QColor buttonColor    READ buttonColor    NOTIFY themeChanged)
    Q_PROPERTY(QColor buttonTextColor READ buttonTextColor NOTIFY themeChanged)
    Q_PROPERTY(QColor disabledTextColor READ disabledTextColor NOTIFY themeChanged)

    // Animation / bone selection
    Q_PROPERTY(QVariantList animationTree   READ animationTree   NOTIFY animationTreeChanged)
    Q_PROPERTY(QString selectedEntityName   READ selectedEntityName   NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedAnimation    READ selectedAnimation    NOTIFY selectionChanged)
    Q_PROPERTY(QStringList boneNames        READ boneNames        NOTIFY boneListChanged)
    Q_PROPERTY(QString selectedBone         READ selectedBone         NOTIFY boneListChanged)

    // Timeline
    Q_PROPERTY(int    sliderValue     READ sliderValue     WRITE setSliderValue     NOTIFY sliderValueChanged)
    Q_PROPERTY(int    sliderMaximum   READ sliderMaximum   NOTIFY animationLengthChanged)
    Q_PROPERTY(double animationLength READ animationLength WRITE setAnimationLength NOTIFY animationLengthChanged)

    // Playback controls (speed multiplier + loop in/out region)
    Q_PROPERTY(double playbackSpeed   READ playbackSpeed   WRITE setPlaybackSpeed   NOTIFY playbackSpeedChanged)
    Q_PROPERTY(double loopStart       READ loopStart       WRITE setLoopStart       NOTIFY loopRegionChanged)
    Q_PROPERTY(double loopEnd         READ loopEnd         WRITE setLoopEnd         NOTIFY loopRegionChanged)
    Q_PROPERTY(bool   loopRegionActive READ loopRegionActive WRITE setLoopRegionActive NOTIFY loopRegionChanged)
    Q_PROPERTY(bool   autoKey         READ autoKey         WRITE setAutoKey         NOTIFY autoKeyChanged)

    // Keyframe tick marks on the timeline (list of ms positions)
    Q_PROPERTY(QVariantList keyframeTicks READ keyframeTicks NOTIFY keyframeTicksChanged)
    Q_PROPERTY(int selectedTick           READ selectedTick  NOTIFY keyframeTicksChanged)

    // Keyframe editing state
    Q_PROPERTY(bool onKeyframe        READ onKeyframe        NOTIFY currentKeyframeChanged)
    Q_PROPERTY(bool canDeleteKeyframe READ canDeleteKeyframe NOTIFY currentKeyframeChanged)
    Q_PROPERTY(bool hasPrevKeyframe   READ hasPrevKeyframe   NOTIFY keyframeTicksChanged)
    Q_PROPERTY(bool hasNextKeyframe   READ hasNextKeyframe   NOTIFY keyframeTicksChanged)
    Q_PROPERTY(bool hasAnimation      READ hasAnimation      NOTIFY selectionChanged)

    // Keyframe values (T/R/S for the closest keyframe to current time)
    Q_PROPERTY(double kfTransX READ kfTransX NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfTransY READ kfTransY NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfTransZ READ kfTransZ NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfScaleX READ kfScaleX NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfScaleY READ kfScaleY NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfScaleZ READ kfScaleZ NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfRotW   READ kfRotW   NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfRotX   READ kfRotX   NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfRotY   READ kfRotY   NOTIFY currentKeyframeChanged)
    Q_PROPERTY(double kfRotZ   READ kfRotZ   NOTIFY currentKeyframeChanged)

public:
    static AnimationControlController* instance();
    static AnimationControlController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    // Theme colors
    QColor panelColor()        const;
    QColor headerColor()       const;
    QColor textColor()         const;
    QColor borderColor()       const;
    QColor inputColor()        const;
    QColor highlightColor()    const;
    QColor buttonColor()       const;
    QColor buttonTextColor()   const;
    QColor disabledTextColor() const;

    // Animation tree
    QVariantList animationTree()     const { return m_animationTree; }
    QString      selectedEntityName() const { return QString::fromStdString(m_selectedEntityName); }
    QString      selectedAnimation()  const { return QString::fromStdString(m_selectedAnimation); }

    // Bone list
    QStringList boneNames()   const { return m_boneNames; }
    QString     selectedBone() const { return QString::fromStdString(m_selectedBone); }

    // Non-QML accessors used by TransformOperator's bone-gizmo path.
    // Both return nullptr when no animated entity is selected.
    Ogre::Entity* selectedEntity() const { return m_selectedEntity; }
    Ogre::Bone*   selectedBonePtr() const;

    /// Whether translation of the given bone is "safe" — true for the
    /// skeleton root (locomotion) and for non-rigged bones (attachment
    /// points like swords/shields/hats), false for any non-root bone
    /// whose handle appears in the mesh's vertex bone assignments
    /// (translating those breaks the rig). Returns true when bone is
    /// null or the entity has no mesh, since we can't disprove safety.
    bool boneCanTranslate(const Ogre::Bone* bone) const;

    // Timeline
    int    sliderValue()     const { return m_sliderValue; }
    int    sliderMaximum()   const { return m_sliderMaximum; }
    double animationLength() const { return m_sliderMaximum / 1000.0; }
    void   setSliderValue(int ms);
    void   setAnimationLength(double length);

    // Playback speed / loop region
    double playbackSpeed()    const { return m_playbackSpeed; }
    double loopStart()        const { return m_loopStart; }
    double loopEnd()          const { return m_loopEnd; }
    bool   loopRegionActive() const { return m_loopRegionActive; }
    bool   autoKey()          const { return m_autoKey; }
    void   setPlaybackSpeed(double s);
    void   setLoopStart(double s);
    void   setLoopEnd(double s);
    void   setLoopRegionActive(bool on);
    void   setAutoKey(bool on);

    /// Invalidate cached track / keyframe pointers and rebuild the bone
    /// list + slider ticks. Call after structural changes (track destroy,
    /// keyframe add/remove) — e.g., from the QUndoStack indexChanged
    /// handler — so subsequent slider scrubs don't read dangling
    /// pointers from the previous state.
    void onUndoRedoCommandApplied();

    // Compute the time after applying speed scaling and (optional) loop wrap.
    // Used by MainWindow::frameRenderingQueued. `currentTime` and `dt` are
    // in seconds; returns the new time position to assign back to the state.
    double advanceTime(double currentTime, double dt) const;

    /// Push a keyframe at the current scrub time on the active bone-track,
    /// capturing the bone's current pose. No-op if autoKey is off, no
    /// animation is selected, or no bone is selected. Called by
    /// TransformOperator at the end of every transform commit.
    void   autoKeyOnTransform();

    // Keyframe ticks
    QVariantList keyframeTicks() const { return m_keyframeTicks; }
    int          selectedTick()  const { return m_selectedTick; }

    // Keyframe state
    bool onKeyframe()        const { return m_currentKeyframe != nullptr; }
    bool canDeleteKeyframe() const { return m_currentKeyframe != nullptr; }
    bool hasPrevKeyframe()   const;
    bool hasNextKeyframe()   const;
    bool hasAnimation()      const { return !m_selectedAnimation.empty(); }

    // Keyframe values
    double kfTransX() const { return m_kfTransX; }
    double kfTransY() const { return m_kfTransY; }
    double kfTransZ() const { return m_kfTransZ; }
    double kfScaleX() const { return m_kfScaleX; }
    double kfScaleY() const { return m_kfScaleY; }
    double kfScaleZ() const { return m_kfScaleZ; }
    double kfRotW()   const { return m_kfRotW; }
    double kfRotX()   const { return m_kfRotX; }
    double kfRotY()   const { return m_kfRotY; }
    double kfRotZ()   const { return m_kfRotZ; }

    // QML-invokable actions
    Q_INVOKABLE void selectAnimation(const QString& entityName, const QString& animName);
    Q_INVOKABLE void selectBone(const QString& boneName);
    Q_INVOKABLE void addKeyframe();
    Q_INVOKABLE void deleteKeyframe();
    Q_INVOKABLE void prevKeyframe();
    Q_INVOKABLE void nextKeyframe();
    Q_INVOKABLE void setKfTransX(double v);
    Q_INVOKABLE void setKfTransY(double v);
    Q_INVOKABLE void setKfTransZ(double v);
    Q_INVOKABLE void setKfScaleX(double v);
    Q_INVOKABLE void setKfScaleY(double v);
    Q_INVOKABLE void setKfScaleZ(double v);
    Q_INVOKABLE void setKfRotW(double v);
    Q_INVOKABLE void setKfRotX(double v);
    Q_INVOKABLE void setKfRotY(double v);
    Q_INVOKABLE void setKfRotZ(double v);

    // ── Dope sheet API (slice C) ─────────────────────────────────────────────
    /// One row per animated bone on the currently selected animation.
    /// Each row is a QVariantMap: { "bone": QString, "keyTimes": QVariantList<double seconds> }.
    /// Returns an empty list when no animation is selected.
    Q_INVOKABLE QVariantList allBoneRows() const;

    /// Move a keyframe on `boneName`'s track from `oldTime` to `newTime`
    /// (both seconds). Match tolerance is 1 ms — same as the existing
    /// keyframe-tick comparison. Refuses the move if `newTime` collides
    /// with another existing keyframe on the same track. Pushes a
    /// MoveKeyframeCommand so the operation is undoable. Returns true
    /// when the move was applied.
    Q_INVOKABLE bool moveKeyframe(const QString& boneName,
                                  double oldTime, double newTime);

    /// Retime a keyframe in place without pushing an undo command. The
    /// caller commits one MoveKeyframeCommand on drag release; without
    /// the preview path each move event triggers Skeleton::reset(true)
    /// via MainWindow's indexChanged handler, blinking to T-pose.
    Q_INVOKABLE bool moveKeyframePreview(const QString& boneName,
                                         double oldTime, double newTime);

    // ── Bulk keyframe ops (slice D1) ──────────────────────────────────────────
    /// Move every selected keyframe by the same `dt` in seconds. Selection
    /// is a list of QVariantMaps with "bone" + "time" keys. Atomic: a
    /// single MoveKeyframesCommand on the undo stack. Returns false if the
    /// shift would push any member out of [0, length] or collide with a
    /// non-selected keyframe on the same track — in that case nothing is
    /// pushed and the entity stays unchanged.
    Q_INVOKABLE bool moveKeyframes(const QVariantList& selection, double dt);

    /// Serialize a selection to a JSON string suitable for
    /// QClipboard. The earliest selected time is `t0` so paste is relative.
    /// Empty selection → empty string.
    Q_INVOKABLE QString serializeKeyframes(const QVariantList& selection) const;

    /// Paste keyframes serialized by serializeKeyframes() at absolute
    /// `atTime` (interpreted as the destination "t0"). Skips entries whose
    /// destination time would collide with an existing keyframe on that
    /// track. Returns the count actually pasted.
    Q_INVOKABLE int pasteKeyframesAt(const QString& json, double atTime);

    // ── Curve editor API (slice D3b) ──────────────────────────────────────────
    /// Returns the scalar value of one channel at every keyframe of `bone`'s
    /// track on the currently-selected animation, in keyframe-time order.
    /// `channel` ∈ {tx, ty, tz, rw, rx, ry, rz, sx, sy, sz}.
    /// Empty when no animation/bone selected or the channel id is unknown.
    Q_INVOKABLE QVariantList channelValuesAt(const QString& boneName,
                                              const QString& channel) const;

    /// Write a single channel value into the keyframe at `time` on `bone`'s
    /// track, leaving the other 9 channels untouched. Pushes a
    /// SetKeyframeValueCommand so Ctrl+Z restores the original value.
    /// Returns true when the keyframe was found and updated.
    Q_INVOKABLE bool setKeyframeValue(const QString& boneName,
                                       const QString& channel,
                                       double time, double value);

    /// Write a channel value in place without pushing an undo command.
    /// Caller commits one SetKeyframeValueCommand on drag release. See
    /// moveKeyframePreview for the rationale.
    Q_INVOKABLE bool setKeyframeValuePreview(const QString& boneName,
                                              const QString& channel,
                                              double time, double value);

    /// Resample the curve segment between two adjacent keyframes for one
    /// channel into dense TransformKeyFrames so Ogre playback follows
    /// the Bezier/Auto/Stepped/Linear shape held in CurveEditModel.
    /// Pushes a single ResampleCurveCommand. Returns true on success.
    /// `t0` and `t1` must each be within 1ms of an existing keyframe.
    Q_INVOKABLE bool resampleCurveSegment(const QString& boneName,
                                          const QString& channel,
                                          double t0, double t1,
                                          double toleranceMul = 1.0,
                                          int fixedFps = 0);

    /// Set the curve handle (in/out tangent + interp mode) for one
    /// keyframe via an undoable command, then sync Ogre's per-animation
    /// interpolation mode (IM_LINEAR vs. IM_SPLINE) so playback follows
    /// the authored shape WITHOUT inserting dense keyframes. The
    /// caller can request an explicit resample later via
    /// resampleAllSegmentsForBone().
    Q_INVOKABLE bool setCurveHandle(const QString& boneName,
                                    const QString& channel,
                                    double keyTime,
                                    double newInTangent,
                                    double newOutTangent,
                                    int newMode);

    /// Walk every adjacent-keyframe pair on `bone`'s `channel` track
    /// and resample each segment. Bundled into one undo macro so
    /// Ctrl+Z reverts the whole bake. Returns the number of segments
    /// resampled. `density` picks the bake mode:
    ///   0 = Sparse (12× tolerance, fewest keys)
    ///   1 = Medium (4× tolerance)
    ///   2 = Dense  (1× tolerance, full adaptive sampling)
    ///   3 = 30 FPS fixed-rate (one key per 1/30s, no simplification)
    ///   4 = 60 FPS fixed-rate (one key per 1/60s)
    Q_INVOKABLE int resampleAllSegmentsForBone(const QString& boneName,
                                               const QString& channel,
                                               int density = 0);

    /// Decimate an already-dense track down to a target FPS by
    /// dropping keyframes that fall closer than 1/targetFps from a
    /// kept neighbor. Channel-agnostic (operates on the whole
    /// track's keyframes). Pushes ResampleCurveCommands per gap so
    /// Ctrl+Z reverts the decimation. Returns the number of frames
    /// removed.
    Q_INVOKABLE int reduceTrackToFps(const QString& boneName,
                                     int targetFps);

public slots:
    void updateAnimationTree();

signals:
    void themeChanged();
    void animationTreeChanged();
    void selectionChanged();
    void boneListChanged();
    void sliderValueChanged();
    void animationLengthChanged();
    void keyframeTicksChanged();
    void currentKeyframeChanged();
    void playbackSpeedChanged();
    void loopRegionChanged();
    void autoKeyChanged();
    /// Emitted when the dope-sheet view should refresh — track edits, clip
    /// selection, or keyframe add/delete/move.
    void boneRowsChanged();

private:
    AnimationControlController();
    ~AnimationControlController() override = default;

    void setAnimationFrame(int ms);
    void refreshBoneList();
    void refreshSliderTicks();
    void pushKeyframeValues();
    void notifyOgreUpdate();

    static AnimationControlController* m_pSingleton;

    QTimer* m_pollTimer = nullptr;

    Ogre::Entity*             m_selectedEntity   = nullptr;
    Ogre::SkeletonInstance*   m_selectedSkeleton = nullptr;
    Ogre::NodeAnimationTrack* m_selectedTrack    = nullptr;
    Ogre::TransformKeyFrame*  m_currentKeyframe  = nullptr;
    std::string               m_selectedEntityName;
    std::string               m_selectedAnimation;
    std::string               m_selectedBone;

    int    m_sliderValue   = 0;
    int    m_sliderMaximum = 0;
    int    m_selectedTick  = -1;

    QVariantList m_animationTree;
    bool         m_animationTreeBuilt = false; ///< true after first updateAnimationTree
    QStringList  m_boneNames;
    QVariantList m_keyframeTicks;

    bool   m_updatingValues = false;
    double m_kfTransX = 0, m_kfTransY = 0, m_kfTransZ = 0;
    double m_kfScaleX = 1, m_kfScaleY = 1, m_kfScaleZ = 1;
    double m_kfRotW   = 1, m_kfRotX   = 0, m_kfRotY   = 0, m_kfRotZ = 0;

    double m_playbackSpeed    = 1.0;
    double m_loopStart        = 0.0;
    double m_loopEnd          = 0.0;
    bool   m_loopRegionActive = false;
    bool   m_autoKey          = false;
};

#endif // ANIMATIONCONTROLCONTROLLER_H
