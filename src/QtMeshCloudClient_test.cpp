#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QJsonObject>
#include "QtMeshCloudClient.h"
#include "FeedbackDiagnostics.h"

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
    auto result = QtMeshCloudClient::fetchProjects(QString(), /*timeoutMs=*/100);
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
