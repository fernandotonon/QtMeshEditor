#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "materialeditor.h"
#include "ui_materialeditor.h"
#include <QApplication>

class MaterialEditorTest : public ::testing::Test {
    protected:
    std::unique_ptr<QApplication> app;
    void SetUp() override {
        int argc{0};
        char* argv[] = { nullptr };
        app = std::make_unique<QApplication>(argc, argv);
    }
};

TEST_F(MaterialEditorTest, SetMaterialTextTest) {
    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();
    editor->setMaterialText("Test Material");

    ASSERT_EQ(editor->getMaterialText(), "Test Material");
}

TEST_F(MaterialEditorTest, SetMaterialEmptyTest) {
    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();
    editor->setMaterial("");

    ASSERT_EQ(editor->getMaterialText(), "material material_name\n{\n}");
    ASSERT_EQ(editor->getMaterialName(), "material_name");
    ASSERT_FALSE(editor->isScrollAreaEnabled());
}
