#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>

// Test that QApplication exists (created by test_main.cpp) - do not create another
TEST(MainTest, QApplicationExists)
{
    // QApplication is created by test_main.cpp - verify it exists
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    ASSERT_NE(app, nullptr);
}
