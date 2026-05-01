/**
 * Custom GTest main that:
 * 1. Creates a QApplication (needed by Qt-based tests)
 * 2. Installs signal handlers that flush gcov coverage data on crash
 *
 * This replaces gtest_main so that a segfault in one test doesn't
 * lose ALL coverage data (gcov normally flushes on clean exit only).
 */
#include <gtest/gtest.h>
#include <QApplication>
#include <csignal>
#include <cstdlib>
#include <OgreLogManager.h>
#include "Manager.h"
#include "TestHelpers.h"

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

// GCC coverage flush — only available when built with --coverage
// The COVERAGE_BUILD macro is set by CMake when coverage flags are enabled
#ifdef COVERAGE_BUILD
extern "C" void __gcov_dump(void);
#endif

// Set to true after RUN_ALL_TESTS() returns so the crash handler
// can distinguish "crash during test" from "crash during teardown".
static volatile bool g_testsCompleted = false;

static void crashHandler(int sig)
{
#ifdef COVERAGE_BUILD
    __gcov_dump();
    if (g_testsCompleted) {
        // Crash during teardown (Ogre/Mesa static destructors) — tests already
        // ran, so preserve the pass/fail result instead of reporting a crash.
        _exit(testing::UnitTest::GetInstance()->failed_test_count() > 0 ? 1 : 0);
    } else {
        // Crash during test execution — report as signal exit so CI detects it.
        _exit(128 + sig);
    }
#else
    signal(sig, SIG_DFL);
    raise(sig);
#endif
}

// Suppress qDebug/qInfo/qWarning noise from Ogre and Qt internals.
// qCritical and qFatal always pass through so real errors are visible.
static void testMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    Q_UNUSED(ctx);
    if (type == QtCriticalMsg || type == QtFatalMsg)
        fprintf(stderr, "%s\n", qPrintable(msg));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // Suppress Ogre log output (debug spam from Root, RenderSystem, plugins).
    // Must be done before any Manager::getSingleton() call creates Root.
    if (!Ogre::LogManager::getSingletonPtr()) {
        auto* logMgr = new Ogre::LogManager();
        logMgr->createLog("ogre.log", true, false, true); // suppressDebugOut, suppressFileOutput
    }

    // Suppress Qt debug messages
    qInstallMessageHandler(testMessageHandler);

    // Install signal handlers AFTER QApplication to avoid Qt overwriting them.
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
#ifndef Q_OS_WIN
    signal(SIGBUS, crashHandler);
#endif

    testing::InitGoogleTest(&argc, argv);

    // All GPU-backed tests require a headless GL context (Xvfb on Linux CI).
    // Fail fast instead of silently skipping hundreds of tests.
    if (!tryInitOgre()) {
        fprintf(stderr,
                "UnitTests FATAL: tryInitOgre() failed — need working DISPLAY / Xvfb for GL.\n");
        return 1;
    }

    int result = RUN_ALL_TESTS();
    g_testsCompleted = true;

    // Clean up Ogre before QApplication destruction to avoid
    // SIGSEGV during static destructor teardown (Ogre::Root vs QApp race).
    Manager::kill();

#ifdef COVERAGE_BUILD
    __gcov_dump();   // Flush all coverage data before exit
    _exit(result);   // Skip static destructors that crash under Mesa/Xvfb
#endif
    return result;
}
