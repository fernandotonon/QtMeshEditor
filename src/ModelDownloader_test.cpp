#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QThread>
#include <QTimer>
#include <cstring>
#define private public
#include "ModelDownloader.h"
#undef private

class FakeNetworkReply : public QNetworkReply {
public:
    explicit FakeNetworkReply(const QByteArray& payload = QByteArray(),
                              QNetworkReply::NetworkError errorCode = QNetworkReply::NoError,
                              const QString& errorText = QString(),
                              QObject* parent = nullptr)
        : QNetworkReply(parent), m_payload(payload)
    {
        // #1036: default to a plain 200 with no Content-Range — exactly what a
        // server that ignores Range returns. Tests that model an honoured
        // resume call withPartialContent() explicitly.
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setUrl(QUrl("https://example.invalid/model.bin"));
        if (errorCode != QNetworkReply::NoError)
            setError(errorCode, errorText);
        setFinished(errorCode == QNetworkReply::NoError);
    }

    void abort() override {}

    /// #1036: model a server that honoured `Range: bytes=<first>-`.
    FakeNetworkReply* withPartialContent(qint64 first, qint64 last, qint64 total)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 206);
        setRawHeader("Content-Range",
                     QByteArray("bytes ") + QByteArray::number(first) + '-'
                     + QByteArray::number(last) + '/' + QByteArray::number(total));
        return this;
    }

    qint64 bytesAvailable() const override
    {
        return m_payload.size() - m_offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if (m_offset >= m_payload.size())
            return -1;

        const qint64 bytesToRead = qMin(maxSize, m_payload.size() - m_offset);
        memcpy(data, m_payload.constData() + m_offset, static_cast<size_t>(bytesToRead));
        m_offset += bytesToRead;
        return bytesToRead;
    }

private:
    QByteArray m_payload;
    qint64 m_offset = 0;
};

class ModelDownloaderTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    ModelDownloader* downloader = nullptr;
    QTemporaryDir tempDir;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        downloader = ModelDownloader::instance();
        ASSERT_NE(downloader, nullptr);

        // Ensure clean state: cancel any lingering download
        downloader->cancelDownload();
        app->processEvents();
        // #1029: the digest is set by startDownload, which these tests bypass
        // when they drive onDownloadFinished directly — so a stale value from
        // a prior test would otherwise leak in under shuffled ordering.
        downloader->m_expectedSha256.clear();
    }

    void TearDown() override {
        // Cancel any in-progress download to leave clean state
        if (downloader) {
            downloader->cancelDownload();
            app->processEvents();
            // Process events again to ensure all deferred deletions complete
            QThread::msleep(10);
            app->processEvents();
        }
    }

    QString tempFilePath(const QString &filename) {
        return tempDir.path() + "/" + filename;
    }
};

// --- Singleton ---

TEST_F(ModelDownloaderTest, SingletonReturnsSameInstance) {
    ModelDownloader* instance1 = ModelDownloader::instance();
    ModelDownloader* instance2 = ModelDownloader::instance();
    EXPECT_EQ(instance1, instance2);
}

TEST_F(ModelDownloaderTest, SingletonIsNotNull) {
    EXPECT_NE(ModelDownloader::instance(), nullptr);
}

// --- Initial / Default State ---

TEST_F(ModelDownloaderTest, InitialStateIsNotDownloading) {
    EXPECT_FALSE(downloader->isDownloading());
}

TEST_F(ModelDownloaderTest, InitialProgressIsZero) {
    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.0f);
}

TEST_F(ModelDownloaderTest, InitialBytesReceivedIsZero) {
    EXPECT_EQ(downloader->bytesReceived(), 0);
}

TEST_F(ModelDownloaderTest, InitialBytesTotalIsZero) {
    EXPECT_EQ(downloader->bytesTotal(), 0);
}

TEST_F(ModelDownloaderTest, InitialDownloadSpeedIsZero) {
    EXPECT_FLOAT_EQ(downloader->downloadSpeed(), 0.0f);
}

TEST_F(ModelDownloaderTest, InitialModelNameIsEmpty) {
    EXPECT_TRUE(downloader->currentModelName().isEmpty());
}

TEST_F(ModelDownloaderTest, QmlInstanceReturnsSingleton)
{
    EXPECT_EQ(ModelDownloader::qmlInstance(nullptr, nullptr), downloader);
}

TEST_F(ModelDownloaderTest, StartDownloadWhileAlreadyDownloadingEmitsError)
{
    downloader->m_isDownloading = true;
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);

    downloader->startDownload("https://example.invalid/model.bin", tempFilePath("model.bin"), "TestModel");

    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_EQ(errorSpy.at(0).at(0).toString(), QString("TestModel"));
    EXPECT_TRUE(errorSpy.at(0).at(1).toString().contains("already in progress"));
}

TEST_F(ModelDownloaderTest, StartDownloadFileOpenFailureEmitsErrorAndKeepsIdleState)
{
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    const QString invalidParentPath = tempFilePath("not-a-directory");
    QFile invalidParent(invalidParentPath);
    ASSERT_TRUE(invalidParent.open(QIODevice::WriteOnly));
    invalidParent.write("x");
    invalidParent.close();

    downloader->startDownload(
        "https://example.invalid/model.bin",
        invalidParentPath + "/model.bin",
        "BrokenModel");

    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_EQ(errorSpy.at(0).at(0).toString(), QString("BrokenModel"));
    EXPECT_TRUE(errorSpy.at(0).at(1).toString().contains("Cannot open file for writing"));
    EXPECT_FALSE(downloader->isDownloading());
    EXPECT_EQ(downloader->currentModelName(), QString("BrokenModel"));
}

TEST_F(ModelDownloaderTest, PauseDownloadWithoutActiveReplyDoesNothing)
{
    QSignalSpy pausedSpy(downloader, &ModelDownloader::downloadPaused);
    downloader->pauseDownload();
    EXPECT_EQ(pausedSpy.count(), 0);
}

TEST_F(ModelDownloaderTest, ResumeDownloadWithoutPausedStateDoesNothing)
{
    QSignalSpy resumedSpy(downloader, &ModelDownloader::downloadResumed);
    downloader->resumeDownload();
    EXPECT_EQ(resumedSpy.count(), 0);
}

TEST_F(ModelDownloaderTest, CancelDownloadResetsStateAndRemovesPartialFile)
{
    const QString partialPath = tempFilePath("partial-model.bin.part");
    QFile seedFile(partialPath);
    ASSERT_TRUE(seedFile.open(QIODevice::WriteOnly));
    seedFile.write("partial");
    seedFile.close();

    downloader->m_isDownloading = true;
    downloader->m_isPaused = true;
    downloader->m_currentUrl = "https://example.invalid/model.bin";
    downloader->m_currentDestinationPath = tempFilePath("final-model.bin");
    downloader->m_currentModelName = "CancelModel";
    downloader->m_tempFilePath = partialPath;
    downloader->m_bytesReceived = 42;
    downloader->m_bytesTotal = 100;
    downloader->m_progress = 0.42f;
    downloader->m_downloadSpeed = 256.0f;
    downloader->m_resumeOffset = 10;
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::Append));
    downloader->m_currentReply = new FakeNetworkReply({}, QNetworkReply::NoError, {}, downloader);

    QSignalSpy canceledSpy(downloader, &ModelDownloader::downloadCanceled);
    downloader->cancelDownload();

    ASSERT_EQ(canceledSpy.count(), 1);
    EXPECT_EQ(canceledSpy.at(0).at(0).toString(), QString("CancelModel"));
    EXPECT_FALSE(downloader->isDownloading());
    EXPECT_TRUE(downloader->currentModelName().isEmpty());
    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.0f);
    EXPECT_EQ(downloader->bytesReceived(), 0);
    EXPECT_EQ(downloader->bytesTotal(), 0);
    EXPECT_FLOAT_EQ(downloader->downloadSpeed(), 0.0f);
    EXPECT_FALSE(QFileInfo::exists(partialPath));
}

TEST_F(ModelDownloaderTest, OnReadyReadWritesReplyDataToOutputFile)
{
    const QString partialPath = tempFilePath("ready-read.bin");
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::WriteOnly));
    downloader->m_currentReply = new FakeNetworkReply("hello world", QNetworkReply::NoError, {}, downloader);

    downloader->onReadyRead();
    downloader->m_outputFile->flush();

    QFile written(partialPath);
    ASSERT_TRUE(written.open(QIODevice::ReadOnly));
    EXPECT_EQ(written.readAll(), QByteArray("hello world"));
}

TEST_F(ModelDownloaderTest, OnDownloadProgressUsesResumeOffset)
{
    downloader->m_currentModelName = "ResumeModel";
    downloader->m_resumeOffset = 100;

    QSignalSpy progressSpy(downloader, &ModelDownloader::downloadProgressUpdated);
    downloader->onDownloadProgress(50, 100);

    EXPECT_EQ(downloader->bytesReceived(), 150);
    EXPECT_EQ(downloader->bytesTotal(), 200);
    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.75f);
    ASSERT_EQ(progressSpy.count(), 1);
    EXPECT_EQ(progressSpy.at(0).at(0).toString(), QString("ResumeModel"));
    EXPECT_EQ(progressSpy.at(0).at(1).toLongLong(), 150);
    EXPECT_EQ(progressSpy.at(0).at(2).toLongLong(), 200);
}

TEST_F(ModelDownloaderTest, OnDownloadProgressWithoutResumeUsesRawTotals)
{
    downloader->m_resumeOffset = 0;
    downloader->onDownloadProgress(25, 100);

    EXPECT_EQ(downloader->bytesReceived(), 25);
    EXPECT_EQ(downloader->bytesTotal(), 100);
    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.25f);
}

TEST_F(ModelDownloaderTest, OnSpeedTimerTimeoutTracksBytesPerSecond)
{
    downloader->m_bytesReceived = 4096;
    downloader->m_lastBytesReceived = 1024;

    QSignalSpy speedSpy(downloader, &ModelDownloader::downloadSpeedChanged);
    downloader->onSpeedTimerTimeout();

    EXPECT_FLOAT_EQ(downloader->downloadSpeed(), 3072.0f);
    EXPECT_EQ(downloader->m_lastBytesReceived, 4096);
    EXPECT_EQ(speedSpy.count(), 1);
}

TEST_F(ModelDownloaderTest, OnDownloadErrorIgnoresCanceledReplyWhilePaused)
{
    downloader->m_isPaused = true;
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);

    downloader->onDownloadError(QNetworkReply::OperationCanceledError);

    EXPECT_EQ(errorSpy.count(), 0);
}

TEST_F(ModelDownloaderTest, OnDownloadErrorEmitsErrorAndMovesToPausedState)
{
    downloader->m_currentModelName = "FailedModel";
    downloader->m_outputFile = new QFile(tempFilePath("error.bin.part"), downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::WriteOnly));
    downloader->m_currentReply = new FakeNetworkReply({}, QNetworkReply::ConnectionRefusedError,
                                                      "connection refused", downloader);

    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    downloader->onDownloadError(QNetworkReply::ConnectionRefusedError);

    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_EQ(errorSpy.at(0).at(0).toString(), QString("FailedModel"));
    EXPECT_TRUE(errorSpy.at(0).at(1).toString().contains("connection refused"));
    EXPECT_TRUE(downloader->m_isPaused);
    EXPECT_FLOAT_EQ(downloader->downloadSpeed(), 0.0f);
    EXPECT_EQ(downloader->m_currentReply, nullptr);
}

TEST_F(ModelDownloaderTest, OnDownloadFinishedRenamesTempFileAndEmitsCompleted)
{
    const QString partialPath = tempFilePath("complete.bin.part");
    const QString finalPath = tempFilePath("complete.bin");

    downloader->m_isDownloading = true;
    downloader->m_currentModelName = "CompletedModel";
    downloader->m_currentDestinationPath = finalPath;
    downloader->m_tempFilePath = partialPath;
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::WriteOnly));
    downloader->m_outputFile->write("payload");
    downloader->m_currentReply = new FakeNetworkReply({}, QNetworkReply::NoError, {}, downloader);

    QSignalSpy completedSpy(downloader, &ModelDownloader::downloadCompleted);
    downloader->onDownloadFinished();

    ASSERT_EQ(completedSpy.count(), 1);
    EXPECT_EQ(completedSpy.at(0).at(0).toString(), QString("CompletedModel"));
    EXPECT_EQ(completedSpy.at(0).at(1).toString(), finalPath);
    EXPECT_TRUE(QFileInfo::exists(finalPath));
    EXPECT_FALSE(downloader->isDownloading());
    EXPECT_TRUE(downloader->currentModelName().isEmpty());
}

TEST_F(ModelDownloaderTest, OnDownloadFinishedRenameFailureEmitsError)
{
    const QString partialPath = tempFilePath("rename-fail.bin.part");
    const QString invalidDestination = tempDir.path();

    downloader->m_currentModelName = "RenameFailModel";
    downloader->m_currentDestinationPath = invalidDestination;
    downloader->m_tempFilePath = partialPath;
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::WriteOnly));
    downloader->m_outputFile->write("payload");
    downloader->m_currentReply = new FakeNetworkReply({}, QNetworkReply::NoError, {}, downloader);

    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    downloader->onDownloadFinished();

    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_EQ(errorSpy.at(0).at(0).toString(), QString("RenameFailModel"));
    EXPECT_TRUE(errorSpy.at(0).at(1).toString().contains("Failed to rename downloaded file"));
}


// ---- #1029: transport + integrity (CWE-494) ---------------------------------

TEST_F(ModelDownloaderTest, RejectsPlainHttpBeforeTouchingTheFilesystem)
{
    // Every model base URL is user-overridable (env / QSettings), so a plain
    // http source is a byte-for-byte MITM injection point. Must be refused
    // BEFORE any side effect: no .part, no created directory.
    const QString dest = tempFilePath("nested/dir/http-model.bin");
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    QSignalSpy startedSpy(downloader, &ModelDownloader::downloadStarted);

    downloader->startDownload("http://example.invalid/model.bin", dest, "HttpModel");
    // The rejection is QUEUED (see startDownload) so a consumer's nested
    // QEventLoop receives it; nothing may have fired synchronously.
    EXPECT_EQ(errorSpy.count(), 0) << "must not emit synchronously — that loses the caller's loop.quit()";
    app->processEvents();

    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_EQ(errorSpy.at(0).at(0).toString(), QString("HttpModel"));
    EXPECT_TRUE(errorSpy.at(0).at(1).toString().contains("https://"));
    EXPECT_EQ(startedSpy.count(), 0);
    EXPECT_FALSE(downloader->isDownloading());
    EXPECT_FALSE(QFileInfo::exists(dest + ".part"));
    EXPECT_FALSE(QFileInfo(dest).absoluteDir().exists())
        << "a refused URL must not create the destination directory";
}

TEST_F(ModelDownloaderTest, RejectsSchemelessAndExoticSchemes)
{
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    for (const char* u : {"example.invalid/model.bin", "ftp://x/m.bin", "javascript:1"}) {
        downloader->startDownload(QString::fromLatin1(u), tempFilePath("x.bin"), "Bad");
    }
    app->processEvents();
    EXPECT_EQ(errorSpy.count(), 3);
    EXPECT_FALSE(downloader->isDownloading());
}

TEST_F(ModelDownloaderTest, AllowedUrlPredicateMatchesTheGate)
{
    // The pure predicate is what a consumer checks up front; it must agree
    // with what startDownload actually refuses.
    EXPECT_TRUE(ModelDownloader::isAllowedDownloadUrl("https://huggingface.co/x/y/resolve/main/m.onnx"));
    EXPECT_TRUE(ModelDownloader::isAllowedDownloadUrl("HTTPS://Example.invalid/m.bin"));
    EXPECT_TRUE(ModelDownloader::isAllowedDownloadUrl("file:///tmp/local-mirror/m.onnx"));
    EXPECT_FALSE(ModelDownloader::isAllowedDownloadUrl("http://example.invalid/m.bin"));
    EXPECT_FALSE(ModelDownloader::isAllowedDownloadUrl("ftp://example.invalid/m.bin"));
    EXPECT_FALSE(ModelDownloader::isAllowedDownloadUrl(""));
}

TEST_F(ModelDownloaderTest, Sha256MismatchDiscardsPartialAndNeverRenames)
{
    // The load-bearing property: a file that fails verification must NEVER
    // reach the destination path, where the next launch would trust and load
    // it, and must not linger as a .part the resume logic would append to.
    const QString partialPath = tempFilePath("bad.bin.part");
    const QString finalPath = tempFilePath("bad.bin");

    downloader->m_isDownloading = true;
    downloader->m_currentModelName = "TamperedModel";
    downloader->m_currentDestinationPath = finalPath;
    downloader->m_tempFilePath = partialPath;
    downloader->m_expectedSha256 =
        "0000000000000000000000000000000000000000000000000000000000000000";
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::WriteOnly));
    downloader->m_outputFile->write("payload");
    downloader->m_currentReply = new FakeNetworkReply({}, QNetworkReply::NoError, {}, downloader);

    QSignalSpy completedSpy(downloader, &ModelDownloader::downloadCompleted);
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    downloader->onDownloadFinished();

    EXPECT_EQ(completedSpy.count(), 0) << "must not report success on a bad digest";
    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_EQ(errorSpy.at(0).at(0).toString(), QString("TamperedModel"));
    EXPECT_TRUE(errorSpy.at(0).at(1).toString().contains("SHA-256"));
    EXPECT_FALSE(QFileInfo::exists(finalPath)) << "tampered file reached the destination";
    EXPECT_FALSE(QFileInfo::exists(partialPath)) << "poisoned .part left behind for resume";
    EXPECT_FALSE(downloader->isDownloading());
}

TEST_F(ModelDownloaderTest, Sha256MatchRenamesAndCompletes)
{
    const QString partialPath = tempFilePath("good.bin.part");
    const QString finalPath = tempFilePath("good.bin");
    const QByteArray payload = "payload";
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());

    downloader->m_isDownloading = true;
    downloader->m_currentModelName = "GoodModel";
    downloader->m_currentDestinationPath = finalPath;
    downloader->m_tempFilePath = partialPath;
    // Uppercase on purpose: HF's LFS oid is lowercase, but a hand-typed
    // manifest may not be, and the compare is documented case-insensitive.
    downloader->m_expectedSha256 = digest.toUpper();
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::WriteOnly));
    downloader->m_outputFile->write(payload);
    downloader->m_currentReply = new FakeNetworkReply({}, QNetworkReply::NoError, {}, downloader);

    QSignalSpy completedSpy(downloader, &ModelDownloader::downloadCompleted);
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    downloader->onDownloadFinished();

    EXPECT_EQ(errorSpy.count(), 0);
    ASSERT_EQ(completedSpy.count(), 1);
    EXPECT_EQ(completedSpy.at(0).at(1).toString(), finalPath);
    EXPECT_TRUE(QFileInfo::exists(finalPath));
}

TEST_F(ModelDownloaderTest, EmptyDigestKeepsLegacyBehaviourForExistingConsumers)
{
    // 21 consumers and two QML call sites pass no digest today; they must keep
    // working unchanged (this is the same case as the pre-existing
    // OnDownloadFinishedRenamesTempFileAndEmitsCompleted, pinned explicitly
    // against the new member).
    downloader->m_expectedSha256.clear();
    const QString partialPath = tempFilePath("legacy.bin.part");
    const QString finalPath = tempFilePath("legacy.bin");
    downloader->m_isDownloading = true;
    downloader->m_currentModelName = "LegacyModel";
    downloader->m_currentDestinationPath = finalPath;
    downloader->m_tempFilePath = partialPath;
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::WriteOnly));
    downloader->m_outputFile->write("anything");
    downloader->m_currentReply = new FakeNetworkReply({}, QNetworkReply::NoError, {}, downloader);

    QSignalSpy completedSpy(downloader, &ModelDownloader::downloadCompleted);
    downloader->onDownloadFinished();
    EXPECT_EQ(completedSpy.count(), 1);
    EXPECT_TRUE(QFileInfo::exists(finalPath));
}


TEST_F(ModelDownloaderTest, FileUrlWithRemoteHostIsRefusedAsRemote)
{
    // file://server/share/model.onnx is a UNC path on Windows — an SMB fetch
    // over the network wearing a local scheme. Letting it through would walk
    // straight around the https-only rule, so it is refused BEFORE any
    // filesystem side effect, and the error must say WHY (a message about
    // "scheme" would be nonsense for a file:// URL).
    const QString dest = tempFilePath("unc/model.onnx");
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    QSignalSpy startedSpy(downloader, &ModelDownloader::downloadStarted);

    downloader->startDownload("file://evil-host/share/model.onnx", dest, "UncModel");
    app->processEvents();

    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_TRUE(errorSpy.at(0).at(1).toString().contains("remote"))
        << errorSpy.at(0).at(1).toString().toStdString();
    EXPECT_EQ(startedSpy.count(), 0);
    EXPECT_FALSE(downloader->isDownloading());
    EXPECT_FALSE(QFileInfo::exists(dest + ".part"));
    EXPECT_FALSE(QFileInfo(dest).absoluteDir().exists());
}

TEST_F(ModelDownloaderTest, HostRulesForFileAndHttps)
{
    // file:// only LOCAL (no host, or localhost); https:// must have a host.
    EXPECT_TRUE (ModelDownloader::isAllowedDownloadUrl("file:///tmp/mirror/m.onnx"));
    EXPECT_TRUE (ModelDownloader::isAllowedDownloadUrl("file://localhost/tmp/mirror/m.onnx"));
    EXPECT_TRUE (ModelDownloader::isAllowedDownloadUrl("file://LOCALHOST/tmp/m.onnx"));
    EXPECT_FALSE(ModelDownloader::isAllowedDownloadUrl("file://evil-host/share/m.onnx"));
    EXPECT_FALSE(ModelDownloader::isAllowedDownloadUrl("file://10.0.0.5/share/m.onnx"));
    EXPECT_TRUE (ModelDownloader::isAllowedDownloadUrl("https://huggingface.co/x/resolve/main/m.onnx"));
    EXPECT_FALSE(ModelDownloader::isAllowedDownloadUrl("https:///no-host/m.onnx"));
    EXPECT_FALSE(ModelDownloader::isAllowedDownloadUrl("https://"));
    EXPECT_FALSE(ModelDownloader::isAllowedDownloadUrl("::not a url::"));
}


TEST_F(ModelDownloaderTest, RejectedUrlErrorRedactsCredentialsAndQuery)
{
    // A user-configured override may carry credentials; the refusal text
    // reaches the GUI status line, so secrets must not ride along. The host
    // and path MUST survive, or the user cannot see what they misconfigured.
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    downloader->startDownload("http://alice:s3cretPW@mirror.example.invalid/models/m.bin?token=TOK123#frag",
                              tempFilePath("m.bin"), "LeakModel");
    app->processEvents();

    ASSERT_EQ(errorSpy.count(), 1);
    const QString msg = errorSpy.at(0).at(1).toString();
    EXPECT_FALSE(msg.contains("s3cretPW")) << msg.toStdString();
    EXPECT_FALSE(msg.contains("TOK123"))   << msg.toStdString();
    EXPECT_FALSE(msg.contains("alice"))    << msg.toStdString();
    EXPECT_TRUE (msg.contains("mirror.example.invalid/models/m.bin")) << msg.toStdString();
}


// ---- #1036: a resume must PROVE the server honoured Range -------------------

TEST_F(ModelDownloaderTest, ResumeAgainst200RestartsFromZeroInsteadOfAppending)
{
    // The reproduced corruption: stale .part + a server that ignores Range
    // (200, full body) => full body appended after the stale prefix. The
    // result must be the payload ALONE.
    const QString partialPath = tempFilePath("resume200.bin.part");
    QFile seed(partialPath);
    ASSERT_TRUE(seed.open(QIODevice::WriteOnly));
    seed.write("STALEPREFIX");           // 11 bytes the server never saw
    seed.close();

    downloader->m_currentModelName = "Resume200";
    downloader->m_tempFilePath = partialPath;
    downloader->m_resumeOffset = 11;
    downloader->m_bytesReceived = 11;
    downloader->m_resumeUnverified = true;
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::Append));
    downloader->m_currentReply = new FakeNetworkReply("FULLBODY", QNetworkReply::NoError, {}, downloader);

    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    downloader->onReadyRead();
    downloader->m_outputFile->flush();

    QFile out(partialPath);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_EQ(out.readAll(), QByteArray("FULLBODY"))
        << "stale prefix survived — the full body was appended, not written from 0";
    EXPECT_EQ(downloader->m_resumeOffset, 0) << "progress would add a phantom offset";
    EXPECT_EQ(downloader->m_bytesReceived, 0);
    EXPECT_FALSE(downloader->m_resumeUnverified);
    EXPECT_EQ(errorSpy.count(), 0) << "a 200 is recoverable, not an error";
}

TEST_F(ModelDownloaderTest, ResumeAgainst206WithMatchingRangeAppends)
{
    // The honoured case must be untouched: 206 + Content-Range starting at our
    // offset => append, keep the offset.
    const QString partialPath = tempFilePath("resume206.bin.part");
    QFile seed(partialPath);
    ASSERT_TRUE(seed.open(QIODevice::WriteOnly));
    seed.write("FIRSTPART");             // 9 bytes
    seed.close();

    downloader->m_currentModelName = "Resume206";
    downloader->m_tempFilePath = partialPath;
    downloader->m_resumeOffset = 9;
    downloader->m_bytesReceived = 9;
    downloader->m_resumeUnverified = true;
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::Append));
    auto* reply = new FakeNetworkReply("REST", QNetworkReply::NoError, {}, downloader);
    reply->withPartialContent(9, 12, 13);
    downloader->m_currentReply = reply;

    downloader->onReadyRead();
    downloader->m_outputFile->flush();

    QFile out(partialPath);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_EQ(out.readAll(), QByteArray("FIRSTPARTREST"));
    EXPECT_EQ(downloader->m_resumeOffset, 9) << "an honoured resume must keep its offset";
}

TEST_F(ModelDownloaderTest, ResumeAgainst206WithWrongStartRestartsFromZero)
{
    // 206 but the window does not start at our offset: appending is still
    // wrong (bytes would land in the wrong place). Treat like an ignored Range.
    const QString partialPath = tempFilePath("resume206bad.bin.part");
    QFile seed(partialPath);
    ASSERT_TRUE(seed.open(QIODevice::WriteOnly));
    seed.write("ABCDE");                 // 5 bytes
    seed.close();

    downloader->m_currentModelName = "Resume206Bad";
    downloader->m_tempFilePath = partialPath;
    downloader->m_resumeOffset = 5;
    downloader->m_bytesReceived = 5;
    downloader->m_resumeUnverified = true;
    downloader->m_outputFile = new QFile(partialPath, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::Append));
    auto* reply = new FakeNetworkReply("XYZ", QNetworkReply::NoError, {}, downloader);
    reply->withPartialContent(0, 2, 3);  // server restarted from 0 on its own
    downloader->m_currentReply = reply;

    downloader->onReadyRead();
    downloader->m_outputFile->flush();

    QFile out(partialPath);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_EQ(out.readAll(), QByteArray("XYZ"));
    EXPECT_EQ(downloader->m_resumeOffset, 0);
}

TEST_F(ModelDownloaderTest, FreshDownloadNeverRunsTheResumeCheck)
{
    // No resume => no Range => the 200 is exactly what we asked for. The
    // check must not fire and must not truncate a fresh write.
    const QString path = tempFilePath("fresh.bin.part");
    downloader->m_resumeOffset = 0;
    downloader->m_resumeUnverified = false;
    downloader->m_outputFile = new QFile(path, downloader);
    ASSERT_TRUE(downloader->m_outputFile->open(QIODevice::WriteOnly));
    downloader->m_currentReply = new FakeNetworkReply("hello", QNetworkReply::NoError, {}, downloader);

    downloader->onReadyRead();
    downloader->m_outputFile->flush();
    QFile out(path);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_EQ(out.readAll(), QByteArray("hello"));
}
