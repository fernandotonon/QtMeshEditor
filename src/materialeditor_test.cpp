#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "materialeditor.h"
#include "ui_materialeditor.h"
#include <QApplication>

class MaterialEditorTest : public ::testing::Test {
};

TEST_F(MaterialEditorTest, SetMaterialTextTest) {
    int argc = 0;
    char* argv[] = { nullptr };
    QApplication app(argc, argv);

    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();
    editor->setMaterialText("Test Material");

    ASSERT_EQ(editor->getMaterialText(), "Test Material");
}
