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
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());

    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QImage img = renderer->renderPreview("BaseWhite");
    ASSERT_FALSE(img.isNull()) << "material preview RTT failed (headless GL required)";
    EXPECT_EQ(img.width(), 64);
    EXPECT_EQ(img.height(), 64);
    EXPECT_EQ(img.format(), QImage::Format_RGBA8888);
}

TEST_F(MaterialPreviewRendererTests, FirstMaterialNameWithCommentLines) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QString path = tmpDir.path() + "/commented.material";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "// This is a comment\n";
    out << "// Another comment\n";
    out << "/* block comment */\n";
    out << "\n";
    out << "// material FakeName (in a comment)\n";
    out << "material RealMaterial\n";
    out << "{\n";
    out << "}\n";
    f.close();

    QString matName = MaterialPreviewRenderer::firstMaterialNameInFile(path);
    EXPECT_EQ(matName, "RealMaterial");
}

TEST_F(MaterialPreviewRendererTests, FirstMaterialNameWithQuotedName) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QString path = tmpDir.path() + "/quoted.material";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "material \"My Material With Spaces\"\n";
    out << "{\n";
    out << "}\n";
    f.close();

    QString matName = MaterialPreviewRenderer::firstMaterialNameInFile(path);
    // Should return some non-empty name (the exact format depends on parsing)
    EXPECT_FALSE(matName.isEmpty());
}

TEST_F(MaterialPreviewRendererTests, FirstMaterialNameOnlyWhitespace) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QString path = tmpDir.path() + "/whitespace.material";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "   \n";
    out << "\t\n";
    out << "\n";
    f.close();

    QString matName = MaterialPreviewRenderer::firstMaterialNameInFile(path);
    EXPECT_TRUE(matName.isEmpty());
}

TEST_F(MaterialPreviewRendererTests, MultipleFirstMaterialNamesReturnsFirst) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QString path = tmpDir.path() + "/multi.material";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "material FirstMaterial\n";
    out << "{\n";
    out << "}\n";
    out << "\n";
    out << "material SecondMaterial\n";
    out << "{\n";
    out << "}\n";
    f.close();

    QString matName = MaterialPreviewRenderer::firstMaterialNameInFile(path);
    EXPECT_EQ(matName, "FirstMaterial");
}

// ===========================================================================
// Slice I — renderInteractivePreview: arbitrary size, shape switch,
// environment yaw. The thumbnail path (renderPreviewAsDataUri) is
// always Sphere + yaw=0 for cache stability; the interactive path is
// used by the Inspector preview pane which re-renders on user input.
// ===========================================================================

TEST_F(MaterialPreviewRendererTests, InteractivePreviewReturnsEmptyForUnknownMaterial) {
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QString uri = renderer->renderInteractivePreview(
        "NonExistent_XYZ_123", 96, MaterialPreviewRenderer::ShapeSphere, 0.0);
    EXPECT_TRUE(uri.isEmpty());
}

TEST_F(MaterialPreviewRendererTests, InteractivePreviewReturnsDataUriForKnownMaterial) {
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QString uri = renderer->renderInteractivePreview(
        "BaseWhite", 96, MaterialPreviewRenderer::ShapeSphere, 0.0);
    ASSERT_FALSE(uri.isEmpty()) << "interactive RTT failed (headless GL required)";
    EXPECT_TRUE(uri.startsWith("data:image/png;base64,"));
}

TEST_F(MaterialPreviewRendererTests, InteractivePreviewClampsSizeToBounds) {
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();

    auto decodeWidth = [](const QString& uri) -> int {
        const QByteArray payload = QByteArray::fromBase64(
            uri.mid(QString("data:image/png;base64,").size()).toLatin1());
        QImage img;
        if (!img.loadFromData(payload, "PNG")) return -1;
        return img.width();
    };

    QString tooSmall = renderer->renderInteractivePreview(
        "BaseWhite", 8, MaterialPreviewRenderer::ShapeSphere, 0.0);
    ASSERT_FALSE(tooSmall.isEmpty());
    EXPECT_EQ(decodeWidth(tooSmall), 32);  // clamped to lower bound

    QString tooLarge = renderer->renderInteractivePreview(
        "BaseWhite", 4096, MaterialPreviewRenderer::ShapeSphere, 0.0);
    ASSERT_FALSE(tooLarge.isEmpty());
    EXPECT_EQ(decodeWidth(tooLarge), 1024); // clamped to upper bound

    QString midband = renderer->renderInteractivePreview(
        "BaseWhite", 128, MaterialPreviewRenderer::ShapeSphere, 0.0);
    ASSERT_FALSE(midband.isEmpty());
    EXPECT_EQ(decodeWidth(midband), 128);
}

TEST_F(MaterialPreviewRendererTests, InteractivePreviewYawWrapsTo360) {
    // yaw=0 vs yaw=360 must produce the same image (wrap-around). yaw=-90
    // wraps to 270, which differs from 0. Confirms the modulo step in
    // renderInteractivePreview is honoured by callers passing arbitrary
    // accumulated drag offsets.
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QString yaw0   = renderer->renderInteractivePreview(
        "BaseWhite", 96, MaterialPreviewRenderer::ShapeSphere, 0.0);
    QString yaw360 = renderer->renderInteractivePreview(
        "BaseWhite", 96, MaterialPreviewRenderer::ShapeSphere, 360.0);
    QString yawNeg = renderer->renderInteractivePreview(
        "BaseWhite", 96, MaterialPreviewRenderer::ShapeSphere, -90.0);

    ASSERT_FALSE(yaw0.isEmpty());
    ASSERT_FALSE(yaw360.isEmpty());
    ASSERT_FALSE(yawNeg.isEmpty());

    EXPECT_EQ(yaw0, yaw360) << "yaw 0 and 360 must wrap to identical render";
    EXPECT_NE(yaw0, yawNeg) << "yaw 0 and -90 must produce different lighting";
}

TEST_F(MaterialPreviewRendererTests, InteractivePreviewShapeSwitchProducesDifferentBytes) {
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QString sphere = renderer->renderInteractivePreview(
        "BaseWhite", 96, MaterialPreviewRenderer::ShapeSphere, 0.0);
    QString cube   = renderer->renderInteractivePreview(
        "BaseWhite", 96, MaterialPreviewRenderer::ShapeCube,   0.0);
    QString plane  = renderer->renderInteractivePreview(
        "BaseWhite", 96, MaterialPreviewRenderer::ShapePlane,  0.0);

    ASSERT_FALSE(sphere.isEmpty());
    ASSERT_FALSE(cube.isEmpty());
    ASSERT_FALSE(plane.isEmpty());

    EXPECT_NE(sphere, cube);
    EXPECT_NE(sphere, plane);
    EXPECT_NE(cube, plane);
}

TEST_F(MaterialPreviewRendererTests, InteractivePreviewOutOfRangeShapeFallsBackToSphere) {
    // shape=99 must be treated as Sphere — the renderer's switch has a
    // default branch that prevents stray UI values from crashing.
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QString sphere = renderer->renderInteractivePreview(
        "BaseWhite", 96, MaterialPreviewRenderer::ShapeSphere, 0.0);
    QString outOfRange = renderer->renderInteractivePreview(
        "BaseWhite", 96, /*shape=*/99, 0.0);

    ASSERT_FALSE(sphere.isEmpty());
    ASSERT_FALSE(outOfRange.isEmpty());
    EXPECT_EQ(sphere, outOfRange);
}

TEST_F(MaterialPreviewRendererTests, InteractivePreviewReusesRttOnSameSize) {
    // Two consecutive renders at the same size must not crash and must
    // both succeed. Internally the RTT texture is reused; this is a
    // regression guard for the resize/reset branch in
    // renderInteractivePreview.
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QString first  = renderer->renderInteractivePreview(
        "BaseWhite", 128, MaterialPreviewRenderer::ShapeSphere, 0.0);
    QString second = renderer->renderInteractivePreview(
        "BaseWhite", 128, MaterialPreviewRenderer::ShapeSphere, 0.0);

    ASSERT_FALSE(first.isEmpty());
    ASSERT_FALSE(second.isEmpty());
    EXPECT_EQ(first, second);
}

TEST_F(MaterialPreviewRendererTests, ThumbnailIsCanonicalAfterInteractivePreview) {
    // Regression: renderInteractivePreview mutates the shared entity
    // (Cube/Plane mesh) and rotates the light. The cached thumbnail
    // path must reset both back to "Sphere + default light" so the
    // material card preview stays canonical regardless of what the
    // user just rendered in the interactive pane.
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();

    // Capture the canonical thumbnail BEFORE any interactive call.
    renderer->clearCache();
    const QString canonical = renderer->renderPreviewAsDataUri("BaseWhite");
    ASSERT_FALSE(canonical.isEmpty());

    // Render an interactive preview on a different shape with a
    // non-zero yaw — guaranteed to mutate the shared scene state.
    QString cube = renderer->renderInteractivePreview(
        "BaseWhite", 128, MaterialPreviewRenderer::ShapeCube, 90.0);
    ASSERT_FALSE(cube.isEmpty());

    // The thumbnail path must still produce the canonical sphere image
    // after the interactive call — confirms the shared scene state was
    // reset before rendering. Bypass the C++ cache to actually re-render.
    renderer->clearCache();
    const QString afterInteractive = renderer->renderPreviewAsDataUri("BaseWhite");
    ASSERT_FALSE(afterInteractive.isEmpty());
    EXPECT_EQ(canonical, afterInteractive)
        << "thumbnail picked up interactive scene state (cube mesh or yawed light)";
}

TEST_F(MaterialPreviewRendererTests, InteractivePreviewResizesRttBetweenCalls) {
    // Different size on the second call must trigger the
    // "remove + recreate" branch in renderInteractivePreview and still
    // produce a valid image. Decoded image dimensions must match
    // the requested (clamped) size.
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* renderer = MaterialPreviewRenderer::instance();
    QString small = renderer->renderInteractivePreview(
        "BaseWhite", 64, MaterialPreviewRenderer::ShapeSphere, 0.0);
    QString large = renderer->renderInteractivePreview(
        "BaseWhite", 256, MaterialPreviewRenderer::ShapeSphere, 0.0);
    ASSERT_FALSE(small.isEmpty());
    ASSERT_FALSE(large.isEmpty());

    auto decode = [](const QString& uri) {
        const QByteArray payload = QByteArray::fromBase64(
            uri.mid(QString("data:image/png;base64,").size()).toLatin1());
        QImage img;
        img.loadFromData(payload, "PNG");
        return img;
    };
    EXPECT_EQ(decode(small).width(), 64);
    EXPECT_EQ(decode(large).width(), 256);
}
