#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryFile>

#include <OgreImage.h>
#include <OgrePixelFormat.h>

#include "PS1/PS1TIM.h"

TEST(PS1TIMTest, SaveThenLoadTim16RoundTrip)
{
    // 2x2 RGBA image with distinct colors.
    std::vector<uint8_t> rgba = {
        255, 0, 0, 255,      0, 255, 0, 255,
        0, 0, 255, 255,      255, 255, 255, 255
    };

    Ogre::Image img;
    img.loadDynamicImage(rgba.data(), 2, 2, 1, Ogre::PF_BYTE_RGBA, false);

    QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tim_XXXXXX.tim");
    tmp.setAutoRemove(true);
    ASSERT_TRUE(tmp.open());
    const QString path = tmp.fileName();
    tmp.close();

    QString err;
    ASSERT_TRUE(PS1TIM::saveOgreImageToTim16(img, path, &err)) << err.toStdString();

    Ogre::Image decoded;
    ASSERT_TRUE(PS1TIM::loadTimToOgreImage(path, decoded, &err)) << err.toStdString();
    ASSERT_EQ(decoded.getFormat(), Ogre::PF_BYTE_RGBA);
    ASSERT_EQ(decoded.getWidth(), 256u);
    ASSERT_EQ(decoded.getHeight(), 256u);

    // Top-left should match (within 5-bit quantization).
    const uint8_t* d = decoded.getData();
    auto px = [&](int x, int y) {
        const size_t i = (size_t(y) * 256u + size_t(x)) * 4u;
        return std::array<uint8_t, 4>{d[i + 0], d[i + 1], d[i + 2], d[i + 3]};
    };
    const auto p00 = px(0, 0);
    EXPECT_GE(p00[0], 240);
    EXPECT_LE(p00[1], 16);
    EXPECT_LE(p00[2], 16);
    EXPECT_EQ(p00[3], 255);
}

