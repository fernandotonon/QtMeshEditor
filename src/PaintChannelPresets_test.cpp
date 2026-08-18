/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — PaintChannelPresets unit tests (Paint v2 Slice D #547)

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>

#include "PaintChannelPresets.h"
#include "PaintChannel.h"

using C = PaintChannelNS::Channel;

// The five presets from issue #547 exist, in a stable order, each non-empty.
TEST(PaintChannelPresetsTest, FivePresetsPresent) {
    const auto& presets = PaintChannelPresets::presets();
    ASSERT_EQ(presets.size(), 5);
    const QStringList expected = {
        "Scratches into roughness", "Emissive sparks", "Edge wear",
        "Dirt build-up", "Sticker"
    };
    for (int i = 0; i < expected.size(); ++i)
        EXPECT_EQ(presets[i].name, expected[i]);
}

// Each preset targets the channel its name implies.
TEST(PaintChannelPresetsTest, PresetsTargetExpectedChannels) {
    PaintChannelPresets::Preset p;
    ASSERT_TRUE(PaintChannelPresets::findPreset("Scratches into roughness", p));
    EXPECT_EQ(p.channel, C::Roughness);
    ASSERT_TRUE(PaintChannelPresets::findPreset("Emissive sparks", p));
    EXPECT_EQ(p.channel, C::Emissive);
    ASSERT_TRUE(PaintChannelPresets::findPreset("Edge wear", p));
    EXPECT_EQ(p.channel, C::Metallic);
    ASSERT_TRUE(PaintChannelPresets::findPreset("Dirt build-up", p));
    EXPECT_EQ(p.channel, C::AO);
    ASSERT_TRUE(PaintChannelPresets::findPreset("Sticker", p));
    EXPECT_EQ(p.channel, C::BaseColor);
}

// Brush params are within sane ranges for every preset.
TEST(PaintChannelPresetsTest, PresetParamsAreSane) {
    for (const auto& p : PaintChannelPresets::presets()) {
        EXPECT_GT(p.radius, 0.0)   << p.name.toStdString();
        EXPECT_GE(p.strength, 0.0); EXPECT_LE(p.strength, 1.0);
        EXPECT_GE(p.falloff, 0.0);
        EXPECT_GE(p.colorR, 0); EXPECT_LE(p.colorR, 255);
        EXPECT_GE(p.colorG, 0); EXPECT_LE(p.colorG, 255);
        EXPECT_GE(p.colorB, 0); EXPECT_LE(p.colorB, 255);
        EXPECT_FALSE(p.note.isEmpty()) << p.name.toStdString();
    }
}

TEST(PaintChannelPresetsTest, PresetNamesMatchTable) {
    // presetNames() (instance method) must mirror the static table order.
    QStringList tableNames;
    for (const auto& p : PaintChannelPresets::presets()) tableNames << p.name;
    EXPECT_EQ(PaintChannelPresets::instance()->presetNames(), tableNames);
}

TEST(PaintChannelPresetsTest, UnknownPresetNotFound) {
    PaintChannelPresets::Preset p;
    EXPECT_FALSE(PaintChannelPresets::findPreset("Nope", p));
}
