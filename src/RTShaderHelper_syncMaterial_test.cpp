#include <gtest/gtest.h>

#include "EmbeddedTextureCache.h"
#include "Manager.h"
#include "RTShaderHelper.h"
#include "TestHelpers.h"

#include <OgreMaterialManager.h>
#include <OgreRTShaderSystem.h>
#include <OgreTextureManager.h>

#include <QBuffer>
#include <QImage>

class SyncMaterialForViewportTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        RTShaderHelper::initialize(Manager::getSingleton()->getSceneMgr());
        EmbeddedTextureCache::clear();
    }

    void TearDown() override
    {
        EmbeddedTextureCache::clear();
        Ogre::MaterialManager::getSingleton().remove("SyncMaterialViewportTest");
    }
};

TEST_F(SyncMaterialForViewportTest, HydratesEmbeddedTextureAndBuildsRtssTechnique)
{
    QImage img(2, 2, QImage::Format_RGBA8888);
    img.fill(Qt::red);
    QByteArray pngBytes;
    QBuffer buffer(&pngBytes);
    ASSERT_TRUE(buffer.open(QIODevice::WriteOnly));
    ASSERT_TRUE(img.save(&buffer, "PNG"));

    EmbeddedTextureCache::store(
        "sync_test_diffuse.png",
        reinterpret_cast<const std::byte*>(pngBytes.constData()),
        static_cast<std::size_t>(pngBytes.size()));

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(
        "SyncMaterialViewportTest",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    pass->setDiffuse(0.0f, 0.0f, 0.0f, 1.0f);
    Ogre::TextureUnitState* tus = pass->createTextureUnitState("sync_test_diffuse.png");
    tus->setName("diffuse_map");

    RTShaderHelper::syncMaterialForViewport(mat);

    EXPECT_TRUE(Ogre::TextureManager::getSingleton().getByName(
        "sync_test_diffuse.png", mat->getGroup()));

    pass = mat->getTechnique(0)->getPass(0);
    ASSERT_NE(pass, nullptr);
    const auto diffuse = pass->getDiffuse();
    EXPECT_GE(diffuse.r, 0.99f);
    Ogre::TextureUnitState* diffuseTus = pass->getTextureUnitState("diffuse_map");
    ASSERT_NE(diffuseTus, nullptr);
    EXPECT_EQ(diffuseTus->getTextureName(), "sync_test_diffuse.png");
}
