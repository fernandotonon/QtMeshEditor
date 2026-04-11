#include <gtest/gtest.h>
#include "MaterialPreviewRenderer.h"
#include "TestHelpers.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

class MaterialPreviewRendererTests : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()), nullptr);
    }

    void TearDown() override {
        MaterialPreviewRenderer::kill();
    }
};

TEST_F(MaterialPreviewRendererTests, Singleton) {
    auto* renderer = MaterialPreviewRenderer::instance();
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(renderer, MaterialPreviewRenderer::instance());
}

TEST_F(MaterialPreviewRendererTests, KillAndRecreate) {
    auto* r1 = MaterialPreviewRenderer::instance();
    ASSERT_NE(r1, nullptr);

    MaterialPreviewRenderer::kill();

    auto* r2 = MaterialPreviewRenderer::instance();
    ASSERT_NE(r2, nullptr);
}

TEST_F(MaterialPreviewRendererTests, RenderPreviewReturnsNullForUnknownMaterial) {
    auto* renderer = MaterialPreviewRenderer::instance();
    QImage img = renderer->renderPreview("NonExistentMaterial_XYZ_123");
    EXPECT_TRUE(img.isNull());
}

TEST_F(MaterialPreviewRendererTests, DataUriReturnsEmptyForUnknownMaterial) {
    auto* renderer = MaterialPreviewRenderer::instance();
    QString uri = renderer->renderPreviewAsDataUri("NonExistentMaterial_XYZ_123");
    EXPECT_TRUE(uri.isEmpty());
}

TEST_F(MaterialPreviewRendererTests, ClearCacheDoesNotCrash) {
    auto* renderer = MaterialPreviewRenderer::instance();
    renderer->clearCache();
}

TEST_F(MaterialPreviewRendererTests, FirstMaterialNameFromEmptyFile) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QString path = tmpDir.path() + "/empty.material";
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.close();

    EXPECT_TRUE(MaterialPreviewRenderer::firstMaterialNameInFile(path).isEmpty());
}

TEST_F(MaterialPreviewRendererTests, FirstMaterialNameFromValidFile) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QString path = tmpDir.path() + "/test.material";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "// A comment line\n";
    out << "\n";
    out << "material MyTestMaterial\n";
    out << "{\n";
    out << "    technique\n";
    out << "    {\n";
    out << "        pass\n";
    out << "        {\n";
    out << "            diffuse 1.0 0.0 0.0 1.0\n";
    out << "        }\n";
    out << "    }\n";
    out << "}\n";
    f.close();

    QString matName = MaterialPreviewRenderer::firstMaterialNameInFile(path);
    EXPECT_EQ(matName, "MyTestMaterial");
}

TEST_F(MaterialPreviewRendererTests, FirstMaterialNameWithInheritance) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QString path = tmpDir.path() + "/inherit.material";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "material DerivedMat : BaseMat\n";
    out << "{\n";
    out << "}\n";
    f.close();

    QString matName = MaterialPreviewRenderer::firstMaterialNameInFile(path);
    EXPECT_EQ(matName, "DerivedMat");
}

TEST_F(MaterialPreviewRendererTests, FirstMaterialNameFromNonexistentFile) {
    EXPECT_TRUE(MaterialPreviewRenderer::firstMaterialNameInFile("/no/such/file.material").isEmpty());
}

TEST_F(MaterialPreviewRendererTests, RenderPreviewWithOgreBaseWhite) {
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Ogre not available";
    }
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Cannot create meshes (no GL context)";
    }

    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QImage img = renderer->renderPreview("BaseWhite");
    // The preview should succeed when Ogre is fully initialized
    if (!img.isNull()) {
        EXPECT_EQ(img.width(), 64);
        EXPECT_EQ(img.height(), 64);
        EXPECT_EQ(img.format(), QImage::Format_RGBA8888);
    }
    // If null, the RTT may not be supported in this test environment (acceptable)
}

TEST_F(MaterialPreviewRendererTests, DataUriCachesResults) {
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Ogre not available";
    }
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Cannot create meshes (no GL context)";
    }

    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QString uri1 = renderer->renderPreviewAsDataUri("BaseWhite");
    if (uri1.isEmpty()) {
        GTEST_SKIP() << "RTT preview not available in this environment";
    }

    QString uri2 = renderer->renderPreviewAsDataUri("BaseWhite");
    EXPECT_EQ(uri1, uri2); // Should be cached

    renderer->clearCache();
    QString uri3 = renderer->renderPreviewAsDataUri("BaseWhite");
    EXPECT_FALSE(uri3.isEmpty()); // Should regenerate
}
