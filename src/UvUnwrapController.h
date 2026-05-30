#ifndef UV_UNWRAP_CONTROLLER_H
#define UV_UNWRAP_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

// QML-facing singleton for the xatlas-backed auto UV unwrap (issue
// #400). Wraps `UvUnwrap::unwrapEntity` plus selection-state property
// so the Inspector button can disable itself when nothing is
// selected. Headless callers (CLI / MCP) use `UvUnwrap` directly.
class UvUnwrapController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    static UvUnwrapController* instance();
    static UvUnwrapController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasSelection() const;
    bool busy() const { return m_busy; }

    // Run the unwrap on the first resolved selected entity AND
    // export the result to `outputPath`. The live entity is bit-
    // identical before and after — only the file on disk reflects
    // the unwrap. This is the GUI-safe entry point; live skinned
    // meshes can't survive in-place vertex-data mutation.
    //
    // Returns a QVariantMap mirroring `UvUnwrapReport::*` so the
    // dialog can surface "atlas 1024×1024, 338 charts, 72.9%
    // utilization" after Apply. Emits `unwrapApplied(report)` on
    // success or `error(msg)` on failure.
    Q_INVOKABLE QVariantMap unwrapSelectedToFile(const QString& outputPath,
                                                 int resolution,
                                                 int padding,
                                                 int channel,
                                                 bool preserveOriginalAsBackup);

    // Open a native save-file dialog and return the chosen path
    // (empty on cancel). Defaults to `<meshName>_unwrapped.glb`
    // alongside the user's chosen folder.
    Q_INVOKABLE QString chooseOutputPath();

signals:
    void selectionChanged();
    void busyChanged();
    void unwrapApplied(const QVariantMap& report);
    void error(const QString& message);

private:
    UvUnwrapController();
    ~UvUnwrapController() override = default;

    static UvUnwrapController* m_pSingleton;
    bool m_busy = false;
};

#endif // UV_UNWRAP_CONTROLLER_H
