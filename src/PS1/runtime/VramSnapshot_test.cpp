#include "VramSnapshot.h"
#include "PsxVramColor.h"

#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

TEST(VramSnapshotTest, WriteRectAndPixel)
{
    VramSnapshot vram;
    const uint16_t cells[] = {0x7C00, 0x03E0, 0x001F};
    vram.writeRect(10, 20, 3, 1, cells);
    EXPECT_EQ(vram.pixel(10, 20), 0x7C00u);
    EXPECT_EQ(vram.pixel(12, 20), 0x001Fu);
}

TEST(VramSnapshotTest, TpageRectDecodesPageOrigin)
{
    const QRect page = VramSnapshot::tpageRect(0x0001);
    EXPECT_EQ(page.x(), 64);
    EXPECT_EQ(page.y(), 0);
    EXPECT_EQ(page.width(), 256);
    EXPECT_EQ(page.height(), 256);
}

TEST(VramSnapshotTest, SavePngRoundTrip)
{
    VramSnapshot vram;
    vram.setPixel(0, 0, PsxVramColor::rgbaToBgr555(255, 0, 0, 255));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vram.png"));
    ASSERT_TRUE(vram.savePng(path));
    EXPECT_TRUE(QFileInfo::exists(path));
}

TEST(VramSnapshotTest, HasNonZeroOutsideRectDetectsTpageRegion)
{
    VramSnapshot vram;
    vram.setPixel(10, 10, 0x7C00u);
    EXPECT_FALSE(vram.hasNonZeroOutsideRect(QRect(0, 0, 320, 240), 1));

    vram.setPixel(400, 300, 0x7C00u);
    EXPECT_TRUE(vram.hasNonZeroOutsideRect(QRect(0, 0, 320, 240), 1));
}
