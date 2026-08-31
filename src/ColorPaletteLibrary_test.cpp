/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — ColorPaletteLibrary unit tests (Paint v2 Slice H, issue #551)

Pure-data: hex parsing, the bundled catalogue, JSON round-trip, the recent-colours
ring, and image colour extraction. Filesystem cases redirect <AppData> via
QStandardPaths test mode so they never touch a real install.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "ColorPaletteLibrary.h"

#include <QDir>
#include <QStandardPaths>

#include <vector>

using ColorPaletteLibrary::Palette;
using ColorPaletteLibrary::Swatch;

namespace {
Swatch mk(int r, int g, int b)
{
    Swatch s;
    s.r = static_cast<uint8_t>(r); s.g = static_cast<uint8_t>(g); s.b = static_cast<uint8_t>(b);
    return s;
}
} // namespace

// --- hex -------------------------------------------------------------------

TEST(ColorPaletteLibraryTest, HexRoundTrips) {
    Swatch s;
    ASSERT_TRUE(ColorPaletteLibrary::swatchFromHex("#4CAF50", s));
    EXPECT_EQ(s.r, 0x4C); EXPECT_EQ(s.g, 0xAF); EXPECT_EQ(s.b, 0x50);
    EXPECT_EQ(ColorPaletteLibrary::swatchToHex(s), "#4caf50");

    // The leading '#' is optional and case does not matter.
    Swatch t;
    ASSERT_TRUE(ColorPaletteLibrary::swatchFromHex("4caf50", t));
    EXPECT_TRUE(s == t);
}

TEST(ColorPaletteLibraryTest, MalformedHexIsRejectedNotSilentlyBlack) {
    // Returning false (rather than black) is what lets a corrupt file drop the
    // bad entry instead of showing a row of invisible swatches.
    Swatch s;
    for (const char* bad : {"", "#", "#12345", "#1234567", "#gg0000", "zzzzzz", "#12 34 56"})
        EXPECT_FALSE(ColorPaletteLibrary::swatchFromHex(bad, s)) << bad;
}

// --- bundled catalogue -----------------------------------------------------

TEST(ColorPaletteLibraryTest, ShipsAtLeastSixBundledPalettes) {
    const auto all = ColorPaletteLibrary::bundledPalettes();
    EXPECT_GE(all.size(), 6u) << "#551 requires at least 6 bundled palettes";
    for (const auto& p : all) {
        EXPECT_TRUE(p.isValid()) << p.name;
        EXPECT_FALSE(p.swatches.empty()) << p.name;
    }
}

TEST(ColorPaletteLibraryTest, BundledNamesAreUniqueAndFindable) {
    const auto all = ColorPaletteLibrary::bundledPalettes();
    for (const auto& p : all) {
        const Palette* found = ColorPaletteLibrary::findBundled(p.name);
        ASSERT_NE(found, nullptr) << p.name;
        EXPECT_EQ(found->swatches.size(), p.swatches.size()) << p.name;
        EXPECT_TRUE(ColorPaletteLibrary::isBundled(p.name)) << p.name;
    }
    // Names double as the custom-override key, so duplicates would make one
    // bundled palette unreachable.
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j)
            EXPECT_NE(all[i].name, all[j].name);
}

TEST(ColorPaletteLibraryTest, UnknownNameIsNotBundled) {
    EXPECT_EQ(ColorPaletteLibrary::findBundled("No Such Palette"), nullptr);
    EXPECT_FALSE(ColorPaletteLibrary::isBundled("No Such Palette"));
}

// --- JSON ------------------------------------------------------------------

TEST(ColorPaletteLibraryTest, JsonRoundTripPreservesSwatches) {
    Palette p;
    p.name = "Test Palette";
    p.swatches = {mk(255, 0, 0), mk(0, 128, 64), mk(1, 2, 3)};

    Palette back;
    ASSERT_TRUE(ColorPaletteLibrary::fromJson(ColorPaletteLibrary::toJson(p), back));
    EXPECT_EQ(back.name, p.name);
    ASSERT_EQ(back.swatches.size(), p.swatches.size());
    for (size_t i = 0; i < p.swatches.size(); ++i)
        EXPECT_TRUE(back.swatches[i] == p.swatches[i]) << i;
}

TEST(ColorPaletteLibraryTest, JsonRejectsUnusableInputButKeepsGoodSwatches) {
    Palette out;
    EXPECT_FALSE(ColorPaletteLibrary::fromJson("not json at all", out));
    EXPECT_FALSE(ColorPaletteLibrary::fromJson("[]", out));
    EXPECT_FALSE(ColorPaletteLibrary::fromJson(R"({"swatches":["#ff0000"]})", out))
        << "a palette with no name is unusable";
    EXPECT_FALSE(ColorPaletteLibrary::fromJson(R"({"name":"Empty","swatches":[]})", out))
        << "a palette with no swatches is unusable";

    // One bad entry must not lose the user's other swatches.
    ASSERT_TRUE(ColorPaletteLibrary::fromJson(
        R"({"name":"Mixed","swatches":["#ff0000","nope","#0000ff"]})", out));
    EXPECT_EQ(out.swatches.size(), 2u);
}

// --- recent colours --------------------------------------------------------

TEST(ColorPaletteLibraryTest, RecentPushesFrontAndCaps) {
    std::vector<Swatch> recent;
    for (int i = 0; i < 20; ++i)
        ColorPaletteLibrary::pushRecent(recent, mk(i, i, i), 12);
    ASSERT_EQ(recent.size(), 12u) << "must cap at maxCount";
    EXPECT_TRUE(recent.front() == mk(19, 19, 19)) << "most recent first";
    EXPECT_TRUE(recent.back() == mk(8, 8, 8));
}

TEST(ColorPaletteLibraryTest, RecentPromotesInsteadOfDuplicating) {
    std::vector<Swatch> recent;
    ColorPaletteLibrary::pushRecent(recent, mk(1, 1, 1));
    ColorPaletteLibrary::pushRecent(recent, mk(2, 2, 2));
    ColorPaletteLibrary::pushRecent(recent, mk(1, 1, 1));   // re-pick
    ASSERT_EQ(recent.size(), 2u) << "re-picking must promote, not duplicate";
    EXPECT_TRUE(recent[0] == mk(1, 1, 1));
    EXPECT_TRUE(recent[1] == mk(2, 2, 2));
}

TEST(ColorPaletteLibraryTest, RecentWithZeroCapClears) {
    std::vector<Swatch> recent{mk(9, 9, 9)};
    ColorPaletteLibrary::pushRecent(recent, mk(1, 1, 1), 0);
    EXPECT_TRUE(recent.empty());
}

// --- extraction ------------------------------------------------------------

TEST(ColorPaletteLibraryTest, ExtractFindsDominantColoursMostFrequentFirst) {
    // 4x1 image: three red pixels, one blue.
    const std::vector<uint8_t> px = {
        255, 0, 0, 255,   255, 0, 0, 255,   255, 0, 0, 255,   0, 0, 255, 255,
    };
    const auto got = ColorPaletteLibrary::extractFromImage(px.data(), 4, 1, 10);
    ASSERT_GE(got.size(), 2u);
    EXPECT_TRUE(got[0] == mk(255, 0, 0)) << "the majority colour must come first";
}

TEST(ColorPaletteLibraryTest, ExtractIgnoresFullyTransparentPixels) {
    // Mostly transparent with two opaque green pixels — a texture with a large
    // empty region must not yield "transparent black" as its main colour.
    std::vector<uint8_t> px(4 * 10, 0);              // 10 px, all alpha 0
    px[0] = 0; px[1] = 200; px[2] = 0; px[3] = 255;  // one opaque green
    px[4] = 0; px[5] = 200; px[6] = 0; px[7] = 255;  // another
    const auto got = ColorPaletteLibrary::extractFromImage(px.data(), 10, 1, 5);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_TRUE(got[0] == mk(0, 200, 0));
}

TEST(ColorPaletteLibraryTest, ExtractRespectsMaxAndHandlesDegenerateInput) {
    std::vector<uint8_t> px(4 * 64, 255);
    for (int i = 0; i < 64; ++i) {                   // 64 distinct-ish colours
        px[i * 4 + 0] = static_cast<uint8_t>(i * 4);
        px[i * 4 + 1] = static_cast<uint8_t>(255 - i * 4);
        px[i * 4 + 2] = 128;
        px[i * 4 + 3] = 255;
    }
    EXPECT_LE(ColorPaletteLibrary::extractFromImage(px.data(), 64, 1, 5).size(), 5u);
    EXPECT_TRUE(ColorPaletteLibrary::extractFromImage(nullptr, 4, 4, 5).empty());
    EXPECT_TRUE(ColorPaletteLibrary::extractFromImage(px.data(), 0, 0, 5).empty());
    EXPECT_TRUE(ColorPaletteLibrary::extractFromImage(px.data(), 64, 1, 0).empty());
}

// --- persistence (isolated from the real <AppData>) ------------------------

class ColorPaletteLibraryFileTest : public ::testing::Test
{
protected:
    void SetUp() override { QStandardPaths::setTestModeEnabled(true); }
    void TearDown() override
    {
        // Remove anything the test wrote before leaving test mode, so runs stay
        // independent (a leftover palette would make a later "must not reload"
        // assertion fail for the wrong reason).
        const std::string dir = ColorPaletteLibrary::palettesDirectory();
        if (!dir.empty()) QDir(QString::fromStdString(dir)).removeRecursively();
        QStandardPaths::setTestModeEnabled(false);
    }
};

TEST_F(ColorPaletteLibraryFileTest, SaveLoadDeleteCustomRoundTrip) {
    Palette p;
    p.name = "My Custom Palette";
    p.swatches = {mk(10, 20, 30), mk(40, 50, 60)};

    const std::string path = ColorPaletteLibrary::saveCustom(p);
    ASSERT_FALSE(path.empty());

    const auto loaded = ColorPaletteLibrary::loadCustomPalettes();
    bool found = false;
    for (const auto& q : loaded)
        if (q.name == p.name && q.swatches.size() == 2u) found = true;
    EXPECT_TRUE(found) << "a saved palette must load back";

    EXPECT_TRUE(ColorPaletteLibrary::deleteCustom(p.name));
    for (const auto& q : ColorPaletteLibrary::loadCustomPalettes())
        EXPECT_NE(q.name, p.name) << "deleted palette must not reload";
}

TEST_F(ColorPaletteLibraryFileTest, CustomOverridesBundledOfTheSameName) {
    const auto bundled = ColorPaletteLibrary::bundledPalettes();
    ASSERT_FALSE(bundled.empty());

    Palette shadow;
    shadow.name = bundled.front().name;          // deliberately collide
    shadow.swatches = {mk(1, 2, 3)};
    ASSERT_FALSE(ColorPaletteLibrary::saveCustom(shadow).empty());

    const auto all = ColorPaletteLibrary::allPalettes();
    int matches = 0;
    for (const auto& p : all) {
        if (p.name != shadow.name) continue;
        ++matches;
        EXPECT_EQ(p.swatches.size(), 1u) << "the custom version must win";
    }
    EXPECT_EQ(matches, 1) << "override must replace, not duplicate, the entry";

    ColorPaletteLibrary::deleteCustom(shadow.name);
}

TEST_F(ColorPaletteLibraryFileTest, SafeFileStemSanitisesPathCharacters) {
    // The stem becomes a filename, so separators must not survive.
    EXPECT_EQ(ColorPaletteLibrary::safeFileStem("Sky Blues"), "Sky_Blues");
    const std::string evil = ColorPaletteLibrary::safeFileStem("../../etc/passwd");
    EXPECT_EQ(evil.find('/'), std::string::npos);
    EXPECT_EQ(evil.find('.'), std::string::npos);
    EXPECT_FALSE(ColorPaletteLibrary::safeFileStem("!!!").empty())
        << "an all-punctuation name must still yield a usable stem";
}
