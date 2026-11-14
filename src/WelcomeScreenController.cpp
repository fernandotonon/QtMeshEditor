#include "WelcomeScreenController.h"
#include "SentryReporter.h"
#include "QtMeshEcosystem.h"
#include <QUrl>
#include <QVariantMap>
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

QVariantList WelcomeScreenController::showcaseGames() const
{
    QVariantList out;
    for (const auto& game : QtMeshEcosystem::showcaseGames()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), game.id);
        m.insert(QStringLiteral("title"), game.title);
        m.insert(QStringLiteral("blurb"), game.blurb);
        m.insert(QStringLiteral("platform"), game.platform);
        // qrc:/ form so a QML Image can load the compiled-in thumbnail.
        m.insert(QStringLiteral("thumbnail"), QStringLiteral("qrc") + game.thumbnail);
        out.append(m);
    }
    return out;
}

QString WelcomeScreenController::tagline() const
{
    return QtMeshEcosystem::tagline();
}

QString WelcomeScreenController::platformsSummary() const
{
    QStringList bits;
    for (const auto& platform : QtMeshEcosystem::platforms()) {
        bits.append(platform.url.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(platform.name, platform.status.toLower())
            : platform.name);
    }
    return bits.join(QStringLiteral(" · "));
}

bool WelcomeScreenController::openEcosystemLink(const QString& id)
{
    static const QString kSurface = QStringLiteral("welcome_overlay");
    for (const auto& game : QtMeshEcosystem::showcaseGames()) {
        if (game.id == id)
            return QtMeshEcosystem::openUrl(QUrl(game.url), QStringLiteral("game:") + id, kSurface);
    }
    using QtMeshEcosystem::Link;
    for (Link link : {Link::Website, Link::GitHub, Link::Cloud, Link::Games,
                      Link::Sponsor, Link::Contribute, Link::Issues}) {
        if (QtMeshEcosystem::linkId(link) == id)
            return QtMeshEcosystem::open(link, kSurface);
    }
    return false;
}
