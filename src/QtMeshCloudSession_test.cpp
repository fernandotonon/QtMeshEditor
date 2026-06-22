#include <gtest/gtest.h>

#include "ProjectPackager.h"
#include "QtMeshCloudSession.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

namespace {

struct ParsedHttpRequest {
    QString method;
    QString path;
    QByteArray body;
};

ParsedHttpRequest parseHttpRequest(const QByteArray& raw)
{
    ParsedHttpRequest req;
    const int headerEnd = raw.indexOf("\r\n\r\n");
    const QByteArray headerBlock = headerEnd >= 0 ? raw.left(headerEnd) : raw;
    req.body = headerEnd >= 0 ? raw.mid(headerEnd + 4) : QByteArray{};

    const QList<QByteArray> lines = headerBlock.split('\n');
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
    response += status == 200 ? " OK\r\n" : " Error\r\n";
    response += "Content-Type: application/json\r\nContent-Length: ";
    response += QByteArray::number(body.size());
    response += "\r\nConnection: close\r\n\r\n";
    response += body;
    socket->write(response);
    socket->flush();
    socket->waitForBytesWritten(2000);
    socket->disconnectFromHost();
}

class CloudUploadFlowMock {
public:
    bool listen()
    {
        if (!m_server.listen(QHostAddress::LocalHost))
            return false;
        m_baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());

        QObject::connect(&m_server, &QTcpServer::newConnection, [this]() {
            QTcpSocket* socket = m_server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                const ParsedHttpRequest req = parseHttpRequest(socket->readAll());

                if (req.method == QStringLiteral("POST") && req.path == QStringLiteral("/v1/projects")) {
                    writeHttpResponse(
                        socket, 200,
                        QByteArray(
                            R"({"project":{"id":"proj-1","ownerSlug":"testowner","slug":"testproj"}})"));
                    return;
                }

                if (req.method == QStringLiteral("POST")
                    && req.path.endsWith(QStringLiteral("/files/upload-urls"))) {
                    QJsonObject upload;
                    upload.insert(QStringLiteral("id"), QStringLiteral("file-main"));
                    upload.insert(QStringLiteral("uploadUrl"),
                                  QStringLiteral("%1/binary/file-main").arg(m_baseUrl));
                    upload.insert(QStringLiteral("sanitizedName"), QStringLiteral("model.obj"));
                    upload.insert(QStringLiteral("role"), QStringLiteral("main"));

                    QJsonObject root;
                    root.insert(QStringLiteral("uploadMethod"), QStringLiteral("PUT"));
                    root.insert(QStringLiteral("uploads"), QJsonArray{upload});
                    writeHttpResponse(socket, 200, QJsonDocument(root).toJson(QJsonDocument::Compact));
                    return;
                }

                if (req.method == QStringLiteral("PUT")
                    && req.path.startsWith(QStringLiteral("/binary/"))) {
                    writeHttpResponse(socket, 200, QByteArray(R"({"ok":true})"));
                    return;
                }

                if (req.method == QStringLiteral("POST")
                    && req.path.endsWith(QStringLiteral("/files/complete"))) {
                    m_completeCalled = true;
                    writeHttpResponse(
                        socket, 200,
                        QByteArray(R"({"ok":true,"scanStatus":"warning","files":[{"id":"file-main"}]})"));
                    return;
                }

                if (req.method == QStringLiteral("PUT")
                    && req.path.contains(QStringLiteral("/files/file-main/report"))) {
                    m_reportCalled = true;
                    if (m_failReport) {
                        writeHttpResponse(socket, 500, QByteArray(R"({"error":"report failed"})"));
                        return;
                    }
                    writeHttpResponse(socket, 200, QByteArray(R"({"ok":true,"file":{"id":"file-main"}})"));
                    return;
                }

                writeHttpResponse(socket, 404, QByteArray(R"({"error":"not found"})"));
            });
        });
        return true;
    }

    quint16 port() const { return m_server.serverPort(); }

    QString baseUrl() const { return m_baseUrl; }

    bool completeCalled() const { return m_completeCalled; }
    bool reportCalled() const { return m_reportCalled; }

    bool m_failReport = false;

private:
    QTcpServer m_server;
    QString m_baseUrl;
    bool m_completeCalled = false;
    bool m_reportCalled = false;
};

QJsonObject minimalScanReport()
{
    QJsonObject report;
    report.insert(QStringLiteral("version"), QStringLiteral("3.0.0"));
    QJsonObject summary;
    summary.insert(QStringLiteral("scanned"), 1);
    report.insert(QStringLiteral("summary"), summary);
    return report;
}

} // namespace

class QtMeshCloudSessionUploadReportTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_hadApiBase = qEnvironmentVariableIsSet("QTMESH_API_BASE");
        if (m_hadApiBase)
            m_originalApiBase = qgetenv("QTMESH_API_BASE");

        m_mock = std::make_unique<CloudUploadFlowMock>();
        ASSERT_TRUE(m_mock->listen());
        qputenv("QTMESH_API_BASE", m_mock->baseUrl().toUtf8());
    }

    void TearDown() override
    {
        if (m_hadApiBase)
            qputenv("QTMESH_API_BASE", m_originalApiBase);
        else
            qunsetenv("QTMESH_API_BASE");
    }

    std::unique_ptr<CloudUploadFlowMock> m_mock;
    QByteArray m_originalApiBase;
    bool m_hadApiBase = false;
};

TEST_F(QtMeshCloudSessionUploadReportTest, ReportFailureDoesNotFailBinaryUpload)
{
    ASSERT_NE(QCoreApplication::instance(), nullptr);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString modelPath = QDir(dir.path()).filePath(QStringLiteral("model.obj"));
    QFile model(modelPath);
    ASSERT_TRUE(model.open(QIODevice::WriteOnly));
    model.write("v 0 0 0\n");
    model.close();

    PackageMetadata manifest = ProjectPackager::buildManifest(modelPath, {}, QStringLiteral("Test"));
    manifest.scanSummary = minimalScanReport();
    ASSERT_FALSE(manifest.files.isEmpty());

    m_mock->m_failReport = true;

    QtMeshCloudSession session(QStringLiteral("session-token"));
    QEventLoop loop;
    bool uploadOk = false;
    QString uploadError;
    QObject::connect(&session, &QtMeshCloudSession::uploadFinished, &loop,
            [&](bool ok, const QString& error, const QString&, const QString&) {
                uploadOk = ok;
                uploadError = error;
                loop.quit();
            });

    session.uploadPackage(manifest);
    QTimer watchdog;
    watchdog.setSingleShot(true);
    watchdog.setInterval(30000);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start();
    loop.exec();

    ASSERT_TRUE(uploadOk) << "uploadFinished never fired (timeout or failure)";
    EXPECT_TRUE(m_mock->completeCalled());
    EXPECT_TRUE(m_mock->reportCalled());
    EXPECT_TRUE(uploadError.contains(QStringLiteral("analysis report upload failed"),
                                     Qt::CaseInsensitive));
}

class CloudProjectsApiMock {
public:
    bool listen()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, [this]() {
            QTcpSocket* socket = m_server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                const ParsedHttpRequest req = parseHttpRequest(socket->readAll());

                if (req.method == QStringLiteral("GET") && req.path.startsWith(QStringLiteral("/v1/projects"))) {
                    ++m_listCalls;
                    if (m_failFirstListWith401 && m_listCalls == 1) {
                        writeHttpResponse(socket, 401, QByteArray(R"({"error":"unauthorized"})"));
                        return;
                    }

                    QJsonObject root;
                    QJsonArray projects;
                    QJsonObject project;
                    project.insert(QStringLiteral("id"), QStringLiteral("proj-1"));
                    project.insert(QStringLiteral("name"), QStringLiteral("Demo"));
                    project.insert(QStringLiteral("slug"), QStringLiteral("demo"));
                    project.insert(QStringLiteral("ownerSlug"), QStringLiteral("me"));
                    projects.append(project);
                    root.insert(QStringLiteral("projects"), projects);
                    writeHttpResponse(socket, 200, QJsonDocument(root).toJson(QJsonDocument::Compact));
                    return;
                }

                if (req.method == QStringLiteral("DELETE")
                    && req.path.endsWith(QStringLiteral("/proj-1"))) {
                    m_deleteCalled = true;
                    writeHttpResponse(socket, 204, QByteArray{});
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

    bool m_failFirstListWith401 = false;

private:
    QTcpServer m_server;
    int m_listCalls = 0;
    bool m_deleteCalled = false;
};

class QtMeshCloudSessionProjectsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_hadApiBase = qEnvironmentVariableIsSet("QTMESH_API_BASE");
        if (m_hadApiBase)
            m_originalApiBase = qgetenv("QTMESH_API_BASE");

        m_mock = std::make_unique<CloudProjectsApiMock>();
        ASSERT_TRUE(m_mock->listen());
        qputenv("QTMESH_API_BASE", m_mock->baseUrl().toUtf8());
    }

    void TearDown() override
    {
        if (m_hadApiBase)
            qputenv("QTMESH_API_BASE", m_originalApiBase);
        else
            qunsetenv("QTMESH_API_BASE");
    }

    std::unique_ptr<CloudProjectsApiMock> m_mock;
    QByteArray m_originalApiBase;
    bool m_hadApiBase = false;
};

TEST_F(QtMeshCloudSessionProjectsTest, ListProjectsHappyPath)
{
    QtMeshCloudSession session(QStringLiteral("session-token"));
    QEventLoop loop;
    int count = 0;
    QObject::connect(&session, &QtMeshCloudSession::projectsListed, &loop,
                     [&](const QList<QtMeshCloudClient::ProjectSummary>& projects,
                         const QString& error,
                         const QString&,
                         bool hasMore) {
                         EXPECT_TRUE(error.isEmpty());
                         count = projects.size();
                         EXPECT_FALSE(hasMore);
                         loop.quit();
                     });
    session.listProjects();
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_EQ(count, 1);
}

TEST_F(QtMeshCloudSessionProjectsTest, DeleteProjectHappyPath)
{
    QtMeshCloudSession session(QStringLiteral("session-token"));
    QEventLoop loop;
    QString deletedId;
    QString deleteError;
    QObject::connect(&session, &QtMeshCloudSession::projectDeleted, &loop,
                     [&](const QString& id, const QString& error) {
                         deletedId = id;
                         deleteError = error;
                         loop.quit();
                     });
    session.deleteProject(QStringLiteral("proj-1"));
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_EQ(deletedId, QStringLiteral("proj-1"));
    EXPECT_TRUE(deleteError.isEmpty());
    EXPECT_TRUE(m_mock->deleteCalled());
}

TEST_F(QtMeshCloudSessionProjectsTest, ListProjects401SurfacesUnauthorized)
{
    m_mock->m_failFirstListWith401 = true;
    QtMeshCloudSession session(QStringLiteral("session-token"));
    QEventLoop loop;
    QString listError;
    QObject::connect(&session, &QtMeshCloudSession::projectsListed, &loop,
                     [&](const QList<QtMeshCloudClient::ProjectSummary>&,
                         const QString& error,
                         const QString&,
                         bool) {
                         listError = error;
                         loop.quit();
                     });
    session.listProjects();
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_EQ(listError, QStringLiteral("unauthorized"));
}

TEST_F(QtMeshCloudSessionProjectsTest, DownloadProjectStubNotImplemented)
{
    QtMeshCloudSession session(QStringLiteral("session-token"));
    QEventLoop loop;
    bool ok = true;
    QString code;
    QObject::connect(&session, &QtMeshCloudSession::downloadComplete, &loop,
                     [&](bool success, const QString&, const QString& errorCode) {
                         ok = success;
                         code = errorCode;
                         loop.quit();
                     });
    session.downloadProject(QStringLiteral("proj-1"), QStringLiteral("/tmp"));
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_FALSE(ok);
    EXPECT_EQ(code, QStringLiteral("not-implemented"));
}
