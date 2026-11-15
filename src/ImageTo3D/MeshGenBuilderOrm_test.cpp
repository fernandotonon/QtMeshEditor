// Tests for MeshGenBuilder::packOcclusionRoughnessMetallic — the packed
// metallic-roughness texture bound to the canonical `metallic` slot.
//
// Why this matters: BOTH consumers of that slot read it as a PACKED glTF
// map (.r=AO, .g=roughness, .b=metallic) — Ogre's Cook-Torrance SRS and
// glTF's metallicRoughnessTexture. Binding a single-channel grayscale PNG
// there expands to R=G=B, so roughness is read out of the METALLIC lane and
// the real roughness map is ignored. That shipped: a generated plastic box
// whose TRELLIS metallic was 0.75 rendered as chrome with roughness 0.75
// instead of its baked 0.62.

#include <gtest/gtest.h>

#include "ImageTo3D/MeshGenBuilder.h"

#include <QImage>

namespace {

QImage grayFilled(int w, int h, int value)
{
    QImage img(w, h, QImage::Format_Grayscale8);
    img.fill(value);
    return img;
}

} // namespace

TEST(MeshGenBuilderOrm, LanesCarryTheRightChannel)
{
    const QImage rough = grayFilled(8, 8, 160);
    const QImage metal = grayFilled(8, 8, 32);

    const QImage orm = MeshGenBuilder::packOcclusionRoughnessMetallic(rough, metal);

    ASSERT_FALSE(orm.isNull());
    EXPECT_EQ(orm.width(), 8);
    EXPECT_EQ(orm.height(), 8);
    const QColor c = orm.pixelColor(3, 3);
    // Occlusion is white: a 0 lane would darken the whole surface on any
    // consumer that honours it (the Unreal ORM rule).
    EXPECT_EQ(c.red(), 255);
    EXPECT_EQ(c.green(), 160) << "green lane must carry ROUGHNESS";
    EXPECT_EQ(c.blue(), 32) << "blue lane must carry METALLIC";
}

TEST(MeshGenBuilderOrm, DoesNotCollapseDistinctInputsIntoOneValue)
{
    // The bug being guarded: a grayscale map has R==G==B, so roughness and
    // metallic become indistinguishable. Packing must keep them apart even
    // when one is much larger than the other.
    const QImage orm = MeshGenBuilder::packOcclusionRoughnessMetallic(
        grayFilled(4, 4, 200), grayFilled(4, 4, 10));

    ASSERT_FALSE(orm.isNull());
    const QColor c = orm.pixelColor(1, 1);
    EXPECT_NE(c.green(), c.blue());
    EXPECT_EQ(c.green(), 200);
    EXPECT_EQ(c.blue(), 10);
}

TEST(MeshGenBuilderOrm, MissingMetallicPacksAsFullyDielectric)
{
    // A roughness-only bake must not invent metal: the blue lane defaults to
    // 0, which is the correct "not metal" answer for every generated prop.
    const QImage orm =
        MeshGenBuilder::packOcclusionRoughnessMetallic(grayFilled(4, 4, 128), QImage());

    ASSERT_FALSE(orm.isNull());
    const QColor c = orm.pixelColor(2, 2);
    EXPECT_EQ(c.green(), 128);
    EXPECT_EQ(c.blue(), 0);
}

TEST(MeshGenBuilderOrm, MissingRoughnessUsesANeutralNotBlack)
{
    // Roughness 0 is a mirror finish. A missing map must land on a plausible
    // dull default instead, or the asset renders as polished chrome.
    const QImage orm =
        MeshGenBuilder::packOcclusionRoughnessMetallic(QImage(), grayFilled(4, 4, 64));

    ASSERT_FALSE(orm.isNull());
    const QColor c = orm.pixelColor(2, 2);
    EXPECT_GT(c.green(), 128) << "absent roughness must default to rough, not mirror";
    EXPECT_EQ(c.blue(), 64);
}

TEST(MeshGenBuilderOrm, BothNullReturnsNull)
{
    EXPECT_TRUE(
        MeshGenBuilder::packOcclusionRoughnessMetallic(QImage(), QImage()).isNull());
}

TEST(MeshGenBuilderOrm, MismatchedSizesDoNotCrashAndCoverTheLargestExtent)
{
    // Both maps come from the same bake so sizes normally match; tolerate a
    // mismatch rather than failing a whole generation over it.
    const QImage orm = MeshGenBuilder::packOcclusionRoughnessMetallic(
        grayFilled(8, 8, 100), grayFilled(4, 4, 200));

    ASSERT_FALSE(orm.isNull());
    EXPECT_EQ(orm.width(), 8);
    EXPECT_EQ(orm.height(), 8);
    // Inside both maps: each lane reads its own source.
    EXPECT_EQ(orm.pixelColor(1, 1).green(), 100);
    EXPECT_EQ(orm.pixelColor(1, 1).blue(), 200);
    // Outside the smaller map: the metallic lane falls back rather than
    // reading out of bounds.
    EXPECT_EQ(orm.pixelColor(7, 7).green(), 100);
    EXPECT_EQ(orm.pixelColor(7, 7).blue(), 0);
}
