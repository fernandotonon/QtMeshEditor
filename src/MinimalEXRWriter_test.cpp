#include "MinimalEXRWriter.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryFile>

#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Tiny stand-alone EXR reader — verifies the writer round-trips correctly
// without pulling in OpenEXR as a test dependency. Only decodes the exact
// subset we produce: 3-channel float32 scanline, NO_COMPRESSION, channel
// order B/G/R, INCREASING_Y. Aborts the test if it sees anything else, so
// drift in the writer's output is loud rather than silent.
// ---------------------------------------------------------------------------

namespace {

struct ParsedEXR {
    int width = 0;
    int height = 0;
    std::vector<float> rgb;  // row-major (y * width + x) * 3 + {R,G,B}
};

uint32_t readU32LE(const QByteArray& buf, int offset) {
    const auto* b = reinterpret_cast<const uint8_t*>(buf.constData() + offset);
    return uint32_t(b[0])
         | (uint32_t(b[1]) <<  8)
         | (uint32_t(b[2]) << 16)
         | (uint32_t(b[3]) << 24);
}
int32_t readI32LE(const QByteArray& buf, int offset) {
    return static_cast<int32_t>(readU32LE(buf, offset));
}
uint64_t readU64LE(const QByteArray& buf, int offset) {
    return static_cast<uint64_t>(readU32LE(buf, offset))
         | (static_cast<uint64_t>(readU32LE(buf, offset + 4)) << 32);
}
float readF32LE(const QByteArray& buf, int offset) {
    uint32_t bits = readU32LE(buf, offset);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// Read a NUL-terminated C string starting at `offset`. Advances `offset`
// past the terminator.
QByteArray readCStr(const QByteArray& buf, int& offset) {
    int start = offset;
    while (offset < buf.size() && buf[offset] != '\0') ++offset;
    QByteArray s = buf.mid(start, offset - start);
    ++offset;  // skip NUL
    return s;
}

ParsedEXR decodeRGB32F(const QString& path) {
    ParsedEXR out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    QByteArray buf = f.readAll();
    if (buf.size() < 8) return out;

    // Magic + version.
    if (readU32LE(buf, 0) != 0x01312f76u) return out;
    if (readU32LE(buf, 4) != 2u) return out;

    int p = 8;
    int dataWindowMinX = 0, dataWindowMinY = 0;
    int dataWindowMaxX = -1, dataWindowMaxY = -1;
    bool sawChannels = false;
    bool channelOrderBGR = false;
    bool compressionNone = false;

    while (p < buf.size() && buf[p] != '\0') {
        QByteArray name = readCStr(buf, p);
        QByteArray type = readCStr(buf, p);
        int32_t sz = readI32LE(buf, p); p += 4;
        const int attrStart = p;
        (void)attrStart;

        if (name == "channels" && type == "chlist") {
            int q = p;
            QByteArray order;
            while (q < p + sz && buf[q] != '\0') {
                QByteArray chName = readCStr(buf, q);
                order.append(chName.left(1));
                q += 16;  // pixelType u32 + pLinear u8 + 3 reserved + xSampling u32 + ySampling u32
            }
            sawChannels = true;
            channelOrderBGR = (order == "BGR");
        } else if (name == "compression" && type == "compression") {
            compressionNone = (static_cast<uint8_t>(buf[p]) == 0);
        } else if (name == "dataWindow" && type == "box2i") {
            dataWindowMinX = readI32LE(buf, p);
            dataWindowMinY = readI32LE(buf, p + 4);
            dataWindowMaxX = readI32LE(buf, p + 8);
            dataWindowMaxY = readI32LE(buf, p + 12);
        }
        p += sz;
    }
    ++p;  // skip terminator NUL

    if (!sawChannels || !channelOrderBGR || !compressionNone) return out;
    if (dataWindowMaxX < dataWindowMinX || dataWindowMaxY < dataWindowMinY) return out;
    const int w = dataWindowMaxX - dataWindowMinX + 1;
    const int h = dataWindowMaxY - dataWindowMinY + 1;

    // Skip the scanline offset table — we trust the writer's layout
    // and walk scanlines linearly.
    p += h * 8;

    std::vector<float> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);
    for (int y = 0; y < h; ++y) {
        const int yRead = readI32LE(buf, p); p += 4;
        const uint32_t blkSize = readU32LE(buf, p); p += 4;
        if (yRead != y) return ParsedEXR{};
        const uint32_t expectedSize = static_cast<uint32_t>(w) * 3u * 4u;
        if (blkSize != expectedSize) return ParsedEXR{};
        for (int x = 0; x < w; ++x) {
            const float b = readF32LE(buf, p + x * 4);
            const float g = readF32LE(buf, p + (w + x) * 4);
            const float r = readF32LE(buf, p + (2 * w + x) * 4);
            const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(w)
                              + static_cast<size_t>(x)) * 3u;
            rgb[idx + 0] = r;
            rgb[idx + 1] = g;
            rgb[idx + 2] = b;
        }
        p += static_cast<int>(blkSize);
    }
    out.width = w;
    out.height = h;
    out.rgb = std::move(rgb);
    return out;
}

QString tempPath() {
    QTemporaryFile t(QDir::tempPath() + "/MinimalEXRWriter_XXXXXX.exr");
    t.open();
    QString p = t.fileName();
    t.close();
    QFile::remove(p);
    return p;
}

} // namespace

TEST(MinimalEXRWriter, RejectsEmptyDimensions) {
    EXPECT_FALSE(MinimalEXR::writeRGB32F("/tmp/x.exr", 0, 10, {}));
    EXPECT_FALSE(MinimalEXR::writeRGB32F("/tmp/x.exr", 10, 0, {}));
    EXPECT_FALSE(MinimalEXR::writeRGB32F("/tmp/x.exr", -1, 10, {}));
}

TEST(MinimalEXRWriter, RejectsBufferSizeMismatch) {
    // 2x3 image = 6 pixels × 3 channels = 18 floats expected.
    std::vector<float> wrongSize(17, 0.0f);
    EXPECT_FALSE(MinimalEXR::writeRGB32F("/tmp/x.exr", 2, 3, wrongSize));
}

TEST(MinimalEXRWriter, RoundTripsSinglePixel) {
    std::vector<float> data { 0.25f, 0.5f, 0.75f };
    QString p = tempPath();
    ASSERT_TRUE(MinimalEXR::writeRGB32F(p, 1, 1, data));

    ParsedEXR decoded = decodeRGB32F(p);
    EXPECT_EQ(decoded.width, 1);
    EXPECT_EQ(decoded.height, 1);
    ASSERT_EQ(decoded.rgb.size(), 3u);
    EXPECT_FLOAT_EQ(decoded.rgb[0], 0.25f);
    EXPECT_FLOAT_EQ(decoded.rgb[1], 0.5f);
    EXPECT_FLOAT_EQ(decoded.rgb[2], 0.75f);

    QFile::remove(p);
}

TEST(MinimalEXRWriter, RoundTripsSubMillimeterValues) {
    // Mixamo-scale numbers: 2.1m bounds, looking for the kind of
    // sub-mm precision uint16 can't represent.
    std::vector<float> data {
        1.234567f, -0.123456f,  0.7891234f,   // pixel 0
        1.234568f, -0.123455f,  0.7891244f,   // pixel 1 — 1 micrometre apart
    };
    QString p = tempPath();
    ASSERT_TRUE(MinimalEXR::writeRGB32F(p, 2, 1, data));

    ParsedEXR decoded = decodeRGB32F(p);
    ASSERT_EQ(decoded.rgb.size(), 6u);
    // Each value must round-trip *exactly*. Lossy storage would
    // collapse adjacent pixels to identical values — exactly the
    // bug the 32-bit path exists to fix.
    EXPECT_FLOAT_EQ(decoded.rgb[0], 1.234567f);
    EXPECT_FLOAT_EQ(decoded.rgb[1], -0.123456f);
    EXPECT_FLOAT_EQ(decoded.rgb[2], 0.7891234f);
    EXPECT_FLOAT_EQ(decoded.rgb[3], 1.234568f);
    EXPECT_FLOAT_EQ(decoded.rgb[4], -0.123455f);
    EXPECT_FLOAT_EQ(decoded.rgb[5], 0.7891244f);
    EXPECT_NE(decoded.rgb[0], decoded.rgb[3]);  // adjacency preserved

    QFile::remove(p);
}

TEST(MinimalEXRWriter, RoundTripsMultiScanline) {
    // 4×3 image — gradient across X and per-row tag to verify scanline
    // offsets and Y-coords are written correctly.
    const int w = 4, h = 3;
    std::vector<float> data;
    data.reserve(static_cast<size_t>(w * h * 3));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            data.push_back(static_cast<float>(x) * 0.1f + 1.0f);
            data.push_back(static_cast<float>(y) * 10.0f);
            data.push_back(static_cast<float>(y * w + x));
        }
    }
    QString p = tempPath();
    ASSERT_TRUE(MinimalEXR::writeRGB32F(p, w, h, data));

    ParsedEXR decoded = decodeRGB32F(p);
    EXPECT_EQ(decoded.width, w);
    EXPECT_EQ(decoded.height, h);
    ASSERT_EQ(decoded.rgb.size(), data.size());

    for (size_t i = 0; i < data.size(); ++i) {
        EXPECT_FLOAT_EQ(decoded.rgb[i], data[i])
            << "Mismatch at index " << i;
    }
    QFile::remove(p);
}

TEST(MinimalEXRWriter, MagicAndVersionPresent) {
    std::vector<float> data(3, 0.0f);
    QString p = tempPath();
    ASSERT_TRUE(MinimalEXR::writeRGB32F(p, 1, 1, data));

    QFile f(p);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    QByteArray buf = f.read(8);
    f.close();

    ASSERT_EQ(buf.size(), 8);
    // Magic 0x01312f76 little-endian = 76 2f 31 01
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0x76);
    EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0x2f);
    EXPECT_EQ(static_cast<uint8_t>(buf[2]), 0x31);
    EXPECT_EQ(static_cast<uint8_t>(buf[3]), 0x01);
    // Version 2.
    EXPECT_EQ(static_cast<uint8_t>(buf[4]), 0x02);

    QFile::remove(p);
}
