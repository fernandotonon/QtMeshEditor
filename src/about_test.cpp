#include <gtest/gtest.h>
#include "about.h"
#include <QApplication>

class AboutTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code if needed
    }

    void TearDown() override {
        // Cleanup code if needed
    }
};

TEST_F(AboutTest, VersionTextIsCorrect) {
    int argc = 1;
    char *argv[1] = {(char*)"test"};
    QApplication app(argc, argv); 

    About aboutDialog;
    QString expectedVersionText = QString("Version: ") + QTMESHEDITOR_VERSION;
    EXPECT_EQ(aboutDialog.getVersionText(), expectedVersionText);
}