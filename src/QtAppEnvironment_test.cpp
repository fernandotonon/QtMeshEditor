#include <QApplication>
#include <gtest/gtest.h>

namespace {

class QtAppEnvironment : public ::testing::Environment
{
public:
    void SetUp() override
    {
        if (QApplication::instance())
            return;

        // Persistent storage for argc/argv so QApplication can outlive individual tests.
        static int argc = 1;
        static char appName[] = "QtMeshTests";
        static char* argv[] = {appName, nullptr};

        // Leak intentionally so that Qt stays alive for the duration of the test process.
        static QApplication* app = new QApplication(argc, argv); // NOSONAR - intentional leak for test lifetime
        (void)app;
    }

    void TearDown() override
    {
        // QApplication is intentionally kept alive to avoid shutdown ordering issues
        // with OGRE and Qt when running inside the unit test harness.
    }
};

::testing::Environment* const qtAppEnv =
    ::testing::AddGlobalTestEnvironment(new QtAppEnvironment()); // NOSONAR - gtest takes ownership

} // namespace

