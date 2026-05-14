#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QThread>

#include "AppConsoleLog.h"

class AppConsoleLogTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()), nullptr);
    }
};

TEST_F(AppConsoleLogTest, InstallIsIdempotent)
{
    AppConsoleLog::install();
    AppConsoleLog::install();    // second call must be a no-op
    AppConsoleLog::install();
    // Reaching here without crashing or recursing is enough.
    SUCCEED();
}

TEST_F(AppConsoleLogTest, DetachNullDoesNothing)
{
    AppConsoleLog::detachMainWindow(nullptr);
    SUCCEED();
}

TEST_F(AppConsoleLogTest, AttachNullIsIgnored)
{
    // Per the contract, attach with a null window must early-out.
    AppConsoleLog::attachMainWindow(nullptr);
    SUCCEED();
}

TEST_F(AppConsoleLogTest, FlushStdioChunksSafeWhenCaptureNotActive)
{
    // flushStdioChunks() should be a no-op when capture isn't running.
    AppConsoleLog::flushStdioChunks();
    SUCCEED();
}

TEST_F(AppConsoleLogTest, MessageHandlerSurvivesAllSeverityLevels)
{
    AppConsoleLog::install();
    // Detach any previous attachment so messages go to the in-process buffer.
    AppConsoleLog::detachMainWindow(nullptr);
    qDebug()    << "test debug";
    qInfo()     << "test info";
    qWarning()  << "test warning";
    QCoreApplication::processEvents();
    SUCCEED();
}

#ifndef Q_OS_WIN
TEST_F(AppConsoleLogTest, StdioCaptureInstallAndShutdownRoundTrip)
{
    // Stdio capture only makes sense in GUI builds; the function is built
    // on POSIX dup2/pipe so we exercise it here to cover both the install
    // and shutdown paths.
    const bool ok = AppConsoleLog::installStdioCapture();
    ASSERT_TRUE(ok);

    // Write something through C stdio so the reader thread sees data.
    std::fputs("hello-from-stdout\n", stdout);
    std::fflush(stdout);
    std::fputs("hello-from-stderr\n", stderr);
    std::fflush(stderr);

    // Give the reader thread time to drain the pipe.
    QThread::msleep(50);

    AppConsoleLog::shutdown();
    SUCCEED();
}
#endif
