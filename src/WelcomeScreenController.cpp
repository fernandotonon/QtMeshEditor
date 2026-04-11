#include "WelcomeScreenController.h"
#include "SentryReporter.h"
#include <QSettings>
#include <QFileInfo>

WelcomeScreenController* WelcomeScreenController::m_pSingleton = nullptr;

WelcomeScreenController::WelcomeScreenController()
    : QObject(nullptr)
{
}

WelcomeScreenController* WelcomeScreenController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new WelcomeScreenController();
    return m_pSingleton;
}

WelcomeScreenController* WelcomeScreenController::qmlInstance(QQmlEngine* engine, QJSEngine* /*scriptEngine*/)
{
    auto* inst = instance();
    engine->setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void WelcomeScreenController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

QStringList WelcomeScreenController::recentFiles() const
{
    QSettings settings;
    return settings.value("RecentFiles/files").toStringList();
}

QStringList WelcomeScreenController::recentFileNames() const
{
    QStringList names;
    for (const QString& path : recentFiles()) {
        QFileInfo fi(path);
        names.append(fi.fileName());
    }
    return names;
}

bool WelcomeScreenController::shouldShow() const
{
    QSettings settings;
    return !settings.value("WelcomeScreen/dontShowAgain", false).toBool();
}

void WelcomeScreenController::setVisible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;
    emit visibleChanged();
}

void WelcomeScreenController::openFile(const QString& path)
{
    SentryReporter::addBreadcrumb("ui.action", "Welcome screen: open recent file");
    setVisible(false);
    emit requestOpenFile(path);
}

void WelcomeScreenController::openFileDialog()
{
    SentryReporter::addBreadcrumb("ui.action", "Welcome screen: open file dialog");
    setVisible(false);
    emit requestOpenFileDialog();
}

void WelcomeScreenController::newScene()
{
    SentryReporter::addBreadcrumb("ui.action", "Welcome screen: new scene");
    setVisible(false);
    emit requestNewScene();
}

void WelcomeScreenController::dismiss(bool dontShowAgain)
{
    SentryReporter::addBreadcrumb("ui.action",
        QString("Welcome screen dismissed (dontShowAgain=%1)").arg(dontShowAgain));

    if (dontShowAgain) {
        QSettings settings;
        settings.setValue("WelcomeScreen/dontShowAgain", true);
    }

    setVisible(false);
}
