#include "CloudCredentialStore.h"
#include "CloudUploadPlanner.h"
#include "ProjectPackager.h"
#include "QtMeshCloudClient.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QEventLoop>
#include <QThread>
#include <QTemporaryDir>
#include <memory>

namespace {

struct ParsedHttpRequest {
    QString method;
    QString path;
    QMap<QString, QString> headers;
    QByteArray body;
};

QString headerValue(const QMap<QString, QString>& headers, const QString& name)
{
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0)
            return it.value();
    }
    return {};
}

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
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = QString::fromUtf8(lines.at(i)).trimmed();
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        req.headers.insert(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
    }
    return req;
}

void writeHttpResponse(QTcpSocket* socket, int status, const QByteArray& body,
                       const char* statusText = "OK")
{
    QByteArray response;
    response += "HTTP/1.1 ";
    response += QByteArray::number(status);
    response += ' ';
    response += statusText;
    response += "\r\nContent-Type: application/json\r\n";
    response += "Content-Length: ";
    response += QByteArray::number(body.size());
    response += "\r\nConnection: close\r\n\r\n";
    response += body;
    socket->write(response);
    socket->flush();
    socket->waitForBytesWritten(2000);
    socket->disconnectFromHost();
}

class CloudUploadE2EMock {
public:
    bool listen()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, [this]() {
            QTcpSocket* socket = m_server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                buffer->append(socket->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;

                const ParsedHttpRequest req = parseHttpRequest(*buffer);
                const int contentLength = req.headers.value(QStringLiteral("Content-Length")).toInt();
                if (buffer->size() < headerEnd + 4 + contentLength)
                    return;

                const QString auth = headerValue(req.headers, QStringLiteral("Authorization"));
                if (!auth.isEmpty())
                    m_lastAuthHeader = auth;

                if (req.method == QStringLiteral("POST") && req.path == QStringLiteral("/v1/projects")) {
                    ++m_createCalls;
                    QJsonObject root;
                    QJsonObject project;
                    project.insert(QStringLiteral("id"), QStringLiteral("proj-e2e"));
                    project.insert(QStringLiteral("ownerSlug"), QStringLiteral("me"));
                    project.insert(QStringLiteral("slug"), QStringLiteral("demo"));
                    root.insert(QStringLiteral("project"), project);
                    writeHttpResponse(socket, 201, QJsonDocument(root).toJson(QJsonDocument::Compact));
                    return;
                }

                if (req.method == QStringLiteral("POST")
                    && req.path.endsWith(QStringLiteral("/files/upload-urls"))) {
                    ++m_uploadUrlCalls;
                    m_lastUploadUrlsBody = req.body;
                    QJsonParseError error;
                    const QJsonDocument doc = QJsonDocument::fromJson(req.body, &error);
                    const QJsonArray files = doc.object().value(QStringLiteral("files")).toArray();
                    QJsonArray uploads;
                    for (int i = 0; i < files.size(); ++i) {
                        QJsonObject upload;
                        upload.insert(QStringLiteral("id"), QStringLiteral("file-%1").arg(i));
                        upload.insert(QStringLiteral("uploadUrl"),
                                      QStringLiteral("http://127.0.0.1:%1/put/%2")
                                          .arg(m_server.serverPort())
                                          .arg(i));
                        uploads.append(upload);
                    }
                    QJsonObject root;
                    root.insert(QStringLiteral("uploads"), uploads);
                    writeHttpResponse(socket, 200, QJsonDocument(root).toJson(QJsonDocument::Compact));
                    return;
                }

                if (req.method == QStringLiteral("PUT") && req.path.startsWith(QStringLiteral("/put/"))) {
                    ++m_putCalls;
                    writeHttpResponse(socket, 200, QByteArray(R"({"ok":true})"));
                    return;
                }

                if (req.method == QStringLiteral("POST")
                    && req.path.endsWith(QStringLiteral("/files/complete"))) {
                    ++m_completeCalls;
                    writeHttpResponse(socket, 200, QByteArray(R"({"ok":true,"scanStatus":"clean"})"));
                    return;
                }

                if (req.method == QStringLiteral("GET")
                    && req.path.startsWith(QStringLiteral("/v1/projects"))) {
                    QJsonObject root;
                    QJsonArray projects;
                    QJsonObject project;
                    project.insert(QStringLiteral("id"), QStringLiteral("proj-e2e"));
                    project.insert(QStringLiteral("ownerSlug"), QStringLiteral("me"));
                    project.insert(QStringLiteral("slug"), QStringLiteral("demo"));
                    project.insert(QStringLiteral("name"), QStringLiteral("Demo"));
                    projects.append(project);
                    root.insert(QStringLiteral("projects"), projects);
                    writeHttpResponse(socket, 200, QJsonDocument(root).toJson(QJsonDocument::Compact));
                    return;
                }

                if (req.method == QStringLiteral("DELETE")
                    && req.path == QStringLiteral("/v1/projects/proj-e2e")) {
                    ++m_deleteCalls;
                    writeHttpResponse(socket, 204, QByteArray{}, "No Content");
                    return;
                }

                writeHttpResponse(socket, 404, QByteArray(R"({"error":"not found"})"), "Not Found");
            });
        });
        return m_server.listen(QHostAddress::LocalHost);
    }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }

    int createCalls() const { return m_createCalls; }
    int uploadUrlCalls() const { return m_uploadUrlCalls; }
    int putCalls() const { return m_putCalls; }
    int completeCalls() const { return m_completeCalls; }
    int deleteCalls() const { return m_deleteCalls; }
    QString lastAuthHeader() const { return m_lastAuthHeader; }
    QByteArray lastUploadUrlsBody() const { return m_lastUploadUrlsBody; }

private:
    QTcpServer m_server;
    int m_createCalls = 0;
    int m_uploadUrlCalls = 0;
    int m_putCalls = 0;
    int m_completeCalls = 0;
    int m_deleteCalls = 0;
    QString m_lastAuthHeader;
    QByteArray m_lastUploadUrlsBody;
};

} // namespace

TEST(CloudUploadE2E, HappyPathUploadListDeleteUsesBearerAuth)
{
    CloudUploadE2EMock mock;
    ASSERT_TRUE(mock.listen());
    qputenv("QTMESH_API_BASE", mock.baseUrl().toUtf8());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString mainPath = dir.filePath(QStringLiteral("hero.obj"));
    QFile mainFile(mainPath);
    ASSERT_TRUE(mainFile.open(QIODevice::WriteOnly));
    mainFile.write("v 0 0 0\n");
    mainFile.close();

    const QString token = QStringLiteral("test-token");
    const auto project = QtMeshCloudClient::createProject(token, QStringLiteral("Demo"), QStringLiteral("demo"));
    ASSERT_TRUE(project.ok);

    const auto descriptors = CloudUploadPlanner::buildAssetFileDescriptors({mainPath});
    const auto uploadUrls = QtMeshCloudClient::requestUploadUrls(
        token, project.ownerSlug, project.projectSlug, descriptors);
    ASSERT_TRUE(uploadUrls.ok) << uploadUrls.errorString.toStdString()
                               << " http=" << uploadUrls.httpStatus;
    ASSERT_EQ(uploadUrls.uploads.size(), 1);

    QtMeshCloudClient::FileUploadResult uploaded;
    QThread* uploadWorker = QThread::create([&]() {
        uploaded = QtMeshCloudClient::uploadFileContent(
            token, uploadUrls.uploads.first(), mainPath, nullptr);
    });
    QEventLoop uploadLoop;
    QObject::connect(uploadWorker, &QThread::finished, &uploadLoop, &QEventLoop::quit);
    uploadWorker->start();
    uploadLoop.exec();
    uploadWorker->wait();
    delete uploadWorker;
    ASSERT_TRUE(uploaded.ok) << uploaded.errorString.toStdString();

    const auto completed = QtMeshCloudClient::completeUpload(
        token, project.ownerSlug, project.projectSlug,
        {uploadUrls.uploads.first().fileId}, uploadUrls.uploads.first().fileId);
    ASSERT_TRUE(completed.ok);

    const auto listed = QtMeshCloudClient::fetchProjects(token);
    ASSERT_TRUE(listed.ok) << listed.errorString.toStdString() << " http=" << listed.httpStatus;
    ASSERT_EQ(listed.projects.size(), 1);

    const auto deleted = QtMeshCloudClient::deleteProject(token, QStringLiteral("proj-e2e"));
    ASSERT_TRUE(deleted.ok);

    EXPECT_EQ(mock.createCalls(), 1);
    EXPECT_EQ(mock.uploadUrlCalls(), 1);
    EXPECT_EQ(mock.putCalls(), 1);
    EXPECT_EQ(mock.completeCalls(), 1);
    EXPECT_EQ(mock.deleteCalls(), 1);
    EXPECT_EQ(mock.lastAuthHeader(), QStringLiteral("Bearer test-token"));
    EXPECT_FALSE(mock.lastUploadUrlsBody().contains("/home/"));
    EXPECT_FALSE(mock.lastUploadUrlsBody().contains("C:"));
}

TEST(CloudUploadE2E, ManifestJsonPassesPathSanitisationLint)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString mainPath = dir.filePath(QStringLiteral("hero.obj"));
    QFile mainFile(mainPath);
    ASSERT_TRUE(mainFile.open(QIODevice::WriteOnly));
    mainFile.write("v 0 0 0\n");
    mainFile.close();

    const PackageMetadata manifest =
        ProjectPackager::buildManifest(mainPath, {}, QStringLiteral("Hero"));
    EXPECT_TRUE(ProjectPackager::jsonPassesPathSanitisationLint(ProjectPackager::toJson(manifest)));
}
