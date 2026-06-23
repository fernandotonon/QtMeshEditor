#ifndef AUTO_RIG_CONTROLLER_H
#define AUTO_RIG_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

// QML-facing singleton for native auto-rigging (issue #407).
// Wraps `AutoRig::rigEntity` (+ optional `SkinWeights::computeAndApply`)
// and exposes selection state so the Animation-Mode button can disable
// itself when the selection isn't a riggable static mesh.
class AutoRigController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // True when the selected entity is a STATIC (skeleton-less) mesh —
    // the only thing auto-rig can sensibly act on. Already-rigged meshes
    // and empty selections disable the button.
    Q_PROPERTY(bool hasRiggableSelection READ hasRiggableSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    static AutoRigController* instance();
    static AutoRigController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasRiggableSelection() const;
    bool busy() const { return m_busy; }

    /// Auto-rig the first resolved selected entity with `templateName`
    /// (humanoid / biped / quadruped / generic). When `alsoSkin` is true,
    /// chains SkinWeights::computeAndApply so the mesh deforms immediately.
    /// Returns a QVariantMap mirroring AutoRig::Report (+ a `skinned` bool).
    /// Emits `rigged(report)` on success or `error(msg)` on failure.
    Q_INVOKABLE QVariantMap autoRigSelected(const QString& templateName,
                                            bool alsoSkin);

signals:
    void selectionChanged();
    void busyChanged();
    void rigged(const QVariantMap& report);
    void error(const QString& message);

private:
    AutoRigController();
    ~AutoRigController() override = default;

    static AutoRigController* m_pSingleton;
    bool m_busy = false;
};

#endif // AUTO_RIG_CONTROLLER_H
