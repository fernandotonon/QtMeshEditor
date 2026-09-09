/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — PaintLayerStack tests (Paint v2 Slice J, issue #553).

#553 names this file explicitly. PaintLayerBlend_test already covers the blend
MATH; this covers the STACK: ordering, visibility/solo, opacity, merge/flatten,
snapshot-restore (which every layer undo command depends on), and the geometry
rules that silently produce a blank composite when broken.

Pure data — no Qt widgets, no Ogre scene.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "PaintLayerStack.h"

#include <cstdint>
#include <vector>

namespace {

constexpr int kW = 4;
constexpr int kH = 4;

/// A buffer filled with one opaque colour.
TexturePaintBuffer solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    TexturePaintBuffer buf(kW, kH);
    auto& px = buf.data();
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
    }
    return buf;
}

/// Top-left texel of a composite.
struct Px { uint8_t r, g, b, a; };
Px firstPixel(const PaintLayerStack& s)
{
    std::vector<uint8_t> out;
    s.compositeTo(out);
    if (out.size() < 4) return {0, 0, 0, 0};
    return {out[0], out[1], out[2], out[3]};
}

/// A stack seeded with one opaque red base layer.
PaintLayerStack seeded()
{
    PaintLayerStack s;
    s.initFromFlatBuffer(solid(255, 0, 0), QStringLiteral("Base"));
    return s;
}

} // namespace

// --- geometry --------------------------------------------------------------

TEST(PaintLayerStackTest, AnEmptyStackHasNoGeometryAndCompositesToNothing) {
    PaintLayerStack s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.layerCount(), 0);
    EXPECT_EQ(s.width(), 0);
    EXPECT_EQ(s.height(), 0);
    std::vector<uint8_t> out;
    s.compositeTo(out);
    EXPECT_TRUE(out.empty()) << "an empty stack must not fabricate pixels";
}

TEST(PaintLayerStackTest, GeometryComesFromTheFirstLayer) {
    // width()/height() read layer 0, so an add that skipped resizing would give
    // a 0x0 stack and every composite would silently return nothing.
    PaintLayerStack s = seeded();
    EXPECT_EQ(s.width(), kW);
    EXPECT_EQ(s.height(), kH);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.layerCount(), 1);
}

TEST(PaintLayerStackTest, AddedLayersMatchTheStackResolution) {
    PaintLayerStack s = seeded();
    const int idx = s.addEmpty(QStringLiteral("Second"));
    ASSERT_GE(idx, 0);
    EXPECT_EQ(s.layer(idx).buffer.width(), kW)
        << "a mismatched layer size would break the composite loop";
    EXPECT_EQ(s.layer(idx).buffer.height(), kH);
}

// --- ordering --------------------------------------------------------------

TEST(PaintLayerStackTest, LayersCompositeBottomUpSoTheTopLayerWins) {
    PaintLayerStack s = seeded();                       // red base
    s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));   // opaque blue on top

    const Px p = firstPixel(s);
    EXPECT_EQ(p.b, 255) << "the LAST layer must win; got r=" << int(p.r)
                        << " b=" << int(p.b);
    EXPECT_EQ(p.r, 0);
}

TEST(PaintLayerStackTest, MoveLayerChangesWhichLayerWins) {
    PaintLayerStack s = seeded();                                  // red at 0
    s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));     // blue at 1 (wins)
    ASSERT_EQ(firstPixel(s).b, 255);

    s.moveLayer(1, 0);                                             // blue to bottom
    const Px p = firstPixel(s);
    EXPECT_EQ(p.r, 255) << "after the move, red should be on top";
    EXPECT_EQ(p.b, 0);
}

// --- visibility / solo -----------------------------------------------------

TEST(PaintLayerStackTest, HidingTheTopLayerRevealsTheOneBeneath) {
    PaintLayerStack s = seeded();
    const int blue = s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));
    s.setVisible(blue, false);
    EXPECT_EQ(firstPixel(s).r, 255) << "a hidden layer must not contribute";
}

TEST(PaintLayerStackTest, SoloOverridesVisibilityForEveryOtherLayer) {
    // This is the rule that makes "include hidden layers" impossible to express
    // through compositeTo (see the Slice I bake), so pin it deliberately.
    PaintLayerStack s = seeded();                                  // red
    const int blue = s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));
    s.setVisible(blue, true);
    s.setSolo(0, true);                                            // solo the RED base

    EXPECT_EQ(firstPixel(s).r, 255)
        << "solo must show only the soloed layer, even though blue is visible";

    s.clearSolo();
    EXPECT_EQ(firstPixel(s).b, 255) << "clearing solo restores normal visibility";
}

TEST(PaintLayerStackTest, ZeroOpacityLayerContributesNothing) {
    PaintLayerStack s = seeded();
    const int blue = s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));
    s.setOpacity(blue, 0.0);
    EXPECT_EQ(firstPixel(s).r, 255);
}

TEST(PaintLayerStackTest, HalfOpacityBlendsTowardTheLayerBelow) {
    PaintLayerStack s = seeded();                                  // red
    const int blue = s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));
    s.setOpacity(blue, 0.5);

    const Px p = firstPixel(s);
    EXPECT_GT(p.r, 40)  << "some red must survive at 50% opacity";
    EXPECT_GT(p.b, 40)  << "some blue must show at 50% opacity";
}

// --- merge / flatten -------------------------------------------------------

TEST(PaintLayerStackTest, MergeDownReducesTheCountAndKeepsTheResult) {
    PaintLayerStack s = seeded();
    s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));
    ASSERT_EQ(s.layerCount(), 2);

    s.mergeDown(1);
    EXPECT_EQ(s.layerCount(), 1) << "merge-down must consume one layer";
    EXPECT_EQ(firstPixel(s).b, 255) << "the merged pixels must be the composite";
}

TEST(PaintLayerStackTest, FlattenCollapsesToASingleLayerPreservingTheComposite) {
    PaintLayerStack s = seeded();
    s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));
    s.addEmpty(QStringLiteral("Empty"));
    ASSERT_EQ(s.layerCount(), 3);

    const Px before = firstPixel(s);
    s.flattenAll();
    EXPECT_EQ(s.layerCount(), 1);
    const Px after = firstPixel(s);
    EXPECT_EQ(after.r, before.r);
    EXPECT_EQ(after.g, before.g);
    EXPECT_EQ(after.b, before.b);
}

TEST(PaintLayerStackTest, DuplicateAddsAnIndependentCopy) {
    PaintLayerStack s = seeded();
    const int dup = s.duplicateLayer(0);
    ASSERT_GE(dup, 0);
    EXPECT_EQ(s.layerCount(), 2);
    // Independent: hiding the copy must not affect the original.
    s.setVisible(dup, false);
    EXPECT_TRUE(s.layer(0).visible);
}

TEST(PaintLayerStackTest, RemoveLayerDropsIt) {
    PaintLayerStack s = seeded();
    s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));
    s.removeLayer(1);
    EXPECT_EQ(s.layerCount(), 1);
    EXPECT_EQ(firstPixel(s).r, 255);
}

TEST(PaintLayerStackTest, RenameLayerKeepsPixelsIntact) {
    PaintLayerStack s = seeded();
    s.renameLayer(0, QStringLiteral("Renamed"));
    EXPECT_EQ(s.layer(0).name, QStringLiteral("Renamed"));
    EXPECT_EQ(firstPixel(s).r, 255);
}

// --- snapshot / restore (the basis of every layer undo command) -------------

TEST(PaintLayerStackTest, SnapshotRestoreRoundTripsTheWholeStack) {
    PaintLayerStack s = seeded();
    s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));
    s.setOpacity(1, 0.5);
    s.setActiveIndex(1);

    const auto snap = s.snapshot();

    // Destroy the state thoroughly.
    s.flattenAll();
    ASSERT_EQ(s.layerCount(), 1);

    s.restore(snap);
    EXPECT_EQ(s.layerCount(), 2) << "restore must reinstate every layer";
    EXPECT_EQ(s.activeIndex(), 1) << "and the active index";
    EXPECT_NEAR(s.layer(1).opacity, 0.5f, 1e-5f) << "and per-layer opacity";
}

TEST(PaintLayerStackTest, RestoreIsIdempotent) {
    PaintLayerStack s = seeded();
    s.addFromBuffer(solid(0, 0, 255), QStringLiteral("Blue"));
    const auto snap = s.snapshot();
    const int n = s.layerCount();

    // Undo/redo can replay one snapshot repeatedly; it must not accumulate.
    s.restore(snap);
    s.restore(snap);
    EXPECT_EQ(s.layerCount(), n);
}

// --- bounds ----------------------------------------------------------------

// PaintLayerStack does NOT bounds-check: layer(index) throws on a bad index,
// and setVisible/setOpacity/setBlendMode go straight through it.
//
// That is the class's actual contract, and it is safe because every caller
// guards first — TexturePaintController::setPaintLayer* all early-return on an
// out-of-range index before touching the stack (e.g. setPaintLayerVisible).
// An earlier version of this test asserted the opposite ("out-of-range is
// ignored"), which the class never promised; pinning the real behaviour keeps
// the guard's location honest, so anyone adding a new caller knows they own it.
TEST(PaintLayerStackTest, OutOfRangeAccessThrowsSoCallersMustBoundsCheck) {
    PaintLayerStack s = seeded();
    EXPECT_ANY_THROW(s.setVisible(99, false));
    EXPECT_ANY_THROW(s.setOpacity(-1, 0.5));
    EXPECT_ANY_THROW(s.setBlendMode(42, PaintLayerBlend::Mode::Normal));
}

// The ops that DO tolerate a bad index must stay tolerant, since they are
// reachable with a stale index from QML list delegates.
TEST(PaintLayerStackTest, StructuralOpsIgnoreOutOfRangeIndices) {
    PaintLayerStack s = seeded();
    EXPECT_NO_THROW(s.removeLayer(99));
    EXPECT_NO_THROW(s.mergeDown(99));
    EXPECT_NO_THROW(s.moveLayer(5, 9));
    EXPECT_EQ(s.layerCount(), 1) << "the stack must be unharmed";
    EXPECT_EQ(firstPixel(s).r, 255);
}

TEST(PaintLayerStackTest, ResizeAllKeepsEveryLayerInStep) {
    PaintLayerStack s = seeded();
    s.addEmpty(QStringLiteral("Second"));
    s.resizeAll(8, 8);
    EXPECT_EQ(s.width(), 8);
    for (int i = 0; i < s.layerCount(); ++i) {
        EXPECT_EQ(s.layer(i).buffer.width(), 8) << "layer " << i;
        EXPECT_EQ(s.layer(i).buffer.height(), 8) << "layer " << i;
    }
}

// --- mismatched-size layers (the all-white decal bug) -----------------------

TEST(PaintLayerStackTest, AddFromBufferRescalesInsteadOfBlankingToWhite) {
    // TexturePaintBuffer::resize() fills with 0xFF (opaque white) and drops the
    // content. addFromBuffer used to call it on any size mismatch, so a decal
    // projected at a different resolution than the stack committed as a blank
    // WHITE layer instead of its pixels. It must RESCALE the content instead.
    PaintLayerStack s = seeded();

    // A differently-sized, fully opaque BLUE source.
    TexturePaintBuffer src(kW * 2, kH * 3);
    for (size_t i = 0; i + 3 < src.data().size(); i += 4) {
        src.data()[i + 0] = 0;   src.data()[i + 1] = 0;
        src.data()[i + 2] = 255; src.data()[i + 3] = 255;
    }

    const int idx = s.addFromBuffer(src, QStringLiteral("Decal"),
                                    PaintLayerStack::LayerType::Generated);
    ASSERT_GE(idx, 0);
    EXPECT_EQ(s.layer(idx).buffer.width(), kW);
    EXPECT_EQ(s.layer(idx).buffer.height(), kH);

    const Px p = firstPixel(s);
    EXPECT_EQ(p.b, 255) << "the layer must keep its BLUE content";
    EXPECT_EQ(p.r, 0)   << "white (255,255,255) here means the content was "
                           "discarded by a bare resize()";
    EXPECT_EQ(p.g, 0);
}

TEST(PaintLayerStackTest, AddFromBufferPreservesTransparencyWhenRescaling) {
    // A decal is mostly transparent outside its footprint; rescaling must not
    // turn those texels opaque (a bare resize made them opaque white).
    PaintLayerStack s = seeded();
    TexturePaintBuffer src(kW * 2, kH * 2);   // all zeroes = fully transparent
    for (auto& b : src.data()) b = 0;

    const int idx = s.addFromBuffer(src, QStringLiteral("Empty decal"),
                                    PaintLayerStack::LayerType::Generated);
    ASSERT_GE(idx, 0);
    EXPECT_EQ(s.layer(idx).buffer.pixel(0, 0).a, 0.0f)
        << "transparent source texels must stay transparent";
    // The red base must still show through the transparent layer.
    const Px p = firstPixel(s);
    EXPECT_EQ(p.r, 255);
}
