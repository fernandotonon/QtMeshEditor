#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/GpuCommandParser.h"

namespace {

uint32_t colorCmd(uint8_t opcode, uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint32_t>(opcode) | (static_cast<uint32_t>(r) << 8)
           | (static_cast<uint32_t>(g) << 16) | (static_cast<uint32_t>(b) << 24);
}

uint32_t pos(int x, int y)
{
    return static_cast<uint32_t>((y & 0xFFFF) << 16) | static_cast<uint32_t>(x & 0xFFFF);
}

} // namespace

TEST(GpuCommandParserTest, ParsesMonochromeTriangle)
{
    const uint32_t words[] = {
        colorCmd(0x20, 30, 20, 10),
        pos(8, 16),
        pos(40, 16),
        pos(24, 32),
    };

    const auto result = GpuCommandParser::parseGp0(words, 4);
    ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    ASSERT_EQ(result.prims.size(), 1);
    EXPECT_EQ(result.prims[0].kind, PrimKind::MonoTri);
    EXPECT_EQ(result.prims[0].vertexCount, 3);
    EXPECT_EQ(result.prims[0].verts[0].r, 30);
    EXPECT_EQ(result.prims[0].verts[0].g, 20);
    EXPECT_EQ(result.prims[0].verts[0].b, 10);
    EXPECT_EQ(result.prims[0].verts[1].x, 40);
}

TEST(GpuCommandParserTest, ParsesDrawModeAndSprite)
{
    const uint32_t words[] = {
        colorCmd(0x60, 0, 0, 0),
        pos(8, 8),
        pos(32, 32),
        0x00080810u,
    };

    const auto result = GpuCommandParser::parseGp0(words, 4);
    ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    ASSERT_EQ(result.prims.size(), 1);
    EXPECT_EQ(result.prims[0].kind, PrimKind::Sprite);

    const uint32_t modeWords[] = {0xE1u | (0x1234u << 8)};
    const auto modeResult = GpuCommandParser::parseGp0(modeWords, 1);
    ASSERT_TRUE(modeResult.error.isEmpty()) << modeResult.error.toStdString();
    EXPECT_EQ(modeResult.drawModes.size(), 1);
    EXPECT_EQ(modeResult.drawModes[0].drawModeBits, modeWords[0]);

    const QString csv = GpuCommandParser::primsToCsv(result.prims);
    EXPECT_TRUE(csv.contains(QStringLiteral("kind,vertexCount")));
}

TEST(GpuCommandParserTest, ParsesTexturedTriangle)
{
    const uint32_t words[] = {
        colorCmd(0x24, 0, 0, 0),
        pos(8, 16),
        0x08100808u,
        pos(40, 16),
        0x08100808u,
        pos(24, 32),
        0x08100808u,
    };

    const auto result = GpuCommandParser::parseGp0(words, 7);
    ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    ASSERT_EQ(result.prims.size(), 1);
    EXPECT_EQ(result.prims[0].kind, PrimKind::TexturedTri);
    EXPECT_EQ(result.prims[0].clut, 0x0810u);
}

TEST(GpuCommandParserTest, ParsesGouraudTriangle)
{
    const uint32_t words[] = {
        colorCmd(0x30, 255, 0, 0),
        pos(10, 20),
        0x0000FF00u,
        pos(30, 20),
        0x00FF0000u,
        pos(20, 40),
    };

    const auto result = GpuCommandParser::parseGp0(words, 6);
    ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    ASSERT_EQ(result.prims.size(), 1);
    EXPECT_EQ(result.prims[0].kind, PrimKind::ShadedTri);
    EXPECT_EQ(result.prims[0].verts[0].r, 255);
    EXPECT_EQ(result.prims[0].verts[1].g, 255);
    EXPECT_EQ(result.prims[0].verts[2].b, 255);
}

TEST(GpuCommandParserTest, SkipsVramCopyCommands)
{
    const uint32_t words[] = {
        colorCmd(0x20, 1, 2, 3),
        pos(0, 0),
        pos(10, 0),
        pos(5, 10),
        0x000000C0u,
        0x00000000u,
        0x00000000u,
    };

    const auto result = GpuCommandParser::parseGp0(words, 7);
    ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    ASSERT_EQ(result.prims.size(), 1);
}

#endif // ENABLE_PS1_RIP
