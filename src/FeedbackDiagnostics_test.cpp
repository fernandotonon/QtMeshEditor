#include "FeedbackDiagnostics.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

class FeedbackDiagnosticsTest : public ::testing::Test {
protected:
    void SetUp() override { FeedbackDiagnostics::resetForTests(); }
    void TearDown() override { FeedbackDiagnostics::resetForTests(); }
};

TEST_F(FeedbackDiagnosticsTest, RedactPathKeepsFilenameOnly)
{
    EXPECT_EQ(FeedbackDiagnostics::redactPath(QStringLiteral("/home/user/secret/models/hero.fbx")),
              QStringLiteral("hero.fbx"));
    EXPECT_EQ(FeedbackDiagnostics::redactPath(QStringLiteral("C:\\Users\\dev\\mesh.obj")),
              QStringLiteral("mesh.obj"));
}

TEST_F(FeedbackDiagnosticsTest, RedactStringRemovesBearerTokensAndAbsolutePaths)
{
    const QString input =
        QStringLiteral("Bearer abc.def.ghi failed on /home/user/model.fbx with token=secret");
    const QString redacted = FeedbackDiagnostics::redactString(input);
    EXPECT_FALSE(redacted.contains(QStringLiteral("Bearer abc")));
    EXPECT_TRUE(redacted.contains(QStringLiteral("Bearer [redacted]")));
    EXPECT_FALSE(redacted.contains(QStringLiteral("/home/user/model.fbx")));
}

TEST_F(FeedbackDiagnosticsTest, CollectDiagnosticsIncludesAllowedKeysOnly)
{
    FeedbackDiagnostics::recordRecentEvent(QStringLiteral("ui.action"), QStringLiteral("Test event"));
    FeedbackDiagnostics::setOperationContext(QStringLiteral("import"),
                                             QStringLiteral("fbx"),
                                             QStringLiteral("E42"),
                                             QStringLiteral("/tmp/bad.fbx broke"));

    const QJsonObject diagnostics = FeedbackDiagnostics::collectDiagnostics(true);
    EXPECT_TRUE(diagnostics.contains(QStringLiteral("appVersion")));
    EXPECT_TRUE(diagnostics.contains(QStringLiteral("osName")));
    EXPECT_TRUE(diagnostics.contains(QStringLiteral("architecture")));
    EXPECT_TRUE(diagnostics.contains(QStringLiteral("locale")));
    EXPECT_TRUE(diagnostics.contains(QStringLiteral("editorSessionId")));
    EXPECT_TRUE(diagnostics.contains(QStringLiteral("cloudConnected")));
    EXPECT_TRUE(diagnostics.contains(QStringLiteral("featureFlags")));
    EXPECT_TRUE(diagnostics.contains(QStringLiteral("recentEvents")));

    const QJsonObject lastOp = diagnostics.value(QStringLiteral("lastOperation")).toObject();
    EXPECT_EQ(lastOp.value(QStringLiteral("operation")).toString(), QStringLiteral("import"));
    EXPECT_EQ(lastOp.value(QStringLiteral("format")).toString(), QStringLiteral("fbx"));
    EXPECT_FALSE(lastOp.value(QStringLiteral("errorMessage")).toString().contains(QStringLiteral("/tmp/")));
}

TEST_F(FeedbackDiagnosticsTest, DiagnosticsJsonStringIsSizeBounded)
{
    for (int i = 0; i < 100; ++i) {
        FeedbackDiagnostics::recordRecentEvent(
            QStringLiteral("ui.action"),
            QStringLiteral("Event %1 with extra detail to grow payload size").arg(i));
    }
    const QString json = FeedbackDiagnostics::diagnosticsJsonString(
        FeedbackDiagnostics::collectDiagnostics(true));
    EXPECT_LE(json.toUtf8().size(), FeedbackDiagnostics::kMaxDiagnosticsJsonBytes);
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    EXPECT_EQ(parseError.error, QJsonParseError::NoError);
}

TEST_F(FeedbackDiagnosticsTest, EditorSessionIdIsStablePerProcess)
{
    const QString first = FeedbackDiagnostics::editorSessionId();
    const QString second = FeedbackDiagnostics::editorSessionId();
    EXPECT_FALSE(first.isEmpty());
    EXPECT_EQ(first, second);
}
