#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>

// ---------------------------------------------------------------------------
// Tests that verify the bundled RTSS shader resources exist at paths where
// addRTSSResources() expects to find them.
//
// The production code (RTShaderHelper.cpp) searches for media/RTShaderLib/
// and media/Main/ relative to QCoreApplication::applicationDirPath().
// These tests verify the install step placed all required files correctly.
// ---------------------------------------------------------------------------

class RTSSResourcesTest : public ::testing::Test {
protected:
    QString rtssDir;
    QString mainDir;

    void SetUp() override {
        QString appDir = QCoreApplication::applicationDirPath();
        rtssDir = appDir + "/media/RTShaderLib";
        mainDir = appDir + "/media/Main";

        if (!QDir(rtssDir).exists()) {
            QString alt = appDir + "/../media/RTShaderLib";
            if (QDir(alt).exists()) {
                rtssDir = QDir(alt).canonicalPath();
                mainDir = QDir(appDir + "/../media/Main").canonicalPath();
            }
        }
        if (!QDir(rtssDir).exists()) {
            const QString srcRoot = QString::fromUtf8(QTMESH_UT_SOURCE_ROOT);
            if (!srcRoot.isEmpty()) {
                rtssDir = QDir(srcRoot).filePath(QStringLiteral("media/RTShaderLib"));
                mainDir = QDir(srcRoot).filePath(QStringLiteral("media/Main"));
            }
        }
        ASSERT_TRUE(QDir(rtssDir).exists())
            << "RTShaderLib not found next to UnitTests or under source tree";
        ASSERT_TRUE(QDir(mainDir).exists()) << mainDir.toStdString();
    }
};

// -- RTShaderLib directory ---------------------------------------------------

// -- Core GLSL shader files --------------------------------------------------

TEST_F(RTSSResourcesTest, HasFFPLibTransform)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/FFPLib_Transform.glsl"));
}

TEST_F(RTSSResourcesTest, HasFFPLibTexturing)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/FFPLib_Texturing.glsl"));
}

TEST_F(RTSSResourcesTest, HasFFPLibFog)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/FFPLib_Fog.glsl"));
}

TEST_F(RTSSResourcesTest, HasFFPLibAlphaTest)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/FFPLib_AlphaTest.glsl"));
}

TEST_F(RTSSResourcesTest, HasRTSLibLighting)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/RTSLib_Lighting.glsl"));
}

TEST_F(RTSSResourcesTest, HasRTSLibColour)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/RTSLib_Colour.glsl"));
}

TEST_F(RTSSResourcesTest, HasSGXLibNormalMap)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/SGXLib_NormalMap.glsl"));
}

TEST_F(RTSSResourcesTest, HasSGXLibPerPixelLighting)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/SGXLib_PerPixelLighting.glsl"));
}

TEST_F(RTSSResourcesTest, HasSGXLibCookTorrance)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/SGXLib_CookTorrance.glsl"));
}

TEST_F(RTSSResourcesTest, HasQTSLibIbl)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/QTSLib_IBL.glsl"));
}

TEST_F(RTSSResourcesTest, HasSGXLibDualQuaternion)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/SGXLib_DualQuaternion.glsl"));
}

// -- Material and LUT files --------------------------------------------------

TEST_F(RTSSResourcesTest, HasRTSSamplersMaterial)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/RTSSamplers.material"));
}

TEST_F(RTSSResourcesTest, HasDfgLUTTexture)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/dfgLUTmultiscatter.dds"));
}

TEST_F(RTSSResourcesTest, HasLTC1Texture)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/ltc_1.dds"));
}

TEST_F(RTSSResourcesTest, HasLTC2Texture)
{
    EXPECT_TRUE(QFile::exists(rtssDir + "/ltc_2.dds"));
}

// -- Main/ support files -----------------------------------------------------

TEST_F(RTSSResourcesTest, HasOgreUnifiedShader)
{
    EXPECT_TRUE(QFile::exists(mainDir + "/OgreUnifiedShader.h"));
}

TEST_F(RTSSResourcesTest, HasHLSLSM4Support)
{
    EXPECT_TRUE(QFile::exists(mainDir + "/HLSL_SM4Support.hlsl"));
}

TEST_F(RTSSResourcesTest, HasGLSLGL3Support)
{
    EXPECT_TRUE(QFile::exists(mainDir + "/GLSL_GL3Support.glsl"));
}

// -- File count sanity check -------------------------------------------------

TEST_F(RTSSResourcesTest, RTShaderLibHasExpectedFileCount)
{
    QDir dir(rtssDir);
    auto entries = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    // 16 .glsl + 3 .dds + 1 .material = 20 files
    EXPECT_GE(entries.size(), 21) << "Expected at least 21 files in RTShaderLib/";
}

TEST_F(RTSSResourcesTest, MainDirHasMinimumFiles)
{
    QDir dir(mainDir);
    auto entries = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    // At minimum: OgreUnifiedShader.h, HLSL_SM4Support.hlsl, GLSL_GL3Support.glsl
    EXPECT_GE(entries.size(), 3) << "Expected at least 3 files in Main/";
}
