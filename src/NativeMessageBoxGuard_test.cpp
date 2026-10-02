// NativeMessageBoxGuard — every QMessageBox is forced onto the widget
// implementation (a native NSAlert crashes on macOS 27).

#include <gtest/gtest.h>

#include "NativeMessageBoxGuard.h"

#include <QApplication>
#include <QMessageBox>
#include <QTimer>

#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)

TEST(NativeMessageBoxGuard, MarksABoxNonNativeWhileItIsConstructed)
{
    NativeMessageBoxGuard* guard = NativeMessageBoxGuard::install(qApp);
    ASSERT_NE(guard, nullptr);
    // The option has to be set by the time the constructor returns: the
    // static helpers call exec() immediately after it.
    QMessageBox box(QMessageBox::Information, QStringLiteral("T"), QStringLiteral("text"),
                    QMessageBox::Ok);
    EXPECT_TRUE(box.testOption(QMessageBox::Option::DontUseNativeDialog));
}

#endif

TEST(NativeMessageBoxGuard, InstallIsIdempotent)
{
    NativeMessageBoxGuard* a = NativeMessageBoxGuard::install(qApp);
    NativeMessageBoxGuard* b = NativeMessageBoxGuard::install(qApp);
    EXPECT_EQ(a, b);
    EXPECT_EQ(qApp->findChildren<NativeMessageBoxGuard*>(QString(), Qt::FindDirectChildrenOnly).size(), 1);
    EXPECT_EQ(NativeMessageBoxGuard::install(nullptr), nullptr);
}

TEST(NativeMessageBoxGuard, LeavesOtherObjectsAlone)
{
    NativeMessageBoxGuard::install(qApp);
    QObject parent;
    QObject child(&parent);   // ChildAdded on a non-message-box: must not crash
    SUCCEED();
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
// Needs Qt 6.6 for QMessageBox::testOption.
TEST(NativeMessageBoxGuard, StaticHelperRunsAsAWidgetDialog)
{
    NativeMessageBoxGuard::install(qApp);
    bool sawWidgetBox = false;
    // A widget QMessageBox shows up as a top-level widget; a native one never
    // does (only its hidden QWidget shell exists, and it is never shown).
    QTimer::singleShot(50, qApp, [&] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (auto* box = qobject_cast<QMessageBox*>(w); box && box->isVisible()) {
                sawWidgetBox = box->testOption(QMessageBox::Option::DontUseNativeDialog);
                box->accept();
            }
        }
    });
    QTimer::singleShot(3000, qApp, [] {   // safety net: never hang the suite
        for (QWidget* w : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QMessageBox*>(w)) box->reject();
    });
    QMessageBox::information(nullptr, QStringLiteral("T"), QStringLiteral("text"));
    EXPECT_TRUE(sawWidgetBox);
}
#endif
