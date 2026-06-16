// Coverage tests for AppLaunchHandler instance-level methods:
//   tryForwardToRunningInstance, startSingleInstanceServer,
//   handleIncomingPaths (via socket round-trip), ~AppLaunchHandler,
//   and eventFilter (QFileOpenEvent).
//
// The static-method coverage lives in AppLaunchHandler_test.cpp; this file uses
// distinct suite names (AppLaunchHandlerCoverageTest) to avoid ODR clashes.
//
// NOTE: test_main.cpp owns the single QApplication. We never create another.

#include "AppLaunchHandler.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QLocalServer>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <gtest/gtest.h>

namespace {

// Expose the protected eventFilter for direct invocation so the test does not
// have to depend on which other event filters QCoreApplication has installed.
class TestableHandler : public AppLaunchHandler
{
public:
    using AppLaunchHandler::AppLaunchHandler;
    bool callEventFilter(QObject* watched, QEvent* event)
    {
        return eventFilter(watched, event);
    }
};

// Write a minimal valid OBJ file and return its absolute path.
QString writeTempObj(QTemporaryDir& dir, const QString& name)
{
    const QString path = dir.filePath(name);
    QFile obj(path);
    if (!obj.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    obj.write("o cube\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    obj.close();
    return QFileInfo(path).absoluteFilePath();
}

QString writeTempFile(QTemporaryDir& dir, const QString& name, const QByteArray& bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write(bytes);
    f.close();
    return QFileInfo(path).absoluteFilePath();
}

// Pump the event loop until the spy fires or timeout elapses.
bool waitForSpy(QSignalSpy& spy, int timeoutMs = 3000)
{
    QElapsedTimer t;
    t.start();
    while (spy.isEmpty() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(20);
    }
    return !spy.isEmpty();
}

// ---------------------------------------------------------------------------
// tryForwardToRunningInstance
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, ForwardEmptyPathsReturnsFalse)
{
    AppLaunchHandler handler;
    EXPECT_FALSE(handler.tryForwardToRunningInstance(QStringList{}));
}

TEST(AppLaunchHandlerCoverageTest, ForwardNoServerListeningReturnsFalse)
{
    // Ensure no leftover server socket from a prior run could accept us.
    QLocalServer::removeServer(QLatin1String(AppLaunchHandler::kServerName));

    AppLaunchHandler handler; // intentionally NOT starting the server
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString mesh = writeTempObj(dir, QStringLiteral("nope.obj"));
    ASSERT_FALSE(mesh.isEmpty());

    // waitForConnected(750) should fail since nobody is listening.
    EXPECT_FALSE(handler.tryForwardToRunningInstance(QStringList{mesh}));
}

// ---------------------------------------------------------------------------
// startSingleInstanceServer
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, StartServerListenSucceedsAndIsIdempotent)
{
    QLocalServer::removeServer(QLatin1String(AppLaunchHandler::kServerName));

    AppLaunchHandler handler;
    EXPECT_TRUE(handler.startSingleInstanceServer());
    // Second call short-circuits (m_server already set) and returns true.
    EXPECT_TRUE(handler.startSingleInstanceServer());
}

// ---------------------------------------------------------------------------
// startSingleInstanceServer + handleIncomingPaths end-to-end round-trip
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, RoundTripEmitsFilesRequestedWithFilteredPaths)
{
    QLocalServer::removeServer(QLatin1String(AppLaunchHandler::kServerName));

    AppLaunchHandler server;
    ASSERT_TRUE(server.startSingleInstanceServer());

    QSignalSpy spy(&server, &AppLaunchHandler::filesRequested);
    ASSERT_TRUE(spy.isValid());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString meshPath = writeTempObj(dir, QStringLiteral("model.obj"));
    ASSERT_FALSE(meshPath.isEmpty());
    // A real, existing, but NON-importable file: must be dropped.
    const QString pdfPath = writeTempFile(dir, QStringLiteral("readme.pdf"),
                                          QByteArray("%PDF-1.4\n"));
    ASSERT_FALSE(pdfPath.isEmpty());
    // An importable extension that does NOT exist on disk: must be dropped.
    const QString missingPath = QFileInfo(dir.filePath(QStringLiteral("ghost.fbx")))
                                    .absoluteFilePath();

    AppLaunchHandler client;
    EXPECT_TRUE(client.tryForwardToRunningInstance(
        QStringList{meshPath, pdfPath, missingPath}));

    ASSERT_TRUE(waitForSpy(spy));
    ASSERT_EQ(spy.count(), 1);
    const QStringList emitted = spy.takeFirst().at(0).toStringList();
    EXPECT_TRUE(emitted.contains(meshPath));
    EXPECT_FALSE(emitted.contains(pdfPath));
    EXPECT_FALSE(emitted.contains(missingPath));
    EXPECT_EQ(emitted.size(), 1);
}

// ---------------------------------------------------------------------------
// Destruction: server close + removeServer (allows a fresh listen afterward).
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, DestructorClosesAndRemovesServer)
{
    QLocalServer::removeServer(QLatin1String(AppLaunchHandler::kServerName));

    {
        AppLaunchHandler first;
        EXPECT_TRUE(first.startSingleInstanceServer());
    } // ~AppLaunchHandler runs close() + removeServer()

    // If destruction cleaned up correctly, a brand-new handler can listen again.
    AppLaunchHandler second;
    EXPECT_TRUE(second.startSingleInstanceServer());
}

// ---------------------------------------------------------------------------
// eventFilter: QFileOpenEvent
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, EventFilterConsumesImportableFileOpen)
{
    QLocalServer::removeServer(QLatin1String(AppLaunchHandler::kServerName));

    TestableHandler handler;
    QSignalSpy spy(&handler, &AppLaunchHandler::filesRequested);
    ASSERT_TRUE(spy.isValid());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString meshPath = writeTempObj(dir, QStringLiteral("open.obj"));
    ASSERT_FALSE(meshPath.isEmpty());

    QFileOpenEvent ev(meshPath);
    // Importable + existing path is consumed (returns true) and routes through
    // handleIncomingPaths, which emits filesRequested synchronously.
    EXPECT_TRUE(handler.callEventFilter(QCoreApplication::instance(), &ev));

    ASSERT_EQ(spy.count(), 1);
    const QStringList emitted = spy.takeFirst().at(0).toStringList();
    ASSERT_EQ(emitted.size(), 1);
    EXPECT_EQ(emitted.front(), meshPath);
}

TEST(AppLaunchHandlerCoverageTest, EventFilterIgnoresNonImportableFileOpen)
{
    TestableHandler handler;
    QSignalSpy spy(&handler, &AppLaunchHandler::filesRequested);
    ASSERT_TRUE(spy.isValid());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdfPath = writeTempFile(dir, QStringLiteral("doc.pdf"),
                                          QByteArray("%PDF-1.4\n"));
    ASSERT_FALSE(pdfPath.isEmpty());

    QFileOpenEvent ev(pdfPath);
    // Non-importable path: falls through to QObject::eventFilter, which returns
    // false for an unaccepted event, and emits nothing.
    EXPECT_FALSE(handler.callEventFilter(QCoreApplication::instance(), &ev));
    EXPECT_EQ(spy.count(), 0);
}

TEST(AppLaunchHandlerCoverageTest, EventFilterPassesThroughNonFileOpenEvent)
{
    TestableHandler handler;
    QSignalSpy spy(&handler, &AppLaunchHandler::filesRequested);
    ASSERT_TRUE(spy.isValid());

    QEvent ev(QEvent::None);
    EXPECT_FALSE(handler.callEventFilter(QCoreApplication::instance(), &ev));
    EXPECT_EQ(spy.count(), 0);
}

} // namespace
