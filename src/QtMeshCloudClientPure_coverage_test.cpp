// Coverage tests for QtMeshCloudClient pure-logic paths that run BEFORE any
// network I/O: requestUploadUrls per-file validation, friendlyFeedbackError
// branch mapping, submitFeedback validation gates, and normalizeFeedbackType.
//
// All cases avoid the network — descriptors/submissions are crafted to fail
// pre-flight validation, and friendlyFeedbackError/normalizeFeedbackType are
// pure static helpers. The single QApplication is owned by test_main.cpp.

#include <gtest/gtest.h>

#include <QString>
#include <QJsonObject>

#include "QtMeshCloudClient.h"

using Client = QtMeshCloudClient;

// ---------------------------------------------------------------------------
// requestUploadUrls — per-file validation loop (pre-network)
// ---------------------------------------------------------------------------

TEST(QtMeshCloudClientPureCoverageTest, RequestUploadUrlsZeroSizeFile)
{
    Client::AssetFileDescriptor desc;
    desc.path = QStringLiteral("/nonexistent/foo.obj");
    desc.sizeBytes = -1; // forces QFileInfo::size() on a missing file -> 0

    const Client::UploadUrlsResult res = Client::requestUploadUrls(
        QStringLiteral("token-abc"),
        QStringLiteral("owner"),
        QStringLiteral("project"),
        {desc});

    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.errorString.contains(
        QStringLiteral("file size must be greater than zero"), Qt::CaseInsensitive));
    // The pathLeaf basename must be embedded in the error.
    EXPECT_TRUE(res.errorString.contains(QStringLiteral("foo.obj"), Qt::CaseInsensitive));
    // No socket I/O reached -> httpStatus stays at default 0.
    EXPECT_EQ(res.httpStatus, 0);
}

TEST(QtMeshCloudClientPureCoverageTest, RequestUploadUrlsExplicitZeroSize)
{
    // An explicit sizeBytes==0 (>=0 so it is used directly) also trips the gate.
    Client::AssetFileDescriptor desc;
    desc.path = QStringLiteral("/tmp/bar.fbx");
    desc.sizeBytes = 0;

    const Client::UploadUrlsResult res = Client::requestUploadUrls(
        QStringLiteral("token-abc"),
        QStringLiteral("owner"),
        QStringLiteral("project"),
        {desc});

    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.errorString.contains(
        QStringLiteral("file size must be greater than zero"), Qt::CaseInsensitive));
    EXPECT_TRUE(res.errorString.contains(QStringLiteral("bar.fbx"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, RequestUploadUrlsMissingSlugsBeforeFileLoop)
{
    // Empty owner/project slug rejects before the file validation loop.
    Client::AssetFileDescriptor desc;
    desc.path = QStringLiteral("/nonexistent/foo.obj");
    desc.sizeBytes = -1;

    const Client::UploadUrlsResult res = Client::requestUploadUrls(
        QStringLiteral("token-abc"),
        QString(),
        QStringLiteral("project"),
        {desc});

    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.errorString.contains(QStringLiteral("owner and project"), Qt::CaseInsensitive));
}

// ---------------------------------------------------------------------------
// friendlyFeedbackError — branch mapping (pure static)
// ---------------------------------------------------------------------------

TEST(QtMeshCloudClientPureCoverageTest, FriendlyErrorInvalidRating)
{
    const QString msg = Client::friendlyFeedbackError(
        400, QStringLiteral("validation_error"), QStringLiteral("Invalid rating value"));
    EXPECT_TRUE(msg.contains(QStringLiteral("rating is not supported"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, FriendlyErrorInvalidRelatedOperation)
{
    const QString msg = Client::friendlyFeedbackError(
        400, QString(), QStringLiteral("Invalid related operation supplied"));
    EXPECT_TRUE(msg.contains(QStringLiteral("related workflow value"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, FriendlyErrorInvalidJson)
{
    const QString msg = Client::friendlyFeedbackError(
        400, QString(), QStringLiteral("Invalid JSON body"));
    EXPECT_TRUE(msg.contains(QStringLiteral("could not be sent"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, FriendlyErrorGeneric400WithFallback)
{
    // 400 with an unmapped, non-empty fallback -> "server could not accept this feedback".
    const QString msg = Client::friendlyFeedbackError(
        400, QString(), QStringLiteral("Some unmapped detail"));
    EXPECT_TRUE(msg.contains(QStringLiteral("server could not accept this feedback"), Qt::CaseInsensitive));
    EXPECT_TRUE(msg.contains(QStringLiteral("Some unmapped detail"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, FriendlyErrorGeneric400EmptyFallback)
{
    const QString msg = Client::friendlyFeedbackError(
        400, QStringLiteral("validation_error"), QString());
    EXPECT_TRUE(msg.contains(QStringLiteral("feedback could not be accepted"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, FriendlyErrorPositiveStatusEmptyFallbackNoCode)
{
    // httpStatus > 0, empty fallback, no recognized code -> HTTP N message.
    const QString msg = Client::friendlyFeedbackError(503, QString(), QString());
    EXPECT_TRUE(msg.contains(QStringLiteral("Could not send feedback (HTTP 503)"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, FriendlyErrorNonPositiveStatusEmptyFallback)
{
    // httpStatus <= 0, empty fallback -> connectivity hint.
    const QString msg = Client::friendlyFeedbackError(0, QString(), QString());
    EXPECT_TRUE(msg.contains(QStringLiteral("Check your connection"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, FriendlyErrorUnmappedStatusFallbackPassthrough)
{
    // Unmapped status with a non-empty fallback returns the fallback verbatim.
    const QString fallback = QStringLiteral("Upstream service exploded");
    const QString msg = Client::friendlyFeedbackError(500, QString(), fallback);
    EXPECT_EQ(msg, fallback);
}

// ---------------------------------------------------------------------------
// submitFeedback — validation gates (no network reached)
// ---------------------------------------------------------------------------

TEST(QtMeshCloudClientPureCoverageTest, SubmitFeedbackUnsupportedType)
{
    Client::FeedbackSubmission sub;
    sub.type = QStringLiteral("not_a_real_type");
    sub.message = QStringLiteral("hello there, this is feedback");

    const Client::FeedbackResult res = Client::submitFeedback(QStringLiteral("token-abc"), sub);

    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.errorString.contains(QStringLiteral("unsupported feedback type"), Qt::CaseInsensitive));
    // userMessage routes through "Invalid feedback type" mapping.
    EXPECT_TRUE(res.userMessage.contains(QStringLiteral("not supported"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, SubmitFeedbackMissingTypeValidationError)
{
    Client::FeedbackSubmission sub;
    sub.type = QStringLiteral("   "); // trims to empty
    sub.message = QStringLiteral("a message");

    const Client::FeedbackResult res = Client::submitFeedback(QStringLiteral("token-abc"), sub);

    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.errorString.contains(QStringLiteral("feedback type is required"), Qt::CaseInsensitive));
    // validation_error 400 with this fallback -> generic server-could-not-accept copy.
    EXPECT_TRUE(res.userMessage.contains(QStringLiteral("server could not accept"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, SubmitFeedbackOversizedMessage)
{
    Client::FeedbackSubmission sub;
    sub.type = QStringLiteral("bug");
    // Non-whitespace so it survives the trim()-empty gate and trips the length gate.
    sub.message = QString(Client::kFeedbackMaxMessageLength + 1, QChar('x'));

    const Client::FeedbackResult res = Client::submitFeedback(QStringLiteral("token-abc"), sub);

    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.errorString.contains(QStringLiteral("maximum length"), Qt::CaseInsensitive));
    // 413 -> "too large" user message.
    EXPECT_TRUE(res.userMessage.contains(QStringLiteral("too large"), Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientPureCoverageTest, SubmitFeedbackMissingTokenUnauthorized)
{
    Client::FeedbackSubmission sub;
    sub.type = QStringLiteral("bug");
    sub.message = QStringLiteral("hi");

    const Client::FeedbackResult res = Client::submitFeedback(QString(), sub);

    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.errorString.contains(QStringLiteral("missing bearer token"), Qt::CaseInsensitive));
    EXPECT_TRUE(res.userMessage.contains(QStringLiteral("session expired"), Qt::CaseInsensitive));
}

// ---------------------------------------------------------------------------
// normalizeFeedbackType — pure static
// ---------------------------------------------------------------------------

TEST(QtMeshCloudClientPureCoverageTest, NormalizeFeedbackTypeLegacyFeature)
{
    EXPECT_EQ(Client::normalizeFeedbackType(QStringLiteral("feature")),
              QStringLiteral("feature_request"));
}

TEST(QtMeshCloudClientPureCoverageTest, NormalizeFeedbackTypeTrimsWhitespace)
{
    EXPECT_EQ(Client::normalizeFeedbackType(QStringLiteral("  bug  ")),
              QStringLiteral("bug"));
    // Whitespace-padded legacy value still normalizes to feature_request.
    EXPECT_EQ(Client::normalizeFeedbackType(QStringLiteral("  feature ")),
              QStringLiteral("feature_request"));
    // Already-canonical value passes through unchanged.
    EXPECT_EQ(Client::normalizeFeedbackType(QStringLiteral("general")),
              QStringLiteral("general"));
}
