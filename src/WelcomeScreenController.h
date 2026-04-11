#ifndef WELCOME_SCREEN_CONTROLLER_H
#define WELCOME_SCREEN_CONTROLLER_H

#include <QObject>
#include <QStringList>
#include <QQmlEngine>

class MainWindow;

/**
 * @brief QML_SINGLETON that bridges the Welcome Screen overlay with MainWindow actions.
 *
 * Provides recent files list, file-open/new-scene triggers, and the "don't show again"
 * persistence via QSettings. Registered as a QML singleton under the "WelcomeScreen" module.
 */
class WelcomeScreenController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QStringList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    Q_PROPERTY(QStringList recentFileNames READ recentFileNames NOTIFY recentFilesChanged)
    Q_PROPERTY(bool visible READ isVisible WRITE setVisible NOTIFY visibleChanged)

public:
    static WelcomeScreenController* instance();
    static WelcomeScreenController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    void setMainWindow(MainWindow* mainWindow) { m_mainWindow = mainWindow; }

    QStringList recentFiles() const;
    QStringList recentFileNames() const;

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible);

    /// Should the welcome screen be shown on startup?
    bool shouldShow() const;

    Q_INVOKABLE void openFile(const QString& path);
    Q_INVOKABLE void openFileDialog();
    Q_INVOKABLE void newScene();
    Q_INVOKABLE void dismiss(bool dontShowAgain);

signals:
    void recentFilesChanged();
    void visibleChanged();
    void requestOpenFile(const QString& path);
    void requestOpenFileDialog();
    void requestNewScene();

private:
    WelcomeScreenController();
    ~WelcomeScreenController() override = default;

    static WelcomeScreenController* m_pSingleton;
    MainWindow* m_mainWindow = nullptr;
    bool m_visible = false;
};

#endif // WELCOME_SCREEN_CONTROLLER_H
