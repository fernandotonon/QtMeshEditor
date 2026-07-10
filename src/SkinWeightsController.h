#ifndef SKIN_WEIGHTS_CONTROLLER_H
#define SKIN_WEIGHTS_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

#include <atomic>
#include <memory>

// QML-facing singleton for automatic skin weights (issue #402).
// Wraps `SkinWeights::computeAndApply` plus selection + skeleton-
// presence state so the Inspector button can disable itself when
// no skinned mesh is selected.
class SkinWeightsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasSkinnedSelection READ hasSkinnedSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // Worker progress for the QML progress bar (the ML skinner takes
    // minutes; the other algorithms finish before the bar shows).
    Q_PROPERTY(bool skinDownloading READ skinDownloading NOTIFY skinProgressChanged)
    Q_PROPERTY(int skinProgress READ skinProgress NOTIFY skinProgressChanged)
    Q_PROPERTY(int skinTotal READ skinTotal NOTIFY skinProgressChanged)
    // True when the SkinTokens models are on disk (labels can say
    // which skinner will actually run).
    Q_PROPERTY(bool mlSkinnerReady READ mlSkinnerReady NOTIFY busyChanged)

public:
    static SkinWeightsController* instance();
    static SkinWeightsController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// True when the currently selected entity has a skeleton
    /// attached. The button binds to this so it disables on
    /// static-mesh selections (where computing weights is a
    /// no-op).
    bool hasSkinnedSelection() const;
    bool busy() const { return m_busy; }
    bool skinDownloading() const { return m_skinDownloading; }
    int  skinProgress() const { return m_skinProgress; }
    int  skinTotal() const { return m_skinTotal; }
    bool mlSkinnerReady() const;

    /// Compute and apply skin weights to the first resolved
    /// selected entity. Returns a QVariantMap mirroring
    /// `SkinWeightsReport`. Emits `weightsApplied(report)` on
    /// success or `error(msg)` on failure.
    ///
    /// `algorithm` is one of "skintokens" (default — the ML
    /// skinner, #819 Slice C; falls back to geodesic-voxel when
    /// models/ONNX are unavailable), "geodesic-voxel",
    /// "inverse-distance", or the deprecated alias "unirig".
    /// `voxelResolution` and `smoothIterations` map to the
    /// same-named SkinWeightsOptions fields.
    Q_INVOKABLE QVariantMap computeWeightsForSelected(int maxInfluencesPerVertex,
                                                       double falloff,
                                                       double maxInfluenceDistance,
                                                       bool skipUnweightedBones,
                                                       bool replaceExisting,
                                                       const QString& algorithm
                                                           = QStringLiteral("skintokens"),
                                                       int voxelResolution = 64,
                                                       int smoothIterations = 3);

    /// #819 Slice D: dual-quaternion display toggle for the selected
    /// entity. Modes: "linear" (default LBS path) or
    /// "dual-quaternion" (RTSS hardware DQS — kills the candy-wrapper
    /// collapse on twists). Runtime shading only; exported weights
    /// are unchanged. Returns the mode applied to / read from the
    /// first selected entity.
    Q_INVOKABLE QString skinningDisplayMode() const;
    Q_INVOKABLE bool setSkinningDisplayMode(const QString& mode);

    /// Async variant of computeWeightsForSelected: prepares on the
    /// main thread, runs the compute on a WORKER (UI stays
    /// responsive; skinProgress/skinTotal drive the progress bar),
    /// commits + pushes the undo command back on the main thread.
    /// Returns false when it could not start (invalid selection /
    /// already busy — `error` is emitted); the real outcome arrives
    /// via weightsApplied(report) or error(msg).
    Q_INVOKABLE bool computeWeightsForSelectedAsync(int maxInfluencesPerVertex,
                                                    double falloff,
                                                    double maxInfluenceDistance,
                                                    bool skipUnweightedBones,
                                                    bool replaceExisting,
                                                    const QString& algorithm
                                                        = QStringLiteral("skintokens"),
                                                    int voxelResolution = 64,
                                                    int smoothIterations = 3);

    /// Cancel an in-flight async skin compute (no-op otherwise).
    Q_INVOKABLE void cancelSkin();

signals:
    void selectionChanged();
    void busyChanged();
    void skinProgressChanged();
    void weightsApplied(const QVariantMap& report);
    void error(const QString& message);

private:
    SkinWeightsController();
    ~SkinWeightsController() override = default;

    static SkinWeightsController* m_pSingleton;
    bool m_busy = false;
    bool m_skinDownloading = false;
    int  m_skinProgress = 0;
    int  m_skinTotal = 0;
    std::shared_ptr<std::atomic_bool> m_skinCancel;
};

#endif // SKIN_WEIGHTS_CONTROLLER_H
