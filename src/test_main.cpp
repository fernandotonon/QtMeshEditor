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
#include "Manager.h"

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

// GCC coverage flush — only available when built with --coverage
// The COVERAGE_BUILD macro is set by CMake when coverage flags are enabled
#ifdef COVERAGE_BUILD
extern "C" void __gcov_dump(void);
#endif

static void crashHandler(int sig)
{
#ifdef COVERAGE_BUILD
    __gcov_dump();
    // Exit immediately — the crash is typically in Ogre/Mesa teardown
    // after tests have passed. Re-raising would produce exit code 139,
    // which the CI counts as a crash and loses the suite's pass status.
    // Use gtest's failure count so real test failures still produce non-zero exit.
    _exit(testing::UnitTest::GetInstance()->failed_test_count() > 0 ? 1 : 0);
#else
    signal(sig, SIG_DFL);
    raise(sig);
#endif
}

int main(int argc, char **argv)
{
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
#ifndef Q_OS_WIN
    signal(SIGBUS, crashHandler);
#endif

    QApplication app(argc, argv);

    testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();

    // Clean up Ogre before QApplication destruction to avoid
    // SIGSEGV during static destructor teardown (Ogre::Root vs QApp race).
    Manager::kill();

#ifdef COVERAGE_BUILD
    __gcov_dump();   // Flush all coverage data before exit
    _exit(result);   // Skip static destructors that crash under Mesa/Xvfb
#endif
    return result;
}
