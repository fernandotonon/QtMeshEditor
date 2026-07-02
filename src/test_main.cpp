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
#include <QCoreApplication>
#include <QThread>
#include <csignal>
#include <cstdlib>
#include <OgreLogManager.h>
#include "Manager.h"
#include "TestHelpers.h"

#ifndef Q_OS_WIN
#include <unistd.h>
#include <execinfo.h>   // backtrace dump from crashHandler (diagnosing CI SIGSEGV)
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
#ifndef Q_OS_WIN
    // Dump a backtrace to stderr so CI logs show WHERE a signal-11 landed
    // (suites that crash under Xvfb are otherwise a black box — the runner
    // only reports "Suite X CRASHED"). backtrace_symbols_fd writes straight
    // to the fd with no malloc, so it is safe enough in a signal handler;
    // main() pre-loads libgcc by calling backtrace() once at startup.
    {
        const char banner[] = "=== crashHandler backtrace ===\n";
        write(STDERR_FILENO, banner, sizeof(banner) - 1);
        void* frames[64];
        const int n = backtrace(frames, 64);
        backtrace_symbols_fd(frames, n, STDERR_FILENO);
    }
#endif
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
#ifdef Q_OS_LINUX
    // Force xcb on Linux so that Ogre's externalWindowHandle path
    // receives a real X11 XID (Wayland's wl_surface handle is incompatible
    // and silently breaks createRenderWindow under Xvfb / desktop sessions).
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");
#endif

    // Tests must be hermetic: no suite may reach out to the network to fetch an
    // AI model. Any code path that calls an `ensure*Blocking()` download helper
    // without this guard would spin a QEventLoop on a real download and hang the
    // suite until the 20-min CI per-suite wall-clock cap SIGKILLs it (issue #411
    // added generate_motion, which lands MotionGenerator/MotionLibrary blocking
    // downloads on the tool path). Force every model fetch to short-circuit.
    // Do NOT override values the caller explicitly set (respect an intentional
    // integration run that wants a real download).
    static const char* const kNoDownloadGuards[] = {
        "QTMESH_PBR_NO_DOWNLOAD",
        "QTMESH_UNIRIG_NO_DOWNLOAD",
        "QTMESH_SEGMENT_NO_DOWNLOAD",
        "QTMESH_INBETWEEN_NO_DOWNLOAD",
        "QTMESH_MOTION_NO_DOWNLOAD",
        "QTMESH_T2M_NO_DOWNLOAD",
        "QTMESH_TRIPOSR_NO_DOWNLOAD",
        "QTMESH_REMBG_NO_DOWNLOAD",
    };
    for (const char* guard : kNoDownloadGuards) {
        if (!qEnvironmentVariableIsSet(guard))
            qputenv(guard, "1");
    }

    QApplication app(argc, argv);

    // Suppress Ogre log output (debug spam from Root, RenderSystem, plugins).
    // Must be done before any Manager::getSingleton() call creates Root.
    if (!Ogre::LogManager::getSingletonPtr()) {
        auto* logMgr = new Ogre::LogManager();
        logMgr->createLog("ogre.log", true, false, true); // suppressDebugOut, suppressFileOutput
    }

    // Suppress Qt debug messages
    qInstallMessageHandler(testMessageHandler);

#ifndef Q_OS_WIN
    // Pre-load libgcc's unwinder so the first backtrace() call inside the
    // signal handler doesn't have to (it may allocate on first use).
    {
        void* preload[2];
        backtrace(preload, 2);
    }
#endif

    // Install signal handlers AFTER QApplication to avoid Qt overwriting them.
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
#ifndef Q_OS_WIN
    signal(SIGBUS, crashHandler);
#endif

    testing::InitGoogleTest(&argc, argv);

    // Prove headless GL works on this runner, then tear down: many suites
    // (Assimp processors, etc.) construct their own Ogre::Root and cannot
    // coexist with a live Manager singleton from a prior init.
    if (!tryInitOgre()) {
        fprintf(stderr,
                "UnitTests FATAL: tryInitOgre() failed — need working DISPLAY / Xvfb for GL.\n");
        return 1;
    }
    Manager::kill();
    if (QCoreApplication::instance())
        QCoreApplication::processEvents();
    QThread::msleep(50);

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
