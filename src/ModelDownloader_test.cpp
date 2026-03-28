#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
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
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setUrl(QUrl("https://example.invalid/model.bin"));
        if (errorCode != QNetworkReply::NoError)
            setError(errorCode, errorText);
        setFinished(errorCode == QNetworkReply::NoError);
    }

    void abort() override {}

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
