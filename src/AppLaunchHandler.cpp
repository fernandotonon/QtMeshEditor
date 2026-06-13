#include "AppLaunchHandler.h"

#include "Manager.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QFileOpenEvent>

namespace {

bool isCliSubcommand(const QString& arg)
{
    static const QStringList kSubcommands = {
        QStringLiteral("info"), QStringLiteral("fix"), QStringLiteral("convert"),
        QStringLiteral("anim"), QStringLiteral("validate"), QStringLiteral("lod"),
        QStringLiteral("pose"), QStringLiteral("turntable"), QStringLiteral("scan"),
        QStringLiteral("material"), QStringLiteral("pack-textures"),
        QStringLiteral("normal-from-height"), QStringLiteral("memory"),
        QStringLiteral("analyze"), QStringLiteral("vertex-cache"),
        QStringLiteral("decimate"), QStringLiteral("atlas"), QStringLiteral("atlas-apply"),
        QStringLiteral("optimize"), QStringLiteral("bake-vertex-colors"),
        QStringLiteral("vat"), QStringLiteral("uv"), QStringLiteral("retopo"),
        QStringLiteral("skin"), QStringLiteral("morph"), QStringLiteral("nodeanim"),
        QStringLiteral("cloud"),
    };
    return kSubcommands.contains(arg);
}

bool isGuiModeFlag(const QString& arg)
{
    return arg == QStringLiteral("--mcp") || arg == QStringLiteral("-mcp")
        || arg == QStringLiteral("--with-mcp") || arg == QStringLiteral("--http-port");
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

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--cli") || arg == QStringLiteral("--help")
            || arg == QStringLiteral("-h") || arg == QStringLiteral("--version")
            || arg == QStringLiteral("-v")) {
            return true;
        }
    }

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
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
        if (arg.startsWith(QLatin1Char('-')))
            continue;
        if (isGuiModeFlag(arg)) {
            if (arg == QStringLiteral("--http-port") && i + 1 < arguments.size())
                ++i;
            continue;
        }
        if (isCliSubcommand(arg))
            break;

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
