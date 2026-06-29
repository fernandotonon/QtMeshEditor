#include <gtest/gtest.h>

#include "HDREnvironmentManager.h"
#include "HdrEquirectLoader.h"
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

TEST_F(HDREnvironmentManagerTest, KillAndRecreate)
{
    auto* mgr = HDREnvironmentManager::getSingleton();
    ASSERT_NE(mgr, nullptr);
    HDREnvironmentManager::kill();
    auto* mgr2 = HDREnvironmentManager::getSingleton();
    ASSERT_NE(mgr2, nullptr);
}

#ifdef ENABLE_OPENEXR
TEST_F(HDREnvironmentManagerTest, LoadEnvironment_CreatesCubeMapTexture)
{
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    createStandardOgreMaterials();

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
    ASSERT_FALSE(cubemap.isNull());
    EXPECT_EQ(cubemap->getTextureType(), Ogre::TEX_TYPE_CUBE_MAP);
    EXPECT_EQ(cubemap->getWidth(), static_cast<Ogre::uint32>(mgr->faceSize()));
    EXPECT_EQ(cubemap->getHeight(), static_cast<Ogre::uint32>(mgr->faceSize()));
    EXPECT_EQ(cubemap->getNumFaces(), 6u);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(HDREnvironmentManagerTest, ReloadSameFile_UsesBakeCache)
{
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    createStandardOgreMaterials();

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
    // Second load should skip decode/bake — typically much faster.
    EXPECT_LT(secondMs, firstMs + 50);
}
#endif

TEST_F(HDREnvironmentManagerTest, LoadMissingFile_ReturnsFalse)
{
    auto* mgr = HDREnvironmentManager::getSingleton();
    EXPECT_FALSE(mgr->loadEnvironment(QStringLiteral("/no/such/environment.hdr")));
}
