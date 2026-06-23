#include "CloudProjectsController.h"
#include "CloudCredentialStore.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {

struct ParsedHttpRequest {
    QString method;
    QString path;
};

ParsedHttpRequest parseHttpRequest(const QByteArray& raw)
{
    ParsedHttpRequest req;
    const QList<QByteArray> lines = raw.split('\n');
    if (!lines.isEmpty()) {
        const QString requestLine = QString::fromUtf8(lines.first()).trimmed();
        req.method = requestLine.section(QLatin1Char(' '), 0, 0);
        req.path = requestLine.section(QLatin1Char(' '), 1, 1);
    }
    return req;
}

void writeHttpResponse(QTcpSocket* socket, int status, const QByteArray& body)
{
    QByteArray response;
    response += "HTTP/1.1 ";
    response += QByteArray::number(status);
    response += status == 204 ? " No Content\r\n" : " OK\r\n";
    if (!body.isEmpty()) {
        response += "Content-Type: application/json\r\nContent-Length: ";
        response += QByteArray::number(body.size());
        response += "\r\n";
    }
    response += "Connection: close\r\n\r\n";
    response += body;
    socket->write(response);
    socket->flush();
    socket->waitForBytesWritten(2000);
    socket->disconnectFromHost();
}

class CloudProjectsControllerMock {
public:
    bool listen()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, [this]() {
            QTcpSocket* socket = m_server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                const ParsedHttpRequest req = parseHttpRequest(socket->readAll());

                if (req.method == QStringLiteral("GET") && req.path.startsWith(QStringLiteral("/v1/projects"))) {
                    ++m_listCalls;
                    const bool page2 = req.path.contains(QStringLiteral("cursor=page2"));
                    QJsonObject root;
                    if (page2) {
                        QJsonArray projects;
                        QJsonObject project;
                        project.insert(QStringLiteral("id"), QStringLiteral("proj-2"));
                        project.insert(QStringLiteral("name"), QStringLiteral("Page Two"));
                        project.insert(QStringLiteral("slug"), QStringLiteral("page-two"));
                        project.insert(QStringLiteral("ownerSlug"), QStringLiteral("me"));
                        projects.append(project);
                        root.insert(QStringLiteral("projects"), projects);
                    } else {
                        QJsonArray projects;
                        QJsonObject project;
                        project.insert(QStringLiteral("id"), QStringLiteral("proj-1"));
                        project.insert(QStringLiteral("name"), QStringLiteral("Page One"));
                        project.insert(QStringLiteral("slug"), QStringLiteral("page-one"));
                        project.insert(QStringLiteral("ownerSlug"), QStringLiteral("me"));
                        project.insert(QStringLiteral("sourceFormat"), QStringLiteral("fbx"));
                        project.insert(QStringLiteral("sizeBytes"), 4096);
                        project.insert(QStringLiteral("updatedAt"), QStringLiteral("2026-06-01T00:00:00Z"));
                        projects.append(project);
                        root.insert(QStringLiteral("projects"), projects);
                        root.insert(QStringLiteral("nextCursor"), QStringLiteral("page2"));
                    }
                    writeHttpResponse(socket, 200, QJsonDocument(root).toJson(QJsonDocument::Compact));
                    return;
                }

                if (req.method == QStringLiteral("DELETE")
                    && req.path.endsWith(QStringLiteral("/proj-1"))) {
                    m_deleteCalled = true;
                    writeHttpResponse(socket, 204, {});
                    return;
                }

                if (req.method == QStringLiteral("GET")
                    && req.path.contains(QStringLiteral("/manifest"))) {
                    ++m_manifestCalls;
                    QJsonArray files;
                    QJsonObject model;
                    model.insert(QStringLiteral("id"), QStringLiteral("file-model"));
                    model.insert(QStringLiteral("originalName"), QStringLiteral("hero.fbx"));
                    model.insert(QStringLiteral("name"), QStringLiteral("hero.fbx"));
                    model.insert(QStringLiteral("role"), QStringLiteral("model"));
                    model.insert(QStringLiteral("extension"), QStringLiteral("fbx"));
                    model.insert(QStringLiteral("sizeBytes"), 2048);
                    model.insert(QStringLiteral("downloadUrl"),
                                 QStringLiteral("http://127.0.0.1:%1/download/hero.fbx")
                                     .arg(m_server.serverPort()));
                    files.append(model);

                    QJsonObject texture;
                    texture.insert(QStringLiteral("id"), QStringLiteral("file-tex"));
                    texture.insert(QStringLiteral("originalName"), QStringLiteral("albedo.png"));
                    texture.insert(QStringLiteral("name"), QStringLiteral("albedo.png"));
                    texture.insert(QStringLiteral("role"), QStringLiteral("texture"));
                    texture.insert(QStringLiteral("extension"), QStringLiteral("png"));
                    texture.insert(QStringLiteral("sizeBytes"), 512);
                    texture.insert(QStringLiteral("downloadUrl"),
                                   QStringLiteral("http://127.0.0.1:%1/download/albedo.png")
                                       .arg(m_server.serverPort()));
                    files.append(texture);

                    QJsonObject root;
                    root.insert(QStringLiteral("files"), files);
                    writeHttpResponse(socket, 200, QJsonDocument(root).toJson(QJsonDocument::Compact));
                    return;
                }

                writeHttpResponse(socket, 404, QByteArray(R"({"error":"not found"})"));
            });
        });
        return m_server.listen(QHostAddress::LocalHost);
    }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }

    int listCalls() const { return m_listCalls; }
    bool deleteCalled() const { return m_deleteCalled; }
    int manifestCalls() const { return m_manifestCalls; }

private:
    QTcpServer m_server;
    int m_listCalls = 0;
    bool m_deleteCalled = false;
    int m_manifestCalls = 0;
};

} // namespace

class CloudProjectsControllerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_previousOrganizationName = QCoreApplication::organizationName();
        m_previousApplicationName = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("QtMeshEditorTests"));
        QCoreApplication::setApplicationName(QStringLiteral("CloudProjectsControllerTest"));
        CloudCredentialStore::resetCacheForTesting();
        CloudProjectsController::kill();
        CloudCredentialStore::clearSession();
        CloudSession session;
        session.token = QStringLiteral("test-token");
        ASSERT_TRUE(CloudCredentialStore::saveSession(session));

        m_hadApiBase = qEnvironmentVariableIsSet("QTMESH_API_BASE");
        if (m_hadApiBase)
            m_originalApiBase = qgetenv("QTMESH_API_BASE");

        m_mock = std::make_unique<CloudProjectsControllerMock>();
        ASSERT_TRUE(m_mock->listen());
        qputenv("QTMESH_API_BASE", m_mock->baseUrl().toUtf8());
    }

    void TearDown() override
    {
        CloudProjectsController::kill();
        CloudCredentialStore::clearSession();
        CloudCredentialStore::resetCacheForTesting();
        QCoreApplication::setOrganizationName(m_previousOrganizationName);
        QCoreApplication::setApplicationName(m_previousApplicationName);
        if (m_hadApiBase)
            qputenv("QTMESH_API_BASE", m_originalApiBase);
        else
            qunsetenv("QTMESH_API_BASE");
    }

    std::unique_ptr<CloudProjectsControllerMock> m_mock;
    QByteArray m_originalApiBase;
    bool m_hadApiBase = false;
    QString m_previousOrganizationName;
    QString m_previousApplicationName;
};

TEST_F(CloudProjectsControllerTest, RefreshLoadsFirstPage)
{
    auto* controller = CloudProjectsController::instance();
    QEventLoop loop;
    QObject::connect(controller, &CloudProjectsController::projectsChanged, &loop, &QEventLoop::quit);
    controller->refresh();
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_EQ(controller->projects().size(), 1);
    EXPECT_TRUE(controller->hasMore());
    EXPECT_TRUE(controller->listError().isEmpty());
}

TEST_F(CloudProjectsControllerTest, LoadMoreAppendsSecondPage)
{
    auto* controller = CloudProjectsController::instance();
    QEventLoop first;
    QObject::connect(controller, &CloudProjectsController::projectsChanged, &first, &QEventLoop::quit);
    controller->refresh();
    QTimer::singleShot(10000, &first, &QEventLoop::quit);
    first.exec();
    ASSERT_EQ(controller->projects().size(), 1);

    QEventLoop second;
    QObject::connect(controller, &CloudProjectsController::projectsChanged, &second, &QEventLoop::quit);
    controller->loadMore();
    QTimer::singleShot(10000, &second, &QEventLoop::quit);
    second.exec();

    EXPECT_EQ(controller->projects().size(), 2);
    EXPECT_GE(m_mock->listCalls(), 2);
}

TEST_F(CloudProjectsControllerTest, DeleteRemovesProjectFromList)
{
    auto* controller = CloudProjectsController::instance();
    QEventLoop loaded;
    QObject::connect(controller, &CloudProjectsController::projectsChanged, &loaded, &QEventLoop::quit);
    controller->refresh();
    QTimer::singleShot(10000, &loaded, &QEventLoop::quit);
    loaded.exec();
    ASSERT_EQ(controller->projects().size(), 1);

    QEventLoop deleted;
    QObject::connect(controller, &CloudProjectsController::projectsChanged, &deleted, &QEventLoop::quit);
    controller->deleteProject(QStringLiteral("proj-1"));
    QTimer::singleShot(10000, &deleted, &QEventLoop::quit);
    deleted.exec();

    EXPECT_TRUE(m_mock->deleteCalled());
    EXPECT_TRUE(controller->projects().isEmpty());
}

TEST_F(CloudProjectsControllerTest, FormatHelpersExposeReadableValues)
{
    auto* controller = CloudProjectsController::instance();
    EXPECT_FALSE(controller->formatFileSize(1024).isEmpty());
    EXPECT_FALSE(controller->formatUpdatedAt(QStringLiteral("2026-06-01T00:00:00Z")).isEmpty());
    EXPECT_FALSE(controller->formatIconForSource(QStringLiteral("fbx")).isEmpty());
    EXPECT_EQ(controller->formatFileRole(QStringLiteral("model")), QStringLiteral("Model"));
}

TEST_F(CloudProjectsControllerTest, BrowseProjectFilesLoadsManifest)
{
    auto* controller = CloudProjectsController::instance();
    QEventLoop loaded;
    QObject::connect(controller, &CloudProjectsController::projectsChanged, &loaded, &QEventLoop::quit);
    controller->refresh();
    QTimer::singleShot(10000, &loaded, &QEventLoop::quit);
    loaded.exec();
    ASSERT_EQ(controller->projects().size(), 1);

    QEventLoop filesLoaded;
    QObject::connect(controller, &CloudProjectsController::projectFilesChanged, &filesLoaded,
                     &QEventLoop::quit);
    controller->browseProjectFiles(QStringLiteral("proj-1"));
    QTimer::singleShot(10000, &filesLoaded, &QEventLoop::quit);
    filesLoaded.exec();

    EXPECT_TRUE(controller->viewingProjectFiles());
    EXPECT_EQ(controller->projectFiles().size(), 2);
    EXPECT_GE(m_mock->manifestCalls(), 1);

    QVariantMap modelFile = controller->projectFiles().first().toMap();
    EXPECT_TRUE(modelFile.value(QStringLiteral("canOpen")).toBool());
    EXPECT_FALSE(controller->canOpenFile(controller->projectFiles().last()));
}
