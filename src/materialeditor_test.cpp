#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "materialeditor.h"
#include "ui_materialeditor.h"
#include <QApplication>

class MaterialEditorTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

TEST_F(MaterialEditorTest, SetMaterialTextTest) {
    int argc = 0;
    char* argv[] = { nullptr };
    QApplication app(argc, argv);

    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();

    QString testMaterial = "Test Material";
    editor->setMaterialText(testMaterial);

    ASSERT_EQ(editor->getMaterialText(), testMaterial.toStdString());
}
