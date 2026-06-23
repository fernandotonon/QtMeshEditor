#ifndef APPLAUNCHHANDLER_H
#define APPLAUNCHHANDLER_H

#include <QObject>
#include <QStringList>

class QLocalServer;
class QEvent;

/// Routes OS file-open requests (argv, Finder QFileOpenEvent, second-instance
/// socket) into the running GUI. CLI activation rules in main.cpp take precedence.
class AppLaunchHandler : public QObject
{
    Q_OBJECT

public:
    static constexpr const char* kServerName = "QtMeshEditorSingleInstance-v1";

    explicit AppLaunchHandler(QObject* parent = nullptr);
    ~AppLaunchHandler() override;

    /// Mirrors main.cpp CLI detection: true when CLIPipeline should run.
    static bool isCliInvocation(int argc, char* argv[]);

    /// Positional mesh/scene paths from QApplication::arguments() (flags skipped).
    static QStringList collectGuiLaunchPaths(const QStringList& arguments);

    /// Extension check against Manager::defaultImportExtensions() (+ scene.glb).
    static bool isImportableMeshPath(const QString& path);

    /// If another GUI instance is running, forward paths and return true (caller exits).
    bool tryForwardToRunningInstance(const QStringList& paths);

    /// Listen for subsequent launches. Emits filesRequested when paths arrive.
    bool startSingleInstanceServer();

signals:
    void filesRequested(const QStringList& paths);
    void cloudProjectOpenRequested(const QString& ownerSlug, const QString& projectSlug);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void handleIncomingPaths(const QStringList& paths);

    QLocalServer* m_server = nullptr;
};

#endif // APPLAUNCHHANDLER_H
