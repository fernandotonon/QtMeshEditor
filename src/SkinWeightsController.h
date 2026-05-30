#ifndef SKIN_WEIGHTS_CONTROLLER_H
#define SKIN_WEIGHTS_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

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

    /// Compute and apply skin weights to the first resolved
    /// selected entity. Returns a QVariantMap mirroring
    /// `SkinWeightsReport`. Emits `weightsApplied(report)` on
    /// success or `error(msg)` on failure.
    Q_INVOKABLE QVariantMap computeWeightsForSelected(int maxInfluencesPerVertex,
                                                       double falloff,
                                                       double maxInfluenceDistance,
                                                       bool skipUnweightedBones,
                                                       bool replaceExisting);

signals:
    void selectionChanged();
    void busyChanged();
    void weightsApplied(const QVariantMap& report);
    void error(const QString& message);

private:
    SkinWeightsController();
    ~SkinWeightsController() override = default;

    static SkinWeightsController* m_pSingleton;
    bool m_busy = false;
};

#endif // SKIN_WEIGHTS_CONTROLLER_H
