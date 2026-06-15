// Coverage tests for MCPServer cloud_* tool handlers (epic #684 slice H).
//
// Targets the six cloud tools:
//   cloud_status, cloud_login, cloud_logout,
//   cloud_list_projects, cloud_delete_project, cloud_upload
//
// callTool() short-circuits Ogre init for any tool name starting with
// "cloud_" (MCPServer.cpp), so these handlers run purely as
// request/response with NO display / GL / tryInitOgre needed. We exercise
// every validation + not-signed-in branch and the offline state machine
// (login -> status connected -> logout -> status disconnected). The
// network-success paths (fetchProjects/deleteProject/uploadPackage) require
// a live server and are intentionally not covered.
//
// Distinct filename + distinct suite name (MCPServerCloudToolsCoverageTest)
// to avoid ODR / duplicate-registration clashes with MCPServer_test.cpp.

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QFileInfo>

#include "MCPServer.h"
#include "CloudCredentialStore.h"

namespace {

// Re-parse the indented JSON document wrapped inside a success result's
// content[0].text back into a QJsonObject so we can assert on keys.
QJsonObject parseSuccessPayload(const QJsonObject &result)
{
    EXPECT_FALSE(result.contains("isError"))
        << "expected a success result (no isError)";
    const QJsonArray content = result.value("content").toArray();
    EXPECT_FALSE(content.isEmpty());
    const QJsonObject first = content.at(0).toObject();
    EXPECT_EQ(first.value("type").toString(), QStringLiteral("text"));
    const QString text = first.value("text").toString();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    EXPECT_EQ(err.error, QJsonParseError::NoError) << "payload was not valid JSON";
    EXPECT_TRUE(doc.isObject());
    return doc.object();
}

// Assert an error result and return the carried message text.
QString errorMessage(const QJsonObject &result)
{
    EXPECT_TRUE(result.value("isError").toBool())
        << "expected an error result (isError == true)";
    const QJsonArray content = result.value("content").toArray();
    EXPECT_FALSE(content.isEmpty());
    const QJsonObject first = content.at(0).toObject();
    EXPECT_EQ(first.value("type").toString(), QStringLiteral("text"));
    return first.value("text").toString();
}

} // namespace

class MCPServerCloudToolsCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // test_main.cpp owns the single QApplication; never create one here.
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr) << "QApplication instance must already exist";

        server = std::make_unique<MCPServer>();

        // Deterministic baseline: drop in-process cache and any persisted
        // session so the not-signed-in branches are reliably hit.
        CloudCredentialStore::resetCacheForTesting();
        CloudCredentialStore::clearSession();
        CloudCredentialStore::resetCacheForTesting();
    }

    void TearDown() override
    {
        // Leave the credential store clean for any later suite.
        CloudCredentialStore::clearSession();
        CloudCredentialStore::resetCacheForTesting();
        server.reset();
    }

    QApplication *app = nullptr;
    std::unique_ptr<MCPServer> server;
};

// --------------------------------------------------------------------------
// cloud_status
// --------------------------------------------------------------------------

TEST_F(MCPServerCloudToolsCoverageTest, StatusDisconnectedAfterClearSession)
{
    const QJsonObject result = server->callTool(QStringLiteral("cloud_status"), {});
    const QJsonObject payload = parseSuccessPayload(result);

    EXPECT_TRUE(payload.contains("connected"));
    EXPECT_FALSE(payload.value("connected").toBool());
    // No email key when disconnected.
    EXPECT_FALSE(payload.contains("email"));
}

TEST_F(MCPServerCloudToolsCoverageTest, StatusSuccessResultContentShape)
{
    const QJsonObject result = server->callTool(QStringLiteral("cloud_status"), {});
    // Success result must NOT carry isError.
    EXPECT_FALSE(result.contains("isError"));
    const QJsonArray content = result.value("content").toArray();
    ASSERT_FALSE(content.isEmpty());
    EXPECT_EQ(content.at(0).toObject().value("type").toString(),
              QStringLiteral("text"));
}

// --------------------------------------------------------------------------
// cloud_login
// --------------------------------------------------------------------------

TEST_F(MCPServerCloudToolsCoverageTest, LoginEmptyApiKeyIsError)
{
    // Missing api_key entirely.
    QJsonObject result = server->callTool(QStringLiteral("cloud_login"), {});
    QString msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("api_key"));

    // Present but blank / whitespace-only (trimmed to empty).
    QJsonObject blank;
    blank.insert(QStringLiteral("api_key"), QStringLiteral("   "));
    result = server->callTool(QStringLiteral("cloud_login"), blank);
    msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("api_key"));
}

TEST_F(MCPServerCloudToolsCoverageTest, LoginSuccessSavesSessionAndStatusConnected)
{
    QJsonObject args;
    args.insert(QStringLiteral("api_key"), QStringLiteral("test-token-abc123"));
    const QJsonObject result = server->callTool(QStringLiteral("cloud_login"), args);

    const QJsonObject payload = parseSuccessPayload(result);
    EXPECT_TRUE(payload.value("ok").toBool());
    EXPECT_TRUE(payload.contains("message"));

    // The session must now be persisted.
    EXPECT_TRUE(CloudCredentialStore::hasSession());
    EXPECT_EQ(CloudCredentialStore::loadSession().token,
              QStringLiteral("test-token-abc123"));

    // cloud_status now reports connected. No email was supplied, so the
    // email key must be omitted (login only sets .token).
    const QJsonObject status =
        parseSuccessPayload(server->callTool(QStringLiteral("cloud_status"), {}));
    EXPECT_TRUE(status.value("connected").toBool());
    EXPECT_FALSE(status.contains("email"));
}

TEST_F(MCPServerCloudToolsCoverageTest, StatusConnectedReportsEmailWhenPresent)
{
    // Seed a session carrying an email directly through the store, then
    // verify cloud_status surfaces it.
    CloudSession session;
    session.token = QStringLiteral("token-with-email");
    session.email = QStringLiteral("user@example.com");
    ASSERT_TRUE(CloudCredentialStore::saveSession(session));
    CloudCredentialStore::resetCacheForTesting();

    const QJsonObject status =
        parseSuccessPayload(server->callTool(QStringLiteral("cloud_status"), {}));
    EXPECT_TRUE(status.value("connected").toBool());
    ASSERT_TRUE(status.contains("email"));
    EXPECT_EQ(status.value("email").toString(),
              QStringLiteral("user@example.com"));
}

// --------------------------------------------------------------------------
// cloud_logout
// --------------------------------------------------------------------------

TEST_F(MCPServerCloudToolsCoverageTest, LogoutOkAndClearsSession)
{
    // Establish a session first so the token-present logout branch runs.
    QJsonObject login;
    login.insert(QStringLiteral("api_key"), QStringLiteral("logout-token"));
    ASSERT_FALSE(server->callTool(QStringLiteral("cloud_login"), login)
                     .contains("isError"));
    ASSERT_TRUE(CloudCredentialStore::hasSession());

    const QJsonObject result = server->callTool(QStringLiteral("cloud_logout"), {});
    const QJsonObject payload = parseSuccessPayload(result);
    EXPECT_TRUE(payload.value("ok").toBool());

    // Session must be gone.
    EXPECT_FALSE(CloudCredentialStore::hasSession());

    // And a subsequent cloud_status reports disconnected.
    const QJsonObject status =
        parseSuccessPayload(server->callTool(QStringLiteral("cloud_status"), {}));
    EXPECT_FALSE(status.value("connected").toBool());
}

TEST_F(MCPServerCloudToolsCoverageTest, LogoutWhenNotSignedInStillOk)
{
    // No session present — empty-token branch (no network logout call).
    ASSERT_FALSE(CloudCredentialStore::hasSession());
    const QJsonObject result = server->callTool(QStringLiteral("cloud_logout"), {});
    const QJsonObject payload = parseSuccessPayload(result);
    EXPECT_TRUE(payload.value("ok").toBool());
    EXPECT_FALSE(CloudCredentialStore::hasSession());
}

// --------------------------------------------------------------------------
// cloud_list_projects
// --------------------------------------------------------------------------

TEST_F(MCPServerCloudToolsCoverageTest, ListProjectsNotSignedInIsError)
{
    ASSERT_FALSE(CloudCredentialStore::hasSession());
    const QJsonObject result =
        server->callTool(QStringLiteral("cloud_list_projects"), {});
    const QString msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("not signed in"));
}

// --------------------------------------------------------------------------
// cloud_delete_project
// --------------------------------------------------------------------------

TEST_F(MCPServerCloudToolsCoverageTest, DeleteProjectMissingIdIsError)
{
    // No project_id at all.
    QJsonObject result =
        server->callTool(QStringLiteral("cloud_delete_project"), {});
    QString msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("project_id"));

    // Whitespace-only project_id is trimmed to empty -> same branch.
    QJsonObject blank;
    blank.insert(QStringLiteral("project_id"), QStringLiteral("  "));
    result = server->callTool(QStringLiteral("cloud_delete_project"), blank);
    msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("project_id"));
}

TEST_F(MCPServerCloudToolsCoverageTest, DeleteProjectNotSignedInIsError)
{
    ASSERT_FALSE(CloudCredentialStore::hasSession());
    QJsonObject args;
    args.insert(QStringLiteral("project_id"), QStringLiteral("proj-123"));
    const QJsonObject result =
        server->callTool(QStringLiteral("cloud_delete_project"), args);
    const QString msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("not signed in"));
}

// --------------------------------------------------------------------------
// cloud_upload
// --------------------------------------------------------------------------

TEST_F(MCPServerCloudToolsCoverageTest, UploadMissingFileIsError)
{
    QJsonObject result = server->callTool(QStringLiteral("cloud_upload"), {});
    QString msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("file"));

    // Explicit empty 'file' value hits the same branch.
    QJsonObject empty;
    empty.insert(QStringLiteral("file"), QString());
    result = server->callTool(QStringLiteral("cloud_upload"), empty);
    msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("file"));
}

TEST_F(MCPServerCloudToolsCoverageTest, UploadFileNotFoundIsError)
{
    QJsonObject args;
    args.insert(QStringLiteral("file"),
                QStringLiteral("/nonexistent/path/does_not_exist_12345.fbx"));
    const QJsonObject result = server->callTool(QStringLiteral("cloud_upload"), args);
    const QString msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("not found"));
}

TEST_F(MCPServerCloudToolsCoverageTest, UploadNotSignedInIsError)
{
    // Real, existing file so the not-found branch passes, but no session so
    // the not-signed-in branch fires (before any network access). Also
    // exercises the default-projectName fallback (no 'name' arg -> base name)
    // up to the token check.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString filePath = dir.path() + "/my_asset.bin";
    {
        QFile f(filePath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("dummy payload");
        f.close();
    }
    ASSERT_TRUE(QFileInfo::exists(filePath));
    ASSERT_FALSE(CloudCredentialStore::hasSession());

    QJsonObject args;
    args.insert(QStringLiteral("file"), filePath);
    // Deliberately omit 'name' to drive the completeBaseName() fallback path.
    const QJsonObject result = server->callTool(QStringLiteral("cloud_upload"), args);
    const QString msg = errorMessage(result);
    EXPECT_TRUE(msg.contains("not signed in"));
}
