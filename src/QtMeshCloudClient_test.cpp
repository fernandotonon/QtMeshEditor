#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <memory>
#include "QtMeshCloudClient.h"
#include "FeedbackDiagnostics.h"

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
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        req.headers.insert(key, value);
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

struct CloudReportHttpMock {
    QTcpServer server;
    bool uploadCompleteCalled = false;
    bool rejectReportBeforeComplete = true;
    int reportStatus = 200;
    QString lastMethod;
    QString lastPath;
    QString lastAuthHeader;
    QString lastContentType;
    QByteArray lastReportBody;
    QStringList requestOrder;

    bool listen()
    {
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            QTcpSocket* socket = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                buffer->append(socket->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;

                const ParsedHttpRequest req = parseHttpRequest(*buffer);
                const int contentLength = headerValue(req.headers, QStringLiteral("Content-Length")).toInt();
                if (buffer->size() < headerEnd + 4 + contentLength)
                    return;

                lastMethod = req.method;
                lastPath = req.path;
                lastAuthHeader = headerValue(req.headers, QStringLiteral("Authorization"));
                lastContentType = headerValue(req.headers, QStringLiteral("Content-Type"));

                if (req.method == QStringLiteral("POST")
                    && req.path.contains(QStringLiteral("/files/complete"))) {
                    uploadCompleteCalled = true;
                    requestOrder.append(QStringLiteral("complete"));
                    writeHttpResponse(socket, 200,
                                      QByteArray(R"({"ok":true,"scanStatus":"warning","files":[]})"));
                    return;
                }

                if (req.method == QStringLiteral("PUT") && req.path.contains(QStringLiteral("/report"))) {
                    requestOrder.append(QStringLiteral("report"));
                    lastReportBody = req.body;
                    if (rejectReportBeforeComplete && !uploadCompleteCalled) {
                        writeHttpResponse(socket, 400, QByteArray(R"({"error":"file incomplete"})"),
                                          "Bad Request");
                        return;
                    }
                    if (reportStatus >= 400) {
                        writeHttpResponse(socket, reportStatus, QByteArray(R"({"error":"server error"})"),
                                          "Error");
                        return;
                    }
                    writeHttpResponse(
                        socket, 200,
                        QByteArray(
                            R"({"ok":true,"file":{"id":"file-main","scanSummary":{"status":"warning"}}})"));
                    return;
                }

                writeHttpResponse(socket, 404, QByteArray(R"({"error":"not found"})"), "Not Found");
            });
        });
        return server.listen(QHostAddress::LocalHost);
    }

    quint16 port() const { return server.serverPort(); }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }
};

QJsonObject sampleScanReport()
{
    QJsonObject report;
    report.insert(QStringLiteral("version"), QStringLiteral("3.0.0"));
    QJsonObject summary;
    summary.insert(QStringLiteral("scanned"), 1);
    summary.insert(QStringLiteral("warnings"), 0);
    summary.insert(QStringLiteral("errors"), 0);
    report.insert(QStringLiteral("summary"), summary);
    return report;
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

TEST(QtMeshCloudClientUploadFileReport, MissingTokenReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::uploadFileReport(QString(), QStringLiteral("me"),
                                                      QStringLiteral("project"), QStringLiteral("file-1"),
                                                      sampleScanReport(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientUploadFileReport, MissingOwnerProjectOrFileIdReturnsErrorImmediately)
{
    const QJsonObject report = sampleScanReport();
    auto missingOwner = QtMeshCloudClient::uploadFileReport(QStringLiteral("token"), QString(),
                                                            QStringLiteral("project"), QStringLiteral("file-1"),
                                                            report, /*timeoutMs=*/100);
    EXPECT_FALSE(missingOwner.ok);
    EXPECT_TRUE(missingOwner.errorString.contains("owner slug", Qt::CaseInsensitive));

    auto missingFileId = QtMeshCloudClient::uploadFileReport(QStringLiteral("token"),
                                                             QStringLiteral("me"), QStringLiteral("project"),
                                                             QString(), report, /*timeoutMs=*/100);
    EXPECT_FALSE(missingFileId.ok);
    EXPECT_TRUE(missingFileId.errorString.contains("fileId", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientUploadFileReport, EmptyReportReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::uploadFileReport(QStringLiteral("token"), QStringLiteral("me"),
                                                      QStringLiteral("project"), QStringLiteral("file-1"),
                                                      QJsonObject{}, /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("empty", Qt::CaseInsensitive));
}

class QtMeshCloudClientUploadFileReportHttpTest : public QtMeshCloudClientApiBaseUrlTest {
protected:
    void SetUp() override
    {
        QtMeshCloudClientApiBaseUrlTest::SetUp();
        ASSERT_TRUE(m_mock.listen());
        qputenv("QTMESH_API_BASE", m_mock.baseUrl().toUtf8());
    }

    CloudReportHttpMock m_mock;
};

TEST_F(QtMeshCloudClientUploadFileReportHttpTest, UsesPutWithAuthAndJsonHeaders)
{
    m_mock.uploadCompleteCalled = true;
    m_mock.rejectReportBeforeComplete = false;

    const QJsonObject report = sampleScanReport();
    const auto result = QtMeshCloudClient::uploadFileReport(
        QStringLiteral("session-token"), QStringLiteral("owner slug"), QStringLiteral("proj/slug"),
        QStringLiteral("file-main"), report, /*timeoutMs=*/5000);

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(m_mock.lastMethod, QStringLiteral("PUT"));
    EXPECT_TRUE(m_mock.lastPath.contains(QStringLiteral("/v1/u/owner%20slug/p/proj%2Fslug/files/file-main/report")));
    EXPECT_EQ(m_mock.lastAuthHeader, QStringLiteral("Bearer session-token"));
    EXPECT_TRUE(m_mock.lastContentType.contains(QStringLiteral("application/json")));
    EXPECT_FALSE(m_mock.lastReportBody.isEmpty());
    const QJsonDocument sent = QJsonDocument::fromJson(m_mock.lastReportBody);
    ASSERT_TRUE(sent.isObject());
    EXPECT_EQ(sent.object().value(QStringLiteral("version")).toString(), QStringLiteral("3.0.0"));
}

TEST_F(QtMeshCloudClientUploadFileReportHttpTest, PropagatesHttpFailure)
{
    m_mock.uploadCompleteCalled = true;
    m_mock.rejectReportBeforeComplete = false;
    m_mock.reportStatus = 413;

    const auto result = QtMeshCloudClient::uploadFileReport(
        QStringLiteral("token"), QStringLiteral("me"), QStringLiteral("project"),
        QStringLiteral("file-1"), sampleScanReport(), /*timeoutMs=*/5000);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.httpStatus, 413);
    EXPECT_FALSE(result.errorString.isEmpty());
    EXPECT_FALSE(result.responseBodySnippet.isEmpty());
}

TEST_F(QtMeshCloudClientUploadFileReportHttpTest, ReportRejectedBeforeCompleteUpload)
{
    m_mock.uploadCompleteCalled = false;
    m_mock.rejectReportBeforeComplete = true;

    const auto result = QtMeshCloudClient::uploadFileReport(
        QStringLiteral("token"), QStringLiteral("me"), QStringLiteral("project"),
        QStringLiteral("file-1"), sampleScanReport(), /*timeoutMs=*/5000);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.httpStatus, 400);
}

TEST_F(QtMeshCloudClientUploadFileReportHttpTest, CompleteUploadMustPrecedeReportUpload)
{
    m_mock.rejectReportBeforeComplete = true;

    const auto completed = QtMeshCloudClient::completeUpload(
        QStringLiteral("token"), QStringLiteral("me"), QStringLiteral("project"),
        QStringList{QStringLiteral("file-1")}, QStringLiteral("file-1"), /*timeoutMs=*/5000);
    ASSERT_TRUE(completed.ok);

    const auto report = QtMeshCloudClient::uploadFileReport(
        QStringLiteral("token"), QStringLiteral("me"), QStringLiteral("project"),
        QStringLiteral("file-1"), sampleScanReport(), /*timeoutMs=*/5000);

    EXPECT_TRUE(report.ok);
    ASSERT_EQ(m_mock.requestOrder.size(), 2);
    EXPECT_EQ(m_mock.requestOrder.at(0), QStringLiteral("complete"));
    EXPECT_EQ(m_mock.requestOrder.at(1), QStringLiteral("report"));
}

TEST(QtMeshCloudClientFetchManifest, MissingOwnerReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::fetchProjectManifest(QStringLiteral("token"), QString(),
                                                         QStringLiteral("project"), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("owner", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientManifestFiles, PicksModelRoleOverEarlierAnimationFile)
{
    QJsonArray files;
    QJsonObject anim;
    anim.insert(QStringLiteral("id"), QStringLiteral("anim-1"));
    anim.insert(QStringLiteral("originalName"), QStringLiteral("walk.fbx"));
    anim.insert(QStringLiteral("role"), QStringLiteral("animation"));
    files.append(anim);

    QJsonObject model;
    model.insert(QStringLiteral("id"), QStringLiteral("model-1"));
    model.insert(QStringLiteral("originalName"), QStringLiteral("hero.fbx"));
    model.insert(QStringLiteral("role"), QStringLiteral("model"));
    files.append(model);

    QJsonObject manifest;
    manifest.insert(QStringLiteral("files"), files);

    const auto entries = QtMeshCloudClient::projectFilesFromManifest(manifest);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries.at(1).role, QStringLiteral("model"));

    const QStringList companions = QtMeshCloudClient::companionFileIdsForOpen(
        entries, QStringLiteral("model-1"));
    EXPECT_EQ(companions.size(), 1);
    EXPECT_EQ(companions.first(), QStringLiteral("model-1"));
}

TEST(QtMeshCloudClientManifestFiles, CompanionDownloadIncludesTextures)
{
    QList<QtMeshCloudClient::ProjectFileEntry> entries;
    QtMeshCloudClient::ProjectFileEntry model;
    model.id = QStringLiteral("model-1");
    model.originalName = QStringLiteral("hero.fbx");
    model.role = QStringLiteral("model");
    entries.append(model);

    QtMeshCloudClient::ProjectFileEntry texture;
    texture.id = QStringLiteral("tex-1");
    texture.originalName = QStringLiteral("textures/albedo.png");
    texture.role = QStringLiteral("texture");
    entries.append(texture);

    const QStringList companions = QtMeshCloudClient::companionFileIdsForOpen(
        entries, QStringLiteral("model-1"));
    EXPECT_EQ(companions.size(), 2);
    EXPECT_TRUE(companions.contains(QStringLiteral("model-1")));
    EXPECT_TRUE(companions.contains(QStringLiteral("tex-1")));
}

TEST(QtMeshCloudClientImportableAsset, RecognizesSceneAndMeshExtensions)
{
    EXPECT_TRUE(QtMeshCloudClient::isImportableCloudAssetPath(QStringLiteral("scene.scene.glb")));
    EXPECT_TRUE(QtMeshCloudClient::isImportableCloudAssetPath(QStringLiteral("hero.fbx")));
    EXPECT_FALSE(QtMeshCloudClient::isImportableCloudAssetPath(QStringLiteral("report.json")));
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
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                buffer->append(socket->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;

                const ParsedHttpRequest req = parseHttpRequest(*buffer);
                const int contentLength = headerValue(req.headers, QStringLiteral("Content-Length")).toInt();
                if (buffer->size() < headerEnd + 4 + contentLength)
                    return;

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
    EXPECT_EQ(page1.projects.first().browserUrl,
              QStringLiteral("https://qtmesh.dev/#/projects/me/first/dashboard"));
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

TEST_F(QtMeshCloudClientFetchProjectsHttpTest, SkipsProjectsMissingOwnerOrSlug)
{
    class IncompleteProjectMock {
    public:
        bool listen()
        {
            QObject::connect(&m_server, &QTcpServer::newConnection, [this]() {
                QTcpSocket* socket = m_server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                    QJsonObject incomplete;
                    incomplete.insert(QStringLiteral("id"), QStringLiteral("proj-no-owner"));
                    incomplete.insert(QStringLiteral("name"), QStringLiteral("Broken"));
                    incomplete.insert(QStringLiteral("slug"), QStringLiteral("broken"));

                    QJsonObject complete;
                    complete.insert(QStringLiteral("id"), QStringLiteral("proj-ok"));
                    complete.insert(QStringLiteral("name"), QStringLiteral("Good"));
                    complete.insert(QStringLiteral("slug"), QStringLiteral("good"));
                    complete.insert(QStringLiteral("ownerSlug"), QStringLiteral("me"));

                    QJsonObject root;
                    root.insert(QStringLiteral("projects"), QJsonArray{incomplete, complete});
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

    IncompleteProjectMock mock;
    ASSERT_TRUE(mock.listen());
    qputenv("QTMESH_API_BASE", mock.baseUrl().toUtf8());

    const auto result = QtMeshCloudClient::fetchProjects(QStringLiteral("token"), QString(), 50, 5000);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.projects.size(), 1);
    EXPECT_EQ(result.projects.first().id, QStringLiteral("proj-ok"));
}

TEST_F(QtMeshCloudClientFetchProjectsHttpTest, DeleteProjectAccepts204)
{
    const auto result = QtMeshCloudClient::deleteProject(QStringLiteral("token"), QStringLiteral("proj-1"), 5000);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(m_mock.deleteCalled());
}
