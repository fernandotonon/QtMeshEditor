#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "materialeditor.h"
#include "ui_materialeditor.h"
#include <QApplication>

class MaterialEditorTest : public ::testing::Test {
protected:
    QApplication* app;

    void SetUp() override {
        int argc = 0;
        char* argv[] = { nullptr };
        app = new QApplication(argc, argv);
    }

    void TearDown() override {
        delete app;
    }
};

TEST_F(MaterialEditorTest, SetMaterialTextTest) {
    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();

    QString testMaterial = "Test Material";
    editor->setMaterialText(testMaterial);

    ASSERT_EQ(editor->getMaterialText(), testMaterial.toStdString());
}
