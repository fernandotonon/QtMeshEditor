#include <gtest/gtest.h>

#include "Manager.h"
#include "ModelIsometricRenderer.h"
#include "PrimitiveObject.h"
#include "TestHelpers.h"

#include <QApplication>
#include <QCoreApplication>

class ModelIsometricRendererTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "Mesh resources unavailable in test environment";
        ModelIsometricRenderer::shutdown();
    }

    void TearDown() override { ModelIsometricRenderer::shutdown(); }
};

TEST_F(ModelIsometricRendererTest, RejectsEmptyEntityList)
{
    QList<QList<QImage>> grid;
    QString err;
    EXPECT_FALSE(ModelIsometricRenderer::renderToGrid({}, nullptr, {}, 1, IsometricOptions{}, &grid, &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(grid.isEmpty());
}

TEST_F(ModelIsometricRendererTest, RejectsNullBoundsWhenEntitiesAreNull)
{
    QList<QList<QImage>> grid;
    QString err;

    QList<Ogre::Entity *> entities;
    entities.append(nullptr);

    EXPECT_FALSE(ModelIsometricRenderer::renderToGrid(entities, nullptr, {}, 1, IsometricOptions{}, &grid, &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(grid.isEmpty());
}

TEST_F(ModelIsometricRendererTest, ClampsMinimumSizeAndDirectionCount)
{
    PrimitiveObject::createCube(QStringLiteral("IsoMinClampCube"));

    QList<Ogre::Entity *> entities;
    for (auto *obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entities.append(static_cast<Ogre::Entity *>(obj));
    }
    ASSERT_FALSE(entities.isEmpty());

    IsometricOptions options;
    options.width = 1;
    options.height = 1;
    options.directionCount = 0;

    QList<QList<QImage>> grid;
    QString err;
    ASSERT_TRUE(ModelIsometricRenderer::renderToGrid(entities, nullptr, {}, 1, options, &grid, &err)) << err.toStdString();
    ASSERT_EQ(grid.size(), 1);
    ASSERT_EQ(grid.first().size(), 1);
    EXPECT_EQ(grid.first().first().width(), 16);
    EXPECT_EQ(grid.first().first().height(), 16);
}

TEST_F(ModelIsometricRendererTest, RejectsOversizedGrid)
{
    PrimitiveObject::createCube(QStringLiteral("IsoOversizeCube"));

    QList<Ogre::Entity *> entities;
    for (auto *obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entities.append(static_cast<Ogre::Entity *>(obj));
    }
    ASSERT_FALSE(entities.isEmpty());

    IsometricOptions options;
    options.width = 512;
    options.height = 512;
    options.directionCount = 64;

    QList<QList<QImage>> grid;
    QString err;
    EXPECT_FALSE(ModelIsometricRenderer::renderToGrid(entities, nullptr, {}, 360, options, &grid, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("Grid too large")));
    EXPECT_TRUE(grid.isEmpty());
}

TEST_F(ModelIsometricRendererTest, StaticGridDimensions)
{
    PrimitiveObject::createSphere(QStringLiteral("IsoTestSphere"));

    QList<Ogre::Entity *> entities;
    for (auto *obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entities.append(static_cast<Ogre::Entity *>(obj));
    }
    ASSERT_FALSE(entities.isEmpty());

    IsometricOptions options;
    options.width = 64;
    options.height = 48;
    options.directionCount = 4;

    QList<QList<QImage>> grid;
    QString err;
    ASSERT_TRUE(ModelIsometricRenderer::renderToGrid(entities, nullptr, {}, 1, options, &grid, &err)) << err.toStdString();
    ASSERT_EQ(grid.size(), 4);
    for (const QList<QImage> &row : grid) {
        ASSERT_EQ(row.size(), 1);
        EXPECT_EQ(row.first().width(), 64);
        EXPECT_EQ(row.first().height(), 48);
    }

    const QImage sheet = ModelIsometricRenderer::composeDirectionGrid(grid);
    EXPECT_EQ(sheet.width(), 64);
    EXPECT_EQ(sheet.height(), 48 * 4);
}

TEST_F(ModelIsometricRendererTest, ComposeDirectionGrid)
{
    QList<QList<QImage>> grid;
    for (int dir = 0; dir < 2; ++dir) {
        QList<QImage> row;
        for (int frame = 0; frame < 3; ++frame)
            row << QImage(10, 8, QImage::Format_RGBA8888);
        grid << row;
    }
    const QImage sheet = ModelIsometricRenderer::composeDirectionGrid(grid);
    EXPECT_EQ(sheet.width(), 30);
    EXPECT_EQ(sheet.height(), 16);
}

TEST(ModelIsometricDirectionOrderStatic, DescribesRowZeroFront)
{
    EXPECT_FALSE(ModelIsometricRenderer::directionOrderConvention().isEmpty());
    EXPECT_TRUE(ModelIsometricRenderer::directionOrderConvention().contains(QStringLiteral("Row 0")));
}

TEST_F(ModelIsometricRendererTest, RejectsNullOutputGrid)
{
    QString err;
    QList<Ogre::Entity *> entities;
    EXPECT_FALSE(ModelIsometricRenderer::renderToGrid(entities, nullptr, {}, 1, IsometricOptions{}, nullptr, &err));
    EXPECT_FALSE(err.isEmpty());
}

TEST_F(ModelIsometricRendererTest, ShutdownIsIdempotent)
{
    ModelIsometricRenderer::shutdown();
    ModelIsometricRenderer::shutdown();
}

TEST(ModelIsometricComposeGridStatic, EmptyReturnsNullImage)
{
    EXPECT_TRUE(ModelIsometricRenderer::composeDirectionGrid({}).isNull());
}
