#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QProcess>

namespace
{
QString propertiesPanelQmlPath()
{
    return QDir(QDir::currentPath()).filePath(QStringLiteral("../qml/PropertiesPanel.qml"));
}
} // namespace

TEST(PropertiesPanelQmlTest, DoesNotUseThemedCheckBox)
{
    // ThemedCheckBox lives in the Material Editor module (MaterialEditorQML palette)
    // and is not available to the Inspector panel — using it prevents the whole
    // PropertiesPanel from loading.
    QFile file(propertiesPanelQmlPath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text)) << file.errorString().toStdString();
    const QString source = QString::fromUtf8(file.readAll());
    EXPECT_FALSE(source.contains(QStringLiteral("ThemedCheckBox {")))
        << "PropertiesPanel.qml must use Controls.CheckBox, not ThemedCheckBox";
}

TEST(PropertiesPanelQmlTest, QmlLintPasses)
{
    const QString path = propertiesPanelQmlPath();
    ASSERT_TRUE(QFile::exists(path)) << path.toStdString();

    QProcess lint;
    lint.start(QStringLiteral("qmllint"), {path});
    ASSERT_TRUE(lint.waitForFinished(10000)) << "qmllint did not finish";
    const QByteArray err = lint.readAllStandardError();
    EXPECT_EQ(lint.exitCode(), 0) << err.constData();
}
