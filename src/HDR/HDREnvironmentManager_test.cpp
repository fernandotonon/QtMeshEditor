#include <gtest/gtest.h>

#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrEquirectLoader.h"
#include "MinimalEXRWriter.h"
#include "TestHelpers.h"

#include <OgreTextureManager.h>

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>

class HDREnvironmentManagerTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        HDREnvironmentManager::kill();
    }
};

class HDREnvironmentManagerOgreTest : public HDREnvironmentManagerTest {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
    }
};

TEST_F(HDREnvironmentManagerTest, KillAndRecreate)
{
    auto* mgr = HDREnvironmentManager::getSingleton();
    ASSERT_NE(mgr, nullptr);
    HDREnvironmentManager::kill();
    auto* mgr2 = HDREnvironmentManager::getSingleton();
    ASSERT_NE(mgr2, nullptr);
}

#ifdef ENABLE_OPENEXR
TEST_F(HDREnvironmentManagerOgreTest, LoadEnvironment_CreatesCubeMapTexture)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const int w = 32;
    const int h = 16;
    std::vector<float> rgb(static_cast<size_t>(w * h * 3), 0.5f);
    const QString path = tmp.filePath(QStringLiteral("studio.exr"));
    ASSERT_TRUE(MinimalEXR::writeRGB32F(path, w, h, rgb));

    auto* mgr = HDREnvironmentManager::getSingleton();
    QSignalSpy spy(mgr, &HDREnvironmentManager::environmentChanged);

    ASSERT_TRUE(mgr->loadEnvironment(path));
    EXPECT_EQ(mgr->currentEnvironment(), path);
    EXPECT_FALSE(mgr->currentCacheKey().isEmpty());
    EXPECT_GT(mgr->faceSize(), 0);

    auto cubemap = mgr->cubemap();
    ASSERT_TRUE(cubemap);
    EXPECT_EQ(cubemap->getTextureType(), Ogre::TEX_TYPE_CUBE_MAP);
    EXPECT_EQ(cubemap->getWidth(), static_cast<Ogre::uint32>(mgr->faceSize()));
    EXPECT_EQ(cubemap->getHeight(), static_cast<Ogre::uint32>(mgr->faceSize()));
    EXPECT_EQ(cubemap->getNumFaces(), 6u);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(HDREnvironmentManagerOgreTest, ReloadSameFile_UsesBakeCache)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const int w = 64;
    const int h = 32;
    std::vector<float> rgb(static_cast<size_t>(w * h * 3), 1.f);
    const QString path = tmp.filePath(QStringLiteral("cached.exr"));
    ASSERT_TRUE(MinimalEXR::writeRGB32F(path, w, h, rgb));

    auto* mgr = HDREnvironmentManager::getSingleton();

    QElapsedTimer first;
    first.start();
    ASSERT_TRUE(mgr->loadEnvironment(path));
    const qint64 firstMs = first.elapsed();

    QElapsedTimer second;
    second.start();
    ASSERT_TRUE(mgr->loadEnvironment(path));
    const qint64 secondMs = second.elapsed();

    EXPECT_EQ(mgr->currentCacheKey(), HdrEquirect::sha1HexOfFile(path));
    EXPECT_LT(secondMs, firstMs + 50);
}

TEST_F(HDREnvironmentManagerOgreTest, SwitchingEnvironment_ReleasesPreviousCubemap)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const int w = 16;
    const int h = 8;
    std::vector<float> rgbA(static_cast<size_t>(w * h * 3), 0.25f);
    std::vector<float> rgbB(static_cast<size_t>(w * h * 3), 0.75f);
    const QString pathA = tmp.filePath(QStringLiteral("a.exr"));
    const QString pathB = tmp.filePath(QStringLiteral("b.exr"));
    ASSERT_TRUE(MinimalEXR::writeRGB32F(pathA, w, h, rgbA));
    ASSERT_TRUE(MinimalEXR::writeRGB32F(pathB, w, h, rgbB));

    auto* mgr = HDREnvironmentManager::getSingleton();
    ASSERT_TRUE(mgr->loadEnvironment(pathA));
    const Ogre::String firstTex = mgr->cubemap()->getName();
    ASSERT_TRUE(mgr->loadEnvironment(pathB));
    const Ogre::String secondTex = mgr->cubemap()->getName();
    EXPECT_NE(firstTex, secondTex);
    EXPECT_FALSE(Ogre::TextureManager::getSingleton().resourceExists(firstTex));
    EXPECT_TRUE(Ogre::TextureManager::getSingleton().resourceExists(secondTex));
}
#endif

TEST_F(HDREnvironmentManagerTest, LoadMissingFile_ReturnsFalse)
{
    auto* mgr = HDREnvironmentManager::getSingleton();
    EXPECT_FALSE(mgr->loadEnvironment(QStringLiteral("/no/such/environment.hdr")));
}
