#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include "QtMeshCloudClient.h"
#include "FeedbackDiagnostics.h"

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

void writeHttpResponse(QTcpSocket* socket, int status, const QByteArray& body,
                       const char* statusText = "OK")
{
    QByteArray response;
    response += "HTTP/1.1 ";
    response += QByteArray::number(status);
    response += ' ';
    response += statusText;
    response += "\r\nContent-Type: application/json\r\n";
    if (!body.isEmpty()) {
        response += "Content-Length: ";
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

} // namespace

TEST(QtMeshCloudClientValidate, AcceptsMinimalValid)
{
    QJsonObject o;
    o["version"] = 1;
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_TRUE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsMissingRules)
{
    QJsonObject o;
    o["version"] = 1;
    o["scan"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsMissingVersion)
{
    QJsonObject o;
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsStringVersion)
{
    QJsonObject o;
    o["version"] = QStringLiteral("foo");
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsBoolVersion)
{
    QJsonObject o;
    o["version"] = true;
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsNonObjectScan)
{
    QJsonObject o;
    o["version"] = 1;
    o["scan"] = 42;       // not an object
    o["rules"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsNonObjectRules)
{
    QJsonObject o;
    o["version"] = 1;
    o["scan"] = QJsonObject{};
    o["rules"] = QStringLiteral("invalid");
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, AcceptsNumericVersionFloat)
{
    QJsonObject o;
    o["version"] = 1.5;    // doubles are allowed (JSON numbers)
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_TRUE(QtMeshCloudClient::validateCloudConfigJson(o));
}

// Snapshots QTMESH_API_BASE in SetUp and restores it in TearDown so individual
// tests can freely mutate the env var without leaking state into the rest of
// the UnitTests run (other suites may rely on the original value).
class QtMeshCloudClientApiBaseUrlTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_hadOriginal = qEnvironmentVariableIsSet("QTMESH_API_BASE");
        if (m_hadOriginal)
            m_original = qgetenv("QTMESH_API_BASE");
    }
    void TearDown() override {
        if (m_hadOriginal)
            qputenv("QTMESH_API_BASE", m_original);
        else
            qunsetenv("QTMESH_API_BASE");
    }
    QByteArray m_original;
    bool m_hadOriginal = false;
};

TEST_F(QtMeshCloudClientApiBaseUrlTest, DefaultUrlWhenEnvUnset)
{
    qunsetenv("QTMESH_API_BASE");
    EXPECT_EQ(QtMeshCloudClient::apiBaseUrl(),
              QStringLiteral("https://api.qtmesh.dev"));
}

TEST_F(QtMeshCloudClientApiBaseUrlTest, EnvOverride)
{
    qputenv("QTMESH_API_BASE", "https://my-host.example.com");
    EXPECT_EQ(QtMeshCloudClient::apiBaseUrl(),
              QStringLiteral("https://my-host.example.com"));
}

TEST_F(QtMeshCloudClientApiBaseUrlTest, TrailingSlashesStripped)
{
    qputenv("QTMESH_API_BASE", "https://my-host.example.com///");
    EXPECT_EQ(QtMeshCloudClient::apiBaseUrl(),
              QStringLiteral("https://my-host.example.com"));
}

TEST_F(QtMeshCloudClientApiBaseUrlTest, WhitespaceTrimmed)
{
    qputenv("QTMESH_API_BASE", "  https://api.test.com/  ");
    EXPECT_EQ(QtMeshCloudClient::apiBaseUrl(),
              QStringLiteral("https://api.test.com"));
}

TEST(QtMeshCloudClientFetchRules, MissingTokenReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::fetchRules(QString(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientUploadScan, MissingTokenReturnsErrorImmediately)
{
    QJsonObject empty;
    auto result = QtMeshCloudClient::uploadScanReport(QString(), empty, /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientDeviceCode, MissingClientNameReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::requestDeviceCode(QString(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("client name", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientDeviceToken, MissingDeviceCodeReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::pollDeviceToken(QString(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("device code", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientCurrentUser, MissingTokenReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::fetchCurrentUser(QString(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientLogout, MissingTokenReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::logout(QString(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientCreateProject, MissingTokenReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::createProject(QString(), QStringLiteral("Project"), QStringLiteral("project"),
                                                  QString(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientCreateProject, MissingNameOrSlugReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::createProject(QStringLiteral("token"), QString(), QStringLiteral("project"),
                                                  QString(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("name and slug", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientFetchProjects, MissingTokenReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::fetchProjects(QString(), QString(), 50, /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientRequestUploadUrls, MissingTokenReturnsErrorImmediately)
{
    QList<QtMeshCloudClient::AssetFileDescriptor> files;
    QtMeshCloudClient::AssetFileDescriptor file;
    file.path = QStringLiteral("model.obj");
    file.sizeBytes = 128;
    files.append(file);

    auto result = QtMeshCloudClient::requestUploadUrls(QString(), QStringLiteral("me"), QStringLiteral("project"),
                                                       files, /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientRequestUploadUrls, EmptyFileListReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::requestUploadUrls(QStringLiteral("token"), QStringLiteral("me"),
                                                       QStringLiteral("project"), {}, /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("at least one file", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientUploadFileContent, IncompleteTargetReturnsErrorImmediately)
{
    QtMeshCloudClient::UploadTarget target;
    auto result = QtMeshCloudClient::uploadFileContent(QStringLiteral("token"), target,
                                                       QStringLiteral("model.obj"), nullptr,
                                                       /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("upload target", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientCompleteUpload, MissingFileIdsReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::completeUpload(QStringLiteral("token"), QStringLiteral("me"),
                                                   QStringLiteral("project"), {}, QString(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("fileIds", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientFetchManifest, MissingOwnerReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::fetchProjectManifest(QStringLiteral("token"), QString(),
                                                         QStringLiteral("project"), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("owner", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientFeedbackPayload, IncludesRequiredFields)
{
    QtMeshCloudClient::FeedbackSubmission submission;
    submission.type = QStringLiteral("feature_request");
    submission.rating = QStringLiteral("great");
    submission.message = QStringLiteral("Test message");
    submission.relatedOperation = QStringLiteral("import");
    submission.relatedFormat = QStringLiteral("fbx");
    submission.includeDiagnostics = false;
    submission.contactAllowed = true;

    const QJsonObject body = QtMeshCloudClient::buildFeedbackPayload(submission);
    EXPECT_EQ(body.value(QStringLiteral("type")).toString(), QStringLiteral("feature_request"));
    EXPECT_EQ(body.value(QStringLiteral("rating")).toString(), QStringLiteral("great"));
    EXPECT_EQ(body.value(QStringLiteral("message")).toString(), QStringLiteral("Test message"));
    EXPECT_EQ(body.value(QStringLiteral("relatedOperation")).toString(), QStringLiteral("import"));
    EXPECT_EQ(body.value(QStringLiteral("relatedFormat")).toString(), QStringLiteral("fbx"));
    EXPECT_FALSE(body.value(QStringLiteral("includeDiagnostics")).toBool());
    EXPECT_TRUE(body.value(QStringLiteral("contactAllowed")).toBool());
    EXPECT_TRUE(body.contains(QStringLiteral("appVersion")));
    EXPECT_TRUE(body.contains(QStringLiteral("editorSessionId")));
    EXPECT_FALSE(body.contains(QStringLiteral("diagnosticsJson")));
}

TEST(QtMeshCloudClientFeedbackPayload, OmitsDiagnosticsWhenNotRequested)
{
    QtMeshCloudClient::FeedbackSubmission submission;
    submission.type = QStringLiteral("general");
    submission.message = QStringLiteral("Hello");
    submission.includeDiagnostics = false;

    const QJsonObject body = QtMeshCloudClient::buildFeedbackPayload(submission);
    EXPECT_FALSE(body.contains(QStringLiteral("diagnosticsJson")));
}

TEST(QtMeshCloudClientFeedbackPayload, IncludesDiagnosticsWhenRequested)
{
    FeedbackDiagnostics::resetForTests();
    QtMeshCloudClient::FeedbackSubmission submission;
    submission.type = QStringLiteral("general");
    submission.message = QStringLiteral("Hello");
    submission.includeDiagnostics = true;

    const QJsonObject body = QtMeshCloudClient::buildFeedbackPayload(submission);
    EXPECT_TRUE(body.value(QStringLiteral("includeDiagnostics")).toBool());
    EXPECT_TRUE(body.contains(QStringLiteral("diagnosticsJson")));
    EXPECT_TRUE(body.value(QStringLiteral("diagnosticsJson")).isObject());
    FeedbackDiagnostics::resetForTests();
}

TEST(QtMeshCloudClientFeedbackPayload, UsesV1FeedbackApiPath)
{
    EXPECT_EQ(QString(QtMeshCloudClient::kFeedbackApiPath), QStringLiteral("/v1/feedback"));
}

TEST(QtMeshCloudClientSubmitFeedback, MissingTokenReturnsErrorImmediately)
{
    QtMeshCloudClient::FeedbackSubmission submission;
    submission.type = QStringLiteral("bug");
    submission.message = QStringLiteral("Something failed");
    auto result = QtMeshCloudClient::submitFeedback(QString(), submission, /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.userMessage.contains(QStringLiteral("Sign in"), Qt::CaseInsensitive)
                || result.userMessage.contains(QStringLiteral("session"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientSubmitFeedback, MissingMessageReturnsValidationError)
{
    QtMeshCloudClient::FeedbackSubmission submission;
    submission.type = QStringLiteral("bug");
    auto result = QtMeshCloudClient::submitFeedback(QStringLiteral("token"), submission, /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.userMessage.isEmpty());
}

TEST(QtMeshCloudClientFeedbackPayload, NormalizesLegacyFeatureType)
{
    QtMeshCloudClient::FeedbackSubmission submission;
    submission.type = QStringLiteral("feature");
    submission.message = QStringLiteral("Hello");
    const QJsonObject body = QtMeshCloudClient::buildFeedbackPayload(submission);
    EXPECT_EQ(body.value(QStringLiteral("type")).toString(), QStringLiteral("feature_request"));
}

TEST(QtMeshCloudClientFriendlyFeedbackError, MapsHttpStatuses)
{
    EXPECT_TRUE(QtMeshCloudClient::friendlyFeedbackError(404, QString(), QString())
                    .contains(QStringLiteral("HTTP 404"), Qt::CaseInsensitive));
    EXPECT_TRUE(QtMeshCloudClient::friendlyFeedbackError(429, QString(), QString())
                    .contains(QStringLiteral("Too many"), Qt::CaseInsensitive));
    EXPECT_TRUE(QtMeshCloudClient::friendlyFeedbackError(400, QString(), QStringLiteral("Message is required"))
                    .contains(QStringLiteral("Enter a message"), Qt::CaseInsensitive));
    EXPECT_TRUE(QtMeshCloudClient::friendlyFeedbackError(413, QString(), QString())
                    .contains(QStringLiteral("too large"), Qt::CaseInsensitive));
    EXPECT_TRUE(QtMeshCloudClient::friendlyFeedbackError(401, QString(), QString())
                    .contains(QStringLiteral("Sign in"), Qt::CaseInsensitive));
}

class CloudProjectsListHttpMock {
public:
    bool listen()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, [this]() {
            QTcpSocket* socket = m_server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                const ParsedHttpRequest req = parseHttpRequest(socket->readAll());

                if (req.method == QStringLiteral("GET") && req.path.startsWith(QStringLiteral("/v1/projects"))) {
                    ++m_listCalls;
                    if (m_firstListUnauthorized && m_listCalls == 1) {
                        writeHttpResponse(socket, 401, QByteArray(R"({"error":"unauthorized"})"), "Unauthorized");
                        return;
                    }

                    const bool secondPage = req.path.contains(QStringLiteral("cursor=page2"));
                    QJsonObject root;
                    if (secondPage) {
                        QJsonArray projects;
                        QJsonObject project;
                        project.insert(QStringLiteral("id"), QStringLiteral("proj-2"));
                        project.insert(QStringLiteral("name"), QStringLiteral("Second"));
                        project.insert(QStringLiteral("slug"), QStringLiteral("second"));
                        project.insert(QStringLiteral("ownerSlug"), QStringLiteral("me"));
                        project.insert(QStringLiteral("sourceFormat"), QStringLiteral("fbx"));
                        project.insert(QStringLiteral("sizeBytes"), 2048);
                        project.insert(QStringLiteral("updatedAt"), QStringLiteral("2026-06-01T12:00:00Z"));
                        projects.append(project);
                        root.insert(QStringLiteral("projects"), projects);
                    } else {
                        QJsonArray projects;
                        QJsonObject project;
                        project.insert(QStringLiteral("id"), QStringLiteral("proj-1"));
                        project.insert(QStringLiteral("name"), QStringLiteral("First"));
                        project.insert(QStringLiteral("slug"), QStringLiteral("first"));
                        project.insert(QStringLiteral("ownerSlug"), QStringLiteral("me"));
                        project.insert(QStringLiteral("sourceFormat"), QStringLiteral("glb"));
                        project.insert(QStringLiteral("sizeBytes"), 1024);
                        project.insert(QStringLiteral("updatedAt"), QStringLiteral("2026-05-01T08:00:00Z"));
                        project.insert(QStringLiteral("mainFile"), QStringLiteral("model.glb"));
                        projects.append(project);
                        root.insert(QStringLiteral("projects"), projects);
                        root.insert(QStringLiteral("nextCursor"), QStringLiteral("page2"));
                    }
                    writeHttpResponse(socket, 200, QJsonDocument(root).toJson(QJsonDocument::Compact));
                    return;
                }

                if (req.method == QStringLiteral("DELETE")
                    && req.path == QStringLiteral("/v1/projects/proj-1")) {
                    m_deleteCalled = true;
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

    int listCalls() const { return m_listCalls; }
    bool deleteCalled() const { return m_deleteCalled; }

    bool m_firstListUnauthorized = false;

private:
    QTcpServer m_server;
    int m_listCalls = 0;
    bool m_deleteCalled = false;
};

class QtMeshCloudClientFetchProjectsHttpTest : public QtMeshCloudClientApiBaseUrlTest {
protected:
    void SetUp() override
    {
        QtMeshCloudClientApiBaseUrlTest::SetUp();
        ASSERT_TRUE(m_mock.listen());
        qputenv("QTMESH_API_BASE", m_mock.baseUrl().toUtf8());
    }

    CloudProjectsListHttpMock m_mock;
};

TEST_F(QtMeshCloudClientFetchProjectsHttpTest, ParsesPaginationAndMetadata)
{
    const auto page1 = QtMeshCloudClient::fetchProjects(QStringLiteral("token"), QString(), 50, 5000);
    ASSERT_TRUE(page1.ok);
    ASSERT_EQ(page1.projects.size(), 1);
    EXPECT_EQ(page1.projects.first().id, QStringLiteral("proj-1"));
    EXPECT_EQ(page1.projects.first().sourceFormat, QStringLiteral("glb"));
    EXPECT_EQ(page1.projects.first().sizeBytes, 1024);
    EXPECT_TRUE(page1.projects.first().browserUrl.contains(QStringLiteral("/projects/proj-1")));
    EXPECT_TRUE(page1.hasMore);
    EXPECT_EQ(page1.nextCursor, QStringLiteral("page2"));

    const auto page2 = QtMeshCloudClient::fetchProjects(QStringLiteral("token"), page1.nextCursor, 50, 5000);
    ASSERT_TRUE(page2.ok);
    ASSERT_EQ(page2.projects.size(), 1);
    EXPECT_EQ(page2.projects.first().id, QStringLiteral("proj-2"));
    EXPECT_FALSE(page2.hasMore);
}

TEST_F(QtMeshCloudClientFetchProjectsHttpTest, ParsesManifestStyleFieldNames)
{
    class ManifestStyleMock {
    public:
        bool listen()
        {
            QObject::connect(&m_server, &QTcpServer::newConnection, [this]() {
                QTcpSocket* socket = m_server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                    QJsonObject project;
                    project.insert(QStringLiteral("id"), QStringLiteral("proj-manifest"));
                    project.insert(QStringLiteral("name"), QStringLiteral("Manifest"));
                    project.insert(QStringLiteral("slug"), QStringLiteral("manifest"));
                    project.insert(QStringLiteral("ownerSlug"), QStringLiteral("me"));
                    project.insert(QStringLiteral("totalSize"), 8192);
                    project.insert(QStringLiteral("main_file"), QStringLiteral("hero.fbx"));
                    project.insert(QStringLiteral("updated_at"), QStringLiteral("2026-06-02T10:15:00Z"));

                    QJsonObject root;
                    root.insert(QStringLiteral("projects"), QJsonArray{project});
                    writeHttpResponse(socket, 200, QJsonDocument(root).toJson(QJsonDocument::Compact));
                });
            });
            return m_server.listen(QHostAddress::LocalHost);
        }

        QString baseUrl() const
        {
            return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
        }

    private:
        QTcpServer m_server;
    };

    ManifestStyleMock mock;
    ASSERT_TRUE(mock.listen());
    qputenv("QTMESH_API_BASE", mock.baseUrl().toUtf8());

    const auto result = QtMeshCloudClient::fetchProjects(QStringLiteral("token"), QString(), 50, 5000);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.projects.size(), 1);
    EXPECT_EQ(result.projects.first().sourceFormat, QStringLiteral("fbx"));
    EXPECT_EQ(result.projects.first().sizeBytes, 8192);
    EXPECT_EQ(result.projects.first().mainFile, QStringLiteral("hero.fbx"));
    EXPECT_EQ(result.projects.first().updatedAt, QStringLiteral("2026-06-02T10:15:00Z"));
}

TEST_F(QtMeshCloudClientFetchProjectsHttpTest, DeleteProjectAccepts204)
{
    const auto result = QtMeshCloudClient::deleteProject(QStringLiteral("token"), QStringLiteral("proj-1"), 5000);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(m_mock.deleteCalled());
}
