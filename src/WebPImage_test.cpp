#include <gtest/gtest.h>

#include "WebPImage.h"

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QTemporaryDir>

#include <OgreTextureManager.h>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <assimp/texture.h>

#include <cstring>

#define private public
#include "Assimp/MaterialProcessor.h"
#undef private

#include "EmbeddedTextureCache.h"
#include "TestHelpers.h"

namespace {

// 4x2 LOSSLESS WebP with alpha (Pillow, lossless=True, exact=True). Pixels,
// row-major: red, green, blue, white@alpha128 / (10,20,30), (200,100,50),
// transparent black, grey.
const unsigned char kLossless4x2[] = {
    0x52, 0x49, 0x46, 0x46, 0x4c, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4c,
    0x40, 0x00, 0x00, 0x00, 0x2f, 0x03, 0x40, 0x00, 0x10, 0x3f, 0x40, 0x16, 0x60, 0xb2, 0x20, 0xfa,
    0x82, 0xca, 0x5f, 0x66, 0x1a, 0x64, 0xc2, 0x10, 0xfb, 0xf5, 0xef, 0xe2, 0x52, 0x0d, 0x99, 0x80,
    0xc5, 0x74, 0xf0, 0x6b, 0xa4, 0x7f, 0x9b, 0x11, 0x48, 0xda, 0xd8, 0x7c, 0x3c, 0xf9, 0x03, 0x80,
    0x57, 0xe4, 0x90, 0x6b, 0xd6, 0x57, 0xeb, 0x41, 0x28, 0x80, 0xc6, 0x60, 0x22, 0x09, 0xa6, 0x5f,
    0x44, 0xff, 0x03, 0x1b};

// 16x16 LOSSY (VP8) WebP of a flat (200, 60, 30), quality 90.
const unsigned char kLossy16[] = {
    0x52, 0x49, 0x46, 0x46, 0x3e, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x20,
    0x32, 0x00, 0x00, 0x00, 0xd0, 0x01, 0x00, 0x9d, 0x01, 0x2a, 0x10, 0x00, 0x10, 0x00, 0x00, 0xc0,
    0x12, 0x25, 0xa0, 0x02, 0x74, 0xba, 0x01, 0xf8, 0x00, 0x03, 0xb0, 0x00, 0xfe, 0xf2, 0xa9, 0x7f,
    0xfd, 0xbb, 0x3f, 0xae, 0xcf, 0xeb, 0xb3, 0xfe, 0xed, 0xbf, 0xfe, 0xb6, 0x0f, 0xeb, 0x60, 0xfe,
    0xb6, 0x0f, 0xfe, 0xb4, 0x00, 0x00};

void expectPixel(const QImage& img, int x, int y, int r, int g, int b, int a)
{
    const QColor c = img.pixelColor(x, y);
    EXPECT_EQ(c.red(), r) << "pixel " << x << "," << y;
    EXPECT_EQ(c.green(), g) << "pixel " << x << "," << y;
    EXPECT_EQ(c.blue(), b) << "pixel " << x << "," << y;
    EXPECT_EQ(c.alpha(), a) << "pixel " << x << "," << y;
}

} // namespace

TEST(WebPImage, DetectsTheContainerByContent)
{
    EXPECT_TRUE(WebPImage::looksLikeWebP(kLossless4x2, sizeof(kLossless4x2)));
    EXPECT_TRUE(WebPImage::looksLikeWebP(kLossy16, sizeof(kLossy16)));
    const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 0};
    EXPECT_FALSE(WebPImage::looksLikeWebP(png, sizeof(png)));
    // A RIFF container that is not WebP (e.g. WAV) must not be taken for one.
    const unsigned char wav[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    EXPECT_FALSE(WebPImage::looksLikeWebP(wav, sizeof(wav)));
    EXPECT_FALSE(WebPImage::looksLikeWebP(kLossless4x2, 11));
    EXPECT_FALSE(WebPImage::looksLikeWebP(nullptr, 64));
}

TEST(WebPImage, LosslessDecodesExactPixelsIncludingAlpha)
{
    QString error;
    const QImage img = WebPImage::decode(kLossless4x2, sizeof(kLossless4x2), &error);
    ASSERT_FALSE(img.isNull()) << error.toStdString();
    EXPECT_EQ(img.format(), QImage::Format_RGBA8888);
    ASSERT_EQ(img.width(), 4);
    ASSERT_EQ(img.height(), 2);
    expectPixel(img, 0, 0, 255, 0, 0, 255);
    expectPixel(img, 1, 0, 0, 255, 0, 255);
    expectPixel(img, 2, 0, 0, 0, 255, 255);
    expectPixel(img, 3, 0, 255, 255, 255, 128);
    expectPixel(img, 0, 1, 10, 20, 30, 255);
    expectPixel(img, 1, 1, 200, 100, 50, 255);
    expectPixel(img, 2, 1, 0, 0, 0, 0);
    expectPixel(img, 3, 1, 128, 128, 128, 255);
    // Byte order is R,G,B,A in memory — what Ogre's PF_BYTE_RGBA expects.
    const uchar* row0 = img.constScanLine(0);
    EXPECT_EQ(row0[4 + 1], 255);  // pixel 1 green byte
    EXPECT_EQ(row0[12 + 3], 128); // pixel 3 alpha byte
}

TEST(WebPImage, LossyDecodesCloseToTheSourceColour)
{
    const QImage img = WebPImage::decode(kLossy16, sizeof(kLossy16));
    ASSERT_FALSE(img.isNull());
    ASSERT_EQ(img.width(), 16);
    ASSERT_EQ(img.height(), 16);
    const QColor c = img.pixelColor(8, 8);
    EXPECT_NEAR(c.red(), 200, 6);
    EXPECT_NEAR(c.green(), 60, 6);
    EXPECT_NEAR(c.blue(), 30, 6);
    EXPECT_EQ(c.alpha(), 255);
}

TEST(WebPImage, RejectsNonWebPAndTruncatedData)
{
    QString error;
    const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 0};
    EXPECT_TRUE(WebPImage::decode(png, sizeof(png), &error).isNull());
    EXPECT_FALSE(error.isEmpty());

    error.clear();
    EXPECT_TRUE(WebPImage::decode(kLossy16, 30, &error).isNull());
    EXPECT_FALSE(error.isEmpty());
}

// ── Import path ──────────────────────────────────────────────────────────────
// glTF EXT_texture_webp: Assimp (>= 6.0.3) hands the WebP bytes over as a
// compressed embedded texture with the "webp" hint; Ogre's stb_image codec
// cannot decode them, so before this fix the material came out untextured.

class WebPImportTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }
};

TEST_F(WebPImportTest, EmbeddedWebPTextureIsDecodedAndBound)
{
    auto& tm = Ogre::TextureManager::getSingleton();
    const Ogre::String texName = "*0";
    if (tm.resourceExists(texName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME))
        tm.remove(texName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    EmbeddedTextureCache::clear();

    aiScene scene;
    scene.mNumTextures = 1;
    scene.mTextures = new aiTexture*[1];
    auto* tex = new aiTexture;
    tex->mWidth = sizeof(kLossless4x2);   // compressed: byte count in mWidth
    tex->mHeight = 0;
    std::strcpy(tex->achFormatHint, "webp");
    tex->pcData = new aiTexel[(sizeof(kLossless4x2) + sizeof(aiTexel) - 1) / sizeof(aiTexel)];
    std::memcpy(tex->pcData, kLossless4x2, sizeof(kLossless4x2));
    scene.mTextures[0] = tex;

    scene.mNumMaterials = 1;
    scene.mMaterials = new aiMaterial*[1];
    scene.mMaterials[0] = new aiMaterial;
    aiString name(std::string("WebPImportTest_mat"));
    scene.mMaterials[0]->AddProperty(&name, AI_MATKEY_NAME);
    aiString texPath(std::string("*0"));
    scene.mMaterials[0]->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, 0));
    if (Ogre::MaterialManager::getSingleton().resourceExists("WebPImportTest_mat"))
        Ogre::MaterialManager::getSingleton().remove("WebPImportTest_mat");

    MaterialProcessor processor;
    processor.loadScene(&scene);
    ASSERT_EQ(processor.size(), 1u);

    Ogre::MaterialPtr mat = processor[0];
    ASSERT_TRUE(mat);
    Ogre::TexturePtr bound;
    auto* pass = mat->getTechnique(0)->getPass(0);
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (tus->getName() == "diffuse_map")
            bound = tus->_getTexturePtr();
    }
    ASSERT_TRUE(bound) << "the WebP diffuse texture was not bound (decode failed?)";
    EXPECT_EQ(bound->getWidth(), 4u);
    EXPECT_EQ(bound->getHeight(), 2u);

    // The FBX exporter re-embeds cached bytes: they must be PNG, not WebP.
    const std::vector<uint8_t> cached = EmbeddedTextureCache::retrieve(texName);
    ASSERT_GE(cached.size(), 8u);
    EXPECT_EQ(std::memcmp(cached.data(), "\x89PNG\r\n\x1a\n", 8), 0);
    const QImage fromCache = QImage::fromData(cached.data(), static_cast<int>(cached.size()), "PNG");
    ASSERT_FALSE(fromCache.isNull());
    EXPECT_EQ(fromCache.pixelColor(1, 0), QColor(0, 255, 0, 255));
    EXPECT_EQ(fromCache.pixelColor(3, 0).alpha(), 128);

    if (tm.resourceExists(texName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME))
        tm.remove(texName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    EmbeddedTextureCache::clear();
}

TEST_F(WebPImportTest, WebPFileNextToTheAssetLoadsWhateverItsExtension)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // A WebP saved under a .png name: detection is by content, not extension.
    const QString path = dir.filePath(QStringLiteral("albedo.png"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(reinterpret_cast<const char*>(kLossy16), sizeof(kLossy16));
    f.close();

    const Ogre::String texName = "WebPImportTest_file_tex";
    auto& tm = Ogre::TextureManager::getSingleton();
    MaterialProcessor processor;
    Ogre::TexturePtr t = processor.loadTextureFromFile(path, texName);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->getWidth(), 16u);
    EXPECT_EQ(t->getHeight(), 16u);
    tm.remove(texName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
}

TEST_F(WebPImportTest, NonWebPBytesStillGoThroughOgre)
{
    // A real PNG must keep the original path (Ogre's codec), unchanged.
    QImage src(3, 5, QImage::Format_RGBA8888);
    src.fill(QColor(10, 200, 30));
    QByteArray png;
    QBuffer buf(&png);
    ASSERT_TRUE(buf.open(QIODevice::WriteOnly));
    ASSERT_TRUE(src.save(&buf, "PNG"));

    const Ogre::String texName = "WebPImportTest_png_tex";
    Ogre::TexturePtr t = MaterialProcessor::loadTextureFromBytes(
        texName, png.constData(), static_cast<std::size_t>(png.size()), "png");
    ASSERT_TRUE(t);
    EXPECT_EQ(t->getWidth(), 3u);
    EXPECT_EQ(t->getHeight(), 5u);
    Ogre::TextureManager::getSingleton().remove(
        texName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
}

TEST_F(WebPImportTest, CorruptWebPThrowsInsteadOfBindingGarbage)
{
    // Header says WebP, payload is cut short: the loader must fail (callers
    // catch and leave the slot empty), never upload uninitialised pixels.
    EXPECT_ANY_THROW(MaterialProcessor::loadTextureFromBytes(
        "WebPImportTest_corrupt_tex", kLossless4x2, 24, "webp"));
}
