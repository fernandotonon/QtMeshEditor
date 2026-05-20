#include <gtest/gtest.h>

#include "Manager.h"
#include "ModelTurntableRenderer.h"
#include "PrimitiveObject.h"
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
