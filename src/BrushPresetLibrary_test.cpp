/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — BrushPresetLibrary unit tests (Paint v2 Slice H, issue #551)

Pure-data: the bundled catalogue, JSON round-trip (including forward/backward
compatibility), persistence, and import/export. Filesystem cases redirect
<AppData> via QStandardPaths test mode so they never touch a real install.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "BrushAssetLibrary.h"
#include "BrushPresetLibrary.h"

#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <set>

using BrushPresetLibrary::Preset;

// --- bundled catalogue -----------------------------------------------------

TEST(BrushPresetLibraryTest, ShipsAtLeastFifteenBundledPresets) {
    const auto all = BrushPresetLibrary::bundledPresets();
    EXPECT_GE(all.size(), 15u) << "#551 requires at least 15 bundled presets";
    for (const auto& p : all) {
        EXPECT_TRUE(p.isValid()) << p.name;
        EXPECT_FALSE(p.note.empty()) << p.name << " should carry a tooltip note";
    }
}

TEST(BrushPresetLibraryTest, BundledNamesAreUniqueAndFindable) {
    const auto all = BrushPresetLibrary::bundledPresets();
    std::set<std::string> seen;
    for (const auto& p : all) {
        // The name is the lookup key AND the custom-override key, so a
        // duplicate would make one preset permanently unreachable.
        EXPECT_TRUE(seen.insert(p.name).second) << "duplicate name: " << p.name;
        ASSERT_NE(BrushPresetLibrary::findBundled(p.name), nullptr) << p.name;
        EXPECT_TRUE(BrushPresetLibrary::isBundled(p.name)) << p.name;
    }
    EXPECT_EQ(BrushPresetLibrary::findBundled("No Such Preset"), nullptr);
    EXPECT_FALSE(BrushPresetLibrary::isBundled("No Such Preset"));
}

TEST(BrushPresetLibraryTest, BundledStampReferencesResolveToRealAssets) {
    // A preset naming a stamp that does not exist would apply silently and
    // leave the user with the previous footprint — the failure would look like
    // "the preset did nothing".
    for (const auto& p : BrushPresetLibrary::bundledPresets()) {
        if (p.stamp.empty()) continue;
        const std::string path = BrushAssetLibrary::resolvePath(
            p.stamp, BrushAssetLibrary::AssetKind::Stamp);
        EXPECT_FALSE(path.empty())
            << p.name << " references missing stamp '" << p.stamp << "'";
    }
}

TEST(BrushPresetLibraryTest, BundledValuesAreInRange) {
    // Guards against a typo in the table shipping an unusable brush (e.g. a
    // negative radius or a strength above 1 that silently clamps).
    for (const auto& p : BrushPresetLibrary::bundledPresets()) {
        EXPECT_GT(p.radius, 0.0) << p.name;
        EXPECT_LE(p.radius, 1.0) << p.name;
        EXPECT_GE(p.strength, 0.0) << p.name;
        EXPECT_LE(p.strength, 1.0) << p.name;
        EXPECT_GE(p.falloff, 0.0) << p.name;
        EXPECT_LE(p.falloff, 1.0) << p.name;
        EXPECT_GE(p.spacing, 0.05) << p.name;
        EXPECT_LE(p.spacing, 2.0) << p.name;
        for (const double j : {p.scatter, p.sizeJitter, p.opacityJitter}) {
            EXPECT_GE(j, 0.0) << p.name;
            EXPECT_LE(j, 1.0) << p.name;
        }
        // A stamp footprint without a stamp name would fall back to a round
        // brush, quietly ignoring the preset's whole point.
        if (p.footprint == 2) EXPECT_FALSE(p.stamp.empty()) << p.name;
    }
}

// --- JSON ------------------------------------------------------------------

TEST(BrushPresetLibraryTest, JsonRoundTripPreservesEveryField) {
    Preset p;
    p.name = "Round Trip";
    p.tool = 4; p.radius = 0.123; p.strength = 0.44; p.falloff = 0.66;
    p.shape = 1; p.channel = 3;
    p.footprint = 2; p.stamp = "Spatter"; p.tiling = "Brick";
    p.spacing = 0.7; p.scatter = 0.3; p.sizeJitter = 0.2; p.opacityJitter = 0.1;
    p.stampRotation = 3; p.stampAngleDeg = 45.0;
    p.colorSource = 1; p.gradientMode = 2; p.rampName = "Sunset";
    p.note = "note text";

    Preset b;
    ASSERT_TRUE(BrushPresetLibrary::fromJson(BrushPresetLibrary::toJson(p), b));
    EXPECT_EQ(b.name, p.name);
    EXPECT_EQ(b.tool, p.tool);
    EXPECT_DOUBLE_EQ(b.radius, p.radius);
    EXPECT_DOUBLE_EQ(b.strength, p.strength);
    EXPECT_DOUBLE_EQ(b.falloff, p.falloff);
    EXPECT_EQ(b.shape, p.shape);
    EXPECT_EQ(b.channel, p.channel);
    EXPECT_EQ(b.footprint, p.footprint);
    EXPECT_EQ(b.stamp, p.stamp);
    EXPECT_EQ(b.tiling, p.tiling);
    EXPECT_DOUBLE_EQ(b.spacing, p.spacing);
    EXPECT_DOUBLE_EQ(b.scatter, p.scatter);
    EXPECT_DOUBLE_EQ(b.sizeJitter, p.sizeJitter);
    EXPECT_DOUBLE_EQ(b.opacityJitter, p.opacityJitter);
    EXPECT_EQ(b.stampRotation, p.stampRotation);
    EXPECT_DOUBLE_EQ(b.stampAngleDeg, p.stampAngleDeg);
    EXPECT_EQ(b.colorSource, p.colorSource);
    EXPECT_EQ(b.gradientMode, p.gradientMode);
    EXPECT_EQ(b.rampName, p.rampName);
    EXPECT_EQ(b.note, p.note);
}

TEST(BrushPresetLibraryTest, JsonFromOlderBuildKeepsDefaultsForMissingFields) {
    // A preset written before a field existed must still load, with the new
    // field taking its struct default rather than zero/garbage.
    Preset out;
    ASSERT_TRUE(BrushPresetLibrary::fromJson(
        R"({"name":"Minimal","radius":0.2})", out));
    EXPECT_EQ(out.name, "Minimal");
    EXPECT_DOUBLE_EQ(out.radius, 0.2);
    EXPECT_DOUBLE_EQ(out.strength, 1.0) << "missing field must keep its default";
    EXPECT_DOUBLE_EQ(out.spacing, 0.35) << "missing field must keep its default";
    EXPECT_EQ(out.footprint, 0);
}

TEST(BrushPresetLibraryTest, JsonRejectsUnusableInput) {
    Preset out;
    EXPECT_FALSE(BrushPresetLibrary::fromJson("not json", out));
    EXPECT_FALSE(BrushPresetLibrary::fromJson("[]", out));
    EXPECT_FALSE(BrushPresetLibrary::fromJson(R"({"radius":0.5})", out))
        << "a preset with no name has no lookup key and is unusable";
}

// --- persistence + import/export (isolated from the real <AppData>) --------

class BrushPresetLibraryFileTest : public ::testing::Test
{
protected:
    void SetUp() override { QStandardPaths::setTestModeEnabled(true); }
    void TearDown() override
    {
        const std::string dir = BrushPresetLibrary::presetsDirectory();
        if (!dir.empty()) QDir(QString::fromStdString(dir)).removeRecursively();
        QStandardPaths::setTestModeEnabled(false);
    }
};

TEST_F(BrushPresetLibraryFileTest, SaveLoadDeleteRoundTrip) {
    Preset p;
    p.name = "My Brush";
    p.radius = 0.077; p.strength = 0.33;

    ASSERT_FALSE(BrushPresetLibrary::saveCustom(p).empty());

    bool found = false;
    for (const auto& q : BrushPresetLibrary::loadCustomPresets())
        if (q.name == p.name && std::abs(q.radius - 0.077) < 1e-9) found = true;
    EXPECT_TRUE(found) << "a saved preset must load back";

    EXPECT_TRUE(BrushPresetLibrary::deleteCustom(p.name));
    for (const auto& q : BrushPresetLibrary::loadCustomPresets())
        EXPECT_NE(q.name, p.name) << "deleted preset must not reload";
}

TEST_F(BrushPresetLibraryFileTest, CustomOverridesBundledOfTheSameName) {
    const auto bundled = BrushPresetLibrary::bundledPresets();
    ASSERT_FALSE(bundled.empty());

    Preset shadow;
    shadow.name = bundled.front().name;         // deliberately collide
    shadow.radius = 0.999;
    ASSERT_FALSE(BrushPresetLibrary::saveCustom(shadow).empty());

    int matches = 0;
    for (const auto& p : BrushPresetLibrary::allPresets()) {
        if (p.name != shadow.name) continue;
        ++matches;
        EXPECT_DOUBLE_EQ(p.radius, 0.999) << "the custom version must win";
    }
    EXPECT_EQ(matches, 1) << "override must replace, not duplicate, the entry";

    Preset viaFind;
    ASSERT_TRUE(BrushPresetLibrary::findPreset(shadow.name, viaFind));
    EXPECT_DOUBLE_EQ(viaFind.radius, 0.999);

    BrushPresetLibrary::deleteCustom(shadow.name);
}

TEST_F(BrushPresetLibraryFileTest, FindPresetSeesBundledAndMissesUnknown) {
    Preset out;
    EXPECT_TRUE(BrushPresetLibrary::findPreset("Soft Round", out));
    EXPECT_EQ(out.name, "Soft Round");
    EXPECT_FALSE(BrushPresetLibrary::findPreset("Nope", out));
}

TEST_F(BrushPresetLibraryFileTest, ExportImportRoundTripsThroughAFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path =
        tmp.filePath(QStringLiteral("brush.json")).toStdString();

    Preset p;
    p.name = "Exported"; p.radius = 0.25; p.stamp = "Charcoal";
    ASSERT_TRUE(BrushPresetLibrary::exportToFile(p, path));

    Preset back;
    ASSERT_TRUE(BrushPresetLibrary::importFromFile(path, back));
    EXPECT_EQ(back.name, "Exported");
    EXPECT_DOUBLE_EQ(back.radius, 0.25);
    EXPECT_EQ(back.stamp, "Charcoal");

    // Missing / unparseable files must fail cleanly, not crash or half-fill.
    Preset junk;
    EXPECT_FALSE(BrushPresetLibrary::importFromFile(
        tmp.filePath(QStringLiteral("nope.json")).toStdString(), junk));
    EXPECT_FALSE(BrushPresetLibrary::exportToFile(Preset{}, path))
        << "an unnamed preset is not exportable";
}

TEST_F(BrushPresetLibraryFileTest, SafeFileStemSanitisesPathCharacters) {
    EXPECT_EQ(BrushPresetLibrary::safeFileStem("Soft Round"), "Soft_Round");
    const std::string evil = BrushPresetLibrary::safeFileStem("../../etc/passwd");
    EXPECT_EQ(evil.find('/'), std::string::npos);
    EXPECT_EQ(evil.find('.'), std::string::npos);
    EXPECT_FALSE(BrushPresetLibrary::safeFileStem("***").empty());
}

TEST_F(BrushPresetLibraryFileTest, NamesCollidingAfterSanitisationDoNotOverwrite) {
    // "My Brush" and "My/Brush" both sanitise to "My_Brush"; the hashed suffix
    // is what keeps them as two distinct files.
    Preset a; a.name = "My Brush"; a.radius = 0.1;
    Preset b; b.name = "My/Brush"; b.radius = 0.2;
    ASSERT_FALSE(BrushPresetLibrary::saveCustom(a).empty());
    ASSERT_FALSE(BrushPresetLibrary::saveCustom(b).empty());

    const auto loaded = BrushPresetLibrary::loadCustomPresets();
    EXPECT_EQ(loaded.size(), 2u) << "colliding stems must not overwrite each other";

    BrushPresetLibrary::deleteCustom(a.name);
    BrushPresetLibrary::deleteCustom(b.name);
}

TEST(BrushPresetLibraryTest, JsonRoundTripsFgBgRampFlag) {
    // The FG/BG flag must be explicit in JSON: inferring it from an empty
    // rampName would make an old file with no name silently become FG/BG.
    BrushPresetLibrary::Preset p;
    p.name = "FgBg"; p.colorSource = 1; p.useFgBgRamp = true;
    BrushPresetLibrary::Preset b;
    ASSERT_TRUE(BrushPresetLibrary::fromJson(BrushPresetLibrary::toJson(p), b));
    EXPECT_TRUE(b.useFgBgRamp);

    p.useFgBgRamp = false; p.rampName = "Sunset";
    ASSERT_TRUE(BrushPresetLibrary::fromJson(BrushPresetLibrary::toJson(p), b));
    EXPECT_FALSE(b.useFgBgRamp);
    EXPECT_EQ(b.rampName, "Sunset");
}
