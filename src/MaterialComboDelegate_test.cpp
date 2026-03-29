#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QComboBox>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QSignalSpy>
#include <QMetaObject>
#include <QThread>
#include "MaterialComboDelegate.h"
#include "Manager.h"
#include "TestHelpers.h"

class MaterialComboDelegateTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();
    }

    void TearDown() override
    {
        Manager::kill();
        if (app)
            app->processEvents();
        QThread::msleep(50);
    }

    QApplication* app = nullptr;
};

TEST_F(MaterialComboDelegateTest, Constructor)
{
    MaterialComboDelegate delegate;
    EXPECT_NE(&delegate, nullptr);
}

TEST_F(MaterialComboDelegateTest, CreateEditorReturnsComboBox)
{
    MaterialComboDelegate delegate;
    QWidget parentWidget;
    QStyleOptionViewItem option;
    QStandardItemModel model(1, 3);
    QModelIndex index = model.index(0, 2);

    QWidget* editor = delegate.createEditor(&parentWidget, option, index);
    ASSERT_NE(editor, nullptr);

    QComboBox* comboBox = qobject_cast<QComboBox*>(editor);
    ASSERT_NE(comboBox, nullptr);

    // Should contain at least the standard materials we created
    EXPECT_GE(comboBox->count(), 2); // BaseWhite and BaseWhiteNoLighting

    delete editor;
}

TEST_F(MaterialComboDelegateTest, SetEditorDataNonMaterialColumn)
{
    MaterialComboDelegate delegate;
    QWidget parentWidget;
    QStyleOptionViewItem option;
    QStandardItemModel model(1, 3);
    model.setData(model.index(0, 0), "TestEntity");
    QModelIndex index = model.index(0, 0); // Column 0, not material column

    QWidget* editor = delegate.createEditor(&parentWidget, option, model.index(0, 2));
    // setEditorData for column != 2 should delegate to parent class
    delegate.setEditorData(editor, index);
    // Should not crash

    delete editor;
}

TEST_F(MaterialComboDelegateTest, SetModelDataNonMaterialColumn)
{
    MaterialComboDelegate delegate;
    QWidget parentWidget;
    QStyleOptionViewItem option;
    QStandardItemModel model(1, 3);
    QModelIndex index = model.index(0, 0); // Column 0

    QWidget* editor = delegate.createEditor(&parentWidget, option, model.index(0, 2));
    // setModelData for column != 2 should delegate to parent class
    delegate.setModelData(editor, &model, index);
    // Should not crash

    delete editor;
}

TEST_F(MaterialComboDelegateTest, UpdateEditorGeometry)
{
    MaterialComboDelegate delegate;
    QWidget parentWidget;
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 200, 30);
    QStandardItemModel model(1, 3);
    QModelIndex index = model.index(0, 2);

    QWidget* editor = delegate.createEditor(&parentWidget, option, index);
    delegate.updateEditorGeometry(editor, option, index);

    EXPECT_EQ(editor->geometry(), option.rect);

    delete editor;
}

TEST_F(MaterialComboDelegateTest, InitStyleOptionNonMaterialColumn)
{
    MaterialComboDelegate delegate;
    QStyleOptionViewItem option;
    QStandardItemModel model(1, 3);
    model.setData(model.index(0, 0), "TestValue");
    QModelIndex index = model.index(0, 0); // Column 0

    // Should delegate to parent
    delegate.initStyleOption(&option, index);
    // Should not crash
}

TEST_F(MaterialComboDelegateTest, SetEditorDataMaterialColumnWithoutSubEntity)
{
    MaterialComboDelegate delegate;
    QWidget parentWidget;
    QStyleOptionViewItem option;
    QStandardItemModel model(1, 3);
    QModelIndex materialIndex = model.index(0, 2);

    QWidget* editor = delegate.createEditor(&parentWidget, option, materialIndex);
    auto* comboBox = qobject_cast<QComboBox*>(editor);
    ASSERT_NE(comboBox, nullptr);

    comboBox->setCurrentText("BaseWhite");
    delegate.setEditorData(editor, materialIndex);

    // Non-editable combo boxes may keep previous text if the requested text does not exist.
    EXPECT_TRUE(comboBox->currentText() == "BaseWhite" || comboBox->currentText().isEmpty());
    delete editor;
}

TEST_F(MaterialComboDelegateTest, SetModelDataMaterialColumnWritesDisplayRole)
{
    MaterialComboDelegate delegate;
    QWidget parentWidget;
    QStyleOptionViewItem option;
    QStandardItemModel model(1, 3);
    QModelIndex materialIndex = model.index(0, 2);

    QWidget* editor = delegate.createEditor(&parentWidget, option, materialIndex);
    auto* comboBox = qobject_cast<QComboBox*>(editor);
    ASSERT_NE(comboBox, nullptr);
    ASSERT_GT(comboBox->count(), 0);

    comboBox->setCurrentIndex(0);
    const QString selectedMaterial = comboBox->currentText();
    delegate.setModelData(editor, &model, materialIndex);

    EXPECT_EQ(model.data(materialIndex, Qt::DisplayRole).toString(), selectedMaterial);
    delete editor;
}

TEST_F(MaterialComboDelegateTest, InitStyleOptionMaterialColumnWithNoSubEntity)
{
    MaterialComboDelegate delegate;
    QStyleOptionViewItem option;
    QStandardItemModel model(1, 3);
    QModelIndex materialIndex = model.index(0, 2);

    delegate.initStyleOption(&option, materialIndex);
    EXPECT_TRUE(option.text.isEmpty());
}

TEST_F(MaterialComboDelegateTest, CommitAndCloseEditorEmitsSignalsFromEditorSender)
{
    MaterialComboDelegate delegate;
    QWidget parentWidget;
    QStyleOptionViewItem option;
    QStandardItemModel model(1, 3);
    QModelIndex materialIndex = model.index(0, 2);

    QWidget* editor = delegate.createEditor(&parentWidget, option, materialIndex);
    auto* comboBox = qobject_cast<QComboBox*>(editor);
    ASSERT_NE(comboBox, nullptr);
    ASSERT_GT(comboBox->count(), 0);

    QSignalSpy commitSpy(&delegate, &QAbstractItemDelegate::commitData);
    QSignalSpy closeSpy(&delegate, &QAbstractItemDelegate::closeEditor);

    comboBox->setCurrentIndex((comboBox->currentIndex() + 1) % comboBox->count());

    EXPECT_EQ(commitSpy.count(), 1);
    EXPECT_EQ(closeSpy.count(), 1);
    EXPECT_EQ(qobject_cast<QWidget*>(commitSpy.at(0).at(0).value<QObject*>()), editor);
    EXPECT_EQ(qobject_cast<QWidget*>(closeSpy.at(0).at(0).value<QObject*>()), editor);

    delete editor;
}

TEST_F(MaterialComboDelegateTest, CommitAndCloseEditorWithoutSenderDoesNotEmit)
{
    MaterialComboDelegate delegate;
    QSignalSpy commitSpy(&delegate, &QAbstractItemDelegate::commitData);
    QSignalSpy closeSpy(&delegate, &QAbstractItemDelegate::closeEditor);

    ASSERT_TRUE(QMetaObject::invokeMethod(&delegate, "commitAndCloseEditor", Qt::DirectConnection));

    EXPECT_EQ(commitSpy.count(), 0);
    EXPECT_EQ(closeSpy.count(), 0);
}
