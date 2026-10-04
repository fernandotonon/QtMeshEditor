#include "AppLaunchHandler.h"

#include "CloudDeepLink.h"
#include "Manager.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QFileOpenEvent>
#include <QUrl>

namespace {

bool isCliSubcommand(const QString& arg)
{
    static const QStringList kSubcommands = {
        QStringLiteral("info"), QStringLiteral("fix"), QStringLiteral("convert"),
        QStringLiteral("anim"), QStringLiteral("validate"), QStringLiteral("lod"),
        QStringLiteral("pose"), QStringLiteral("turntable"), QStringLiteral("isometric"), QStringLiteral("scan"),
        QStringLiteral("material"), QStringLiteral("pack-textures"),
        QStringLiteral("normal-from-height"), QStringLiteral("memory"),
        QStringLiteral("analyze"), QStringLiteral("vertex-cache"),
        QStringLiteral("decimate"), QStringLiteral("atlas"), QStringLiteral("atlas-apply"),
        QStringLiteral("optimize"), QStringLiteral("bake-vertex-colors"),
        QStringLiteral("paint-bake"), QStringLiteral("paint"),
        QStringLiteral("vat"), QStringLiteral("uv"), QStringLiteral("hdri"), QStringLiteral("light"),
        QStringLiteral("retopo"),
        QStringLiteral("skin"), QStringLiteral("rig"), QStringLiteral("facerig"),
        QStringLiteral("segment"), QStringLiteral("lattice"),
        QStringLiteral("generate3d"),
        QStringLiteral("morph"),
        QStringLiteral("nodeanim"), QStringLiteral("ps1"), QStringLiteral("cloud"),
        QStringLiteral("mocap"), QStringLiteral("lipsync"),
    };
    return kSubcommands.contains(arg);
}

// Flags that take a value — the value must not be mistaken for a launch path.
bool isGuiModeValueFlag(const QString& arg)
{
    // --http-token is listed so its VALUE is still consumed (never read as a
    // path or a subcommand) even though main() refuses it (#984 review: a
    // secret on argv is visible to every local user via ps).
    return arg == QStringLiteral("--http-port") || arg == QStringLiteral("--http-token")
        || arg == QStringLiteral("--http-token-file") || arg == QStringLiteral("--http-bind");
}

bool isGuiModeFlag(const QString& arg)
{
    return arg == QStringLiteral("--mcp") || arg == QStringLiteral("-mcp")
        || arg == QStringLiteral("--with-mcp") || isGuiModeValueFlag(arg);
}

} // namespace

AppLaunchHandler::AppLaunchHandler(QObject* parent)
    : QObject(parent)
{
    if (qobject_cast<QCoreApplication*>(QCoreApplication::instance()))
        QCoreApplication::instance()->installEventFilter(this);
}

AppLaunchHandler::~AppLaunchHandler()
{
    if (m_server) {
        m_server->close();
        QLocalServer::removeServer(QLatin1String(kServerName));
    }
}

bool AppLaunchHandler::isCliInvocation(int argc, char* argv[])
{
    const QString execName = QFileInfo(QString::fromLocal8Bit(argv[0])).fileName().toLower();
    if (execName.startsWith(QStringLiteral("qtmesh")) && !execName.contains(QStringLiteral("editor")))
        return true;

    // #984 review: the values of --http-port/--http-token-file/--http-bind are
    // opaque — a token file named "scan" or a value "--cli" must not route the
    // launch into the CLI. Consume each value in BOTH scans.
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (isGuiModeValueFlag(arg)) { ++i; continue; }
        if (arg == QStringLiteral("--cli") || arg == QStringLiteral("--help")
            || arg == QStringLiteral("-h") || arg == QStringLiteral("--version")
            || arg == QStringLiteral("-v")) {
            return true;
        }
    }

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (isGuiModeValueFlag(arg)) { ++i; continue; }
        if (arg.startsWith(QLatin1Char('-')))
            continue;
        if (isCliSubcommand(arg))
            return true;
        break;
    }
    return false;
}

bool AppLaunchHandler::isImportableMeshPath(const QString& path)
{
    const QString lower = path.toLower();
    if (lower.endsWith(QStringLiteral(".scene.glb"))
        || lower.endsWith(QStringLiteral(".scene.gltf"))) {
        return true;
    }

    const QStringList extensions =
        Manager::defaultImportExtensions().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& ext : extensions) {
        if (lower.endsWith(ext, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

QStringList AppLaunchHandler::collectGuiLaunchPaths(const QStringList& arguments)
{
    QStringList paths;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString& arg = arguments.at(i);
        // Value-taking flags FIRST: the generic '-' skip below used to run
        // before this branch, so the ++i never executed and the VALUE was
        // examined as a launch path (#984 — a --http-token naming an existing
        // mesh would have been opened in the editor).
        if (isGuiModeFlag(arg)) {
            if (isGuiModeValueFlag(arg) && i + 1 < arguments.size())
                ++i;
            continue;
        }
        if (arg.startsWith(QLatin1Char('-')))
            continue;
        if (isCliSubcommand(arg))
            break;

        CloudDeepLinkTarget cloud;
        if (CloudDeepLink::parseUrl(arg, &cloud)) {
            const QString token = CloudDeepLink::encodeLaunchToken(cloud.ownerSlug, cloud.projectSlug);
            if (!token.isEmpty() && !paths.contains(token))
                paths.append(token);
            continue;
        }

        const QFileInfo info(arg);
        if (!isImportableMeshPath(arg))
            continue;
        if (!info.exists() || !info.isFile() || !info.isReadable())
            continue;
        paths.append(info.absoluteFilePath());
    }
    return paths;
}

bool AppLaunchHandler::tryForwardToRunningInstance(const QStringList& paths)
{
    if (paths.isEmpty())
        return false;

    QLocalSocket socket;
    socket.connectToServer(QLatin1String(kServerName));
    if (!socket.waitForConnected(750))
        return false;

    QByteArray payload;
    {
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << paths;
    }
    socket.write(payload);
    socket.flush();
    socket.waitForBytesWritten(1500);
    socket.disconnectFromServer();
    return true;
}

bool AppLaunchHandler::startSingleInstanceServer()
{
    if (m_server)
        return true;

    QLocalServer::removeServer(QLatin1String(kServerName));
    m_server = new QLocalServer(this);
    if (!m_server->listen(QLatin1String(kServerName)))
        return false;

    connect(m_server, &QLocalServer::newConnection, this, [this]() {
        while (m_server->hasPendingConnections()) {
            QLocalSocket* socket = m_server->nextPendingConnection();
            connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
                const QByteArray payload = socket->readAll();
                if (payload.isEmpty())
                    return;
                QDataStream in(payload);
                in.setVersion(QDataStream::Qt_6_0);
                QStringList paths;
                in >> paths;
                handleIncomingPaths(paths);
                socket->disconnectFromServer();
                socket->deleteLater();
            });
        }
    });
    return true;
}

void AppLaunchHandler::handleIncomingPaths(const QStringList& paths)
{
    QStringList accepted;
    for (const QString& path : paths) {
        CloudDeepLinkTarget cloud;
        if (CloudDeepLink::decodeLaunchToken(path, &cloud)) {
            SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.open_in_editor"),
                QStringLiteral("Deep link %1/%2").arg(cloud.ownerSlug, cloud.projectSlug));
            emit cloudProjectOpenRequested(cloud.ownerSlug, cloud.projectSlug);
            continue;
        }
        if (!isImportableMeshPath(path))
            continue;
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile())
            continue;
        accepted.append(info.absoluteFilePath());
    }
    if (!accepted.isEmpty()) {
        SentryReporter::addBreadcrumb(QStringLiteral("app.launch.file_open"),
            QStringLiteral("Received %1 file(s) via launch handler").arg(accepted.size()));
        emit filesRequested(accepted);
    }
}

bool AppLaunchHandler::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FileOpen) {
        auto* openEvent = static_cast<QFileOpenEvent*>(event);
        const QUrl url = openEvent->url();
        if (url.isValid()) {
            CloudDeepLinkTarget cloud;
            if (CloudDeepLink::parseUrl(url, &cloud)) {
                SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.open_in_editor"),
                    QStringLiteral("macOS FileOpen URL %1/%2").arg(cloud.ownerSlug, cloud.projectSlug));
                emit cloudProjectOpenRequested(cloud.ownerSlug, cloud.projectSlug);
                return true;
            }
        }
        const QString path = openEvent->file();
        if (!path.isEmpty() && isImportableMeshPath(path)) {
            SentryReporter::addBreadcrumb(QStringLiteral("app.launch.file_open"),
                QStringLiteral("macOS FileOpen: %1").arg(QFileInfo(path).fileName()));
            handleIncomingPaths({QFileInfo(path).absoluteFilePath()});
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
