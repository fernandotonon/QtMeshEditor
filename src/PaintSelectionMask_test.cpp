#include <gtest/gtest.h>

#include "PaintSelectionMask.h"
#include "TexturePaintBuffer.h"

#include <OgreColourValue.h>

TEST(PaintSelectionMaskTest, DefaultIsEmpty)
{
    PaintSelectionMask m;
    m.resize(8, 4);
    EXPECT_EQ(m.width(), 8);
    EXPECT_EQ(m.height(), 4);
    EXPECT_TRUE(m.isEmpty());
    EXPECT_EQ(m.selectedCount(), 0);
    EXPECT_TRUE(m.bbox().empty());
}

TEST(PaintSelectionMaskTest, SetSelectedUpdatesCountAndBBox)
{
    PaintSelectionMask m;
    m.resize(8, 8);
    m.setSelected(2, 3, true);
    m.setSelected(5, 7, true);
    EXPECT_EQ(m.selectedCount(), 2);
    EXPECT_FALSE(m.isEmpty());
    const auto& b = m.bbox();
    EXPECT_EQ(b.x0, 2);
    EXPECT_EQ(b.y0, 3);
    EXPECT_EQ(b.x1, 6);
    EXPECT_EQ(b.y1, 8);
}

TEST(PaintSelectionMaskTest, SetSelectedOutOfBoundsIsNoop)
{
    PaintSelectionMask m;
    m.resize(4, 4);
    m.setSelected(-1, -1, true);
    m.setSelected(4, 4, true);
    EXPECT_TRUE(m.isEmpty());
}

TEST(PaintSelectionMaskTest, SelectAllAndInvert)
{
    PaintSelectionMask m;
    m.resize(4, 4);
    m.selectAll();
    EXPECT_EQ(m.selectedCount(), 16);
    EXPECT_FALSE(m.isEmpty());
    m.invert();
    EXPECT_TRUE(m.isEmpty());
    m.invert();
    EXPECT_EQ(m.selectedCount(), 16);
}

TEST(PaintSelectionMaskTest, ClearResetsAll)
{
    PaintSelectionMask m;
    m.resize(4, 4);
    m.selectAll();
    m.clear();
    EXPECT_TRUE(m.isEmpty());
    EXPECT_TRUE(m.bbox().empty());
}

TEST(PaintSelectionMaskTest, SmartSelectExactColorMatchOnly)
{
    TexturePaintBuffer buf(4, 4);
    // Default = opaque white. Paint a 2x2 red corner.
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            buf.setPixel(x, y, Ogre::ColourValue::Red);

    PaintSelectionMask m;
    m.resize(4, 4);
    const int n = m.smartSelect(buf, 0, 0, 0.0f);
    EXPECT_EQ(n, 4);
    EXPECT_EQ(m.selectedCount(), 4);
    EXPECT_TRUE(m.isSelected(0, 0));
    EXPECT_TRUE(m.isSelected(1, 1));
    EXPECT_FALSE(m.isSelected(2, 2));  // white, not red
}

TEST(PaintSelectionMaskTest, SmartSelectAddMode)
{
    TexturePaintBuffer buf(4, 4);
    // All white. Set one pixel red.
    buf.setPixel(0, 0, Ogre::ColourValue::Red);

    PaintSelectionMask m;
    m.resize(4, 4);
    // Replace: pick red pixel — should select 1.
    m.smartSelect(buf, 0, 0, 0.0f, PaintSelectionMask::CombineMode::Replace);
    EXPECT_EQ(m.selectedCount(), 1);

    // Add: pick a white pixel — should grow to include all white.
    m.smartSelect(buf, 3, 3, 0.0f, PaintSelectionMask::CombineMode::Add);
    // 1 (red) + 15 (white pixels) = 16
    EXPECT_EQ(m.selectedCount(), 16);
}

TEST(PaintSelectionMaskTest, SmartSelectSubMode)
{
    TexturePaintBuffer buf(4, 4);
    PaintSelectionMask m;
    m.resize(4, 4);
    m.selectAll();
    EXPECT_EQ(m.selectedCount(), 16);

    // Subtract the white flood from the all-selected mask.
    const int n = m.smartSelect(buf, 0, 0, 0.0f, PaintSelectionMask::CombineMode::Sub);
    EXPECT_EQ(n, 16);
    EXPECT_TRUE(m.isEmpty());
}

TEST(PaintSelectionMaskTest, SmartSelectToleranceExpandsRegion)
{
    TexturePaintBuffer buf(4, 4);
    // Plant a near-white pixel (off by 10/255 = 0.039 per channel).
    Ogre::ColourValue offWhite(245.0f/255.0f, 245.0f/255.0f, 245.0f/255.0f, 1.0f);
    buf.setPixel(2, 2, offWhite);

    PaintSelectionMask m;
    m.resize(4, 4);
    // Strict: only white selected (off-white blocks the fill at (2,2)).
    m.smartSelect(buf, 0, 0, 0.0f);
    const int strict = m.selectedCount();

    // Wide tolerance: the off-white pixel falls within range, so the
    // flood reaches the entire 4×4.
    m.smartSelect(buf, 0, 0, 0.1f);
    const int wide = m.selectedCount();
    EXPECT_LT(strict, wide);
    EXPECT_EQ(wide, 16);
}

TEST(PaintSelectionMaskTest, SmartSelectOutOfBoundsSeedReturnsZero)
{
    TexturePaintBuffer buf(4, 4);
    PaintSelectionMask m;
    m.resize(4, 4);
    EXPECT_EQ(m.smartSelect(buf, -1, 0, 0.5f), 0);
    EXPECT_EQ(m.smartSelect(buf, 0, 99, 0.5f), 0);
    EXPECT_TRUE(m.isEmpty());
}

TEST(PaintSelectionMaskTest, SmartSelectMaskSizeMismatchReturnsZero)
{
    TexturePaintBuffer buf(4, 4);
    PaintSelectionMask m;
    m.resize(2, 2); // wrong size
    EXPECT_EQ(m.smartSelect(buf, 0, 0, 0.5f), 0);
}
