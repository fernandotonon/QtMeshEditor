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
#endif
    signal(sig, SIG_DFL);
    raise(sig);
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
    return RUN_ALL_TESTS();
}
