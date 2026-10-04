#include <gtest/gtest.h>

#include "Manager.h"
#include "ModelTurntableRenderer.h"
#include "PrimitiveObject.h"
#include "RTShaderHelper.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <QApplication>
#include <QCoreApplication>

class ModelTurntableRendererTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ModelTurntableRenderer::shutdown();
    }

    void TearDown() override { ModelTurntableRenderer::shutdown(); }
};

TEST_F(ModelTurntableRendererTest, RejectsEmptyEntityList)
{
    QList<QImage> frames;
    QString err;
    EXPECT_FALSE(ModelTurntableRenderer::renderToImages({}, TurntableOptions{}, &frames, &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(frames.isEmpty());
}

TEST_F(ModelTurntableRendererTest, RejectsNullBoundsWhenEntitiesAreNull)
{
    QList<QImage> frames;
    QString err;

    QList<Ogre::Entity*> entities;
    entities.append(nullptr);

    EXPECT_FALSE(ModelTurntableRenderer::renderToImages(entities, TurntableOptions{}, &frames, &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(frames.isEmpty());
}

TEST_F(ModelTurntableRendererTest, ClampsMinimumSizeAndFrameCount)
{
    PrimitiveObject::createCube(QStringLiteral("TurntableMinClampCube"));

    QList<Ogre::Entity*> entities;
    for (auto* obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entities.append(static_cast<Ogre::Entity*>(obj));
    }
    ASSERT_FALSE(entities.isEmpty());

    TurntableOptions options;
    options.width = 1;
    options.height = 1;
    options.frameCount = 0;

    QList<QImage> frames;
    QString err;
    ASSERT_TRUE(ModelTurntableRenderer::renderToImages(entities, options, &frames, &err)) << err.toStdString();
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(frames.first().width(), 16);
    EXPECT_EQ(frames.first().height(), 16);
}

TEST_F(ModelTurntableRendererTest, RendersFramesForPrimitive)
{
    PrimitiveObject::createSphere(QStringLiteral("TurntableTestSphere"));

    QList<Ogre::Entity *> entities;
    for (auto *obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entities.append(static_cast<Ogre::Entity *>(obj));
    }
    ASSERT_FALSE(entities.isEmpty());

    TurntableOptions options;
    options.width = 128;
    options.height = 128;
    options.frameCount = 4;

    QList<QImage> frames;
    QString err;
    ASSERT_TRUE(ModelTurntableRenderer::renderToImages(entities, options, &frames, &err)) << err.toStdString();
    ASSERT_EQ(frames.size(), 4);
    for (const QImage &img : frames) {
        EXPECT_EQ(img.width(), 128);
        EXPECT_EQ(img.height(), 128);
        EXPECT_FALSE(img.isNull());
    }
}

TEST_F(ModelTurntableRendererTest, ComposeSpriteSheet)
{
    QList<QImage> frames;
    frames << QImage(4, 4, QImage::Format_RGBA8888);
    frames << QImage(4, 4, QImage::Format_RGBA8888);
    const QImage sheet = ModelTurntableRenderer::composeSpriteSheet(frames, 0);
    EXPECT_EQ(sheet.width(), 8);
    EXPECT_EQ(sheet.height(), 4);
}

TEST(ModelTurntableParseAxisStatic, NullOutReturnsFalse)
{
    EXPECT_FALSE(ModelTurntableRenderer::parseAxis("y", nullptr));
}

TEST(ModelTurntableParseAxisStatic, TrimsWhitespace)
{
    TurntableAxis axis = TurntableAxis::Y;
    ASSERT_TRUE(ModelTurntableRenderer::parseAxis("  Z ", &axis));
    EXPECT_EQ(axis, TurntableAxis::Z);
}

TEST_F(ModelTurntableRendererTest, RejectsNullOutputFrameList)
{
    QString err;
    QList<Ogre::Entity *> entities;
    EXPECT_FALSE(ModelTurntableRenderer::renderToImages(entities, TurntableOptions{}, nullptr, &err));
    EXPECT_FALSE(err.isEmpty());
}

TEST_F(ModelTurntableRendererTest, RendersFramesAxisXAndZ)
{
    PrimitiveObject::createCube(QStringLiteral("TurntableAxisCube"));

    QList<Ogre::Entity *> entities;
    for (auto *obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entities.append(static_cast<Ogre::Entity *>(obj));
    }
    ASSERT_FALSE(entities.isEmpty());

    TurntableOptions opt;
    opt.width = 64;
    opt.height = 64;
    opt.frameCount = 3;
    opt.axis = TurntableAxis::X;

    QList<QImage> frames;
    QString err;
    ASSERT_TRUE(ModelTurntableRenderer::renderToImages(entities, opt, &frames, &err)) << err.toStdString();
    ASSERT_EQ(static_cast<int>(frames.size()), 3);

    opt.axis = TurntableAxis::Z;
    frames.clear();
    ASSERT_TRUE(ModelTurntableRenderer::renderToImages(entities, opt, &frames, &err)) << err.toStdString();
    EXPECT_EQ(static_cast<int>(frames.size()), 3);
}

TEST(ModelTurntableComposeSheetStatic, EmptyReturnsNullImage)
{
    EXPECT_TRUE(ModelTurntableRenderer::composeSpriteSheet({}, 4).isNull());
}

TEST(ModelTurntableComposeSheetStatic, ColumnsLayoutTwoByTwo)
{
    QList<QImage> frames;
    for (int i = 0; i < 4; ++i)
        frames << QImage(10, 10, QImage::Format_RGBA8888);
    const QImage sheet = ModelTurntableRenderer::composeSpriteSheet(frames, 2);
    EXPECT_FALSE(sheet.isNull());
    EXPECT_EQ(sheet.width(), 20);
    EXPECT_EQ(sheet.height(), 20);
}

TEST(ModelTurntableComposeSheetStatic, SkipsMismatchedFrameSizes)
{
    QList<QImage> frames;
    frames << QImage(8, 8, QImage::Format_RGBA8888);
    frames << QImage(16, 16, QImage::Format_RGBA8888); // skipped in painter loop
    const QImage sheet = ModelTurntableRenderer::composeSpriteSheet(frames, 0);
    ASSERT_FALSE(sheet.isNull());
    EXPECT_EQ(sheet.width(), 16); // only first frame drawn (second skipped → transparent hole)
    EXPECT_EQ(sheet.height(), 8);
}

TEST_F(ModelTurntableRendererTest, ShutdownIsIdempotent)
{
    ModelTurntableRenderer::shutdown();
    ModelTurntableRenderer::shutdown();
}

TEST_F(ModelTurntableRendererTest, ExcludeNormalMapFromFfpChainRemovesDuplicateUnits)
{
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);
    RTShaderHelper::initialize(sceneMgr);

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(
        "TurntableDupNormalMat", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* pass = mat->getTechnique(0)->getPass(0);
    pass->createTextureUnitState("diffuse.png")->setName("diffuse_map");
    pass->createTextureUnitState("body_normal.png")->setName("NormalMap");
    pass->createTextureUnitState("body_normal.png")->setName("NormalMap");

    RTShaderHelper::finalizeShaderGenMaterial(mat, "body_normal.png");

    unsigned short normalUnits = 0;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        const auto& n = pass->getTextureUnitState(i)->getName();
        if (n == "normal_map" || n == "NormalMap")
            ++normalUnits;
    }
    EXPECT_EQ(normalUnits, 1u);

    RTShaderHelper::shutdown(sceneMgr);
}

// Rendering frames a model by shifting its scene node so the bounds centre
// sits at the world origin. The CLI exits right after, so it never noticed —
// but in-editor callers (the #521 Pose Library thumbnail) render a LIVE scene
// entity, and without a restore the model stayed parked at the origin after
// every thumbnail. The node must come back exactly where it started.
TEST_F(ModelTurntableRendererTest, RestoresNodePositionAfterRender)
{
    PrimitiveObject::createCube(QStringLiteral("TurntableRecenterCube"));

    QList<Ogre::Entity*> entities;
    for (auto* obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entities.append(static_cast<Ogre::Entity*>(obj));
    }
    ASSERT_FALSE(entities.isEmpty());

    Ogre::SceneNode* node = entities.first()->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    // Park the model well away from the origin, the way an author's scene is.
    const Ogre::Vector3 original(3.0f, 7.5f, -2.25f);
    node->setPosition(original);

    TurntableOptions options;
    options.width = 32;
    options.height = 32;
    options.frameCount = 1;

    QList<QImage> frames;
    QString err;
    ASSERT_TRUE(ModelTurntableRenderer::renderToImages(entities, options, &frames, &err))
        << err.toStdString();

    const Ogre::Vector3 after = node->getPosition();
    EXPECT_NEAR(after.x, original.x, 1e-3f);
    EXPECT_NEAR(after.y, original.y, 1e-3f);
    EXPECT_NEAR(after.z, original.z, 1e-3f);
}

// prepareSceneForCapture clears the editor's selection so the capture has no
// highlight. The CLI exits right after rendering so it never noticed, but the
// #521 Pose Library renders thumbnails against the LIVE scene — so saving a
// pose deselected whatever the author had selected. The selection must come
// back exactly as it was.
TEST_F(ModelTurntableRendererTest, RestoresSelectionAfterRender)
{
    PrimitiveObject::createCube(QStringLiteral("TurntableSelectionCube"));

    QList<Ogre::Entity*> entities;
    for (auto* obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entities.append(static_cast<Ogre::Entity*>(obj));
    }
    ASSERT_FALSE(entities.isEmpty());

    auto* sel = SelectionSet::getSingleton();
    ASSERT_NE(sel, nullptr);
    sel->clear();
    sel->selectOne(entities.first());
    ASSERT_EQ(sel->getEntitiesSelectionList().size(), 1);

    TurntableOptions options;
    options.width = 32;
    options.height = 32;
    options.frameCount = 1;

    QList<QImage> frames;
    QString err;
    ASSERT_TRUE(ModelTurntableRenderer::renderToImages(entities, options, &frames, &err))
        << err.toStdString();

    const auto after = sel->getEntitiesSelectionList();
    ASSERT_EQ(after.size(), 1) << "render cleared the author's selection";
    EXPECT_EQ(after.first(), entities.first());

    sel->clear();
}
