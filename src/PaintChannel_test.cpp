/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — PaintChannel unit tests (Paint v2 Slice D, issue #547)

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>
#include <string>

#include "PaintChannel.h"

using namespace PaintChannelNS;

TEST(PaintChannelTest, ScalarClassification) {
    EXPECT_TRUE(isScalar(Channel::Roughness));
    EXPECT_TRUE(isScalar(Channel::Metallic));
    EXPECT_TRUE(isScalar(Channel::AO));
    EXPECT_TRUE(isScalar(Channel::Height));

    EXPECT_FALSE(isScalar(Channel::BaseColor));
    EXPECT_FALSE(isScalar(Channel::Normal));
    EXPECT_FALSE(isScalar(Channel::Emissive));
    EXPECT_FALSE(isScalar(Channel::VertexColor));
}

TEST(PaintChannelTest, ColorClassification) {
    EXPECT_TRUE(isColor(Channel::BaseColor));
    EXPECT_TRUE(isColor(Channel::Emissive));
    EXPECT_FALSE(isColor(Channel::Roughness));
    EXPECT_FALSE(isColor(Channel::Normal));
}

TEST(PaintChannelTest, CanonicalSlotNames) {
    EXPECT_STREQ(slotName(Channel::BaseColor), "albedo");
    EXPECT_STREQ(slotName(Channel::Normal),    "normal_map");
    EXPECT_STREQ(slotName(Channel::Roughness), "roughness");
    EXPECT_STREQ(slotName(Channel::Metallic),  "metallic");
    EXPECT_STREQ(slotName(Channel::AO),        "ao");
    EXPECT_STREQ(slotName(Channel::Emissive),  "emissive");
    // Height bakes into Normal — no direct slot.
    EXPECT_STREQ(slotName(Channel::Height), "");
    // VertexColor is not a texture channel.
    EXPECT_STREQ(slotName(Channel::VertexColor), "");
}

TEST(PaintChannelTest, IdRoundTrip) {
    for (int i = 0; i < static_cast<int>(Channel::Count); ++i) {
        auto c = static_cast<Channel>(i);
        EXPECT_EQ(fromId(id(c)), c) << "round-trip failed for channel index " << i;
    }
    EXPECT_EQ(fromId("not-a-channel"), Channel::Count);
    EXPECT_EQ(fromId(""), Channel::Count);
}

TEST(PaintChannelTest, IdsAreUniqueAndNonEmpty) {
    for (int i = 0; i < static_cast<int>(Channel::Count); ++i) {
        auto c = static_cast<Channel>(i);
        EXPECT_FALSE(std::string(id(c)).empty());
        EXPECT_FALSE(std::string(label(c)).empty());
        for (int j = i + 1; j < static_cast<int>(Channel::Count); ++j) {
            EXPECT_STRNE(id(c), id(static_cast<Channel>(j)));
        }
    }
}

TEST(PaintChannelTest, TexturePaintChannelCountExcludesVertexColor) {
    // The brush-panel picker shows every channel except VertexColor.
    EXPECT_EQ(kTexturePaintChannelCount,
              static_cast<int>(Channel::Count) - 1);
    // VertexColor is the last enumerator so the first N are the picker set.
    EXPECT_EQ(static_cast<Channel>(kTexturePaintChannelCount), Channel::VertexColor);
}

TEST(PaintChannelTest, DefaultBrushColorIsWhiteAndInRange) {
    for (int i = 0; i < kTexturePaintChannelCount; ++i) {
        int r = -1, g = -1, b = -1;
        defaultBrushColor(static_cast<Channel>(i), r, g, b);
        // White default for every channel (max scalar value / any hue).
        EXPECT_EQ(r, 255);
        EXPECT_EQ(g, 255);
        EXPECT_EQ(b, 255);
    }
}
