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
    int argc = 0;
    QApplication app(argc, nullptr);

    About aboutDialog;
    About aboutDialog2(&aboutDialog); // Also test with a parent widget.
    QString expectedVersionText = QString("Version: ") + QTMESHEDITOR_VERSION;
    EXPECT_EQ(aboutDialog.getVersionText(), expectedVersionText);
    EXPECT_EQ(aboutDialog2.getVersionText(), expectedVersionText);
}
