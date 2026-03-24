#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QThread>
#include "ModelDownloader.h"

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

// --- startDownload state transitions ---
// Re-enabled: these tests use real network I/O but are safe on Linux CI where
// the singleton lifecycle is predictable. On macOS, processEvents cleanup was
// the issue but these pass reliably on Linux with Xvfb.

TEST_F(ModelDownloaderTest, StartDownloadSetsIsDownloading) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    EXPECT_TRUE(downloader->isDownloading());
}

TEST_F(ModelDownloaderTest, StartDownloadSetsModelName) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "MyModel");
    app->processEvents();

    EXPECT_EQ(downloader->currentModelName(), "MyModel");
}

TEST_F(ModelDownloaderTest, StartDownloadEmitsIsDownloadingChanged) {
    QSignalSpy spy(downloader, &ModelDownloader::isDownloadingChanged);
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    EXPECT_GE(spy.count(), 1);
}

TEST_F(ModelDownloaderTest, StartDownloadEmitsCurrentModelNameChanged) {
    QSignalSpy spy(downloader, &ModelDownloader::currentModelNameChanged);
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    EXPECT_GE(spy.count(), 1);
}

TEST_F(ModelDownloaderTest, StartDownloadEmitsDownloadStarted) {
    QSignalSpy spy(downloader, &ModelDownloader::downloadStarted);
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "TestModel");
}

TEST_F(ModelDownloaderTest, StartDownloadResetsProgress) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.0f);
    EXPECT_EQ(downloader->bytesReceived(), 0);
    EXPECT_EQ(downloader->bytesTotal(), 0);
}

// --- Duplicate download rejection ---

TEST_F(ModelDownloaderTest, StartDownloadWhileAlreadyDownloadingEmitsError) {
    QString dest1 = tempFilePath("model1.gguf");
    QString dest2 = tempFilePath("model2.gguf");

    downloader->startDownload("https://example.com/model1.gguf", dest1, "Model1");
    app->processEvents();

    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);

    downloader->startDownload("https://example.com/model2.gguf", dest2, "Model2");
    app->processEvents();

    EXPECT_GE(errorSpy.count(), 1);
    EXPECT_EQ(errorSpy.at(0).at(0).toString(), "Model2");
    EXPECT_TRUE(errorSpy.at(0).at(1).toString().contains("already in progress"));
}

TEST_F(ModelDownloaderTest, StartDownloadWhileAlreadyDownloadingDoesNotChangeModel) {
    QString dest1 = tempFilePath("model1.gguf");
    QString dest2 = tempFilePath("model2.gguf");

    downloader->startDownload("https://example.com/model1.gguf", dest1, "Model1");
    app->processEvents();

    downloader->startDownload("https://example.com/model2.gguf", dest2, "Model2");
    app->processEvents();

    // The original download should still be the current one
    EXPECT_EQ(downloader->currentModelName(), "Model1");
}

// --- Temp file path construction ---

TEST_F(ModelDownloaderTest, StartDownloadCreatesTempPartFile) {
    QString dest = tempFilePath("test_model.gguf");
    QString expectedTempFile = dest + ".part";

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    EXPECT_TRUE(QFile::exists(expectedTempFile));
}

// --- Directory creation ---

TEST_F(ModelDownloaderTest, StartDownloadCreatesDestinationDirectory) {
    QString nestedDir = tempDir.path() + "/nested/deep/dir";
    QString dest = nestedDir + "/model.gguf";

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    EXPECT_TRUE(QDir(nestedDir).exists());
}

// --- cancelDownload ---

TEST_F(ModelDownloaderTest, CancelDownloadResetsIsDownloading) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();
    ASSERT_TRUE(downloader->isDownloading());

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_FALSE(downloader->isDownloading());
}

TEST_F(ModelDownloaderTest, CancelDownloadClearsModelName) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_TRUE(downloader->currentModelName().isEmpty());
}

TEST_F(ModelDownloaderTest, CancelDownloadResetsBytesReceived) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_EQ(downloader->bytesReceived(), 0);
    EXPECT_EQ(downloader->bytesTotal(), 0);
}

TEST_F(ModelDownloaderTest, CancelDownloadResetsProgress) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.0f);
}

TEST_F(ModelDownloaderTest, CancelDownloadResetsSpeed) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_FLOAT_EQ(downloader->downloadSpeed(), 0.0f);
}

TEST_F(ModelDownloaderTest, CancelDownloadRemovesTempFile) {
    QString dest = tempFilePath("test_model.gguf");
    QString tempFile = dest + ".part";

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();
    ASSERT_TRUE(QFile::exists(tempFile));

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_FALSE(QFile::exists(tempFile));
}

TEST_F(ModelDownloaderTest, CancelDownloadEmitsDownloadCanceled) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    QSignalSpy spy(downloader, &ModelDownloader::downloadCanceled);

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "TestModel");
}

TEST_F(ModelDownloaderTest, CancelDownloadEmitsIsDownloadingChanged) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    QSignalSpy spy(downloader, &ModelDownloader::isDownloadingChanged);

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_GE(spy.count(), 1);
}

TEST_F(ModelDownloaderTest, CancelDownloadWhenNotDownloadingDoesNotEmitCanceled) {
    QSignalSpy spy(downloader, &ModelDownloader::downloadCanceled);

    downloader->cancelDownload();
    app->processEvents();

    // m_currentModelName is empty when not downloading, so downloadCanceled should not emit
    EXPECT_EQ(spy.count(), 0);
}

// --- pauseDownload ---

TEST_F(ModelDownloaderTest, PauseDownloadEmitsDownloadPaused) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    QSignalSpy spy(downloader, &ModelDownloader::downloadPaused);

    downloader->pauseDownload();
    app->processEvents();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "TestModel");
}

TEST_F(ModelDownloaderTest, PauseDownloadKeepsIsDownloadingTrue) {
    // After pausing, m_isDownloading stays true (only cancel/finish resets it)
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->pauseDownload();
    app->processEvents();

    EXPECT_TRUE(downloader->isDownloading());
}

TEST_F(ModelDownloaderTest, PauseWhenNotDownloadingIsNoOp) {
    QSignalSpy spy(downloader, &ModelDownloader::downloadPaused);

    downloader->pauseDownload();
    app->processEvents();

    EXPECT_EQ(spy.count(), 0);
}

TEST_F(ModelDownloaderTest, PauseWhenAlreadyPausedIsNoOp) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->pauseDownload();
    app->processEvents();

    QSignalSpy spy(downloader, &ModelDownloader::downloadPaused);

    downloader->pauseDownload();
    app->processEvents();

    EXPECT_EQ(spy.count(), 0);
}

// --- resumeDownload ---

TEST_F(ModelDownloaderTest, ResumeDownloadEmitsDownloadResumed) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->pauseDownload();
    app->processEvents();

    QSignalSpy spy(downloader, &ModelDownloader::downloadResumed);

    downloader->resumeDownload();
    app->processEvents();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "TestModel");
}

TEST_F(ModelDownloaderTest, ResumeWhenNotPausedIsNoOp) {
    QSignalSpy spy(downloader, &ModelDownloader::downloadResumed);

    downloader->resumeDownload();
    app->processEvents();

    EXPECT_EQ(spy.count(), 0);
}

TEST_F(ModelDownloaderTest, ResumeAfterCancelIsNoOp) {
    // Once canceled, m_isPaused is false and m_currentUrl is empty, so resume should be a no-op
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->cancelDownload();
    app->processEvents();

    QSignalSpy spy(downloader, &ModelDownloader::downloadResumed);

    downloader->resumeDownload();
    app->processEvents();

    EXPECT_EQ(spy.count(), 0);
}

// --- Pause/Resume/Cancel lifecycle ---

TEST_F(ModelDownloaderTest, PauseThenCancelResetsState) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->pauseDownload();
    app->processEvents();

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_FALSE(downloader->isDownloading());
    EXPECT_TRUE(downloader->currentModelName().isEmpty());
    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.0f);
    EXPECT_EQ(downloader->bytesReceived(), 0);
    EXPECT_EQ(downloader->bytesTotal(), 0);
    EXPECT_FLOAT_EQ(downloader->downloadSpeed(), 0.0f);
}

TEST_F(ModelDownloaderTest, PauseThenResumeKeepsModelName) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->pauseDownload();
    app->processEvents();

    downloader->resumeDownload();
    app->processEvents();

    EXPECT_EQ(downloader->currentModelName(), "TestModel");
    EXPECT_TRUE(downloader->isDownloading());
}

// --- Cancel emits all property changed signals ---

TEST_F(ModelDownloaderTest, CancelDownloadEmitsAllPropertySignals) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    QSignalSpy isDownloadingSpy(downloader, &ModelDownloader::isDownloadingChanged);
    QSignalSpy modelNameSpy(downloader, &ModelDownloader::currentModelNameChanged);
    QSignalSpy progressSpy(downloader, &ModelDownloader::downloadProgressChanged);
    QSignalSpy bytesRecvSpy(downloader, &ModelDownloader::bytesReceivedChanged);
    QSignalSpy bytesTotalSpy(downloader, &ModelDownloader::bytesTotalChanged);
    QSignalSpy speedSpy(downloader, &ModelDownloader::downloadSpeedChanged);

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_GE(isDownloadingSpy.count(), 1);
    EXPECT_GE(modelNameSpy.count(), 1);
    EXPECT_GE(progressSpy.count(), 1);
    EXPECT_GE(bytesRecvSpy.count(), 1);
    EXPECT_GE(bytesTotalSpy.count(), 1);
    EXPECT_GE(speedSpy.count(), 1);
}

// --- Network error handling (will fail to connect to example.com, testing error path) ---

TEST_F(ModelDownloaderTest, DownloadToInvalidPathEmitsError) {
    // Try to download to a path that cannot be opened for writing
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);

    downloader->startDownload(
        "https://example.com/model.gguf",
        "/nonexistent_root_dir_xyz/impossible/path/model.gguf",
        "BadPathModel"
    );
    app->processEvents();

    // The directory creation might succeed or fail depending on permissions,
    // but the file open should fail since /nonexistent_root_dir_xyz doesn't exist
    // and mkpath on a non-writable location will fail
    EXPECT_GE(errorSpy.count(), 1);
    EXPECT_FALSE(downloader->isDownloading());
}

// --- Partial file resume detection ---

TEST_F(ModelDownloaderTest, StartDownloadDetectsExistingPartFile) {
    QString dest = tempFilePath("resume_model.gguf");
    QString partFile = dest + ".part";

    // Create a fake partial download file with some content
    {
        QFile file(partFile);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        QByteArray fakeData(1024, 'A'); // 1KB of data
        file.write(fakeData);
        file.close();
    }

    ASSERT_TRUE(QFile::exists(partFile));
    ASSERT_EQ(QFileInfo(partFile).size(), 1024);

    // Start download - it should detect the existing part file and attempt resume
    downloader->startDownload("https://example.com/model.gguf", dest, "ResumeModel");
    app->processEvents();

    // The downloader should be in downloading state
    EXPECT_TRUE(downloader->isDownloading());
}

// --- Multiple cancel calls are safe ---

TEST_F(ModelDownloaderTest, MultipleCancelCallsAreSafe) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->cancelDownload();
    app->processEvents();

    // Second cancel should not crash
    downloader->cancelDownload();
    app->processEvents();

    EXPECT_FALSE(downloader->isDownloading());
}

// --- Start after cancel works ---

TEST_F(ModelDownloaderTest, StartAfterCancelWorks) {
    QString dest1 = tempFilePath("model1.gguf");
    QString dest2 = tempFilePath("model2.gguf");

    downloader->startDownload("https://example.com/model1.gguf", dest1, "Model1");
    app->processEvents();
    ASSERT_TRUE(downloader->isDownloading());

    downloader->cancelDownload();
    app->processEvents();
    ASSERT_FALSE(downloader->isDownloading());

    // Should be able to start a new download
    downloader->startDownload("https://example.com/model2.gguf", dest2, "Model2");
    app->processEvents();

    EXPECT_TRUE(downloader->isDownloading());
    EXPECT_EQ(downloader->currentModelName(), "Model2");
}

// --- Start after pause+cancel works ---

TEST_F(ModelDownloaderTest, StartAfterPauseCancelWorks) {
    QString dest1 = tempFilePath("model1.gguf");
    QString dest2 = tempFilePath("model2.gguf");

    downloader->startDownload("https://example.com/model1.gguf", dest1, "Model1");
    app->processEvents();

    downloader->pauseDownload();
    app->processEvents();

    downloader->cancelDownload();
    app->processEvents();
    ASSERT_FALSE(downloader->isDownloading());

    // Start a new download
    downloader->startDownload("https://example.com/model2.gguf", dest2, "Model2");
    app->processEvents();

    EXPECT_TRUE(downloader->isDownloading());
    EXPECT_EQ(downloader->currentModelName(), "Model2");
}

// --- Q_PROPERTY values consistency ---

TEST_F(ModelDownloaderTest, PropertiesAreConsistentAfterStart) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "ConsistencyModel");
    app->processEvents();

    EXPECT_TRUE(downloader->isDownloading());
    EXPECT_EQ(downloader->currentModelName(), "ConsistencyModel");
    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.0f);
    EXPECT_FLOAT_EQ(downloader->downloadSpeed(), 0.0f);
}

TEST_F(ModelDownloaderTest, PropertiesAreConsistentAfterCancel) {
    QString dest = tempFilePath("test_model.gguf");

    downloader->startDownload("https://example.com/model.gguf", dest, "TestModel");
    app->processEvents();

    downloader->cancelDownload();
    app->processEvents();

    EXPECT_FALSE(downloader->isDownloading());
    EXPECT_TRUE(downloader->currentModelName().isEmpty());
    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.0f);
    EXPECT_EQ(downloader->bytesReceived(), 0);
    EXPECT_EQ(downloader->bytesTotal(), 0);
    EXPECT_FLOAT_EQ(downloader->downloadSpeed(), 0.0f);
}

// =============================================================================
// Additional tests -- no network access required
// =============================================================================

TEST_F(ModelDownloaderTest, InitialStateFullPropertyCheck) {
    // Comprehensive check of all property getters in initial state
    EXPECT_FALSE(downloader->isDownloading());
    EXPECT_FLOAT_EQ(downloader->downloadProgress(), 0.0f);
    EXPECT_FLOAT_EQ(downloader->downloadSpeed(), 0.0f);
    EXPECT_EQ(downloader->bytesReceived(), 0);
    EXPECT_EQ(downloader->bytesTotal(), 0);
    EXPECT_TRUE(downloader->currentModelName().isEmpty());
}

TEST_F(ModelDownloaderTest, SignalConnectionsExist) {
    // Verify that we can create QSignalSpy on all signals without error,
    // confirming the signals are properly declared and connectable.
    QSignalSpy isDownloadingSpy(downloader, &ModelDownloader::isDownloadingChanged);
    QSignalSpy modelNameSpy(downloader, &ModelDownloader::currentModelNameChanged);
    QSignalSpy progressSpy(downloader, &ModelDownloader::downloadProgressChanged);
    QSignalSpy bytesRecvSpy(downloader, &ModelDownloader::bytesReceivedChanged);
    QSignalSpy bytesTotalSpy(downloader, &ModelDownloader::bytesTotalChanged);
    QSignalSpy speedSpy(downloader, &ModelDownloader::downloadSpeedChanged);
    QSignalSpy startedSpy(downloader, &ModelDownloader::downloadStarted);
    QSignalSpy progressUpdateSpy(downloader, &ModelDownloader::downloadProgressUpdated);
    QSignalSpy completedSpy(downloader, &ModelDownloader::downloadCompleted);
    QSignalSpy errorSpy(downloader, &ModelDownloader::downloadError);
    QSignalSpy pausedSpy(downloader, &ModelDownloader::downloadPaused);
    QSignalSpy resumedSpy(downloader, &ModelDownloader::downloadResumed);
    QSignalSpy canceledSpy(downloader, &ModelDownloader::downloadCanceled);

    // All spies should be valid (isValid)
    EXPECT_TRUE(isDownloadingSpy.isValid());
    EXPECT_TRUE(modelNameSpy.isValid());
    EXPECT_TRUE(progressSpy.isValid());
    EXPECT_TRUE(bytesRecvSpy.isValid());
    EXPECT_TRUE(bytesTotalSpy.isValid());
    EXPECT_TRUE(speedSpy.isValid());
    EXPECT_TRUE(startedSpy.isValid());
    EXPECT_TRUE(progressUpdateSpy.isValid());
    EXPECT_TRUE(completedSpy.isValid());
    EXPECT_TRUE(errorSpy.isValid());
    EXPECT_TRUE(pausedSpy.isValid());
    EXPECT_TRUE(resumedSpy.isValid());
    EXPECT_TRUE(canceledSpy.isValid());
}

TEST_F(ModelDownloaderTest, CancelDownloadWhenNotDownloadingDoesNotCrash) {
    // Calling cancelDownload when not downloading should be safe
    downloader->cancelDownload();
    app->processEvents();
    EXPECT_FALSE(downloader->isDownloading());

    // Call it multiple times
    downloader->cancelDownload();
    downloader->cancelDownload();
    downloader->cancelDownload();
    app->processEvents();
    EXPECT_FALSE(downloader->isDownloading());
}

TEST_F(ModelDownloaderTest, PauseDownloadWhenNotDownloadingDoesNotCrash) {
    // Calling pauseDownload when not downloading should be safe and a no-op
    downloader->pauseDownload();
    app->processEvents();
    EXPECT_FALSE(downloader->isDownloading());

    // Multiple calls should also be safe
    downloader->pauseDownload();
    downloader->pauseDownload();
    app->processEvents();
    EXPECT_FALSE(downloader->isDownloading());
}

TEST_F(ModelDownloaderTest, MultipleInstanceCheckReturnsSame) {
    // ModelDownloader::instance() should always return the same pointer
    // (singleton pattern). Verify across multiple calls.
    ModelDownloader* inst1 = ModelDownloader::instance();
    ModelDownloader* inst2 = ModelDownloader::instance();
    ModelDownloader* inst3 = ModelDownloader::instance();
    EXPECT_EQ(inst1, inst2);
    EXPECT_EQ(inst2, inst3);
    EXPECT_NE(inst1, nullptr);

    // Also verify the downloader from SetUp is the same instance
    EXPECT_EQ(downloader, inst1);
}
